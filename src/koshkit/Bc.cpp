/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the bc utility in koshkit. The
 * bc utility evaluates decimal arithmetic statements.
 */

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

static pure fn bc_digit_value(char byte) wontthrow -> u32
{
  if (byte >= '0' && byte <= '9') {
    return static_cast<u32>(byte - '0');
  }
  if (byte >= 'A' && byte <= 'F') {
    return static_cast<u32>(byte - 'A' + 10);
  }
  return ~u32{0};
}

static fn bc_strip_comments(StringView program, Allocator allocator) throws
    -> String
{
  let stripped = String{allocator};
  bool is_in_comment = false;
  bool is_in_string = false;
  for (usize position = 0; position < program.length; position++) {
    if (is_in_comment) {
      if (program[position] == '*' && position + 1 < program.length &&
          program[position + 1] == '/')
      {
        is_in_comment = false;
        position++;
      } else if (program[position] == '\n') {
        stripped.push('\n');
      } else {
        stripped.push(' ');
      }
      continue;
    }
    if (!is_in_string && program[position] == '/' &&
        position + 1 < program.length && program[position + 1] == '*')
    {
      is_in_comment = true;
      stripped.push(' ');
      position++;
      continue;
    }
    if (!is_in_string && program[position] == '\\' &&
        position + 1 < program.length && program[position + 1] == '\n')
    {
      position++;
      continue;
    }
    if (!is_in_string && program[position] == '\\' &&
        position + 2 < program.length && program[position + 1] == '\r' &&
        program[position + 2] == '\n')
    {
      position += 2;
      continue;
    }
    if (program[position] == '"') is_in_string = !is_in_string;
    stripped.push(program[position]);
  }
  return stripped;
}

static fn bc_radix_number(StringView number, u32 input_base,
                          Allocator allocator) throws -> String
{
  if (input_base == 10) return String{allocator, number};
  let const point = number.find_character('.');
  let const integer =
      point.has_value() ? number.substring_of_length(0, *point) : number;
  let const fraction =
      point.has_value() ? number.substring(*point + 1) : StringView{};
  let translated = String{allocator, "("};
  if (integer.is_empty()) {
    translated += '0';
  } else {
    translated += String::from(input_base, allocator);
    translated += '#';
    translated += integer;
  }
  if (!fraction.is_empty()) {
    translated += "+(";
    translated += String::from(input_base, allocator);
    translated += '#';
    translated += fraction;
    translated += '*';
    translated += "1.";
    translated.append_repeated('0', fraction.length);
    translated += "/(";
    translated += String::from(input_base, allocator);
    translated += "**";
    translated += String::from(fraction.length, allocator);
    translated += "))";
  }
  translated += ')';
  return translated;
}

static fn bc_translate_expression(StringView expression, u32 input_base,
                                  u32 output_base, u32 scale,
                                  bool has_math_library, EvalContext &cxt,
                                  Allocator allocator) throws -> String
{
  static constexpr static_string_entry<StringView> MATH_ENTRIES[] = {
      {SSK("a"), "atan"},
      {SSK("c"), "cos" },
      {SSK("e"), "exp" },
      {SSK("l"), "ln"  },
      {SSK("s"), "sin" },
  };
  static constexpr StaticStringMap MATH_FUNCTIONS{MATH_ENTRIES};
  static constexpr PackedStringKey SCALED_FUNCTION_ENTRIES[] = {
      SSK("atan"), SSK("cos"), SSK("exp"), SSK("ln"), SSK("sin"), SSK("sqrt"),
  };
  static constexpr StaticStringSet SCALED_FUNCTIONS{SCALED_FUNCTION_ENTRIES};
  let translated = String{allocator};
  usize position = 0;
  while (position < expression.length) {
    let const byte = expression[position];
    if (byte == '^') {
      translated += "**";
      position++;
      continue;
    }
    if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') ||
        (byte == '.' && position + 1 < expression.length &&
         expression[position + 1] >= '0' && expression[position + 1] <= '9'))
    {
      let const number_start = position;
      while (position < expression.length &&
             ((expression[position] >= '0' && expression[position] <= '9') ||
              (expression[position] >= 'A' && expression[position] <= 'F') ||
              expression[position] == '.'))
        position++;
      let const radix_number = bc_radix_number(
          expression.substring_of_length(number_start, position - number_start),
          input_base, allocator);
      if (input_base == 10)
        translated += radix_number.view();
      else
        translated +=
            cxt.evaluate_calculator_arithmetic_text(radix_number.view());
      continue;
    }
    if (lexer::is_variable_name_start(byte)) {
      let const name_start = position;
      while (position < expression.length &&
             lexer::is_variable_name(expression[position]))
        position++;
      let const name =
          expression.substring_of_length(name_start, position - name_start);
      usize next_position = position;
      while (next_position < expression.length &&
             (expression[next_position] == ' ' ||
              expression[next_position] == '\t'))
        next_position++;
      let const is_function =
          next_position < expression.length && expression[next_position] == '(';
      if (!is_function && name == "ibase") {
        translated += String::from(input_base, allocator);
      } else if (!is_function && name == "obase") {
        translated += String::from(output_base, allocator);
      } else if (!is_function && name == "scale") {
        translated += String::from(scale, allocator);
      } else if (is_function &&
                 (name == "length" || name == "scale" || name == "sqrt"))
      {
        translated += name;
      } else if (let const math_function = MATH_FUNCTIONS.find(name);
                 is_function && has_math_library && math_function.has_value())
      {
        translated += *math_function;
      } else {
        let variable_name = String{allocator, "__bc_"};
        variable_name += name;
        translated += variable_name.view();
        if (cxt.lookup_shell_variable(variable_name.view()) == NULL)
          cxt.set_shell_variable(variable_name.view(), "0");
      }
      continue;
    }
    translated.push(byte);
    position++;
  }

  let scaled = String{allocator};
  position = 0;
  while (position < translated.count()) {
    usize name_end = position;
    while (name_end < translated.count() &&
           lexer::is_variable_name(translated[name_end]))
      name_end++;
    let const function_name =
        translated.view().substring_of_length(position, name_end - position);
    if (name_end < translated.count() && translated[name_end] == '(' &&
        SCALED_FUNCTIONS.contains(function_name))
    {
      usize inner_position = name_end + 1;
      usize depth = 1;
      while (inner_position < translated.count() && depth != 0) {
        if (translated[inner_position] == '(')
          depth++;
        else if (translated[inner_position] == ')')
          depth--;
        inner_position++;
      }
      if (depth == 0) {
        scaled += translated.view().substring_of_length(
            position, inner_position - position - 1);
        scaled += ',';
        scaled += String::from(scale, allocator);
        scaled += ')';
        position = inner_position;
        continue;
      }
    }
    scaled.push(translated[position++]);
  }
  return scaled;
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
  if (position < statement.length && statement[position] == '[') {
    usize depth = 1;
    position++;
    while (position < statement.length && depth != 0) {
      if (statement[position] == '[')
        depth++;
      else if (statement[position] == ']')
        depth--;
      position++;
    }
    if (depth != 0) return false;
  }
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position >= statement.length) return false;
  if (statement[position] == '=')
    return position + 1 == statement.length || statement[position + 1] != '=';
  return position + 1 < statement.length &&
         (statement[position] == '+' || statement[position] == '-' ||
          statement[position] == '*' || statement[position] == '/' ||
          statement[position] == '%' || statement[position] == '^') &&
         statement[position + 1] == '=';
}

static pure fn bc_register_value(StringView statement,
                                 StringView name) wontthrow -> Maybe<StringView>
{
  usize position = 0;
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (!statement.substring(position).starts_with(name)) return {};
  position += name.length;
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position >= statement.length || statement[position] != '=') {
    return {};
  }
  position++;
  return statement.substring(position).trim_blanks();
}

static fn bc_parse_register(StringView value, u32 input_base, u32 output_base,
                            EvalContext &cxt, Allocator allocator) throws
    -> Maybe<u32>
{
  if (value.length == 1) {
    let const digit = bc_digit_value(value[0]);
    if (digit != ~u32{0}) return digit;
  }
  let const translated = bc_translate_expression(value, input_base, output_base,
                                                 0, false, cxt, allocator);
  let const result = cxt.evaluate_calculator_arithmetic_text(translated.view());
  let const parsed = utils::parse_decimal_u64(result.view());
  if (parsed.is_error() || parsed.value() > ~u32{0}) return {};
  return static_cast<u32>(parsed.value());
}

static fn bc_base_digit(u32 digit) wontthrow -> char
{
  return digit < 10 ? static_cast<char>('0' + digit)
                    : static_cast<char>('A' + digit - 10);
}

static fn bc_format_in_base(StringView decimal, u32 output_base,
                            EvalContext &cxt, Allocator allocator) throws
    -> String
{
  if (output_base == 10) return String{allocator, decimal};
  bool is_negative = !decimal.is_empty() && decimal[0] == '-';
  if (is_negative) decimal = decimal.substring(1);
  let const point = decimal.find_character('.');
  let integer = String{allocator, point.has_value()
                                      ? decimal.substring_of_length(0, *point)
                                      : decimal};
  if (integer.is_empty()) integer += '0';
  let reversed = String{allocator};
  while (integer.view() != "0") {
    let expression = String{allocator, "("};
    expression += integer.view();
    expression += ")%";
    expression += String::from(output_base, allocator);
    let const remainder =
        cxt.evaluate_calculator_arithmetic_text(expression.view());
    let const digit = utils::parse_decimal_u64(remainder.view());
    if (digit.is_error() || digit.value() >= output_base)
      throw std::bad_alloc{};
    reversed.push(bc_base_digit(static_cast<u32>(digit.value())));
    expression = String{allocator, "("};
    expression += integer.view();
    expression += ")/";
    expression += String::from(output_base, allocator);
    integer = cxt.evaluate_calculator_arithmetic_text(expression.view());
  }

  let formatted = String{allocator};
  if (is_negative) formatted += '-';
  if (reversed.is_empty()) {
    formatted += '0';
  } else {
    for (usize position = reversed.count(); position > 0; position--)
      formatted += reversed[position - 1];
  }

  if (!point.has_value()) return formatted;
  let const fraction_digits = decimal.length - *point - 1;
  if (fraction_digits == 0) return formatted;
  let const output_digit_count = static_cast<usize>(
      std::ceil(static_cast<double>(fraction_digits) * std::log(10.0) /
                std::log(static_cast<double>(output_base))));
  let fraction = String{allocator, "0."};
  fraction += decimal.substring(*point + 1);
  formatted += '.';
  for (usize position = 0; position < output_digit_count; position++) {
    let expression = String{allocator, "("};
    expression += fraction.view();
    expression += ")*";
    expression += String::from(output_base, allocator);
    let const product =
        cxt.evaluate_calculator_arithmetic_text(expression.view());
    let const product_point = product.find_character('.');
    let const whole = product_point.has_value()
                          ? product.substring_of_length(0, *product_point)
                          : product.view();
    let const digit = utils::parse_decimal_u64(whole);
    if (digit.is_error() || digit.value() >= output_base)
      throw std::bad_alloc{};
    formatted += bc_base_digit(static_cast<u32>(digit.value()));
    fraction = String{allocator, "0"};
    if (product_point.has_value())
      fraction += product.substring(*product_point);
  }
  return formatted;
}

struct bc_function_parameter
{
  explicit bc_function_parameter(Allocator allocator) : name(allocator) {}

  String name;
  bool is_array{false};
};

struct bc_function
{
  explicit bc_function(Allocator allocator)
      : name(allocator), parameters(allocator), body(allocator)
  {}

  String name;
  ArrayList<bc_function_parameter> parameters;
  String body;
};

struct bc_function_argument
{
  explicit bc_function_argument(Allocator allocator)
      : scalar(allocator), array(allocator)
  {}

  String scalar;
  ArrayList<String> array;
};

struct bc_runtime
{
  explicit bc_runtime(Allocator allocator)
      : functions(allocator), return_value(allocator)
  {}

  u32 scale{0};
  u32 input_base{10};
  u32 output_base{10};
  i32 status{0};
  bool has_math_library{false};
  ArrayList<bc_function> functions;
  String return_value;
};

enum class bc_flow : u8
{
  Normal,
  Break,
  Return,
  Quit,
};

static fn bc_run_program(StringView program, const ExecContext &ec,
                         EvalContext &cxt, bc_runtime &runtime) throws
    -> bc_flow;

static fn bc_expand_function_calls(StringView expression, const ExecContext &ec,
                                   EvalContext &cxt, bc_runtime &runtime,
                                   Allocator allocator) throws -> String;

static fn bc_group_nested_assignment(StringView expression,
                                     Allocator allocator) throws -> String
{
  usize depth = 0;
  usize name_start = 0;
  usize name_end = 0;
  for (usize position = 0; position < expression.length; position++) {
    if (expression[position] == '(' || expression[position] == '[') {
      depth++;
      continue;
    }
    if ((expression[position] == ')' || expression[position] == ']') &&
        depth != 0)
    {
      depth--;
      continue;
    }
    if (depth == 0 && lexer::is_variable_name_start(expression[position])) {
      name_start = position;
      while (position < expression.length &&
             lexer::is_variable_name(expression[position]))
        position++;
      name_end = position;
      position--;
      continue;
    }
    if (depth != 0 || position + 1 >= expression.length ||
        expression[position + 1] != '=' ||
        (expression[position] != '+' && expression[position] != '-' &&
         expression[position] != '*' && expression[position] != '/' &&
         expression[position] != '%' && expression[position] != '^'))
      continue;
    usize previous = position;
    while (previous > 0 && (expression[previous - 1] == ' ' ||
                            expression[previous - 1] == '\t'))
      previous--;
    if (previous != name_end) continue;
    usize prefix_end = name_start;
    while (prefix_end > 0 && (expression[prefix_end - 1] == ' ' ||
                              expression[prefix_end - 1] == '\t'))
      prefix_end--;
    if (prefix_end == 0) continue;
    let grouped =
        String{allocator, expression.substring_of_length(0, name_start)};
    grouped += '(';
    grouped += expression.substring(name_start);
    grouped += ')';
    return grouped;
  }
  return String{allocator, expression};
}

static fn bc_evaluate_text(StringView expression, EvalContext &cxt,
                           bc_runtime &runtime, const ExecContext &ec) throws
    -> String
{
  let const allocator = cxt.scratch_allocator();
  let const grouped = bc_group_nested_assignment(expression, allocator);
  let const expanded =
      bc_expand_function_calls(grouped.view(), ec, cxt, runtime, allocator);
  let const translated = bc_translate_expression(
      expanded.view(), runtime.input_base, runtime.output_base, runtime.scale,
      runtime.has_math_library, cxt, allocator);
  return cxt.evaluate_bc_arithmetic_text(translated.view(), runtime.scale);
}

static fn bc_print_result(String result, const ExecContext &ec,
                          EvalContext &cxt, bc_runtime &runtime) throws -> void
{
  let const allocator = cxt.scratch_allocator();
  bool is_zero = true;
  for (usize position = 0; position < result.count(); position++) {
    if (result[position] != '0' && result[position] != '.' &&
        result[position] != '-')
    {
      is_zero = false;
      break;
    }
  }
  if (is_zero) result = String{allocator, "0"};
  result =
      bc_format_in_base(result.view(), runtime.output_base, cxt, allocator);
  if (runtime.output_base == 10 && result.length() > 1 && result[0] == '0' &&
      result[1] == '.')
  {
    result = String{allocator, result.view().substring(1)};
  } else if (runtime.output_base == 10 && result.length() > 2 &&
             result[0] == '-' && result[1] == '0' && result[2] == '.')
  {
    let formatted = String{allocator, "-"};
    formatted += result.view().substring(2);
    result = steal(formatted);
  }
  constexpr usize OUTPUT_LINE_LENGTH = 68;
  let output = String{allocator};
  usize position = 0;
  while (result.length() - position > OUTPUT_LINE_LENGTH) {
    output += result.view().substring_of_length(position, OUTPUT_LINE_LENGTH);
    output += "\\\n";
    position += OUTPUT_LINE_LENGTH;
  }
  output += result.view().substring(position);
  output += '\n';
  ec.print_to_stdout(output);
}

static fn bc_find_function(bc_runtime &runtime, StringView name) wontthrow
    -> bc_function *
{
  for (let &function : runtime.functions) {
    if (function.name.view() == name) return &function;
  }
  return NULL;
}

static fn bc_define_function(StringView statement, bc_runtime &runtime,
                             Allocator allocator) throws -> bool
{
  if (!statement.starts_with("define")) return false;
  usize position = 6;
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position >= statement.length ||
      !lexer::is_variable_name_start(statement[position]))
    return false;
  let const name_start = position;
  while (position < statement.length &&
         lexer::is_variable_name(statement[position]))
    position++;
  let const name =
      statement.substring_of_length(name_start, position - name_start);
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position >= statement.length || statement[position] != '(') {
    return false;
  }
  let const parameter_start = ++position;
  usize depth = 1;
  while (position < statement.length && depth != 0) {
    if (statement[position] == '(')
      depth++;
    else if (statement[position] == ')')
      depth--;
    position++;
  }
  if (depth != 0) return false;
  let const parameters = statement.substring_of_length(
      parameter_start, position - parameter_start - 1);
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t' ||
          statement[position] == '\n'))
    position++;
  if (position >= statement.length || statement[position] != '{') {
    return false;
  }
  let const body_start = ++position;
  usize brace_depth = 1;
  while (position < statement.length && brace_depth != 0) {
    if (statement[position] == '{')
      brace_depth++;
    else if (statement[position] == '}')
      brace_depth--;
    position++;
  }
  if (brace_depth != 0 ||
      !statement.substring(position).trim_blanks().is_empty())
    return false;

  bc_function function{allocator};
  function.name = String{allocator, name};
  function.body = String{allocator, statement.substring_of_length(
                                        body_start, position - body_start - 1)};
  usize part_start = 0;
  for (usize part_end = 0; part_end <= parameters.length; part_end++) {
    if (part_end != parameters.length && parameters[part_end] != ',') {
      continue;
    }
    let parameter =
        parameters.substring_of_length(part_start, part_end - part_start)
            .trim_blanks();
    part_start = part_end + 1;
    if (parameter.is_empty()) continue;
    bc_function_parameter parsed{allocator};
    if (parameter.length >= 2 && parameter[parameter.length - 2] == '[' &&
        parameter[parameter.length - 1] == ']')
    {
      parsed.is_array = true;
      parameter =
          parameter.substring_of_length(0, parameter.length - 2).trim_blanks();
    }
    if (parameter.is_empty()) return false;
    parsed.name = String{allocator, parameter};
    function.parameters.push(steal(parsed));
  }

  if (let *existing = bc_find_function(runtime, name); existing != NULL)
    *existing = steal(function);
  else
    runtime.functions.push(steal(function));
  return true;
}

static fn bc_declare_auto(StringView statement, EvalContext &cxt,
                          Allocator allocator) throws -> bool
{
  if (!statement.starts_with("auto") ||
      (statement.length > 4 && statement[4] != ' ' && statement[4] != '\t'))
    return false;
  let const declarations = statement.substring(4);
  usize part_start = 0;
  for (usize part_end = 0; part_end <= declarations.length; part_end++) {
    if (part_end != declarations.length && declarations[part_end] != ',')
      continue;
    let name =
        declarations.substring_of_length(part_start, part_end - part_start)
            .trim_blanks();
    part_start = part_end + 1;
    if (name.is_empty()) continue;
    bool is_array = false;
    if (name.length >= 2 && name[name.length - 2] == '[' &&
        name[name.length - 1] == ']')
    {
      is_array = true;
      name = name.substring_of_length(0, name.length - 2).trim_blanks();
    }
    let translated = String{allocator, "__bc_"};
    translated += name;
    cxt.declare_local(translated.view(), true);
    if (is_array)
      cxt.set_indexed_array(translated.view(),
                            ArrayList<String>{heap_allocator()});
    else
      cxt.set_shell_variable(translated.view(), "0");
  }
  return true;
}

static fn bc_evaluate_function_call(StringView statement, const ExecContext &ec,
                                    EvalContext &cxt,
                                    bc_runtime &runtime) throws -> Maybe<String>
{
  usize position = 0;
  if (statement.is_empty() ||
      !lexer::is_variable_name_start(statement[position]))
    return {};
  let const name_start = position;
  while (position < statement.length &&
         lexer::is_variable_name(statement[position]))
    position++;
  let const name =
      statement.substring_of_length(name_start, position - name_start);
  let *function = bc_find_function(runtime, name);
  if (function == NULL) return {};
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position >= statement.length || statement[position] != '(') {
    return {};
  }
  let const arguments_start = ++position;
  usize depth = 1;
  while (position < statement.length && depth != 0) {
    if (statement[position] == '(')
      depth++;
    else if (statement[position] == ')')
      depth--;
    position++;
  }
  if (depth != 0 || !statement.substring(position).trim_blanks().is_empty())
    return {};
  let const arguments_text = statement.substring_of_length(
      arguments_start, position - arguments_start - 1);
  let arguments = ArrayList<StringView>{cxt.scratch_allocator()};
  usize part_start = 0;
  depth = 0;
  for (usize part_end = 0; part_end <= arguments_text.length; part_end++) {
    if (part_end != arguments_text.length) {
      if (arguments_text[part_end] == '(' || arguments_text[part_end] == '[')
        depth++;
      else if ((arguments_text[part_end] == ')' ||
                arguments_text[part_end] == ']') &&
               depth != 0)
        depth--;
    }
    if (part_end != arguments_text.length &&
        (arguments_text[part_end] != ',' || depth != 0))
      continue;
    let const argument =
        arguments_text.substring_of_length(part_start, part_end - part_start)
            .trim_blanks();
    part_start = part_end + 1;
    if (!argument.is_empty()) arguments.push(argument);
  }

  if (arguments.count() != function->parameters.count()) {
    throw ErrorWithDetails{"Wrong number of arguments for '" + String{name} +
                               "'",
                           "The function requires " +
                               String::from(function->parameters.count(),
                                            cxt.scratch_allocator()) +
                               " arguments"};
  }

  let prepared = ArrayList<bc_function_argument>{cxt.scratch_allocator()};
  for (usize index = 0; index < function->parameters.count(); index++) {
    bc_function_argument argument{cxt.scratch_allocator()};
    let const source =
        index < arguments.count() ? arguments[index] : StringView{"0"};
    if (function->parameters[index].is_array) {
      let source_name = source;
      if (source_name.length >= 2 &&
          source_name[source_name.length - 2] == '[' &&
          source_name[source_name.length - 1] == ']')
        source_name = source_name.substring_of_length(0, source_name.length - 2)
                          .trim_blanks();
      let translated = String{cxt.scratch_allocator(), "__bc_"};
      translated += source_name;
      if (let const *array = cxt.lookup_indexed_array(translated.view());
          array != NULL)
        argument.array = array->clone();
    } else {
      argument.scalar = bc_evaluate_text(source, cxt, runtime, ec);
    }
    prepared.push(steal(argument));
  }

  cxt.enter_function_scope();
  defer { cxt.leave_function_scope(); };
  for (usize index = 0; index < function->parameters.count(); index++) {
    let translated = String{cxt.scratch_allocator(), "__bc_"};
    translated += function->parameters[index].name.view();
    cxt.declare_local(translated.view(), true);
    if (function->parameters[index].is_array)
      cxt.set_indexed_array(translated.view(), steal(prepared[index].array));
    else
      cxt.set_shell_variable(translated.view(), prepared[index].scalar.view());
  }

  let saved_return = runtime.return_value.clone();
  runtime.return_value.clear();
  let const body = function->body.clone();
  let const flow = bc_run_program(body.view(), ec, cxt, runtime);
  let result = flow == bc_flow::Return ? runtime.return_value.clone()
                                       : String{cxt.scratch_allocator(), "0"};
  runtime.return_value = steal(saved_return);
  return result;
}

static fn bc_expand_function_calls(StringView expression, const ExecContext &ec,
                                   EvalContext &cxt, bc_runtime &runtime,
                                   Allocator allocator) throws -> String
{
  let expanded = String{allocator};
  usize position = 0;
  while (position < expression.length) {
    if (!lexer::is_variable_name_start(expression[position])) {
      expanded += expression[position++];
      continue;
    }

    let const name_start = position;
    while (position < expression.length &&
           lexer::is_variable_name(expression[position]))
      position++;
    let const name =
        expression.substring_of_length(name_start, position - name_start);
    usize opening_position = position;
    while (opening_position < expression.length &&
           (expression[opening_position] == ' ' ||
            expression[opening_position] == '\t'))
      opening_position++;
    if (bc_find_function(runtime, name) == NULL ||
        opening_position >= expression.length ||
        expression[opening_position] != '(')
    {
      expanded +=
          expression.substring_of_length(name_start, position - name_start);
      continue;
    }

    usize call_end = opening_position + 1;
    usize depth = 1;
    while (call_end < expression.length && depth != 0) {
      if (expression[call_end] == '(')
        depth++;
      else if (expression[call_end] == ')')
        depth--;
      call_end++;
    }
    if (depth != 0) {
      expanded +=
          expression.substring_of_length(name_start, position - name_start);
      continue;
    }

    let result = bc_evaluate_function_call(
        expression.substring_of_length(name_start, call_end - name_start), ec,
        cxt, runtime);
    if (!result.has_value()) {
      expanded +=
          expression.substring_of_length(name_start, call_end - name_start);
    } else {
      expanded += '(';
      expanded += result->view();
      expanded += ')';
    }
    position = call_end;
  }

  return expanded;
}

static fn bc_try_function_call(StringView statement, bool should_print,
                               const ExecContext &ec, EvalContext &cxt,
                               bc_runtime &runtime) throws -> bool
{
  let result = bc_evaluate_function_call(statement, ec, cxt, runtime);
  if (!result.has_value()) return false;
  if (should_print) bc_print_result(steal(*result), ec, cxt, runtime);
  return true;
}

static fn bc_execute_simple(StringView statement, bool should_print,
                            const ExecContext &ec, EvalContext &cxt,
                            bc_runtime &runtime) throws -> void
{
  let const allocator = cxt.scratch_allocator();
  if (statement.length >= 2 && statement[0] == '"' &&
      statement[statement.length - 1] == '"')
  {
    if (should_print)
      ec.print_to_stdout(
          statement.substring_of_length(1, statement.length - 2));
    return;
  }
  if (bc_try_function_call(statement, should_print, ec, cxt, runtime)) return;

  if (let const value = bc_register_value(statement, "ibase");
      value.has_value())
  {
    let const parsed = bc_parse_register(*value, runtime.input_base,
                                         runtime.output_base, cxt, allocator);
    if (!parsed.has_value() || *parsed < 2 || *parsed > 16) {
      report_soft_koshkit_error(ec, cxt, "bc: invalid ibase");
      runtime.status = 1;
    } else {
      runtime.input_base = *parsed;
    }
    return;
  }
  if (let const value = bc_register_value(statement, "obase");
      value.has_value())
  {
    let const parsed = bc_parse_register(*value, runtime.input_base,
                                         runtime.output_base, cxt, allocator);
    if (!parsed.has_value() || *parsed < 2 || *parsed > 16) {
      report_soft_koshkit_error(ec, cxt, "bc: invalid obase");
      runtime.status = 1;
    } else {
      runtime.output_base = *parsed;
    }
    return;
  }
  if (let const value = bc_register_value(statement, "scale");
      value.has_value())
  {
    let const parsed = bc_parse_register(*value, runtime.input_base,
                                         runtime.output_base, cxt, allocator);
    if (!parsed.has_value() || *parsed > 100000) {
      report_soft_koshkit_error(ec, cxt, "bc: invalid scale");
      runtime.status = 1;
    } else {
      runtime.scale = *parsed;
    }
    return;
  }

  try {
    let result = bc_evaluate_text(statement, cxt, runtime, ec);
    if (!should_print || bc_is_assignment(statement)) return;
    bc_print_result(steal(result), ec, cxt, runtime);
  } catch (const Error &error) {
    report_soft_koshkit_error(ec, cxt, "bc: " + error.to_string());
    runtime.status = 1;
  }
}

static fn bc_condition_is_true(StringView condition, const ExecContext &ec,
                               EvalContext &cxt, bc_runtime &runtime) throws
    -> bool
{
  let const allocator = cxt.scratch_allocator();
  let const expanded =
      bc_expand_function_calls(condition, ec, cxt, runtime, allocator);
  let const translated = bc_translate_expression(
      expanded.view(), runtime.input_base, runtime.output_base, runtime.scale,
      runtime.has_math_library, cxt, allocator);
  let expression = String{allocator, "("};
  expression += translated.view();
  expression += ")!=0";
  return cxt.evaluate_bc_arithmetic_text(expression.view(), runtime.scale) !=
         "0";
}

static pure fn bc_control_parts(StringView statement, StringView keyword,
                                StringView &header, StringView &body) wontthrow
    -> bool
{
  if (!statement.starts_with(keyword)) return false;
  usize position = keyword.length;
  while (position < statement.length &&
         (statement[position] == ' ' || statement[position] == '\t'))
    position++;
  if (position >= statement.length || statement[position] != '(') {
    return false;
  }
  let const header_start = ++position;
  usize depth = 1;
  while (position < statement.length && depth != 0) {
    if (statement[position] == '(')
      depth++;
    else if (statement[position] == ')')
      depth--;
    position++;
  }
  if (depth != 0) return false;
  header =
      statement.substring_of_length(header_start, position - header_start - 1);
  body = statement.substring(position).trim_blanks();
  return !body.is_empty();
}

static fn bc_execute_statement(StringView statement, const ExecContext &ec,
                               EvalContext &cxt, bc_runtime &runtime) throws
    -> bc_flow
{
  statement = statement.trim_blanks();
  if (statement.is_empty()) return bc_flow::Normal;
  if (statement == "break") return bc_flow::Break;
  if (statement == "quit") return bc_flow::Quit;
  if (bc_define_function(statement, runtime, cxt.scratch_allocator()))
    return bc_flow::Normal;
  if (bc_declare_auto(statement, cxt, cxt.scratch_allocator()))
    return bc_flow::Normal;
  if (statement.starts_with("return") &&
      (statement.length == 6 || statement[6] == ' ' || statement[6] == '\t' ||
       statement[6] == '('))
  {
    let value = statement.substring(6).trim_blanks();
    if (value.length >= 2 && value[0] == '(' && value[value.length - 1] == ')')
      value = value.substring_of_length(1, value.length - 2).trim_blanks();
    runtime.return_value = value.is_empty()
                               ? String{cxt.scratch_allocator(), "0"}
                               : bc_evaluate_text(value, cxt, runtime, ec);
    return bc_flow::Return;
  }
  if (statement.length >= 2 && statement[0] == '{' &&
      statement[statement.length - 1] == '}')
    return bc_run_program(
        statement.substring_of_length(1, statement.length - 2), ec, cxt,
        runtime);

  StringView header;
  StringView body;
  if (bc_control_parts(statement, "if", header, body)) {
    if (!bc_condition_is_true(header, ec, cxt, runtime)) return bc_flow::Normal;
    return bc_execute_statement(body, ec, cxt, runtime);
  }
  if (bc_control_parts(statement, "while", header, body)) {
    while (bc_condition_is_true(header, ec, cxt, runtime)) {
      let const flow = bc_execute_statement(body, ec, cxt, runtime);
      if (flow == bc_flow::Quit || flow == bc_flow::Return) {
        return flow;
      }
      if (flow == bc_flow::Break) break;
      if (os::INTERRUPT_REQUESTED) break;
    }
    return bc_flow::Normal;
  }
  if (bc_control_parts(statement, "for", header, body)) {
    StringView parts[3];
    usize part_count = 0;
    usize part_start = 0;
    usize depth = 0;
    for (usize position = 0; position <= header.length; position++) {
      if (position != header.length) {
        if (header[position] == '(')
          depth++;
        else if (header[position] == ')' && depth != 0)
          depth--;
      }
      if (position != header.length && (header[position] != ';' || depth != 0))
        continue;
      if (part_count >= 3) return bc_flow::Normal;
      parts[part_count++] =
          header.substring_of_length(part_start, position - part_start)
              .trim_blanks();
      part_start = position + 1;
    }
    if (part_count != 3) return bc_flow::Normal;
    if (!parts[0].is_empty())
      bc_execute_simple(parts[0], false, ec, cxt, runtime);
    while (parts[1].is_empty() ||
           bc_condition_is_true(parts[1], ec, cxt, runtime))
    {
      let const flow = bc_execute_statement(body, ec, cxt, runtime);
      if (flow == bc_flow::Quit || flow == bc_flow::Return) {
        return flow;
      }
      if (flow == bc_flow::Break) break;
      if (!parts[2].is_empty())
        bc_execute_simple(parts[2], false, ec, cxt, runtime);
      if (os::INTERRUPT_REQUESTED) break;
    }
    return bc_flow::Normal;
  }

  bc_execute_simple(statement, true, ec, cxt, runtime);
  return bc_flow::Normal;
}

static fn bc_run_program(StringView program, const ExecContext &ec,
                         EvalContext &cxt, bc_runtime &runtime) throws
    -> bc_flow
{
  usize statement_start = 0;
  usize parenthesis_depth = 0;
  usize brace_depth = 0;
  bool is_in_string = false;
  for (usize position = 0; position <= program.length; position++) {
    if (position != program.length) {
      let const byte = program[position];
      if (byte == '"') is_in_string = !is_in_string;
      if (!is_in_string) {
        if (byte == '(')
          parenthesis_depth++;
        else if (byte == ')' && parenthesis_depth != 0)
          parenthesis_depth--;
        else if (byte == '{')
          brace_depth++;
        else if (byte == '}' && brace_depth != 0)
          brace_depth--;
      }
      if (is_in_string || parenthesis_depth != 0 || brace_depth != 0 ||
          (byte != ';' && byte != '\n'))
        continue;
    }

    let const statement =
        program.substring_of_length(statement_start, position - statement_start)
            .trim_blanks();
    statement_start = position + 1;
    let const flow = bc_execute_statement(statement, ec, cxt, runtime);
    if (flow != bc_flow::Normal) return flow;
  }
  return bc_flow::Normal;
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
  program = bc_strip_comments(program.view(), cxt.scratch_allocator());

  bc_runtime runtime{cxt.scratch_allocator()};
  runtime.has_math_library = FLAG_BC_MATH_LIBRARY.is_enabled();
  if (runtime.has_math_library) runtime.scale = 20;
  unused(bc_run_program(program.view(), ec, cxt, runtime));
  return runtime.status;
}

} // namespace koshka::koshkit
