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
#include "../base/Path.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-L] [-c] [-f] file ...");

HELP_DESCRIPTION_DECL(
    "The goodstat utility presents file metadata as a readable report.");

FLAG(GOODSTAT_DEREFERENCE, Bool, 'L', "dereference", "Follow symbolic links.");
FLAG(GOODSTAT_CHECKSUM, Bool, 'c', "checksum", "Calculate a CRC32C checksum.");
FLAG(GOODSTAT_FILESYSTEM, Bool, 'f', "filesystem",
     "Report filesystem capacity and identity.");
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

fn file_crc32c(const ExecContext &ec, StringView path,
               Allocator allocator) throws -> Maybe<String>
{
  let const input = open_named_or_stdin(ec, path);
  if (!input.has_value()) return None;
  defer
  {
    if (input->should_close) unused(os::close_fd(input->descriptor));
  };

  u32 crc = 0xffffffffu;
  char buffer[65536];
  loop
  {
    let const read_count =
        os::read_fd(input->descriptor, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) break;
    crc = os::crc32c_update(crc, buffer, *read_count);
    if (os::INTERRUPT_REQUESTED) return None;
  }

  let digest = String::from_in_base(~crc, false, int_base::hex, allocator);
  if (digest.length() < 8) {
    let padded = String{allocator};
    padded.append_repeated('0', 8 - digest.length());
    padded += digest.view();
    return padded;
  }
  return digest;
}

fn percent_used(const os::filesystem_status &filesystem) wontthrow -> u64
{
  if (filesystem.total_blocks == 0) return 0;
  let const used = filesystem.total_blocks -
                   (filesystem.free_blocks > filesystem.total_blocks
                        ? filesystem.total_blocks
                        : filesystem.free_blocks);
  let const whole_percent = used / filesystem.total_blocks;
  let const remainder = used % filesystem.total_blocks;
  return whole_percent * 100 +
         static_cast<u64>((static_cast<u128>(remainder) * 100 +
                           filesystem.total_blocks / 2) /
                          filesystem.total_blocks);
}

fn append_subject(String &output, StringView operand,
                  const os::file_status &status, bool should_color,
                  bool should_report_filesystem, bool should_report_checksum,
                  const ExecContext &ec, Allocator allocator) throws -> void
{
  append_report_text(output, operand, colors::ansi::BOLD_BLUE, should_color);
  output += "\n";
  let table = ReportTable{allocator};
  table.add_column("FIELD", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("VALUE");
  let const do_append_field = [&](StringView name, StringView value,
                                  StringView style, bool unused_color) throws {
    unused(unused_color);
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({name, style});
    cells.push({value, {}});
    table.add_row(cells);
  };
  let const described_type = describe_file_type(operand, status, allocator);
  do_append_field("Type",
                  described_type.has_value() ? described_type->view()
                                             : file_type_name(status),
                  colors::ansi::BOLD_CYAN, should_color);

  if (os::file_type_letter(status.mode) == 'l') {
    let const target = os::read_symlink(operand, allocator);
    if (target.has_value()) {
      do_append_field("Target", target->view(), colors::ansi::BOLD_CYAN,
                      should_color);
    }
  }

  do_append_field("Size", size_text(status.size, allocator).view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Permissions", permission_text(status.mode, allocator).view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Owner", id_name(status.owner_id, true, allocator).view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Group", id_name(status.group_id, false, allocator).view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Inode", String::from(status.file_id, allocator).view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Links", String::from(status.link_count, allocator).view(),
                  colors::ansi::BOLD_CYAN, should_color);

  let device = String::from(os::device_major(status.device_id), allocator);
  device += ",";
  device += String::from(os::device_minor(status.device_id), allocator).view();
  do_append_field("Device", device.view(), colors::ansi::BOLD_CYAN,
                  should_color);

  let const type_letter = os::file_type_letter(status.mode);
  if (type_letter == 'b' || type_letter == 'c') {
    let special =
        String::from(os::device_major(status.special_device_id), allocator);
    special += ",";
    special +=
        String::from(os::device_minor(status.special_device_id), allocator)
            .view();
    do_append_field("Device type", special.view(), colors::ansi::BOLD_CYAN,
                    should_color);
  }

  let blocks = String::from(status.blocks, allocator);
  blocks += " of 512 bytes";
  do_append_field("Blocks", blocks.view(), colors::ansi::BOLD_CYAN,
                  should_color);
  do_append_field("Accessed",
                  format_file_timestamp(status.access_time,
                                        status.access_nanoseconds, allocator)
                      .view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Modified",
                  format_file_timestamp(status.modification_time,
                                        status.modification_nanoseconds,
                                        allocator)
                      .view(),
                  colors::ansi::BOLD_CYAN, should_color);
  do_append_field("Changed",
                  format_file_timestamp(status.change_time,
                                        status.change_nanoseconds, allocator)
                      .view(),
                  colors::ansi::BOLD_CYAN, should_color);

  if (should_report_filesystem) {
    let filesystem = os::filesystem_status{};
    if (os::stat_filesystem(operand, filesystem)) {
      do_append_field("Filesystem", StringView{filesystem.type_name},
                      colors::ansi::BOLD_CYAN, should_color);
      do_append_field("Filesystem block size",
                      String::from(filesystem.block_size, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
      do_append_field("Filesystem capacity",
                      String::from(percent_used(filesystem), allocator).view() +
                          "%",
                      colors::ansi::BOLD_CYAN, should_color);
      do_append_field("Filesystem blocks",
                      String::from(filesystem.total_blocks, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
      do_append_field("Filesystem free blocks",
                      String::from(filesystem.free_blocks, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
      do_append_field(
          "Filesystem available blocks",
          String::from(filesystem.available_blocks, allocator).view(),
          colors::ansi::BOLD_CYAN, should_color);
      do_append_field("Filesystem id",
                      String::from(filesystem.filesystem_id, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
    }
  }

  if (should_report_checksum && os::file_type_letter(status.mode) == '-') {
    if (let const checksum = file_crc32c(ec, operand, allocator))
      do_append_field("CRC32C", checksum->view(), colors::ansi::BOLD_CYAN,
                      should_color);
  }

  output += table.to_string(should_color).view();
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
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  let const should_color = koshkit_should_color();
  let const should_report_filesystem = FLAG_GOODSTAT_FILESYSTEM.is_enabled();
  let const should_report_checksum = FLAG_GOODSTAT_CHECKSUM.is_enabled();

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
    operand_paths.push(Path{operand.view(), allocator});
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
                   should_report_filesystem, should_report_checksum, ec,
                   allocator);
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
