/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the du utility. It recursively totals allocated disk
 * space without following symbolic links and formats byte or human-readable
 * totals.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../HashSet.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-sh] [path ...]");

HELP_DESCRIPTION_DECL("The du utility prints the disk usage of each path.");

FLAG(DU_SUMMARY, Bool, 's', "", "Print only the total for each path.");
FLAG(DU_HUMAN, Bool, 'h', "",
     "Print the size in a human-readable form such as 4.0K or 1.5M.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Du);

namespace koshka {

namespace koshkit {

struct du_output_row
{
  u64 size_bytes;
  String size;
  String path;
};

struct du_size_result
{
  u64 size_bytes;
  bool should_emit;
};

fn append_output_row(ArrayList<du_output_row> &rows, u64 size, StringView path,
                     usize &size_width, Allocator allocator) throws -> void
{
  let rendered_size = FLAG_DU_HUMAN.is_enabled()
                          ? format_human_size(size, allocator)
                          : String::from(size, allocator);
  if (rendered_size.length() > size_width) size_width = rendered_size.length();
  rows.push({
      size, steal(rendered_size), String{allocator, path}
  });
}

static fn total_size(const ExecContext &ec, EvalContext &cxt, const Path &path,
                     bool &has_failure, ArrayList<du_output_row> *output_rows,
                     usize &size_width, HashSet &seen_links,
                     Allocator allocator,
                     const os::file_status *known_status = nullptr) throws
    -> Maybe<du_size_result>
{
  os::file_status queried_status{};
  if (known_status == nullptr) {
    if (!os::stat_path(path.view(), queried_status)) {
      report_soft_koshkit_util_error(ec, cxt, "du",
                                     "cannot read '" + path.text() + "': " +
                                         os::last_system_error_message());
      has_failure = true;
      return None;
    }

    known_status = &queried_status;
  }

  let const type_letter = os::file_type_letter(known_status->mode);
  if (type_letter != 'd' && known_status->has_file_identity &&
      known_status->link_count > 1)
  {
    const u64 identity[] = {known_status->device_id, known_status->file_id};
    let const key =
        StringView{reinterpret_cast<const char *>(identity), sizeof(identity)};
    if (!seen_links.add(key)) return du_size_result{0, false};
  }

  if (known_status->blocks > UINT64_MAX / 512) {
    report_soft_koshkit_util_error(ec, cxt, "du",
                                   "cannot read '" + path.text() +
                                       "': the total size is too large");
    has_failure = true;
    return None;
  }

  let const allocated_size_bytes = known_status->blocks * 512;
  if (type_letter == 'd') {
    u64 total_bytes = allocated_size_bytes;
    let children = os::list_directory_status(path.view(), allocator);
    if (!children.has_value()) {
      report_soft_koshkit_util_error(ec, cxt, "du",
                                     "cannot read '" + path.text() + "': " +
                                         os::last_system_error_message());
      has_failure = true;
      return None;
    }

    children->sort([](const os::directory_status_entry &left,
                      const os::directory_status_entry &right) {
      return left.child.name.view() < right.child.name.view();
    });

    for (let const &child_entry : *children) {
      if (os::INTERRUPT_REQUESTED) return None;

      let child = Path{path.view(), allocator};
      child.append(child_entry.child.name.view());
      let const child_status =
          child_entry.has_status ? &child_entry.status : nullptr;
      let const child_size =
          total_size(ec, cxt, child, has_failure, output_rows, size_width,
                     seen_links, allocator, child_status);
      if (!child_size.has_value()) {
        if (os::INTERRUPT_REQUESTED) return None;
        continue;
      }
      if (child_size->size_bytes > UINT64_MAX - total_bytes) {
        report_soft_koshkit_util_error(ec, cxt, "du",
                                       "cannot read '" + child.text() +
                                           "': the total size is too large");
        has_failure = true;
        return None;
      }
      total_bytes += child_size->size_bytes;
    }

    if (output_rows != nullptr)
      append_output_row(*output_rows, total_bytes, path.view(),
                        size_width, allocator);

    return du_size_result{total_bytes, true};
  }

  if (output_rows != nullptr)
    append_output_row(*output_rows, allocated_size_bytes, path.view(),
                      size_width, allocator);

  return du_size_result{allocated_size_bytes, true};
}

fn append_size_line(String &output, const du_output_row &row, usize size_width,
                    bool should_color) throws -> void
{
  append_report_column(output, row.size.view(), size_width, true,
                       colors::ansi::BOLD_GREEN, should_color);
  output += "  ";
  append_report_text(output, row.path.view(), colors::ansi::BOLD_CYAN,
                     should_color);
  output += '\n';
}

Du::Du() = default;

pure fn Du::kind() const wontthrow -> Utility::Kind { return Kind::Du; }

fn Du::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();
  let targets = ArrayList<Path>{allocator};
  let target_statuses = ArrayList<os::file_status>{allocator};
  let is_target_status_known = ArrayList<bool>{allocator};
  if (operands.is_empty()) {
    targets.push(Path{"."});
  } else {
    targets.reserve(operands.count());
    for (let const &operand : operands)
      targets.push(Path{operand.view()});
  }

  target_statuses.reserve(targets.count());
  is_target_status_known.reserve(targets.count());
  let batch = os::Batch{allocator};
  batch.reserve(targets.count());
  for (usize index = 0; index < targets.count(); index++) {
    target_statuses.push({});
    batch.add(
        os::batch_operation::lstat(targets[index], target_statuses[index]));
  }
  let const target_results = batch.execute();
  for (let const &result : target_results)
    is_target_status_known.push(result.error_number == 0);

  let output_rows = ArrayList<du_output_row>{allocator};
  output_rows.reserve(targets.count());
  let seen_links = HashSet{allocator};
  usize size_width = 0;
  i32 status = 0;
  bool has_failure = false;
  bool was_interrupted = false;
  for (usize index = 0; index < targets.count(); index++) {
    let const &target = targets[index];
    if (!is_target_status_known[index]) {
      os::set_last_system_error(target_results[index].error_number);
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     "cannot access '" + target.text() + "': " +
                                         os::last_system_error_message());
      status = 1;
      continue;
    }

    let const total =
        total_size(ec, cxt, target, has_failure,
                   FLAG_DU_SUMMARY.is_enabled() ? nullptr : &output_rows,
                   size_width, seen_links, allocator, &target_statuses[index]);
    if (os::INTERRUPT_REQUESTED) {
      was_interrupted = true;
      break;
    }
    if (!total.has_value()) {
      status = 1;
      continue;
    }
    if (FLAG_DU_SUMMARY.is_enabled() && total->should_emit)
      append_output_row(output_rows, total->size_bytes, target.view(),
                        size_width, allocator);
  }

  output_rows.sort([](const du_output_row &left, const du_output_row &right) {
    if (left.size_bytes != right.size_bytes)
      return left.size_bytes > right.size_bytes;
    return left.path.view() < right.path.view();
  });

  let output = String{allocator};
  let const should_color = koshkit_should_color();
  for (let const &row : output_rows)
    append_size_line(output, row, size_width, should_color);

  ec.print_to_stdout(output);
  if (was_interrupted) return 130;
  if (has_failure) status = 1;
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
