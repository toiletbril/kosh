/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the tput utility in koshkit.
 * The tput utility writes terminal capability values and control strings.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-T type] operand [argument ...]");

HELP_DESCRIPTION_DECL(
    "The tput utility writes terminal capability values and control strings.");

FLAG(TPUT_TERMINAL, String, 'T', "terminal", "Use this terminal type.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tput);

namespace koshka::koshkit {

Tput::Tput() = default;

pure fn Tput::kind() const wontthrow -> Utility::Kind { return Kind::Tput; }

fn Tput::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const capability = operands[0].view();
  static constexpr static_string_entry<StringView> STRING_ENTRIES[] = {
      {SSK("bel"),   "\a"           },
      {SSK("blink"), "\033[5m"      },
      {SSK("bold"),  "\033[1m"      },
      {SSK("civis"), "\033[?25l"    },
      {SSK("clear"), "\033[H\033[2J"},
      {SSK("cnorm"), "\033[?25h"    },
      {SSK("cub1"),  "\033[D"       },
      {SSK("cud1"),  "\033[B"       },
      {SSK("cuf1"),  "\033[C"       },
      {SSK("cuu1"),  "\033[A"       },
      {SSK("dch1"),  "\033[P"       },
      {SSK("dl1"),   "\033[M"       },
      {SSK("ed"),    "\033[J"       },
      {SSK("el"),    "\033[K"       },
      {SSK("home"),  "\033[H"       },
      {SSK("ich1"),  "\033[@"       },
      {SSK("il1"),   "\033[L"       },
      {SSK("ind"),   "\n"           },
      {SSK("init"),  "\033[0m"      },
      {SSK("reset"), "\033[0m"      },
      {SSK("rev"),   "\033[7m"      },
      {SSK("rmso"),  "\033[27m"     },
      {SSK("rmul"),  "\033[24m"     },
      {SSK("sgr0"),  "\033[0m"      },
      {SSK("smso"),  "\033[7m"      },
      {SSK("smul"),  "\033[4m"      },
  };
  static constexpr StaticStringMap STRING_CAPABILITIES{STRING_ENTRIES};
  if (let const value = STRING_CAPABILITIES.find(capability); value.has_value())
  {
    ec.print_to_stdout(*value);
    return 0;
  }
  if (capability == "longname") {
    let const configured = cxt.get_variable_value("TERM");
    let const terminal_type = FLAG_TPUT_TERMINAL.is_set()
                                  ? FLAG_TPUT_TERMINAL.value()
                              : configured.has_value() ? configured->view()
                                                       : StringView{"unknown"};
    ec.print_to_stdout(terminal_type);
    ec.print_to_stdout(" terminal\n");
    return 0;
  }
  if (capability == "cup") {
    if (operands.count() != 3)
      return report_usage_error(ec, cxt, args[0].view());
    let const row = utils::parse_decimal_u64(operands[1].view());
    let const column = utils::parse_decimal_u64(operands[2].view());
    if (row.is_error() || column.is_error())
      throw Error{"tput: invalid position"};
    let output = String{cxt.scratch_allocator(), "\033["};
    output += String::from(row.value() + 1, cxt.scratch_allocator());
    output += ';';
    output += String::from(column.value() + 1, cxt.scratch_allocator());
    output += 'H';
    ec.print_to_stdout(output);
    return 0;
  }
  if (capability == "cols" || capability == "lines") {
    u32 columns = 0;
    u32 rows = 0;
    if (!os::terminal_size(columns, rows, ec.out_fd.value_or(KOSH_STDOUT)))
      return 1;
    let output = String::from(capability == "cols" ? columns : rows,
                              cxt.scratch_allocator());
    output += '\n';
    ec.print_to_stdout(output);
    return 0;
  }
  return 1;
}

} // namespace koshka::koshkit
