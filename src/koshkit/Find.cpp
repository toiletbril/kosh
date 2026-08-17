#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL(
    "[path ...] [-name glob] [-type fdl] [-maxdepth n] [-mindepth n]");

HELP_DESCRIPTION_DECL(
    "The find utility walks each path and prints every entry under it.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Find);

namespace koshka {

namespace koshkit {

struct find_options
{
  const ArrayList<StringView> *name_patterns{nullptr};
  const Bitset *glob_active{nullptr};
  char type_filter{0};
  i64 max_depth{-1};
  i64 min_depth{0};
};

static fn find_entry_matches(char type_letter, StringView filename, usize depth,
                             const find_options &options) throws -> bool
{
  if (static_cast<i64>(depth) < options.min_depth) return false;
  if (options.max_depth >= 0 && static_cast<i64>(depth) > options.max_depth) {
    return false;
  }

  switch (options.type_filter) {
  case 'f':
    if (type_letter != '-') return false;
    break;
  case 'd':
    if (type_letter != 'd') return false;
    break;
  case 'l':
    if (type_letter != 'l') return false;
    break;
  default: break;
  }

  if (options.name_patterns != nullptr) {
    for (let const &pattern : *options.name_patterns) {
      if (!utils::glob_matches(pattern, filename, *options.glob_active, 0))
        return false;
    }
  }

  return true;
}

static fn find_walk(const ExecContext &ec, EvalContext &cxt, const Path &path,
                    StringView display, usize depth,
                    const find_options &options, String &output,
                    i32 &exit_status, Allocator allocator) throws -> void
{
  /* The stat reads the symlink, not its target, and a failed stat yields the
     marker '\0' that matches no -type filter and is not descended. */
  os::file_status status{};
  let const type_letter = os::stat_path(path.text().view(), status)
                              ? os::file_type_letter(status.mode)
                              : '\0';

  if (find_entry_matches(type_letter, path.filename(), depth, options)) {
    output += display;
    output += '\n';
  }

  let const should_descend =
      options.max_depth < 0 || static_cast<i64>(depth) < options.max_depth;
  if (!should_descend || type_letter != 'd') {
    return;
  }

  Maybe<ArrayList<String>> names = Path::read_directory(path);
  if (!names.has_value()) {
    if (!path.is_readable()) {
      report_soft_koshkit_error(ec, cxt,
                                "find: '" + String{allocator, display} +
                                    "': Permission denied");
      exit_status = 1;
    }

    return;
  }
  names->sort();

  for (let const &child_name : *names) {
    if (os::INTERRUPT_REQUESTED) return;

    String child_display{allocator, display};
    if (!child_display.is_empty() && child_display.back() != '/') {
      child_display += '/';
    }
    child_display += child_name.view();
    let const child_path = Path{child_display.view()};
    find_walk(ec, cxt, child_path, child_display.view(), depth + 1, options,
              output, exit_status, allocator);
  }
}

static fn parse_depth_argument(const ArrayList<String> &args, usize index,
                               StringView predicate, Allocator allocator) throws
    -> i64
{
  if (index >= args.count())
    throw Error{
        "find: " + String{allocator, predicate}
          + " expects a number"
    };

  /* A negative value is rejected rather than parsed, since max_depth carries -1
     as its no-limit sentinel, so a negative -maxdepth would otherwise read as
     an unbounded walk rather than the error find gives. */
  let const parsed_value = args[index].view().to<i64>();
  if (parsed_value.is_error() || parsed_value.value() < 0) {
    throw Error{
        "find: " + String{allocator, predicate}
          +
        " expects a non-negative number, got '" + args[index] + "'"
    };
  }

  return parsed_value.value();
}

Find::Find() = default;

pure fn Find::kind() const wontthrow -> Utility::Kind { return Kind::Find; }

fn Find::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  unused(arg_locations);

  ArrayList<StringView> roots{cxt.scratch_allocator()};
  ArrayList<StringView> name_patterns{cxt.scratch_allocator()};
  find_options options{};
  Bitset name_glob_active{cxt.scratch_allocator()};

  /* The flag parser is bypassed, a predicate such as -name is not a
     single-letter flag bundle. An empty argument is a start path, not a
     predicate, so it is collected as a root. */
  usize index = 1;
  while (index < args.count()) {
    let const start_argument = args[index].view();
    if (!start_argument.is_empty() && start_argument[0] == '-') {
      break;
    }

    roots.push(start_argument);
    index++;
  }

  for (; index < args.count(); index++) {
    let const predicate = args[index].view();
    if (predicate == "--help") {
      print_util_help(ec, args[0].view(), HELP_SYNOPSIS[0], HELP_DESCRIPTION,
                      FLAG_LIST);
      return 0;
    } else if (predicate == "-print") {
      /* The walk prints every matched entry already, so -print is the default
         and needs no action. */
    } else if (predicate == "-name") {
      if (index + 1 >= args.count())
        throw ErrorWithDetails{"find: -name expects a pattern",
                               "Pass a glob after `-name`, e.g. `-name '*.c'`"};
      name_patterns.push(args[index + 1].view());
      index++;
    } else if (predicate == "-type") {
      if (index + 1 >= args.count())
        throw ErrorWithDetails{"find: -type expects one of f, d, or l",
                               "Pass `f`, `d`, or `l` after `-type`"};
      let const type = args[index + 1].view();
      if (type.length != 1 ||
          (type[0] != 'f' && type[0] != 'd' && type[0] != 'l'))
      {
        throw ErrorWithDetails{"find: -type expects one of f, d, or l",
                               "Pass `f`, `d`, or `l` after `-type`"};
      }
      options.type_filter = type[0];
      index++;
    } else if (predicate == "-maxdepth") {
      options.max_depth = parse_depth_argument(args, index + 1, predicate,
                                               cxt.scratch_allocator());
      index++;
    } else if (predicate == "-mindepth") {
      options.min_depth = parse_depth_argument(args, index + 1, predicate,
                                               cxt.scratch_allocator());
      index++;
    } else {
      throw Error{
          "find: unknown predicate '" +
          String{cxt.scratch_allocator(), predicate}
          + "'"
      };
    }
  }

  if (!name_patterns.is_empty()) {
    usize longest_pattern_length = 0;
    for (let const &pattern : name_patterns) {
      if (pattern.length > longest_pattern_length)
        longest_pattern_length = pattern.length;
    }

    name_glob_active.reserve(longest_pattern_length);
    for (usize i = 0; i < longest_pattern_length; i++)
      name_glob_active.push(true);

    options.name_patterns = &name_patterns;
    options.glob_active = &name_glob_active;
  }

  if (roots.is_empty()) roots.push(StringView{"."});

  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (let const &root : roots) {
    let const root_path = Path{root};
    if (!root_path.exists()) {
      report_soft_koshkit_error(ec, cxt,
                                "find: '" +
                                    String{cxt.scratch_allocator(), root} +
                                    "': no such file or directory");
      status = 1;
      continue;
    }
    find_walk(ec, cxt, root_path, root, 0, options, output, status,
              cxt.scratch_allocator());
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
