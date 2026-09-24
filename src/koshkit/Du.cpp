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
#include "../Koshkit.hpp"
#include "../Utils.hpp"
#include "../base/Arena.hpp"
#include "../base/HashSet.hpp"
#include "../base/Path.hpp"

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
  String path;
};

struct du_size_result
{
  u64 size_bytes;
  bool should_emit;
};

struct du_directory_frame
{
  Path path;
  usize parent_index{SIZE_MAX};
  u64 total_bytes{0};
  usize pending_stat_count{0};
  usize pending_directory_count{0};
  bool is_enumerated{false};
  bool has_failure{false};
  bool is_complete{false};
};

struct du_stat_work
{
  Path path;
  usize parent_index{SIZE_MAX};
  os::file_status status{};
};

fn append_output_row(ArrayList<du_output_row> &rows, u64 size, StringView path,
                     Allocator allocator) throws -> void
{
  rows.push({
      size, String{allocator, path}
  });
}

static fn total_size(const ExecContext &ec, EvalContext &cxt, const Path &path,
                     bool &has_failure, ArrayList<du_output_row> *output_rows,
                     HashSet &seen_links, Allocator allocator,
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
  if (type_letter != 'd') {
    if (output_rows != nullptr)
      append_output_row(*output_rows, allocated_size_bytes, path.view(),
                        allocator);

    return du_size_result{allocated_size_bytes, true};
  }

  let wave_arena = BumpArena{};
  let const wave_allocator = bump_allocator(wave_arena);
  let list_arena = BumpArena{};
  let const list_allocator = bump_allocator(list_arena);
  let frames = ArrayList<du_directory_frame>{allocator};
  let directory_queue = ArrayList<usize>{allocator};
  let stat_work = ArrayList<du_stat_work>{allocator};
  let stat_batch = os::Batch{allocator};
  let batch_results = ArrayList<os::batch_result>{allocator};
  frames.reserve(32);
  directory_queue.reserve(32);
  stat_work.reserve(512);
  stat_batch.reserve(512);
  batch_results.reserve(512);
  frames.push(du_directory_frame{
      Path{path.view(), allocator},
      SIZE_MAX, allocated_size_bytes, 0, 0, false,
      false, false
  });
  directory_queue.push(0);
  bool is_root_complete = false;
  du_size_result root_result{0, false};

  let const do_try_complete = [&](usize frame_index) throws -> void {
    while (frame_index != SIZE_MAX) {
      let &frame = frames[frame_index];
      if (frame.is_complete || !frame.is_enumerated ||
          frame.pending_stat_count != 0 || frame.pending_directory_count != 0)
        return;

      frame.is_complete = true;
      if (frame.has_failure) {
        has_failure = true;
        if (frame.parent_index != SIZE_MAX)
          frames[frame.parent_index].has_failure = true;
      } else {
        if (output_rows != nullptr)
          append_output_row(*output_rows, frame.total_bytes, frame.path.view(),
                            allocator);
        if (frame.parent_index == SIZE_MAX) {
          is_root_complete = true;
          root_result = du_size_result{frame.total_bytes, true};
          return;
        }

        let &parent = frames[frame.parent_index];
        if (frame.total_bytes > UINT64_MAX - parent.total_bytes) {
          report_soft_koshkit_util_error(ec, cxt, "du",
                                         "cannot read '" + frame.path.text() +
                                             "': the total size is too large");
          parent.has_failure = true;
          has_failure = true;
        } else {
          parent.total_bytes += frame.total_bytes;
        }
      }

      if (frame.parent_index == SIZE_MAX) {
        is_root_complete = true;
        root_result = du_size_result{frame.total_bytes, !frame.has_failure};
        return;
      }

      let const parent_index = frame.parent_index;
      let &parent = frames[parent_index];
      if (parent.pending_directory_count != 0) parent.pending_directory_count--;
      frame_index = parent_index;
    }
  };

  let const do_flush_stat_work = [&]() throws -> void {
    if (stat_work.is_empty()) return;

    stat_batch.clear();
    for (let &work : stat_work)
      stat_batch.add(os::batch_operation::lstat(work.path, work.status));
    stat_batch.execute(batch_results, os::batch_deduplication::Disabled);
    for (usize index = 0; index < stat_work.count(); index++) {
      let &work = stat_work[index];
      let const parent_index = work.parent_index;
      if (batch_results[index].error_number != 0) {
        os::set_last_system_error(batch_results[index].error_number);
        report_soft_koshkit_util_error(
            ec, cxt, "du",
            "cannot read '" + work.path.text() +
                "': " + os::last_system_error_message());
        frames[parent_index].has_failure = true;
        has_failure = true;
        frames[parent_index].pending_stat_count--;
        do_try_complete(parent_index);
        continue;
      }

      let const &status = work.status;
      let const type = os::file_type_letter(status.mode);
      if (type != 'd' && status.has_file_identity && status.link_count > 1) {
        const u64 identity[] = {status.device_id, status.file_id};
        let const key = StringView{reinterpret_cast<const char *>(identity),
                                   sizeof(identity)};
        if (!seen_links.add(key)) {
          frames[parent_index].pending_stat_count--;
          do_try_complete(parent_index);
          continue;
        }
      }
      if (status.blocks > UINT64_MAX / 512) {
        report_soft_koshkit_util_error(ec, cxt, "du",
                                       "cannot read '" + work.path.text() +
                                           "': the total size is too large");
        frames[parent_index].has_failure = true;
        has_failure = true;
        frames[parent_index].pending_stat_count--;
        do_try_complete(parent_index);
        continue;
      }

      let const allocated_size_bytes = status.blocks * 512;
      if (type == 'd') {
        frames[parent_index].pending_directory_count++;
        frames.push(du_directory_frame{
            Path{work.path.view(), allocator},
            work.parent_index,
            allocated_size_bytes, 0, 0, false, false, false
        });
        directory_queue.push(frames.count() - 1);
      } else {
        if (allocated_size_bytes >
            UINT64_MAX - frames[parent_index].total_bytes)
        {
          report_soft_koshkit_util_error(ec, cxt, "du",
                                         "cannot read '" + work.path.text() +
                                             "': the total size is too large");
          frames[parent_index].has_failure = true;
          has_failure = true;
        } else {
          frames[parent_index].total_bytes += allocated_size_bytes;
          if (output_rows != nullptr)
            append_output_row(*output_rows, allocated_size_bytes,
                              work.path.view(), allocator);
        }
      }
      frames[parent_index].pending_stat_count--;
      do_try_complete(parent_index);
    }
    stat_batch.clear();
    stat_work.clear();
    wave_arena.reset();
  };

  usize directory_index = 0;
  while (directory_index < directory_queue.count() && !is_root_complete) {
    let const frontier_end = directory_index + 32 < directory_queue.count()
                                 ? directory_index + 32
                                 : directory_queue.count();
    for (; directory_index < frontier_end; directory_index++) {
      if (os::INTERRUPT_REQUESTED) return None;
      let const list_mark = list_arena.mark();
      defer { list_arena.release(list_mark); };
      let const frame_index = directory_queue[directory_index];
      let children =
          Path::read_directory_typed(frames[frame_index].path, list_allocator);
      if (!children.has_value()) {
        report_soft_koshkit_util_error(
            ec, cxt, "du",
            "cannot read '" + frames[frame_index].path.text() +
                "': " + os::last_system_error_message());
        frames[frame_index].has_failure = true;
        frames[frame_index].is_enumerated = true;
        has_failure = true;
        do_try_complete(frame_index);
        continue;
      }
      children->sort([](const Path::directory_child &left,
                        const Path::directory_child &right) {
        return left.name.view() < right.name.view();
      });
      frames[frame_index].pending_stat_count += children->count();
      frames[frame_index].is_enumerated = true;
      for (let const &child : *children) {
        let child_path = Path{frames[frame_index].path.view(), wave_allocator};
        child_path.append(child.name.view());
        stat_work.push(du_stat_work{steal(child_path), frame_index});
        if (stat_work.count() == 512) do_flush_stat_work();
      }
      do_try_complete(frame_index);
    }
    do_flush_stat_work();
  }

  if (!stat_work.is_empty()) do_flush_stat_work();
  if (os::INTERRUPT_REQUESTED) return None;
  if (!is_root_complete || !root_result.should_emit) return None;
  return root_result;
}

fn append_size_line(String &output, const du_output_row &row,
                    StringView rendered_size, usize size_width,
                    bool should_color) throws -> void
{
  append_report_column(output, rendered_size, size_width, true,
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
    targets.push(Path{".", allocator});
  } else {
    targets.reserve(operands.count());
    for (let const &operand : operands)
      targets.push(Path{operand.view(), allocator});
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
                   seen_links, allocator, &target_statuses[index]);
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
                        allocator);
  }

  output_rows.sort([](const du_output_row &left, const du_output_row &right) {
    if (left.size_bytes != right.size_bytes)
      return left.size_bytes > right.size_bytes;
    return left.path.view() < right.path.view();
  });

  let rendered_sizes = ArrayList<String>{allocator};
  rendered_sizes.reserve(output_rows.count());
  usize size_width = 0;
  for (let const &row : output_rows) {
    let rendered_size = FLAG_DU_HUMAN.is_enabled()
                            ? format_human_size(row.size_bytes, allocator)
                            : String::from(row.size_bytes, allocator);
    if (rendered_size.length() > size_width)
      size_width = rendered_size.length();
    rendered_sizes.push(steal(rendered_size));
  }

  let output = String{allocator};
  let const should_color = koshkit_should_color();
  for (usize index = 0; index < output_rows.count(); index++)
    append_size_line(output, output_rows[index], rendered_sizes[index].view(),
                     size_width, should_color);

  ec.print_to_stdout(output);
  if (was_interrupted) return 130;
  if (has_failure) status = 1;
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
