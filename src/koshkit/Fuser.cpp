/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the fuser utility in koshkit.
 * The fuser utility lists the process IDs that use each file.
 */

#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-cfu] file ...");

HELP_DESCRIPTION_DECL(
    "The fuser utility lists the process IDs that use each file.");

FLAG(FUSER_FILESYSTEM, Bool, 'c', "", "Match every file on the filesystem.");
FLAG(FUSER_FILE, Bool, 'f', "", "Match only the named file.");
FLAG(FUSER_USER, Bool, 'u', "", "Print each process owner's user name.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Fuser);

namespace koshka {

namespace koshkit {

Fuser::Fuser() = default;

pure fn Fuser::kind() const wontthrow -> Utility::Kind { return Kind::Fuser; }

static pure fn has_use(u8 mask, os::process_file_use use) wontthrow -> bool
{
  return (mask & static_cast<u8>(use)) != 0;
}

static fn append_use_letters(String &output, u8 mask) throws -> void
{
  if (has_use(mask, os::process_file_use::Root)) output.push('r');
  if (has_use(mask, os::process_file_use::Cwd)) output.push('c');
  if (has_use(mask, os::process_file_use::Executable)) output.push('e');
  if (has_use(mask, os::process_file_use::Mapped)) output.push('m');
  if (has_use(mask, os::process_file_use::File)) output.push('f');
}

fn Fuser::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_FUSER_FILESYSTEM.is_enabled() && FLAG_FUSER_FILE.is_enabled()) {
    let const conflict_location =
        FLAG_FUSER_FILESYSTEM.position() > FLAG_FUSER_FILE.position()
            ? FLAG_FUSER_FILESYSTEM.value_location()
            : FLAG_FUSER_FILE.value_location();
    report_soft_koshkit_util_error(ec, cxt, conflict_location, args[0].view(),
                                   "-c and -f cannot be used together");
    return 2;
  }

  let queries = ArrayList<os::process_file_query>{cxt.scratch_allocator()};
  i32 status = 0;
  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    os::file_status file_status{};
    if (!os::stat_path_following(operands[operand_position].view(),
                                 file_status))
    {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "'" + operands[operand_position] +
              "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    if (!os::process_file_query_is_supported(
            file_status, FLAG_FUSER_FILESYSTEM.is_enabled()))
    {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "filesystem and directory queries are unsupported on Windows");
      status = 1;
      continue;
    }

    let const is_block_device = os::file_type_letter(file_status.mode) == 'b';
    let const should_match_device =
        FLAG_FUSER_FILESYSTEM.is_enabled() ||
        (is_block_device && !FLAG_FUSER_FILE.is_enabled());
    queries.push(os::process_file_query{
        operands[operand_position].view(),
        should_match_device && is_block_device ? file_status.special_device_id
                                               : file_status.device_id,
        file_status.file_id, static_cast<u32>(operand_position),
        should_match_device});
  }

  let users = ArrayList<os::process_file_user>{cxt.scratch_allocator()};
  let failed_query_position = Maybe<u32>{};
  if (!queries.is_empty())
    failed_query_position =
        os::scan_process_file_users(queries, users, cxt.scratch_allocator());
  if (failed_query_position.has_value()) {
    let const operand_position = static_cast<usize>(*failed_query_position);
    report_soft_koshkit_util_error(ec, cxt, operand_locations[operand_position],
                                   args[0].view(),
                                   "unable to inspect running processes: " +
                                       os::last_system_error_message());
    return 1;
  }
  if (users.is_empty() && status == 0) status = 1;
  users.sort([](const os::process_file_user &left,
                const os::process_file_user &right) {
    if (left.query_position != right.query_position)
      return left.query_position < right.query_position;
    return left.pid < right.pid;
  });

  let standard_output = String{cxt.scratch_allocator()};
  let standard_error = String{cxt.scratch_allocator()};
  let combined_output = String{cxt.scratch_allocator()};
  let const output_descriptor = ec.out_fd.value_or(KOSH_STDOUT);
  let const error_descriptor = ec.err_fd.value_or(KOSH_STDERR);
  let const is_combined =
      os::descriptors_refer_to_same_file(output_descriptor, error_descriptor);
  usize user_position = 0;
  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    if (user_position >= users.count() ||
        users[user_position].query_position != operand_position)
    {
      continue;
    }
    let &metadata_output = is_combined ? combined_output : standard_error;
    metadata_output += operands[operand_position].view();
    metadata_output.push(':');

    while (user_position < users.count() &&
           users[user_position].query_position == operand_position)
    {
      let const &user = users[user_position];
      let pid = String::from(user.pid, cxt.scratch_allocator());
      if (is_combined) {
        metadata_output += pid.view();
      } else {
        standard_output += pid.view();
        standard_output.push(' ');
      }
      append_use_letters(metadata_output, user.use_mask);
      if (FLAG_FUSER_USER.is_enabled()) {
        metadata_output.push('(');
        if (let const owner = os::process_owner_name(user.pid, user.owner_id,
                                                     cxt.scratch_allocator()))
          metadata_output += owner->view();
        else
          metadata_output +=
              String::from(user.owner_id, cxt.scratch_allocator());
        metadata_output.push(')');
      }
      user_position++;
    }
    metadata_output.push('\n');
    if (!is_combined) standard_output.push('\n');
  }

  if (is_combined)
    ec.print_to_stdout(combined_output.view());
  else {
    ec.print_to_stdout(standard_output.view());
    ec.print_to_stderr(standard_error.view());
  }
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
