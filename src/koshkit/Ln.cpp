/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the ln utility in koshkit. The
 * ln utility creates links to files.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-fLPs] target ... link");

HELP_DESCRIPTION_DECL("The ln utility creates links to files.");

FLAG(LN_SYMBOLIC, Bool, 's', "", "Create a symbolic link.");
FLAG(LN_FORCE, Bool, 'f', "", "Remove an existing destination first.");
FLAG(LN_LOGICAL, Bool, 'L', "", "Follow symbolic link sources.");
FLAG(LN_PHYSICAL, Bool, 'P', "", "Link symbolic link sources themselves.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Ln);

namespace koshka {

namespace koshkit {

Ln::Ln() = default;

pure fn Ln::kind() const wontthrow -> Utility::Kind { return Kind::Ln; }

fn Ln::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  let const destination = operands[operands.count() - 1].view();
  let const is_destination_directory = Path{destination}.is_directory();

  if (operands.count() > 2 && !is_destination_directory) {
    throw Error{
        "ln: the destination '" + String{cxt.scratch_allocator(), destination}
          +
        "' is not a directory, so it cannot hold several links"
    };
  }

  i32 status = 0;
  for (usize i = 0; i + 1 < operands.count(); i++) {
    let const target = operands[i].view();
    let link = String{cxt.scratch_allocator(), destination};
    if (is_destination_directory)
      link = PathBuilder{destination}
                 .append(Path{target}.filename())
                 .build()
                 .text();

    if (FLAG_LN_FORCE.is_enabled()) os::remove_file(link.view());

    let link_target = target;
    Maybe<Path> resolved_target;
    if (!FLAG_LN_SYMBOLIC.is_enabled() &&
        FLAG_LN_LOGICAL.position() > FLAG_LN_PHYSICAL.position())
    {
      resolved_target = os::canonical_path(Path{target});
      if (resolved_target.has_value())
        link_target = resolved_target->text().view();
    }
    let const did_create = FLAG_LN_SYMBOLIC.is_enabled()
                               ? os::create_symlink(link_target, link.view())
                               : os::create_hard_link(link_target, link.view());
    if (!did_create) {
      let const reason =
          os::last_system_error_is_missing_file()
              ? String{cxt.scratch_allocator(), "No such file or directory"}
              : os::last_system_error_message();
      report_soft_koshkit_error(
          ec, cxt, "ln: cannot create link '" + link + "': " + reason);
      status = 1;
    }
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
