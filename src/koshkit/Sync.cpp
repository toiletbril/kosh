/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the sync utility. It flushes every mounted filesystem
 * when it receives no operand, and it flushes the data or the data and the
 * metadata of each named file when it receives one.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-df] [file ...]");

HELP_DESCRIPTION_DECL(
    "The sync utility flushes pending writes to permanent storage.");

FLAG(SYNC_DATA, Bool, 'd', "data", "Flush the data of each operand only.");
FLAG(SYNC_FILESYSTEM, Bool, 'f', "file-system",
     "Flush the filesystem that holds each operand.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Sync);

namespace koshka::koshkit {

Sync::Sync() = default;

pure fn Sync::kind() const wontthrow -> Utility::Kind { return Kind::Sync; }

fn Sync::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_SYNC_DATA.is_enabled() && FLAG_SYNC_FILESYSTEM.is_enabled()) {
    let conflict_location = FLAG_SYNC_DATA.value_location();
    if (FLAG_SYNC_FILESYSTEM.position() > FLAG_SYNC_DATA.position())
      conflict_location = FLAG_SYNC_FILESYSTEM.value_location();
    KOSHKIT_REPORT_ERROR_AT(conflict_location, "cannot combine -d with -f",
                            "select one of the two modes");
    return 1;
  }

  if (operands.is_empty()) {
    if (FLAG_SYNC_DATA.is_enabled() || FLAG_SYNC_FILESYSTEM.is_enabled()) {
      return report_usage_error(ec, cxt, args[0].view());
    }

    if (!os::sync_filesystems()) {
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     "cannot flush the mounted filesystems: " +
                                         os::last_system_error_message());
      return 1;
    }

    return 0;
  }

  i32 status = 0;
  let const mode = FLAG_SYNC_DATA.is_enabled() ? os::sync_mode::DataOnly
                                               : os::sync_mode::All;
  for (usize operand_index = 0; operand_index < operands.count();
       operand_index++)
  {
    let const &operand = operands[operand_index];
    if (!os::sync_path(operand.view(), mode)) {
      KOSHKIT_REPORT_ERROR_AT(operand_locations[operand_index],
                              "cannot flush '" + operand +
                                  "': " + os::last_system_error_message());
      status = 1;
    }
  }

  return status;
}

} // namespace koshka::koshkit
