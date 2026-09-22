/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements goodnode. It reports inode and filesystem metadata,
 * calculates a CRC32C checksum, and locates an inode beneath a bounded search
 * root.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-i inode] [-r root] [path ...]");

HELP_DESCRIPTION_DECL(
    "The goodnode utility reports inode metadata and a CRC32C checksum.");

FLAG(GOODNODE_INODE, String, 'i', "inode", "Locate this inode.");
FLAG(GOODNODE_ROOT, String, 'r', "root", "Search beneath this path.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(GoodNode);

namespace koshka::koshkit {

namespace {

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

  crc = ~crc;
  let digest = String::from_in_base(crc, false, int_base::hex, allocator);
  if (digest.length() < 8) {
    let padded = String{allocator};
    padded.append_repeated('0', 8 - digest.length());
    padded += digest.view();
    return padded;
  }

  return digest;
}

fn filesystem_features(StringView filesystem_type) throws -> Maybe<StringView>
{
  static constexpr static_string_entry<StringView> FEATURE_ENTRIES[] = {
      {SSK("NTFS"),           "transaction log, encryption, quotas, compression" },
      {SSK("apfs"),           "clones, snapshots, encryption, metadata checksums"},
      {SSK("btrfs"),
       "copy-on-write, snapshots, compression, data and metadata checksums"      },
      {SSK("ext2/ext3/ext4"), "journaling, journal checksums, file encryption"   },
      {SSK("ntfs"),           "transaction log, encryption, quotas, compression" },
  };
  static constexpr StaticStringMap FEATURES{FEATURE_ENTRIES};

  return FEATURES.find(filesystem_type);
}

fn append_node_report(String &output, const ExecContext &ec, StringView path,
                      const os::file_status &status, bool should_color,
                      Allocator allocator) throws -> void
{
  append_report_text(output, path, colors::ansi::BOLD_BLUE, should_color);
  output += '\n';
  let table = ReportTable{allocator};
  table.add_column("FIELD", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("VALUE");
  let const do_append_field = [&](StringView name, StringView value) throws {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({name, colors::ansi::BOLD_CYAN});
    cells.push({value, {}});
    table.add_row(cells);
  };
  do_append_field("Type", file_type_name(status));
  do_append_field("Inode", String::from(status.file_id, allocator));
  do_append_field("Device", String::from(status.device_id, allocator));
  do_append_field("Links", String::from(status.link_count, allocator));
  do_append_field("Logical size", format_human_size(status.size, allocator));
  do_append_field("Allocated", format_human_size(scaled_filesystem_blocks(
                                                     status.blocks, 512, 1),
                                                 allocator));

  os::filesystem_status filesystem{};
  if (os::stat_filesystem(path, filesystem)) {
    let const filesystem_type = StringView{filesystem.type_name};
    do_append_field("Filesystem", filesystem_type.is_empty()
                                      ? StringView{"unknown"}
                                      : filesystem_type);
    do_append_field("Filesystem ID",
                    String::from(filesystem.filesystem_id, allocator));
    do_append_field("Name limit", String::from(filesystem.name_max, allocator));
    let const features = filesystem_features(filesystem_type);
    if (features.has_value()) do_append_field("Features", *features);
  }

  if (let const evidence = os::read_filesystem_integrity_evidence(path);
      evidence.has_value())
  {
    switch (evidence->kind) {
    case os::filesystem_integrity_kind::BtrfsDeviceErrorCounters: {
      let const &errors = evidence->counters;
      let const has_errors =
          errors.read_count != 0 || errors.write_count != 0 ||
          errors.flush_count != 0 || errors.corruption_count != 0 ||
          errors.generation_count != 0;
      do_append_field("Integrity", has_errors
                                       ? StringView{"device errors recorded"}
                                       : StringView{"no device errors recorded"});
      if (!has_errors) break;

      let error_text = String{allocator, "read "};
      error_text += String::from(errors.read_count, allocator).view();
      error_text += ", write ";
      error_text += String::from(errors.write_count, allocator).view();
      error_text += ", flush ";
      error_text += String::from(errors.flush_count, allocator).view();
      error_text += ", corruption ";
      error_text += String::from(errors.corruption_count, allocator).view();
      error_text += ", generation ";
      error_text += String::from(errors.generation_count, allocator).view();
      do_append_field("Errors", error_text.view());
      break;
    }
    case os::filesystem_integrity_kind::Ext4RecordedErrors:
      do_append_field(
          "Integrity",
          String::from(evidence->recorded_error_count, allocator).view() +
              (evidence->recorded_error_count == 1 ? " recorded error"
                                                   : " recorded errors"));
      break;
    case os::filesystem_integrity_kind::NtfsDirtyFlag:
      do_append_field("Integrity", evidence->is_dirty
                                       ? StringView{"dirty bit set"}
                                       : StringView{"dirty bit clear"});
      break;
    }
  } else {
    do_append_field("Integrity", "not verified");
  }
  if (status.link_count == 0) do_append_field("Link state", "unlinked");

  if (os::file_type_letter(status.mode) == '-') {
    let const digest = file_crc32c(ec, path, allocator);
    if (digest.has_value()) do_append_field("CRC32C", digest->view());
  }

  output += table.to_string(should_color).view();
}

fn find_inode(const Path &path, const os::file_status &status, u64 inode,
              String &found_path, Allocator allocator) throws -> bool
{
  if (status.has_file_identity && status.file_id == inode) {
    found_path = path.text().clone();
    return true;
  }
  if (os::file_type_letter(status.mode) != 'd') return false;

  let children = os::list_directory_status(path.view(), allocator);
  if (!children.has_value()) return false;

  for (let const &child : *children) {
    if (os::INTERRUPT_REQUESTED) return false;
    if (!child.has_status) continue;

    let child_path = path.clone();
    child_path.append(child.child.name.view());

    if (find_inode(child_path, child.status, inode, found_path, allocator))
      return true;
  }

  return false;
}

pure fn path_is_beneath(const Path &root, const Path &candidate) wontthrow
    -> bool
{
  let const root_text = root.view();
  let const candidate_text = candidate.view();
  if (candidate_text == root_text) return true;
  if (root_text.is_empty() || !candidate_text.starts_with(root_text))
    return false;

  return os::is_directory_separator(root_text[root_text.length - 1]) ||
         (candidate_text.length > root_text.length &&
          os::is_directory_separator(candidate_text[root_text.length]));
}

} // namespace

GoodNode::GoodNode() = default;

pure fn GoodNode::kind() const wontthrow -> Utility::Kind
{
  return Kind::GoodNode;
}

fn GoodNode::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();
  let paths = ArrayList<String>{allocator};
  for (let const &operand : operands)
    paths.push(String{allocator, operand.view()});

  if (FLAG_GOODNODE_ROOT.is_set() && !FLAG_GOODNODE_INODE.is_set()) {
    KOSHKIT_REPORT_ERROR_AT(FLAG_GOODNODE_ROOT.value_location(),
                            "root requires an inode",
                            "pass --inode with --root");
    return 1;
  }

  if (FLAG_GOODNODE_INODE.is_set()) {
    let const parsed = utils::parse_integer_in_base(FLAG_GOODNODE_INODE.value(),
                                                    int_base::decimal);
    if (parsed.is_error() || parsed.value() < 0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_GOODNODE_INODE.value_location(),
                              "invalid inode",
                              "the inode must be a non-negative integer");
      return 1;
    }

    let found = String{allocator};
    let const root = FLAG_GOODNODE_ROOT.is_set() ? FLAG_GOODNODE_ROOT.value()
                                                 : StringView{"."};
    let const root_path = Path{root};
    let const inode = static_cast<u64>(parsed.value());
    if (let canonical_root = os::canonical_path(root_path);
        canonical_root.has_value() &&
        canonical_root->view() == root_path.view())
    {
      let const direct_path =
          os::path_from_file_id(canonical_root->text().view(), inode);
      if (direct_path.has_value() &&
          path_is_beneath(*canonical_root, *direct_path))
      {
        os::file_status direct_status{};
        if (os::stat_path(direct_path->text().view(), direct_status) &&
            direct_status.has_file_identity && direct_status.file_id == inode)
        {
          found = direct_path->text().clone();
        }
      }
    }

    if (found.is_empty()) {
      os::file_status root_status{};
      if (os::stat_path(root_path.view(), root_status))
        unused(find_inode(root_path, root_status, inode, found, allocator));
    }

    if (found.is_empty()) {
      if (os::INTERRUPT_REQUESTED) return 130;
      KOSHKIT_REPORT_ERROR_AT(FLAG_GOODNODE_INODE.value_location(),
                              "inode was not found",
                              "choose a narrower or different search root");
      return 1;
    }
    paths.push(steal(found));
  }

  if (paths.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  let const should_color = koshkit_should_color();

  let output = String{allocator};
  i32 exit_status = 0;
  let report_paths = ArrayList<Path>{allocator};
  let report_statuses = ArrayList<os::file_status>{allocator};
  let report_batch = os::Batch{allocator};
  report_paths.reserve(paths.count());
  report_statuses.reserve(paths.count());
  report_batch.reserve(paths.count());
  for (let const &path : paths) {
    report_paths.push(Path{path.view()});
    report_statuses.push({});
  }
  for (usize index = 0; index < paths.count(); index++)
    report_batch.add(os::batch_operation::lstat(report_paths[index],
                                                report_statuses[index]));
  let const report_results = report_batch.execute();

  for (usize index = 0; index < paths.count(); index++) {
    let const &path = paths[index];
    if (report_results[index].error_number != 0) {
      os::set_last_system_error(report_results[index].error_number);
      report_soft_koshkit_error(ec, cxt,
                                "cannot inspect '" + path +
                                    "': " + os::last_system_error_message());
      exit_status = 1;
      continue;
    }

    if (!output.is_empty()) output += '\n';
    append_node_report(output, ec, path.view(), report_statuses[index],
                       should_color, allocator);
  }

  ec.print_to_stdout(output);
  return exit_status;
}

} // namespace koshka::koshkit
