/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the chmod utility. It applies octal or symbolic modes to
 * files and recursively traverses directories when requested.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "Mode.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-R] mode file ...");

HELP_DESCRIPTION_DECL("The chmod utility changes file permission modes.");

FLAG(CHMOD_RECURSIVE, Bool, 'R', "recursive",
     "Change directories and their contents recursively.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Chmod);

namespace koshka::koshkit {

static fn change_mode(const ExecContext &ec, EvalContext &cxt, const Path &path,
                      StringView expression, bool should_recurse) throws -> bool
{
  os::file_status status{};
  if (!os::stat_path_following(path.text().view(), status)) {
    report_soft_koshkit_error(ec, cxt,
                              "chmod: cannot access '" + path.text() +
                                  "': " + os::last_system_error_message());
    return false;
  }

  let const parsed =
      parse_file_mode(expression, status.mode, os::get_file_creation_mask(),
                      os::file_type_letter(status.mode) == 'd');
  if (!parsed.has_value())
    throw Error{
        "chmod: invalid mode '" + String{cxt.scratch_allocator(), expression}
          +
        "'"
    };

  bool did_succeed = true;
  if (!os::set_file_mode(path.text().view(), *parsed)) {
    report_soft_koshkit_error(ec, cxt,
                              "chmod: cannot change mode of '" + path.text() +
                                  "': " + os::last_system_error_message());
    did_succeed = false;
  }

  if (!should_recurse || os::file_type_letter(status.mode) != 'd')
    return did_succeed;

  let children = Path::read_directory(path);
  if (!children.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "chmod: cannot read directory '" + path.text() +
                                  "': " + os::last_system_error_message());
    return false;
  }

  for (const String &name : *children) {
    let child = PathBuilder{path.text().view()}.append(name.view()).build();
    if (child.is_symbolic_link()) continue;
    if (!change_mode(ec, cxt, child, expression, true)) did_succeed = false;
  }

  return did_succeed;
}

Chmod::Chmod() = default;

pure fn Chmod::kind() const wontthrow -> Utility::Kind { return Kind::Chmod; }

fn Chmod::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());
  let const expression = operands[0].view();
  i32 status = 0;

  for (usize index = 1; index < operands.count(); index++)
    if (!change_mode(ec, cxt, Path{operands[index].view()}, expression,
                     FLAG_CHMOD_RECURSIVE.is_enabled()))
      status = 1;

  return status;
}

} // namespace koshka::koshkit
