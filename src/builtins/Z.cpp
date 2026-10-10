/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the z builtin. The z builtin
 * changes to a frequently visited directory ranked by frecency.
 */

#include "../Builtin.hpp"
#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../ToiletlineHistory.hpp"
#include "../Utils.hpp"
#include "../base/Path.hpp"
#include "../base/Trace.hpp"

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

struct frecency_rank_comparator
{
  pure fn operator()(const frecency_entry &left,
                     const frecency_entry &right) const wontthrow->bool
  {
    return left.rank > right.rank;
  }
};

static fn frecency_store_path() throws -> Maybe<Path>
{
  if (let const override_path =
          os::get_environment_variable("KOSH_DIRECTORY_HISTORY");
      override_path.has_value() && !override_path->is_empty())
  {
    return Path{override_path->view()};
  }
  static let const home = os::get_home_directory();
  if (!home.has_value()) return None;
  let path = home->clone();
  path.append(".kosh_directory_history");
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

  unused(toiletline::write_history_file_atomically(
      *path, ".kosh_directory_history_write", out.view()));
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

static fn equals_ignore_case(StringView text, StringView needle) wontthrow
    -> bool
{
  return text.length == needle.length && contains_ignore_case(text, needle);
}

} /* namespace */

fn z_completion_candidates(StringView query, Allocator allocator) throws
    -> ArrayList<String>
{
  let entries = read_frecency_store(allocator);
  let const now = now_epoch_seconds();

  entries.sort([now](const frecency_entry &left, const frecency_entry &right) {
    let const left_score =
        static_cast<double>(left.rank) * recency_weight(now - left.last_access);
    let const right_score = static_cast<double>(right.rank) *
                            recency_weight(now - right.last_access);
    if (left_score != right_score) return left_score > right_score;
    if (left.last_access != right.last_access)
      return left.last_access > right.last_access;
    return left.path.view() < right.path.view();
  });

  let candidates = ArrayList<String>{allocator};
  for (let const &entry : entries) {
    if (!query.is_empty() && !contains_ignore_case(entry.path.view(), query))
      continue;

    let const directory = Path{entry.path.view()}.to_absolute().normalized();
    if (!directory.is_directory()) continue;
    candidates.push(String{allocator, directory.c_str()});
  }
  return candidates;
}

fn record_directory_access(StringView directory, Allocator allocator) throws
    -> void
{
  if (directory.is_empty()) return;

  let entries = read_frecency_store(allocator);
  let const now = now_epoch_seconds();
  let visited = Maybe<frecency_entry>{};
  for (usize i = 0; i < entries.count(); i++) {
    if (!os::paths_match_for_history(entries[i].path.view(), directory))
      continue;

    visited = steal(entries[i]);
    visited->rank += 1;
    visited->last_access = now;
    entries.remove(i);
    break;
  }
  if (!visited.has_value())
    visited = frecency_entry{String{allocator, directory}, 1, now};

  if (entries.count() >= Z_FRECENCY_MAX) {
    let const strongest =
        steal(entries).make_sorted(frecency_rank_comparator{});
    entries = ArrayList<frecency_entry>{allocator};
    entries.reserve(Z_FRECENCY_MAX);
    for (usize i = 0; i + 1 < Z_FRECENCY_MAX; i++)
      entries.push(frecency_entry{String{allocator, strongest[i].path.view()},
                                  strongest[i].rank, strongest[i].last_access});
  }
  entries.push(steal(*visited));
  write_frecency_store(entries, allocator);
}

fn Z::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const operands = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let const do_change_directory = [&](StringView directory) throws -> i32 {
    let const status = run_cd_to_directory(cxt, ec, directory);
    if (status == 0 && !cxt.execution_store().shell_is_interactive())
      record_directory_access(directory, cxt.scratch_allocator());

    return status;
  };

  if (operands.count() == 1) {
    let const home_directory = os::get_home_directory();
    if (!home_directory.has_value()) {
      throw ErrorWithLocationAndDetails{
          ec.source_location(), "Unable to determine the home directory",
          "Set `HOME` to a valid path"};
    }

    return do_change_directory(home_directory->text());
  }

  let query = String{cxt.scratch_allocator()};
  for (usize i = 1; i < operands.count(); i++) {
    if (i > 1) query += ' ';
    query.append(operands[i]);
  }

  if (operands.count() == 2) {
    let const literal_directory = Path{query.view()}.to_absolute().normalized();
    if (literal_directory.is_directory()) {
      let const status = do_change_directory(literal_directory.text());
      if (status != 0) return status;

      ec.print_to_stdout(literal_directory.text() + "\n");
      return 0;
    }
  }

  LOG(Debug, "z ranking the frecency store against query '%s'", query.c_str());

  let entries = read_frecency_store(cxt.scratch_allocator());
  let const now = now_epoch_seconds();

  let const do_match_tier = [&](StringView path) wontthrow -> i32 {
    if (equals_ignore_case(path, query.view())) return 2;
    if (equals_ignore_case(Path::filename(path), query.view())) return 1;
    return 0;
  };

  const frecency_entry *best = nullptr;
  let best_score = -1.0;
  i32 best_tier = -1;
  for (let const &entry : entries) {
    if (!query.is_empty() &&
        !contains_ignore_case(entry.path.view(), query.view()))
    {
      continue;
    }
    if (!Path{entry.path.view()}.to_absolute().normalized().is_directory())
      continue;
    let const tier = do_match_tier(entry.path.view());
    let const score = static_cast<double>(entry.rank) *
                      recency_weight(now - entry.last_access);
    if (tier > best_tier || (tier == best_tier && score > best_score)) {
      best_tier = tier;
      best_score = score;
      best = &entry;
    }
  }

  if (best == nullptr)
    throw make_error_for_arg(
        ec, 1, StringView{"No matching directory for '"} + query + "'");

  let const target = Path{best->path.view()}.to_absolute().normalized();

  LOG(Info, "z changing directory to '%s'", target.c_str());

  let const status = do_change_directory(target.text());
  if (status != 0) return status;

  ec.print_to_stdout(target.text() + "\n");
  return 0;
}

} /* namespace koshka */
