#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[query ...]");

HELP_DESCRIPTION_DECL(
    "The z builtin changes to a frequently visited directory ranked by "
    "frecency.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Z);

namespace koshka {

namespace {

constexpr usize Z_FRECENCY_MAX = 500;

struct frecency_entry
{
  String path;
  i64 rank;
  i64 last_access;
};

static fn frecency_store_path() throws -> Maybe<Path>
{
  if (let const override_path =
          os::get_environment_variable("KOSH_DIRECTORY_HISTORY");
      override_path.has_value() && !override_path->is_empty())
  {
    return Path{override_path->view()};
  }
  let home = os::get_home_directory();
  if (!home) return None;
  let path = *home;
  path.push_component(".kosh_directory_history");
  return path;
}

static fn now_epoch_seconds() wontthrow -> i64
{
  return static_cast<i64>(std::time(nullptr));
}

static fn recency_weight(i64 age_seconds) wontthrow -> double
{
  if (age_seconds < 3600) return 4.0;
  if (age_seconds < 86400) return 2.0;
  if (age_seconds < 604800) return 0.5;
  return 0.25;
}

static fn read_frecency_store(Allocator allocator) throws
    -> ArrayList<frecency_entry>
{
  let entries = ArrayList<frecency_entry>{allocator};
  let path = frecency_store_path();
  if (!path) return entries;
  let content = path->read_entire_file();
  if (!content) return entries;

  let const text = content->view();
  usize line_start = 0;
  for (usize i = 0; i <= text.length; i++) {
    if (i != text.length && text[i] != '\n') {
      continue;
    }
    let const line = text.substring_of_length(line_start, i - line_start);
    line_start = i + 1;
    if (line.is_empty()) continue;

    /* Each line is path, rank, and last-access, tab separated. */
    let const first_tab = line.find_character('\t');
    if (!first_tab) continue;
    let const after_path = line.substring(*first_tab + 1);
    let const second_tab = after_path.find_character('\t');
    if (!second_tab) continue;

    let const path_field = line.substring_of_length(0, *first_tab);
    let const rank_field = after_path.substring_of_length(0, *second_tab);
    let const time_field = after_path.substring(*second_tab + 1);
    let const rank = rank_field.to<i64>();
    let const last = time_field.to<i64>();
    if (rank.is_error() || last.is_error()) {
      continue;
    }
    entries.push(frecency_entry{
        String{allocator, path_field},
        rank.value(), last.value()
    });
  }
  return entries;
}

static fn write_frecency_store(const ArrayList<frecency_entry> &entries,
                               Allocator allocator) throws -> void
{
  let path = frecency_store_path();
  if (!path) return;

  let out = String{allocator};
  for (let const &entry : entries) {
    out.append(entry.path.view());
    out += '\t';
    out.append(String::from(entry.rank, allocator));
    out += '\t';
    out.append(String::from(entry.last_access, allocator));
    out += '\n';
  }

  let const fd = os::open_file_descriptor(path->text().view(),
                                          os::file_open_mode::Truncate);
  if (!fd) return;
  /* The store was just truncated, so a short write would drop entries. The
     write loops until the whole buffer lands or the descriptor stops accepting
     it. */
  usize total_written = 0;
  while (total_written < out.count()) {
    let const written = os::write_fd(*fd, out.c_str() + total_written,
                                     out.count() - total_written);
    if (!written || *written == 0) {
      break;
    }
    total_written += *written;
  }
  os::close_fd(*fd);
}

static fn contains_ignore_case(StringView haystack, StringView needle) wontthrow
    -> bool
{
  if (needle.is_empty()) return true;
  if (needle.length > haystack.length) return false;
  let const do_lower = [](char c) wontthrow -> char {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
  };
  for (usize i = 0; i + needle.length <= haystack.length; i++) {
    let is_matched = true;
    for (usize j = 0; j < needle.length; j++) {
      if (do_lower(haystack[i + j]) != do_lower(needle[j])) {
        is_matched = false;
        break;
      }
    }
    if (is_matched) return true;
  }
  return false;
}

} /* namespace */

fn record_directory_access(StringView directory, Allocator allocator) throws
    -> void
{
  if (directory.is_empty()) return;

  let entries = read_frecency_store(allocator);
  let const now = now_epoch_seconds();
  let was_found = false;
  for (let &entry : entries) {
    if (entry.path.view() == directory) {
      entry.rank += 1;
      entry.last_access = now;
      was_found = true;
      break;
    }
  }
  if (!was_found) {
    entries.push(frecency_entry{
        String{allocator, directory},
        1, now
    });
    /* The weakest older entry is overwritten with the just-added last entry.
       The search excludes the new entry, so a brand-new directory that is the
       rank minimum is kept rather than evicting itself. */
    if (entries.count() > Z_FRECENCY_MAX) {
      let const newest = entries.count() - 1;
      usize weakest = 0;
      for (usize i = 1; i < newest; i++)
        if (entries[i].rank < entries[weakest].rank) weakest = i;
      entries[weakest] = steal(entries[newest]);
      entries.pop_back();
    }
  }
  write_frecency_store(entries, allocator);
}

Z::Z() = default;

pure fn Z::kind() const wontthrow -> Builtin::Kind { return Kind::Z; }

fn Z::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const operands = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (operands.count() == 1) {
    let const home_directory = os::get_home_directory();
    if (!home_directory.has_value()) {
      throw ErrorWithLocationAndDetails{
          ec.source_location(), "Unable to determine the home directory",
          "Set `HOME` to a valid path"};
    }

    return run_cd_to_directory(cxt, ec, home_directory->text());
  }

  let query = String{cxt.scratch_allocator()};
  for (usize i = 1; i < operands.count(); i++) {
    if (i > 1) query += ' ';
    query.append(operands[i]);
  }

  LOG(Debug, "z ranking the frecency store against query '%s'", query.c_str());

  let entries = read_frecency_store(cxt.scratch_allocator());
  let const now = now_epoch_seconds();

  const frecency_entry *best = NULL;
  let best_score = -1.0;
  for (let const &entry : entries) {
    if (!query.is_empty() &&
        !contains_ignore_case(entry.path.view(), query.view()))
    {
      continue;
    }
    if (!Path{entry.path.view()}.to_absolute().normalized().is_directory())
      continue;
    let const score = static_cast<double>(entry.rank) *
                      recency_weight(now - entry.last_access);
    if (score > best_score) {
      best_score = score;
      best = &entry;
    }
  }

  if (best == NULL)
    throw make_error_for_arg(
        ec, 1, StringView{"No matching directory for '"} + query + "'");

  let const target = Path{best->path.view()}.to_absolute().normalized();

  LOG(Info, "z changing directory to '%s'", target.text().c_str());

  let const status = run_cd_to_directory(cxt, ec, target.text());
  if (status != 0) return status;
  record_directory_access(target.text().view(), cxt.scratch_allocator());

  ec.print_to_stdout(target.text() + "\n");
  return 0;
}

} /* namespace koshka */
