#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Lexer.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-lq] [file ...]");

HELP_DESCRIPTION_DECL(
    "The bc utility evaluates decimal arithmetic statements.");

FLAG(BC_MATH_LIBRARY, Bool, 'l', "mathlib", "Enable common math functions.");
FLAG(BC_QUIET, Bool, 'q', "quiet", "Suppress an interactive banner.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Bc);

namespace koshka::koshkit {

static fn bc_scaled_expression(StringView expression, u32 scale,
                               Allocator allocator) throws -> String
{
  String translated{allocator};
  usize position = 0;

  while (position < expression.length) {
    let const byte = expression[position];
    if (byte == '^') {
      translated += "**";
      position++;
      continue;
    }
    if (byte < '0' || byte > '9') {
      translated += byte;
      position++;
      continue;
    }

    let const number_start = position;
    bool has_decimal_point = false;
    while (position < expression.length &&
           ((expression[position] >= '0' && expression[position] <= '9') ||
            expression[position] == '.'))
    {
      if (expression[position] == '.') has_decimal_point = true;
      position++;
    }
    translated +=
        expression.substring_of_length(number_start, position - number_start);
    usize next_position = position;
    while (
        next_position < expression.length &&
        (expression[next_position] == ' ' || expression[next_position] == '\t'))
      next_position++;
    usize previous_position = number_start;
    while (previous_position != 0 &&
           (expression[previous_position - 1] == ' ' ||
            expression[previous_position - 1] == '\t'))
      previous_position--;
    let const is_division_operand =
        (next_position < expression.length &&
         expression[next_position] == '/') ||
        (previous_position != 0 && expression[previous_position - 1] == '/');
    if (scale != 0 && !has_decimal_point && is_division_operand) {
      translated += '.';
      for (u32 digit = 0; digit < scale; digit++)
        translated += '0';
    }
  }

  return translated;
}

static pure fn bc_is_assignment(StringView statement) wontthrow -> bool
{
  usize position = 0;
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position == statement.length ||
      !lexer::is_variable_name_start(statement[position]))
    return false;
  while (position < statement.length &&
         lexer::is_variable_name(statement[position]))
    position++;
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  return position < statement.length && statement[position] == '=' &&
         (position + 1 == statement.length || statement[position + 1] != '=');
}

Bc::Bc() = default;

pure fn Bc::kind() const wontthrow -> Utility::Kind { return Kind::Bc; }

fn Bc::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  String program{cxt.scratch_allocator()};
  for (let const source : sources) {
    let const content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "bc: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      return 1;
    }
    program += content->view();
    program += '\n';
  }

  u32 scale = 0;
  usize statement_start = 0;
  i32 status = 0;
  for (usize position = 0; position <= program.length(); position++) {
    if (position != program.length() && program[position] != ';' &&
        program[position] != '\n')
      continue;

    let const statement =
        program.view()
            .substring_of_length(statement_start, position - statement_start)
            .trim_blanks();
    statement_start = position + 1;
    if (statement.is_empty()) continue;
    if (statement == "quit") break;
    if (statement.starts_with("scale=")) {
      let const parsed =
          utils::parse_decimal_u64(statement.substring(6).trim_blanks());
      if (parsed.is_error() || parsed.value() > 100000) {
        report_soft_koshkit_error(ec, cxt, "bc: invalid scale");
        status = 1;
      } else {
        scale = static_cast<u32>(parsed.value());
      }
      continue;
    }

    try {
      let const expression =
          bc_scaled_expression(statement, scale, cxt.scratch_allocator());
      let result = cxt.evaluate_calculator_arithmetic_text(expression.view());
      if (!bc_is_assignment(statement)) {
        if (result.length() > 1 && result[0] == '0' && result[1] == '.') {
          result = String{cxt.scratch_allocator(), result.view().substring(1)};
        } else if (result.length() > 2 && result[0] == '-' &&
                   result[1] == '0' && result[2] == '.')
        {
          let formatted = String{cxt.scratch_allocator(), "-"};
          formatted += result.view().substring(2);
          result = steal(formatted);
        }
        result += '\n';
        ec.print_to_stdout(result);
      }
    } catch (const Error &error) {
      report_soft_koshkit_error(ec, cxt, "bc: " + error.to_string());
      status = 1;
    }
  }

  return status;
}

} // namespace koshka::koshkit
