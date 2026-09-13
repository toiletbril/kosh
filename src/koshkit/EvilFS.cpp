/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evilfs utility. It reports mounted filesystem
 * sources, targets, types, and options in content-derived columns.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a]");

HELP_DESCRIPTION_DECL(
    "The evilfs utility reports the filesystems mounted on the host.");

FLAG(EVILFS_ALL, Bool, 'a', "all",
     "Show volume identity and operating system metadata.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilFS);

namespace koshka::koshkit {

EvilFS::EvilFS() = default;

pure fn EvilFS::kind() const wontthrow -> Utility::Kind { return Kind::EvilFS; }

static fn append_filesystem_id(String &output, StringView name, u64 value,
                               Allocator allocator, bool should_color) throws
    -> void
{
  let text = String{allocator, "0x"};
  text += String::from_in_base(value, false, int_base::hex, allocator).view();
  append_report_field(output, name, text.view(), colors::ansi::BOLD_CYAN,
                      should_color);
}

static fn append_detailed_filesystem(String &output,
                                     const os::mounted_filesystem &mount,
                                     Allocator allocator,
                                     bool should_color) throws -> void
{
  output += "  ";
  append_report_text(output, mount.target.view(), colors::ansi::BOLD_BLUE,
                     should_color);
  output += "\n";
  let identity = String{allocator};
  append_report_field(identity, "Source", mount.source.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(identity, "Volume",
                      mount.volume_name.is_empty() ? StringView{"-"}
                                                   : mount.volume_name.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(identity, "UUID",
                      mount.volume_uuid.is_empty() ? StringView{"-"}
                                                   : mount.volume_uuid.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(identity, "Type", mount.type.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(identity, "Options", mount.options.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_body(output, identity.view(), "    ");

  os::filesystem_status status{};
  if (!os::stat_filesystem(mount.target.view(), status)) return;

  output += "    ";
  append_report_text(output, "OS METADATA", colors::ansi::BOLD_MAGENTA,
                     should_color);
  output += "\n";
  let metadata = String{allocator};
  append_filesystem_id(metadata, "Filesystem ID", status.filesystem_id,
                       allocator, should_color);
  append_filesystem_id(metadata, "Type ID", status.type_id, allocator,
                       should_color);
  append_report_field(metadata, "Block size",
                      format_human_size(status.block_size, allocator),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(
      metadata, "Fundamental block size",
      format_human_size(status.fundamental_block_size, allocator),
      colors::ansi::BOLD_CYAN, should_color);
  let blocks = String::from(status.total_blocks, allocator);
  blocks += " total, ";
  blocks += String::from(status.free_blocks, allocator).view();
  blocks += " free, ";
  blocks += String::from(status.available_blocks, allocator).view();
  blocks += " available";
  append_report_field(metadata, "Blocks", blocks.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  let files = String::from(status.total_files, allocator);
  files += " total, ";
  files += String::from(status.free_files, allocator).view();
  files += " free";
  append_report_field(metadata, "Files", files.view(), colors::ansi::BOLD_CYAN,
                      should_color);
  append_report_field(metadata, "Name limit",
                      String::from(status.name_max, allocator),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_body(output, metadata.view(), "      ");
}

fn EvilFS::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0], "unexpected operand",
                            "this utility accepts no operands");
    return 1;
  }

  let mounts = os::mounted_filesystems();
  mounts.sort([](const os::mounted_filesystem &left,
                 const os::mounted_filesystem &right) {
    if (left.target.view() != right.target.view()) {
      return left.target.view() < right.target.view();
    }

    return left.source.view() < right.source.view();
  });

  usize source_width = 6;
  usize target_width = 6;
  usize type_width = 4;
  for (let const &mount : mounts) {
    if (mount.source.length() > source_width)
      source_width = mount.source.length();
    if (mount.target.length() > target_width)
      target_width = mount.target.length();
    if (mount.type.length() > type_width) type_width = mount.type.length();
  }

  let output = String{cxt.scratch_allocator()};
  let const should_color = koshkit_should_color();
  if (FLAG_EVILFS_ALL.is_enabled()) {
    append_report_text(output, "FILESYSTEMS", colors::ansi::BOLD_BLUE,
                       should_color);
    output += "\n";
    for (let const &mount : mounts) {
      if (!output.is_empty()) output += "\n";
      append_detailed_filesystem(output, mount, cxt.scratch_allocator(),
                                 should_color);
    }
    ec.print_to_stdout(output);
    return mounts.is_empty() ? 1 : 0;
  }

  append_report_column(output, "SOURCE", source_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "TARGET", target_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "TYPE", type_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_text(output, "OPTIONS", colors::ansi::BOLD_CYAN, should_color);
  output += "\n";

  for (let const &mount : mounts) {
    append_report_column(output, mount.source.view(), source_width, false,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, mount.target.view(), target_width, false,
                         colors::ansi::BOLD_GREEN, should_color);
    output += "  ";
    append_report_column(output, mount.type.view(), type_width, false,
                         colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_text(output, mount.options.view(), colors::ansi::DIM,
                       should_color);
    output += "\n";
  }

  ec.print_to_stdout(output);
  return mounts.is_empty() ? 1 : 0;
}

} /* namespace koshka::koshkit */
