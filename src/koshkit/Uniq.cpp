/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the uniq utility in koshkit.
 * The uniq utility collapses each run of adjacent equal lines into one.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-c] [file]");

HELP_DESCRIPTION_DECL(
    "The uniq utility collapses each run of adjacent equal lines into one.");

FLAG(UNIQ_COUNT, Bool, 'c', "", "Prefix each line with the count of its run.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Uniq);

namespace koshka {

namespace koshkit {

static fn count_prefix(u64 run_length, Allocator allocator) throws -> String
{
  let const digits = String::from(run_length, allocator);
  String prefix{allocator};
  for (usize i = digits.count(); i < 7; i++)
    prefix += ' ';
  prefix += digits.view();
  prefix += ' ';
  return prefix;
}

Uniq::Uniq() = default;

pure fn Uniq::kind() const wontthrow -> Utility::Kind { return Kind::Uniq; }

fn Uniq::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const source = operands.is_empty() ? StringView{"-"} : operands[0].view();
  let const input = open_named_or_stdin(ec, source);
  if (!input.has_value())
    throw Error{
        "uniq: cannot read '" + String{cxt.scratch_allocator(), source}
          +
        "': " + os::last_system_error_message()
    };
  defer
  {
    if (input->should_close) os::close_fd(input->descriptor);
  };

  let const should_show_count = FLAG_UNIQ_COUNT.is_enabled();
  let output = String{cxt.scratch_allocator()};
  bool has_previous = false;
  let previous = String{heap_allocator()};
  u64 run_length = 0;

  let const do_flush = [&]() throws -> void {
    if (!has_previous) return;
    if (should_show_count)
      output += count_prefix(run_length, cxt.scratch_allocator());
    output += previous.view();
    output += '\n';
    if (output.count() >= 65536) {
      ec.print_to_stdout(output);
      output.clear();
    }
  };

  let reader = utils::BufferedLineReader{input->descriptor};
  loop
  {
    let const result = reader.next();
    if (result == utils::BufferedLineReader::Result::End) break;
    if (result == utils::BufferedLineReader::Result::Error) {
      if (os::INTERRUPT_REQUESTED) return 130;
      report_soft_koshkit_error(
          ec, cxt,
          "uniq: " + String{cxt.scratch_allocator(), source} + ": " +
              os::last_system_error_message());
      return 1;
    }
    let const line = reader.get_line();
    if (has_previous && line == previous.view()) {
      run_length++;
      continue;
    }

    do_flush();
    previous.clear();
    previous.append(line);
    run_length = 1;
    has_previous = true;
  }

  do_flush();

  ec.print_to_stdout(output);
  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
