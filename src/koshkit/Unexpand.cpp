/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the unexpand utility. It tracks display columns and
 * replaces eligible blank runs with tabs under default or explicit tab stops.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "TextProcessing.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [-t tablist] [file ...]");

HELP_DESCRIPTION_DECL("The unexpand utility converts spaces to tabs.");

FLAG(UNEXPAND_ALL, Bool, 'a', "all", "Convert blanks beyond line prefixes.");
FLAG(UNEXPAND_TABS, String, 't', "tabs", "Use these tab stops.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Unexpand);

namespace koshka::koshkit {

static fn append_unexpanded_blanks(String &output, usize start_column,
                                   usize end_column,
                                   const ArrayList<usize> &tab_stops) throws
    -> void
{
  usize column = start_column;

  while (column < end_column) {
    let const target = next_tab_column(column, tab_stops);
    if (target > column + 1 && target <= end_column) {
      output += '\t';
      column = target;
    } else {
      output += ' ';
      column++;
    }
  }
}

Unexpand::Unexpand() = default;

pure fn Unexpand::kind() const wontthrow -> Utility::Kind
{
  return Kind::Unexpand;
}

fn Unexpand::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let tab_stops = ArrayList<usize>{cxt.scratch_allocator()};
  if (FLAG_UNEXPAND_TABS.is_set()) {
    let const parsed = parse_tab_stop_list(FLAG_UNEXPAND_TABS.value(),
                                           cxt.scratch_allocator());
    if (!parsed.has_value()) throw Error{"unexpand: invalid tab list"};
    tab_stops = steal(*parsed);
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  for (let const source : sources) {
    let const content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "unexpand: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    usize column = 0;
    usize blank_start_column = 0;
    bool has_pending_blanks = false;
    bool has_nonblank = false;

    let const do_flush_blanks = [&]() throws -> void {
      if (!has_pending_blanks) return;
      append_unexpanded_blanks(output, blank_start_column, column, tab_stops);
      has_pending_blanks = false;
    };

    for (usize position = 0; position < content->length(); position++) {
      let const byte = (*content)[position];
      let const should_convert =
          FLAG_UNEXPAND_ALL.is_enabled() || !has_nonblank;
      if (should_convert && (byte == ' ' || byte == '\t')) {
        if (!has_pending_blanks) {
          blank_start_column = column;
          has_pending_blanks = true;
        }
        if (byte == ' ')
          column++;
        else {
          let const target = next_tab_column(column, tab_stops);
          column = target == column ? column + 1 : target;
        }
        continue;
      }

      do_flush_blanks();
      output += byte;
      if (byte == '\n' || byte == '\r') {
        column = 0;
        has_nonblank = false;
      } else if (byte == '\b') {
        if (column > 0) column--;
        has_nonblank = true;
      } else {
        column++;
        has_nonblank = true;
      }
    }

    do_flush_blanks();
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
