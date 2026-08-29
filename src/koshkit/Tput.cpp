#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("operand [argument ...]");

HELP_DESCRIPTION_DECL(
    "The tput utility writes terminal capability values and control strings.");

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
  if (capability == "clear") {
    ec.print_to_stdout("\033[H\033[2J");
    return 0;
  }
  if (capability == "init" || capability == "reset" || capability == "sgr0") {
    ec.print_to_stdout("\033[0m");
    return 0;
  }
  if (capability == "bold") {
    ec.print_to_stdout("\033[1m");
    return 0;
  }
  if (capability == "smul") {
    ec.print_to_stdout("\033[4m");
    return 0;
  }
  if (capability == "rmul") {
    ec.print_to_stdout("\033[24m");
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
