#include "../Cli.hpp"
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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const mode = FLAG_TEE_APPEND.is_enabled() ? os::file_open_mode::Append
                                                : os::file_open_mode::Truncate;
  let output_descriptors = ArrayList<os::descriptor>{cxt.scratch_allocator()};
  let output_names = ArrayList<StringView>{cxt.scratch_allocator()};
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
    usize output_index = output_descriptors.count();
    while (output_index > 0) {
      output_index--;
      if (os::write_all(output_descriptors[output_index], buffer, *read_size))
        continue;

      report_soft_koshkit_error(
          ec, cxt,
          "tee: " +
              String{cxt.scratch_allocator(), output_names[output_index]} +
              ": " + os::last_system_error_message());
      os::close_fd(output_descriptors[output_index]);
      output_descriptors.remove(output_index);
      output_names.remove(output_index);
      status = 1;
    }
  }

  return status;
}

} // namespace koshkit

} // namespace koshka
