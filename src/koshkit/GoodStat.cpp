/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the goodstat utility. It presents portable file
 * metadata as a labeled terminal report.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-L] file ...");

HELP_DESCRIPTION_DECL(
    "The goodstat utility presents file metadata as a readable report.");

FLAG(GOODSTAT_DEREFERENCE, Bool, 'L', "dereference", "Follow symbolic links.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(GoodStat);

namespace koshka::koshkit {

namespace {

fn id_name(u32 id, bool is_owner, Allocator allocator) throws -> String
{
  let const named =
      is_owner ? os::uid_to_username(id) : os::gid_to_groupname(id);
  if (!named.has_value()) return String::from(id, allocator);

  let result = String{allocator, named->view()};
  result += " (";
  result += String::from(id, allocator).view();
  result += ")";
  return result;
}

fn permission_text(u32 mode, Allocator allocator) throws -> String
{
  let result = os::format_mode_string(mode);
  result += " (";
  let const digits =
      String::from_in_base(mode & 07777u, false, int_base::octal, allocator);
  result.append_repeated('0', 4 - digits.length());
  result += digits.view();
  result += ")";
  return result;
}

fn size_text(u64 size, Allocator allocator) throws -> String
{
  let result = format_human_size(size, allocator);
  result += " (";
  result += String::from(size, allocator).view();
  result += size == 1 ? " byte)" : " bytes)";
  return result;
}

fn append_subject(String &output, StringView operand,
                  const os::file_status &status, bool should_color,
                  Allocator allocator) throws -> void
{
  append_report_text(output, operand, colors::ansi::BOLD_BLUE, should_color);
  output += "\n";
  let body = String{allocator};
  let const described_type = describe_file_type(operand, status, allocator);
  append_report_field(body, "Type",
                      described_type.has_value() ? described_type->view()
                                                 : file_type_name(status),
                      colors::ansi::BOLD_CYAN, should_color);

  if (os::file_type_letter(status.mode) == 'l') {
    let const target = os::read_symlink(operand, allocator);
    if (target.has_value()) {
      append_report_field(body, "Target", target->view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }

  append_report_field(body, "Size", size_text(status.size, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Permissions",
                      permission_text(status.mode, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Owner",
                      id_name(status.owner_id, true, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Group",
                      id_name(status.group_id, false, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Inode",
                      String::from(status.file_id, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Links",
                      String::from(status.link_count, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);

  let device = String::from(os::device_major(status.device_id), allocator);
  device += ",";
  device += String::from(os::device_minor(status.device_id), allocator).view();
  append_report_field(body, "Device", device.view(), colors::ansi::BOLD_CYAN,
                      should_color);

  let const type_letter = os::file_type_letter(status.mode);
  if (type_letter == 'b' || type_letter == 'c') {
    let special =
        String::from(os::device_major(status.special_device_id), allocator);
    special += ",";
    special +=
        String::from(os::device_minor(status.special_device_id), allocator)
            .view();
    append_report_field(body, "Device type", special.view(),
                        colors::ansi::BOLD_CYAN, should_color);
  }

  let blocks = String::from(status.blocks, allocator);
  blocks += " of 512 bytes";
  append_report_field(body, "Blocks", blocks.view(), colors::ansi::BOLD_CYAN,
                      should_color);
  append_report_field(body, "Accessed",
                      format_file_timestamp(status.access_time,
                                            status.access_nanoseconds,
                                            allocator)
                          .view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Modified",
                      format_file_timestamp(status.modification_time,
                                            status.modification_nanoseconds,
                                            allocator)
                          .view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(body, "Changed",
                      format_file_timestamp(status.change_time,
                                            status.change_nanoseconds,
                                            allocator)
                          .view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_body(output, body.view());
}

} // namespace

GoodStat::GoodStat() = default;

pure fn GoodStat::kind() const wontthrow -> Utility::Kind
{
  return Kind::GoodStat;
}

fn GoodStat::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  let const should_color = koshkit_should_color();

  let const allocator = cxt.scratch_allocator();
  let output = String{allocator};
  i32 status = 0;
  let operand_paths = ArrayList<Path>{allocator};
  let file_statuses = ArrayList<os::file_status>{allocator};
  let batch = os::Batch{allocator};
  operand_paths.reserve(operands.count());
  file_statuses.reserve(operands.count());
  batch.reserve(operands.count());
  for (let const &operand : operands) {
    operand_paths.push(Path{operand.view()});
    file_statuses.push({});
  }
  for (usize index = 0; index < operands.count(); index++) {
    if (FLAG_GOODSTAT_DEREFERENCE.is_enabled()) {
      batch.add(os::batch_operation::stat(operand_paths[index],
                                          file_statuses[index]));
    } else {
      batch.add(os::batch_operation::lstat(operand_paths[index],
                                           file_statuses[index]));
    }
  }
  let const results = batch.execute();

  for (usize index = 0; index < operands.count(); index++) {
    let const &operand = operands[index];
    if (results[index].error_number != 0) {
      os::set_last_system_error(results[index].error_number);
      KOSHKIT_REPORT_ERROR_AT(operand_locations[index],
                              "cannot stat '" + operand +
                                  "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    if (!output.is_empty()) output += "\n";
    append_subject(output, operand.view(), file_statuses[index], should_color,
                   allocator);
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
