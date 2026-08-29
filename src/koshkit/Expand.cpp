#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "TextProcessing.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-t tablist] [file ...]");

HELP_DESCRIPTION_DECL("The expand utility converts tabs to spaces.");

FLAG(EXPAND_TABS, String, 't', "tabs", "Use these tab stops.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Expand);

namespace koshka::koshkit {

Expand::Expand() = default;

pure fn Expand::kind() const wontthrow -> Utility::Kind { return Kind::Expand; }

fn Expand::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let tab_stops = ArrayList<usize>{cxt.scratch_allocator()};
  if (FLAG_EXPAND_TABS.is_set()) {
    let const parsed =
        parse_tab_stop_list(FLAG_EXPAND_TABS.value(), cxt.scratch_allocator());
    if (!parsed.has_value()) throw Error{"expand: invalid tab list"};
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
                                "expand: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    usize column = 0;
    for (usize position = 0; position < content->length(); position++) {
      let const byte = (*content)[position];
      if (byte == '\t') {
        let const target = next_tab_column(column, tab_stops);
        if (target == column) {
          output += '\t';
        } else {
          while (column < target) {
            output += ' ';
            column++;
          }
        }
      } else {
        output += byte;
        if (byte == '\n' || byte == '\r')
          column = 0;
        else if (byte == '\b') {
          if (column > 0) column--;
        } else {
          column++;
        }
      }
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
