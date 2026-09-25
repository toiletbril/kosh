/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements goodcore. It captures a running process or accepts
 * an existing core, collects its executable and mapped libraries, records host
 * metadata, and creates a compressed debugging archive.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../ProgramResolver.hpp"
#include "../Utils.hpp"
#include "../base/Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-q] [-b executable] [-o archive] [--no-compress] "
                   "(-p pid | core)");

HELP_DESCRIPTION_DECL(
    "The goodcore utility captures or packages a core dump with its "
    "executable, mapped libraries, and host metadata.");

FLAG(GOODCORE_PID, String, 'p', "pid", "Capture this running process.");
FLAG(GOODCORE_BINARY, String, 'b', "binary",
     "Use this executable for an existing core dump.");
FLAG(GOODCORE_OUTPUT, String, 'o', "output", "Write the archive to this path.");
FLAG(GOODCORE_QUIET, Bool, 'q', "quiet", "Print only errors.");
FLAG(GOODCORE_NO_COMPRESS, Bool, '\0', "no-compress",
     "Create an uncompressed tar archive.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(GoodCore);

namespace koshka::koshkit {

namespace {

fn print_progress(const ExecContext &ec, bool should_show,
                  StringView message) throws -> void
{
  if (!should_show) return;
  let const is_terminal = colors::stderr_is_a_terminal();
  let const should_color = colors::stderr_wants_color();
  let output = String{heap_allocator()};
  if (is_terminal) output += "\r\x1b[2K";
  append_report_text(output, "goodcore: ", colors::ansi::BOLD_CYAN,
                     should_color);
  append_report_text(output, message, colors::ansi::BOLD_WHITE, should_color);
  if (!is_terminal) output += "\n";
  ec.print_to_stderr(output);
}

fn run_tool(const Path &tool, ArrayList<String> arguments,
            os::measured_output output) throws -> bool
{
  let command = ArrayList<String>{heap_allocator()};
  command.reserve(arguments.count() + 1);
  command.push(tool.text().clone());
  for (let &argument : arguments)
    command.push(steal(argument));

  let const result = os::run_measured(command, {}, output);
  return result.has_value() && result->exit_status == 0;
}

fn write_text_file(StringView path, StringView text) throws -> bool
{
  let const descriptor =
      os::open_file_descriptor(path, os::file_open_mode::Truncate);
  if (!descriptor.has_value()) return false;
  defer { unused(os::close_fd(*descriptor)); };

  usize written_count = 0;
  while (written_count < text.length) {
    let const chunk = os::write_fd(*descriptor, text.data + written_count,
                                   text.length - written_count);
    if (!chunk.has_value() || *chunk == 0) return false;
    written_count += *chunk;
  }

  return true;
}

pure fn stripped_root(StringView path) wontthrow -> StringView
{
  usize start_position = 0;
  while (start_position < path.length &&
         (path[start_position] == '/' || path[start_position] == '\\'))
  {
    start_position++;
  }

  return path.substring_of_length(start_position, path.length - start_position);
}

fn copy_into_root(const Path &stage, StringView source) throws -> bool
{
  if (!Path{source}.is_regular_file()) return false;

  let const relative = stripped_root(source);
  if (relative.is_empty()) return false;

  let destination = stage.clone();
  destination.append("root");
  destination.append(relative);
  if (!make_directories(destination.parent(), 0700)) return false;

  return copy_file_contents(source, destination.view(), false) ==
         copy_file_result::Success;
}

fn append_unique_path(ArrayList<String> &paths, StringView path,
                      Allocator allocator) throws -> void
{
  if (path.is_empty() || path[0] != '/') return;
  for (let const &existing : paths) {
    if (existing.view() == path) return;
  }

  if (Path{path, allocator}.is_regular_file())
    paths.push(String{allocator, path});
}

fn collect_paths_from_output(StringView output, ArrayList<String> &paths,
                             Allocator allocator) throws -> void
{
  for (let view : utils::split_lines(output, allocator, false)) {
    view = view.trim_blanks();
    let const start = view.find_character('/');
    if (!start.has_value()) continue;

    usize end_position = *start;
    while (end_position < view.length && view[end_position] != ' ' &&
           view[end_position] != '\t' && view[end_position] != ')' &&
           view[end_position] != '(')
    {
      end_position++;
    }

    append_unique_path(paths,
                       view.substring_of_length(*start, end_position - *start),
                       allocator);
  }
}

fn infer_binary(EvalContext &cxt, StringView core, Allocator allocator) throws
    -> Maybe<String>
{
  let const file = resolve_util_program(cxt, "file");
  if (!file.has_value()) return None;

  let arguments = ArrayList<String>{heap_allocator()};
  arguments.push(String{core});
  let const output =
      capture_util_program_output(*file, steal(arguments), 30'000'000'000);
  if (!output.has_value()) return None;

  constexpr StringView markers[] = {"execfn: '", "from '"};
  for (let const marker : markers) {
    let const marker_position = output->view().find_substring(marker);
    if (!marker_position.has_value()) continue;

    let const start_position = *marker_position + marker.length;
    let const remainder = output->view().substring_of_length(
        start_position, output->length() - start_position);
    let const end_position = remainder.find_character('\'');
    if (!end_position.has_value()) continue;

    let const candidate = remainder.substring_of_length(0, *end_position);
    if (Path{candidate, allocator}.is_regular_file())
      return String{allocator, candidate};

    let const matches = cxt.get_program_resolver().search(
        Path{candidate, allocator}.filename(),
        ProgramResolver::SearchMode::First,
        ProgramResolver::Requirement::Runnable,
        ProgramResolver::CachePolicy::Bypass);
    if (!matches.is_empty()) return matches[0].text().clone();
  }

  return None;
}

fn collect_core_libraries(EvalContext &cxt, StringView core, StringView binary,
                          ArrayList<String> &paths, Allocator allocator) throws
    -> void
{
  if (let const gdb = resolve_util_program(cxt, "gdb")) {
    let arguments = ArrayList<String>{heap_allocator()};
    arguments.push(String{"--batch"});
    arguments.push(String{"--nx"});
    arguments.push(String{"--eval-command=info sharedlibrary"});
    arguments.push(String{"-c"});
    arguments.push(String{core});
    arguments.push(String{binary});
    if (let const output =
            capture_util_program_output(*gdb, steal(arguments), 30'000'000'000))
      collect_paths_from_output(output->view(), paths, allocator);
  }

  let const platform_tools = os::goodcore_tools();
  if (let const dependency_tool =
          resolve_util_program(cxt, platform_tools.dependency_program))
  {
    let arguments = ArrayList<String>{heap_allocator()};
    if (platform_tools.dependency_uses_library_flag)
      arguments.push(String{"-L"});
    arguments.push(String{binary});
    if (let const output = capture_util_program_output(
            *dependency_tool, steal(arguments), 30'000'000'000))
      collect_paths_from_output(output->view(), paths, allocator);
  }
}

fn remove_stage(const Path &stage, Allocator allocator) throws -> void
{
  unused(remove_path(stage.view(), allocator, removal_mode::Recursive));
}

} // namespace

GoodCore::GoodCore() = default;

pure fn GoodCore::kind() const wontthrow -> Utility::Kind
{
  return Kind::GoodCore;
}

fn GoodCore::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();
  let const has_pid = FLAG_GOODCORE_PID.is_set();
  let const should_show_progress = !FLAG_GOODCORE_QUIET.is_enabled();
  defer
  {
    if (should_show_progress && colors::stderr_is_a_terminal())
      ec.print_to_stderr("\r\x1b[2K\n");
  };
  if (!has_pid && operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  if (has_pid && !operands.is_empty()) {
    let conflict_location = FLAG_GOODCORE_PID.value_location();
    if (operand_locations[0].position > conflict_location.position)
      conflict_location = operand_locations[0];
    KOSHKIT_REPORT_ERROR_AT(conflict_location, "conflicting input",
                            "pass either one running pid or one core file");
    return 2;
  }
  if (operands.count() > 1) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[1],
                            "extra operand '" + operands[1] + "'",
                            "pass exactly one core file");
    return 2;
  }

  if (has_pid && FLAG_GOODCORE_BINARY.is_set()) {
    let conflict_location = FLAG_GOODCORE_PID.value_location();
    if (FLAG_GOODCORE_BINARY.value_location().position >
        conflict_location.position)
    {
      conflict_location = FLAG_GOODCORE_BINARY.value_location();
    }
    KOSHKIT_REPORT_ERROR_AT(
        conflict_location, "conflicting flags",
        "--binary applies only when packaging an existing core file");
    return 2;
  }

  i64 process_id = 0;
  if (has_pid) {
    let const parsed = utils::parse_integer_in_base(FLAG_GOODCORE_PID.value(),
                                                    nullptr, int_base::decimal);
    if (parsed.is_error() || parsed.value() <= 0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_GOODCORE_PID.value_location(),
                              "invalid process id",
                              "the process id must be a positive integer");
      return 1;
    }
    process_id = parsed.value();
  }

  let binary = Maybe<String>{};
  let paths = ArrayList<String>{allocator};
  if (has_pid) {
    for (let const &file : os::list_process_open_files(process_id, allocator)) {
      if (file.use == os::process_file_use::Executable) {
        binary = file.path.clone();
      }
      if (file.use == os::process_file_use::Executable ||
          file.use == os::process_file_use::Mapped)
      {
        append_unique_path(paths, file.path.view(), allocator);
      }
    }
  } else if (FLAG_GOODCORE_BINARY.is_set()) {
    binary = Path{FLAG_GOODCORE_BINARY.value()}.to_absolute().text().clone();
  } else {
    binary = infer_binary(cxt, operands[0].view(), allocator);
  }

  if (!binary.has_value() || !Path{binary->view()}.is_regular_file()) {
    let const error_location = FLAG_GOODCORE_BINARY.is_set()
                                   ? FLAG_GOODCORE_BINARY.value_location()
                               : has_pid ? FLAG_GOODCORE_PID.value_location()
                                         : operand_locations[0];
    report_soft_koshkit_util_error(ec, cxt, error_location, args[0].view(),
                                   "executable not found",
                                   "pass its path with --binary");
    return 1;
  }

  let const stage_directory =
      os::make_temp_directory(Path::temp_directory(), "goodcore");
  if (!stage_directory.has_value()) {
    report_soft_koshkit_error(ec, cxt, "cannot create staging directory",
                              os::last_system_error_message());
    return 1;
  }
  let const stage = *stage_directory;
  defer { remove_stage(stage, allocator); };

  let dump_directory = stage.clone();
  dump_directory.append("dump");
  if (!os::make_directory(dump_directory.view(), 0700)) {
    report_soft_koshkit_error(ec, cxt, "cannot create dump directory",
                              os::last_system_error_message());
    return 1;
  }

  let core = dump_directory.clone();
  core.append("core");
  if (has_pid) {
    print_progress(ec, should_show_progress, "capturing process core");
    let const platform_tools = os::goodcore_tools();
    let debugger = Maybe<Path>{};
    if (!platform_tools.debugger_program.is_empty())
      debugger = resolve_util_program(cxt, platform_tools.debugger_program);
    let capture_arguments = ArrayList<String>{heap_allocator()};
    switch (platform_tools.capture_mode) {
    case os::goodcore_capture_mode::Lldb:
      capture_arguments.push(String{"--batch"});
      capture_arguments.push(String{"-p"});
      capture_arguments.push(String::from(process_id, heap_allocator()));
      capture_arguments.push(String{"-o"});
      capture_arguments.push(String{"process save-core "} + core.text());
      capture_arguments.push(String{"-o"});
      capture_arguments.push(String{"process detach"});
      break;
    case os::goodcore_capture_mode::Gcore:
      capture_arguments.push(String{"-o"});
      capture_arguments.push(core.text().clone());
      capture_arguments.push(String::from(process_id, heap_allocator()));
      break;
    case os::goodcore_capture_mode::Unsupported: break;
    }
    if (!debugger.has_value() || !run_tool(*debugger, steal(capture_arguments),
                                           os::measured_output::Inherit))
    {
      report_soft_koshkit_error(
          ec, cxt, "capture failed",
          "install the platform debugger and check process permissions");
      return 1;
    }
    if (platform_tools.capture_mode == os::goodcore_capture_mode::Gcore) {
      let const captured_core =
          Path{core.text() + "." + String::from(process_id, allocator)};
      if (!os::rename_path(captured_core.view(), core.view())) {
        report_soft_koshkit_error(ec, cxt, "capture failed",
                                  "the debugger produced no usable core file");
        return 1;
      }
    }
  } else {
    print_progress(ec, should_show_progress, "copying existing core");
    let const source = Path{operands[0].view()}.to_absolute();
    if (!source.is_regular_file()) {
      report_soft_koshkit_error(ec, cxt, "core file not found",
                                "the operand must name a regular file");
      return 1;
    }

    if (copy_file_contents(source.view(), core.view(), false) !=
        copy_file_result::Success)
    {
      report_soft_koshkit_error(ec, cxt, "cannot copy core file",
                                os::last_system_error_message());
      return 1;
    }
  }

  os::file_status core_status{};
  if (!os::stat_path_following(core.view(), core_status) ||
      core_status.size == 0)
  {
    report_soft_koshkit_error(ec, cxt, "capture failed",
                              "the core file is missing or empty");
    return 1;
  }

  print_progress(ec, should_show_progress,
                 "collecting executable and libraries");
  collect_core_libraries(cxt, core.view(), binary->view(), paths, allocator);
  print_progress(ec, should_show_progress,
                 String{"collected "} + String::from(paths.count(), allocator) +
                     " candidate files");

  append_unique_path(paths, binary->view(), allocator);
  usize copied_path_count = 0;
  for (let const &path : paths) {
    print_progress(ec, should_show_progress, String{"copying "} + path.view());
    if (!copy_into_root(stage, path.view())) {
      report_soft_koshkit_error(ec, cxt, "cannot copy required file",
                                path.view());
      return 1;
    }
    copied_path_count++;
  }

  let metadata = String{allocator};
  metadata += "Core: ";
  metadata += "dump/core";
  metadata += "\nExecutable: ";
  metadata += binary->view();
  metadata += "\nCollected files: ";
  metadata += String::from(copied_path_count, allocator).view();
  metadata += "\n";
  if (let const uname = resolve_util_program(cxt, "uname")) {
    let uname_arguments = ArrayList<String>{heap_allocator()};
    uname_arguments.push(String{"-a"});
    if (let const output = capture_util_program_output(
            *uname, steal(uname_arguments), 30'000'000'000))
    {
      metadata += "Host: ";
      metadata += output->view();
      if (metadata.is_empty() || metadata[metadata.length() - 1] != '\n') {
        metadata += "\n";
      }
    }
  }
  let metadata_path = stage.clone();
  metadata_path.append("INFO.txt");
  if (!write_text_file(metadata_path.view(), metadata.view())) {
    report_soft_koshkit_error(ec, cxt, "cannot write metadata",
                              os::last_system_error_message());
    return 1;
  }

  let const zstd = FLAG_GOODCORE_NO_COMPRESS.is_enabled()
                       ? Maybe<Path>{None}
                       : resolve_util_program(cxt, "zstd");
  let output =
      FLAG_GOODCORE_OUTPUT.is_set()
          ? Path{FLAG_GOODCORE_OUTPUT.value()}.to_absolute()
          : Path{String{"goodcore-"} + core.filename() +
                 (FLAG_GOODCORE_NO_COMPRESS.is_enabled() ? StringView{".tar"}
                  : zstd.has_value() ? StringView{".tar.zst"}
                                     : StringView{".tar.gz"})}
                .to_absolute();
  if (output.is_same_file_as(Path{binary->view()}) ||
      (!has_pid && output.is_same_file_as(Path{operands[0].view()})))
  {
    report_soft_koshkit_error(ec, cxt, "unsafe output path",
                              "the archive cannot replace an input file");
    return 1;
  }

  let const tar = resolve_util_program(cxt, "tar");
  if (!tar.has_value()) {
    report_soft_koshkit_error(ec, cxt, "tar is unavailable",
                              "install tar or place it on PATH");
    return 1;
  }

  let const temporary_output =
      os::write_to_named_temp_file(output.parent(), ".goodcore", StringView{});
  if (!temporary_output.has_value()) {
    report_soft_koshkit_error(ec, cxt, "cannot create temporary archive",
                              os::last_system_error_message());
    return 1;
  }
  defer { unused(os::remove_file(temporary_output->text().view())); };

  let archive_arguments = ArrayList<String>{heap_allocator()};
  archive_arguments.push(String{"-cf"});
  archive_arguments.push(temporary_output->text().clone());
  archive_arguments.push(String{"-C"});
  archive_arguments.push(stage.text().clone());
  archive_arguments.push(String{"."});
  bool did_archive = false;
  if (FLAG_GOODCORE_NO_COMPRESS.is_enabled()) {
    print_progress(ec, should_show_progress, "creating tar archive");
    did_archive =
        run_tool(*tar, steal(archive_arguments), os::measured_output::Suppress);
  } else if (zstd.has_value()) {
    print_progress(ec, should_show_progress,
                   "creating tar archive and compressing with zstd");
    let const temporary_tar = os::write_to_named_temp_file(
        output.parent(), ".goodcore-tar", StringView{});
    if (!temporary_tar.has_value()) {
      report_soft_koshkit_error(ec, cxt, "cannot create temporary archive",
                                os::last_system_error_message());
      return 1;
    }
    defer { unused(os::remove_file(temporary_tar->text().view())); };

    archive_arguments[1] = temporary_tar->text().clone();
    did_archive =
        run_tool(*tar, steal(archive_arguments), os::measured_output::Suppress);
    if (did_archive) {
      let compression_arguments = ArrayList<String>{heap_allocator()};
      compression_arguments.push(String{"-q"});
      compression_arguments.push(String{"-f"});
      compression_arguments.push(temporary_tar->text().clone());
      compression_arguments.push(String{"-o"});
      compression_arguments.push(temporary_output->text().clone());
      did_archive = run_tool(*zstd, steal(compression_arguments),
                             os::measured_output::Suppress);
    }
  } else {
    print_progress(ec, should_show_progress,
                   "creating tar archive and compressing with gzip");
    archive_arguments[0] = String{"-czf"};
    did_archive =
        run_tool(*tar, steal(archive_arguments), os::measured_output::Suppress);
  }

  if (!did_archive) {
    report_soft_koshkit_error(ec, cxt, "archive creation failed",
                              "the selected archiver returned a failure");
    return 1;
  }

  if (!os::rename_path(temporary_output->view(), output.view())) {
    report_soft_koshkit_error(ec, cxt, "cannot publish archive",
                              os::last_system_error_message());
    return 1;
  }

  if (!FLAG_GOODCORE_QUIET.is_enabled()) {
    let const should_color = koshkit_should_color();
    let table = ReportTable{allocator};
    table.add_column("FIELD", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("VALUE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.reserve(2);
    cells.push({"Archive", colors::ansi::BOLD_CYAN});
    cells.push({output.view(), colors::ansi::GREEN});
    table.add_row(cells);
    cells.clear();
    cells.push({"Executable", colors::ansi::BOLD_CYAN});
    cells.push({binary->view(), colors::ansi::GREEN});
    table.add_row(cells);
    cells.clear();
    let const file_count = String::from(copied_path_count, allocator);
    cells.push({"Files", colors::ansi::BOLD_CYAN});
    cells.push({file_count.view(), colors::ansi::GREEN});
    table.add_row(cells);

    let result = String{allocator};
    result += table.to_string(should_color, "").view();
    ec.print_to_stdout(result);
  }

  return 0;
}

} // namespace koshka::koshkit
