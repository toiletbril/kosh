/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the tee utility in koshkit.
 * The tee utility copies standard input to standard output and to each named
 * file. With -a it appends to the files. The default truncates them.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [file ...]");

HELP_DESCRIPTION_DECL(
    "The tee utility copies standard input to standard output and to each "
    "named "
    "file. With -a it appends to the files. The default truncates them.");

FLAG(TEE_APPEND, Bool, 'a', "", "Append to the files.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tee);

namespace koshka {

namespace koshkit {

Tee::Tee() = default;

pure fn Tee::kind() const wontthrow -> Utility::Kind { return Kind::Tee; }

fn Tee::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const mode = FLAG_TEE_APPEND.is_enabled() ? os::file_open_mode::Append
                                                : os::file_open_mode::Truncate;
  let output_descriptors = ArrayList<os::descriptor>{cxt.scratch_allocator()};
  let output_names = ArrayList<StringView>{cxt.scratch_allocator()};
  let written_byte_counts = ArrayList<usize>{cxt.scratch_allocator()};
  let output_positions = ArrayList<usize>{cxt.scratch_allocator()};
  let batch = os::Batch{cxt.scratch_allocator()};
  let batch_results = ArrayList<os::batch_result>{cxt.scratch_allocator()};
  i32 status = 0;
  for (const String &operand : operands) {
    let const fd = os::open_file_descriptor(operand.view(), mode);
    if (!fd.has_value()) {
      report_soft_koshkit_error(
          ec, cxt, "tee: " + operand + ": " + os::last_system_error_message());
      status = 1;
      continue;
    }
    output_descriptors.push(*fd);
    output_names.push(operand.view());
  }
  written_byte_counts.reserve(output_descriptors.count());
  output_positions.reserve(output_descriptors.count());
  batch.reserve(output_descriptors.count());
  batch_results.reserve(output_descriptors.count());
  defer
  {
    for (let const descriptor : output_descriptors)
      os::close_fd(descriptor);
  };

  char buffer[65536];
  loop
  {
    let const read_size =
        os::read_fd(ec.in_fd.value_or(KOSH_STDIN), buffer, sizeof(buffer));
    if (!read_size.has_value()) {
      if (os::INTERRUPT_REQUESTED) return 130;
      report_soft_koshkit_error(
          ec, cxt, "tee: read failed: " + os::last_system_error_message());
      return 1;
    }
    if (*read_size == 0) break;

    ec.print_to_stdout(StringView{buffer, *read_size});
    written_byte_counts.clear();
    for (usize index = 0; index < output_descriptors.count(); index++)
      written_byte_counts.push(0);

    loop
    {
      batch.clear();
      output_positions.clear();
      usize output_index = output_descriptors.count();
      while (output_index > 0) {
        output_index--;
        let const written_byte_count = written_byte_counts[output_index];
        if (written_byte_count == *read_size) continue;

        batch.add(os::batch_operation::write_current(
            output_descriptors[output_index], buffer + written_byte_count,
            *read_size - written_byte_count));
        output_positions.push(output_index);
      }
      if (batch.count() == 0) break;

      batch.execute(batch_results);
      for (usize result_index = 0; result_index < batch_results.count();
           result_index++)
      {
        let const output_position = output_positions[result_index];
        let const &result = batch_results[result_index];
        let const remaining_byte_count =
            *read_size - written_byte_counts[output_position];
        if (result.error_number == 0 && result.transferred_byte_count > 0 &&
            result.transferred_byte_count <= remaining_byte_count)
        {
          written_byte_counts[output_position] += result.transferred_byte_count;
          continue;
        }

        let reason = String{cxt.scratch_allocator()};
        if (result.error_number != 0) {
          os::set_last_system_error(result.error_number);
          reason = os::last_system_error_message();
        } else {
          reason = "write made no progress";
        }
        report_soft_koshkit_error(
            ec, cxt,
            "tee: " +
                String{cxt.scratch_allocator(), output_names[output_position]} +
                ": " + reason);
        os::close_fd(output_descriptors[output_position]);
        output_descriptors.remove(output_position);
        output_names.remove(output_position);
        written_byte_counts.remove(output_position);
        status = 1;
      }
      if (os::INTERRUPT_REQUESTED) return 130;
    }
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
