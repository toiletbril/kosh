#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-sf] target ... link");

HELP_DESCRIPTION_DECL("The ln utility creates a symbolic link to the target.");

FLAG(LN_SYMBOLIC, Bool, 's', "", "Create a symbolic link.");
FLAG(LN_FORCE, Bool, 'f', "", "Remove an existing destination first.");
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

  if (!FLAG_LN_SYMBOLIC.is_enabled())
    throw ErrorWithDetails{"ln supports only symbolic links",
                           "Re-run with `-s` to make a symlink"};

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

    if (!os::create_symlink(target, link.view())) {
      report_soft_koshkit_error(ec, cxt,
                                "ln: cannot create symbolic link '" + link +
                                    "': " + os::last_system_error_message());
      status = 1;
    }
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
