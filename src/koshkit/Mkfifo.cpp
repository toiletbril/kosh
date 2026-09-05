/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the mkfifo utility. It parses the requested creation
 * mode, applies the file creation mask, and creates each named FIFO through the
 * platform interface.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "Mode.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-m mode] file ...");

HELP_DESCRIPTION_DECL("The mkfifo utility creates FIFO special files.");

FLAG(MKFIFO_MODE, String, 'm', "mode", "Set the FIFO permission mode.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Mkfifo);

namespace koshka::koshkit {

Mkfifo::Mkfifo() = default;

pure fn Mkfifo::kind() const wontthrow -> Utility::Kind { return Kind::Mkfifo; }

fn Mkfifo::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  u32 mode = 0666;
  if (FLAG_MKFIFO_MODE.is_set()) {
    let const parsed =
        parse_file_mode(FLAG_MKFIFO_MODE.value(), mode, 0, false);
    if (!parsed.has_value())
      throw Error{
          "mkfifo: invalid mode '" +
          String{cxt.scratch_allocator(), FLAG_MKFIFO_MODE.value()}
          + "'"
      };
    mode = *parsed;
  }

  i32 status = 0;
  for (const String &operand : operands) {
    if (!os::make_fifo(operand.view(), mode)) {
      report_soft_koshkit_error(ec, cxt,
                                "mkfifo: cannot create '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }
    if (FLAG_MKFIFO_MODE.is_set() && !os::set_file_mode(operand.view(), mode)) {
      report_soft_koshkit_error(ec, cxt,
                                "mkfifo: cannot set mode of '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
    }
  }

  return status;
}

} // namespace koshka::koshkit
