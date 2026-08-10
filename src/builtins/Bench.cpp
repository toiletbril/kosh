#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Colors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

FLAG(bench_runs, String, '\0', "runs",
     "The exact number of samples per command.");
FLAG(bench_duration, String, '\0', "duration",
     "The sampling budget per command in milliseconds.");
FLAG(BENCH_IGNORE_EXIT, Bool, '\0', "ignore-exit-code",
     "Keep sampling even when the command returns a non-zero exit code.");
FLAG(BENCH_SHOW_OUTPUT, Bool, '\0', "show-output",
     "Show the command's output instead of suppressing it.");
FLAG(BENCH_NO_SHELL, Bool, '\0', "no-shell",
     "Fork the command directly, without wrapping it in a shell.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

HELP_SYNOPSIS_DECL("[--runs n] [--duration ms] [--ignore-exit-code] "
                   "[--show-output] [--no-shell] command [command ...]");

HELP_DESCRIPTION_DECL(
    "The bench builtin measures and compares the runtime of each command.");

REGISTER_BUILTIN_FLAGS(Bench);

namespace koshka {

namespace {

constexpr usize MAX_SAMPLES = 100000;
constexpr usize MIN_SAMPLES = 3;
constexpr u64 DEFAULT_DURATION_MILLIS = 3000;

enum class metric_unit : u8
{
  Nanoseconds,
  Bytes,
  Count,
};

struct metric_stats
{
  double mean{0};
  double std_dev{0};
  double min{0};
  double max{0};
};

struct bench_sample
{
  double wall_nanos{0};
  double peak_rss_bytes{0};
  double cpu_cycles{0};
  double instructions{0};
  double cache_references{0};
  double cache_misses{0};
  double branch_misses{0};
};

class CommandResult
{
public:
  explicit CommandResult(Allocator allocator) : label(allocator) {}
  String label;
  usize sample_count{0};
  bool has_perf{false};
  bool is_perf_system_wide{false};
  metric_stats wall_time{};
  metric_stats peak_rss{};
  metric_stats cpu_cycles{};
  metric_stats instructions{};
  metric_stats cache_references{};
  metric_stats cache_misses{};
  metric_stats branch_misses{};
};

fn colored(StringView code, bool should_color) wontthrow -> StringView
{
  return should_color ? code : StringView{};
}

template <typename Accessor>
fn compute_stats(const ArrayList<bench_sample> &samples,
                 Accessor accessor) throws -> metric_stats
{
  let stats = metric_stats{};
  if (samples.is_empty()) return stats;

  double sum = 0;
  double minimum = accessor(samples[0]);
  double maximum = accessor(samples[0]);
  for (usize i = 0; i < samples.count(); i++) {
    const double value = accessor(samples[i]);
    sum += value;
    if (value < minimum) minimum = value;
    if (value > maximum) maximum = value;
  }

  const double count = static_cast<double>(samples.count());
  stats.mean = sum / count;
  stats.min = minimum;
  stats.max = maximum;

  double variance_sum = 0;
  for (usize i = 0; i < samples.count(); i++) {
    const double delta = accessor(samples[i]) - stats.mean;
    variance_sum += delta * delta;
  }
  stats.std_dev =
      (samples.count() > 1) ? std::sqrt(variance_sum / (count - 1)) : 0.0;

  return stats;
}

cold fn format_metric(double value, metric_unit unit,
                      Allocator allocator) throws -> String
{
  double scaled = value;
  const char *suffix = "";

  switch (unit) {
  case metric_unit::Nanoseconds:
    if (value >= 1e9) {
      scaled = value / 1e9;
      suffix = "s";
    } else if (value >= 1e6) {
      scaled = value / 1e6;
      suffix = "ms";
    } else if (value >= 1e3) {
      scaled = value / 1e3;
      suffix = "us";
    } else {
      suffix = "ns";
    }
    break;
  case metric_unit::Bytes:
    if (value >= 1024.0 * 1024 * 1024) {
      scaled = value / (1024.0 * 1024 * 1024);
      suffix = "GB";
    } else if (value >= 1024.0 * 1024) {
      scaled = value / (1024.0 * 1024);
      suffix = "MB";
    } else if (value >= 1024.0) {
      scaled = value / 1024.0;
      suffix = "KB";
    } else {
      suffix = "B";
    }
    break;
  case metric_unit::Count:
    if (value >= 1e9) {
      scaled = value / 1e9;
      suffix = "G";
    } else if (value >= 1e6) {
      scaled = value / 1e6;
      suffix = "M";
    } else if (value >= 1e3) {
      scaled = value / 1e3;
      suffix = "K";
    } else {
      suffix = "";
    }
    break;
  }

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.3g%s", scaled, suffix);
  return String{allocator, buffer};
}

/* The color escapes are added at render time, not here, since they carry no
   display width and would corrupt the width measurement. */
class MetricRow
{
public:
  explicit MetricRow(Allocator allocator)
      : mean(allocator), std_dev(allocator), min(allocator), max(allocator)
  {}
  StringView name{};
  metric_unit unit{metric_unit::Count};
  String mean;
  String std_dev;
  String min;
  String max;
};

fn make_metric_row(StringView name, const metric_stats &stats, metric_unit unit,
                   Allocator allocator) throws -> MetricRow
{
  let row = MetricRow{allocator};
  row.name = name;
  row.unit = unit;
  row.mean = format_metric(stats.mean, unit, allocator);
  row.std_dev = format_metric(stats.std_dev, unit, allocator);
  row.min = format_metric(stats.min, unit, allocator);
  row.max = format_metric(stats.max, unit, allocator);
  return row;
}

fn append_padding(String &out, usize count) throws -> void
{
  for (usize i = 0; i < count; i++)
    out.push(' ');
}

fn append_metric_line(String &out, const MetricRow &row, usize mean_width,
                      usize std_dev_width, bool should_color) throws -> void
{
  out += "  ";
  out.append(row.name);
  let const name_padding = row.name.length < 22 ? 22 - row.name.length : 1;
  append_padding(out, name_padding);

  out.append(colored(colors::ansi::BOLD_GREEN, should_color));
  out.append(row.mean.view());
  out.append(colored(colors::ansi::RESET, should_color));
  append_padding(out, mean_width - row.mean.length());

  out += " +/- ";
  out.append(colored(colors::ansi::BOLD_GREEN, should_color));
  out.append(row.std_dev.view());
  out.append(colored(colors::ansi::RESET, should_color));
  append_padding(out, std_dev_width - row.std_dev.length());

  out += "  (";
  out.append(colored(colors::ansi::BOLD_CYAN, should_color));
  out.append(row.min.view());
  out.append(colored(colors::ansi::RESET, should_color));
  out += " ... ";
  out.append(colored(colors::ansi::BOLD_MAGENTA, should_color));
  out.append(row.max.view());
  out.append(colored(colors::ansi::RESET, should_color));
  out += ")\n";
}

fn append_relative_line(String &out, StringView name, const metric_stats &first,
                        const metric_stats &other, bool should_color) throws
    -> void
{
  out += "  ";
  out.append(name);
  for (usize pad = name.length; pad < 18; pad++)
    out.push(' ');

  if (first.mean <= 0 || other.mean <= 0) {
    out += "n/a\n";
    return;
  }

  const double ratio = other.mean / first.mean;

  /* The relative uncertainty is the sum in quadrature of the two coefficients
     of variation, scaled onto the ratio, so a noisy pair reports a wider
     band. */
  const double first_cv = first.std_dev / first.mean;
  const double other_cv = other.std_dev / other.mean;
  const double ratio_uncertainty =
      ratio * std::sqrt(first_cv * first_cv + other_cv * other_cv);

  char buffer[128];
  if (ratio > 1.0) {
    std::snprintf(buffer, sizeof(buffer), "%.2fx slower +/- %.2f", ratio,
                  ratio_uncertainty);
    out.append(colored(colors::ansi::BOLD_RED, should_color));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2fx faster +/- %.2f", 1.0 / ratio,
                  ratio_uncertainty / (ratio * ratio));
    out.append(colored(colors::ansi::BOLD_GREEN, should_color));
  }
  out += StringView{buffer};
  out.append(colored(colors::ansi::RESET, should_color));
  out += "\n";
}

fn parse_count_flag(const FlagString &flag, StringView flag_name) throws -> u64
{
  let const parsed = flag.value().to<u64>();
  if (parsed.is_error()) {
    let const error = Error{StringView{"--"} + flag_name +
                            " expects a number, got '" + flag.value() + "'"};
    relocate_error(error, flag.value_location());
  }
  return parsed.value();
}

constexpr u64 PROGRESS_INTERVAL_NANOS = 16ULL * 1000000ULL;
const StringView CLEAR_PROGRESS_LINE{"\r\x1b[2K"};

fn progress_is_enabled() throws -> bool { return colors::stderr_wants_color(); }

fn draw_progress(StringView command, u64 percent, Allocator allocator) throws
    -> void
{
  let line = String{allocator};
  line += CLEAR_PROGRESS_LINE;
  line += "Benchmarking '";
  line.append(command);
  line += "' ";
  line += String::from(percent > 100 ? 100 : percent, allocator);
  line += "%..";
  print_error(line.view());
}

fn clear_progress() wontthrow -> void
{
  unused(os::write_fd(KOSH_STDERR, CLEAR_PROGRESS_LINE.data,
                      CLEAR_PROGRESS_LINE.count()));
}

fn sample_command(StringView shell_binary, StringView command,
                  Maybe<u64> run_limit, u64 duration_millis,
                  bool should_show_progress, bool should_suppress_output,
                  bool should_ignore_exit_code, bool should_use_shell,
                  bool &was_interrupted, bool &did_command_fail,
                  i64 &failure_status, Allocator allocator) throws
    -> CommandResult
{
  defer
  {
    if (should_show_progress) clear_progress();
  };

  let result = CommandResult{allocator};
  result.label = command;

  let child_argv = ArrayList<String>{allocator};
  if (should_use_shell) {
    child_argv.push(String{allocator, shell_binary});
    child_argv.push(String{allocator, "-c"});
    child_argv.push(String{allocator, command});
  } else {
    usize start = 0;
    for (usize i = 0; i <= command.length; i++) {
      let const at_end = i == command.length;
      if (at_end || command[i] == ' ' || command[i] == '\t') {
        if (i > start)
          child_argv.push(
              String{allocator, command.substring_of_length(start, i - start)});
        start = i + 1;
      }
    }

    if (child_argv.is_empty())
      throw Error{StringView{"bench --no-shell needs a command word in '"} +
                  command + "'"};
  }

  let samples = ArrayList<bench_sample>{allocator};
  const u64 duration_nanos = duration_millis * 1000000ULL;
  const u64 start_nanos = os::monotonic_nanos();
  u64 last_progress_nanos = 0;
  bool has_perf = true;
  bool is_perf_system_wide = false;

  for (usize i = 0;; i++) {
    if (os::INTERRUPT_REQUESTED) {
      was_interrupted = true;
      break;
    }

    const u64 elapsed_nanos = os::monotonic_nanos() - start_nanos;
    if (run_limit.has_value() && i >= *run_limit) break;
    if (!run_limit.has_value() && i >= MIN_SAMPLES &&
        elapsed_nanos >= duration_nanos)
    {
      break;
    }
    if (i >= MAX_SAMPLES) break;

    if (should_show_progress &&
        (elapsed_nanos - last_progress_nanos) >= PROGRESS_INTERVAL_NANOS)
    {
      last_progress_nanos = elapsed_nanos;
      u64 percent = 0;
      if (run_limit.has_value() && *run_limit > 0) {
        percent = static_cast<u64>(i) * 100 / *run_limit;
      } else if (duration_nanos > 0) {
        percent = static_cast<u64>(static_cast<u128>(elapsed_nanos) * 100 /
                                   duration_nanos);
      }
      draw_progress(command, percent, allocator);
    }

    let const measured = os::run_measured(
        child_argv, should_suppress_output ? os::measured_output::Suppress
                                           : os::measured_output::Inherit);
    if (!measured.has_value())
      throw Error{StringView{"Unable to run '"} + command +
                  "': " + os::last_system_error_message()};

    if (os::INTERRUPT_REQUESTED) {
      was_interrupted = true;
      break;
    }

    if (measured->exit_status != 0 && !should_ignore_exit_code) {
      did_command_fail = true;
      failure_status = measured->exit_status;
      break;
    }

    let sample = bench_sample{};
    sample.wall_nanos = static_cast<double>(measured->wall_nanos);
    sample.peak_rss_bytes = static_cast<double>(measured->peak_rss_bytes);
    has_perf = has_perf && measured->has_perf;
    is_perf_system_wide = is_perf_system_wide || measured->is_perf_system_wide;
    if (measured->has_perf) {
      sample.cpu_cycles = static_cast<double>(measured->perf.cpu_cycles);
      sample.instructions = static_cast<double>(measured->perf.instructions);
      sample.cache_references =
          static_cast<double>(measured->perf.cache_references);
      sample.cache_misses = static_cast<double>(measured->perf.cache_misses);
      sample.branch_misses = static_cast<double>(measured->perf.branch_misses);
    }
    samples.push(sample);
  }

  result.sample_count = samples.count();
  result.has_perf = !samples.is_empty() && has_perf;
  result.is_perf_system_wide = result.has_perf && is_perf_system_wide;
  result.wall_time = compute_stats(
      samples, [](const bench_sample &s) { return s.wall_nanos; });
  result.peak_rss = compute_stats(
      samples, [](const bench_sample &s) { return s.peak_rss_bytes; });
  result.cpu_cycles = compute_stats(
      samples, [](const bench_sample &s) { return s.cpu_cycles; });
  result.instructions = compute_stats(
      samples, [](const bench_sample &s) { return s.instructions; });
  result.cache_references = compute_stats(
      samples, [](const bench_sample &s) { return s.cache_references; });
  result.cache_misses = compute_stats(
      samples, [](const bench_sample &s) { return s.cache_misses; });
  result.branch_misses = compute_stats(
      samples, [](const bench_sample &s) { return s.branch_misses; });

  return result;
}

fn append_summary(String &out, const CommandResult &result, bool should_color,
                  Allocator allocator) throws -> void
{
  out.append(colored(colors::ansi::BOLD, should_color));
  out += "Benchmark: ";
  out += result.label;
  out.append(colored(colors::ansi::RESET, should_color));
  out += " (" + String::from(result.sample_count, allocator) + " runs)\n";

  let rows = ArrayList<MetricRow>{allocator};
  rows.push(make_metric_row("wall time", result.wall_time,
                            metric_unit::Nanoseconds, allocator));
  rows.push(make_metric_row("peak rss", result.peak_rss, metric_unit::Bytes,
                            allocator));
  if (result.has_perf) {
    let const cpu_cycles_name = result.is_perf_system_wide
                                    ? StringView{"cpu cycles (system)"}
                                    : StringView{"cpu cycles"};
    let const instructions_name = result.is_perf_system_wide
                                      ? StringView{"instructions (system)"}
                                      : StringView{"instructions"};
    let const cache_references_name = result.is_perf_system_wide
                                          ? StringView{"cache refs (system)"}
                                          : StringView{"cache refs"};
    let const cache_misses_name = result.is_perf_system_wide
                                      ? StringView{"cache misses (system)"}
                                      : StringView{"cache misses"};
    let const branch_misses_name = result.is_perf_system_wide
                                       ? StringView{"branch misses (system)"}
                                       : StringView{"branch misses"};
    rows.push(make_metric_row(cpu_cycles_name, result.cpu_cycles,
                              metric_unit::Count, allocator));
    rows.push(make_metric_row(instructions_name, result.instructions,
                              metric_unit::Count, allocator));
    rows.push(make_metric_row(cache_references_name, result.cache_references,
                              metric_unit::Count, allocator));
    rows.push(make_metric_row(cache_misses_name, result.cache_misses,
                              metric_unit::Count, allocator));
    rows.push(make_metric_row(branch_misses_name, result.branch_misses,
                              metric_unit::Count, allocator));
  }

  usize mean_width = 0;
  usize std_dev_width = 0;
  for (usize i = 0; i < rows.count(); i++) {
    if (rows[i].mean.length() > mean_width) mean_width = rows[i].mean.length();
    if (rows[i].std_dev.length() > std_dev_width)
      std_dev_width = rows[i].std_dev.length();
  }

  for (usize i = 0; i < rows.count(); i++)
    append_metric_line(out, rows[i], mean_width, std_dev_width, should_color);
}

fn append_comparison(String &out, const CommandResult &first,
                     const CommandResult &other, bool should_color) throws
    -> void
{
  out.append(colored(colors::ansi::BOLD, should_color));
  out += "Relative to: ";
  out += first.label;
  out.append(colored(colors::ansi::RESET, should_color));
  out += "\n  ";
  out.append(colored(colors::ansi::BOLD, should_color));
  out += other.label;
  out.append(colored(colors::ansi::RESET, should_color));
  out += "\n";

  append_relative_line(out, "wall time", first.wall_time, other.wall_time,
                       should_color);
  append_relative_line(out, "peak rss", first.peak_rss, other.peak_rss,
                       should_color);
  if (first.has_perf && other.has_perf) {
    let const cpu_cycles_name = first.is_perf_system_wide
                                    ? StringView{"cpu cycles (system)"}
                                    : StringView{"cpu cycles"};
    let const instructions_name = first.is_perf_system_wide
                                      ? StringView{"instructions (system)"}
                                      : StringView{"instructions"};
    append_relative_line(out, cpu_cycles_name, first.cpu_cycles,
                         other.cpu_cycles, should_color);
    append_relative_line(out, instructions_name, first.instructions,
                         other.instructions, should_color);
  }
}

} /* namespace */

Bench::Bench() = default;

pure fn Bench::kind() const wontthrow -> Builtin::Kind { return Kind::Bench; }

cold fn Bench::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  /* The flag parser keeps argv[0], so the commands start at index 1. */
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const arguments =
      PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (arguments.count() < 2) {
    throw Error{StringView{"No command given"}};
  }

  unused(cxt.materialize_kosh_identity());

  Maybe<u64> run_limit = None;
  if (FLAG_bench_runs.is_set())
    run_limit = parse_count_flag(FLAG_bench_runs, StringView{"runs"});

  if (run_limit.has_value() && (*run_limit == 0 || *run_limit > MAX_SAMPLES))
    throw ErrorWithLocation{
        FLAG_bench_runs.value_location(),
        "--runs expects a number from 1 through " +
            String::from(MAX_SAMPLES, cxt.scratch_allocator())};

  u64 duration_millis = DEFAULT_DURATION_MILLIS;
  if (FLAG_bench_duration.is_set())
    duration_millis =
        parse_count_flag(FLAG_bench_duration, StringView{"duration"});
  if (duration_millis > UINT64_MAX / 1000000ULL)
    throw ErrorWithLocation{FLAG_bench_duration.value_location(),
                            "--duration is too large"};

  let const should_color = colors::stdout_wants_color();
  let const should_show_progress = progress_is_enabled();
  let const should_ignore_exit_code = FLAG_BENCH_IGNORE_EXIT.is_enabled();
  let const should_suppress_output = !FLAG_BENCH_SHOW_OUTPUT.is_enabled();
  let const should_use_shell = !FLAG_BENCH_NO_SHELL.is_enabled();

  let shell_binary =
      String{cxt.scratch_allocator(), cxt.shell_executable_path()};
  if (shell_binary.is_empty()) {
    if (let const detected = os::current_executable_path())
      shell_binary = String{cxt.scratch_allocator(), detected->view()};
  }
  if (should_use_shell && shell_binary.is_empty())
    throw Error{
        StringView{"bench cannot find the kosh binary to run a sample"}};

  LOG(Debug, "bench sampling %zu commands for %llu ms each",
      arguments.count() - 1, static_cast<unsigned long long>(duration_millis));

  let results = ArrayList<CommandResult>{cxt.scratch_allocator()};
  for (usize i = 1; i < arguments.count(); i++) {
    bool was_interrupted = false;
    bool did_command_fail = false;
    i64 failure_status = 0;
    results.push(sample_command(
        shell_binary.view(), arguments[i].view(), run_limit, duration_millis,
        should_show_progress, should_suppress_output, should_ignore_exit_code,
        should_use_shell, was_interrupted, did_command_fail, failure_status,
        cxt.scratch_allocator()));

    if (was_interrupted) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }

    if (did_command_fail) {
      let const operand_location = i < operand_locations.count()
                                       ? operand_locations[i]
                                       : ec.source_location();
      report_soft_builtin_error(
          ec, cxt, operand_location,
          StringView{"the command `"} + arguments[i].view() +
              "` exited with status " +
              String::from(failure_status, cxt.scratch_allocator()),
          "Pass `--ignore-exit-code` to benchmark it regardless");
      return static_cast<i32>(failure_status);
    }
  }

  let out = String{cxt.scratch_allocator()};
  for (usize i = 0; i < results.count(); i++) {
    if (i > 0) out += "\n";
    append_summary(out, results[i], should_color, cxt.scratch_allocator());
  }

  if (results.count() > 1) {
    out += "\n";
    for (usize i = 1; i < results.count(); i++)
      append_comparison(out, results[0], results[i], should_color);
  }

  ec.print_to_stdout(out);
  return 0;
}

} /* namespace koshka */
