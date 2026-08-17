#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-v var] format [argument ...]");

HELP_DESCRIPTION_DECL(
    "The printf builtin writes its arguments under the control of a format "
    "string.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Printf);

namespace koshka {

namespace {

struct printf_number
{
  i64 value;
  bool is_valid;
  bool is_hex;
  bool is_out_of_range;
};

fn decode_utf8_code_point(const String &arg, usize start) throws -> i64
{
  let const first = static_cast<unsigned char>(arg[start]);
  if (first < 0x80) return first;

  usize continuation_count;
  i64 code_point;
  if ((first & 0xE0) == 0xC0) {
    continuation_count = 1;
    code_point = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    continuation_count = 2;
    code_point = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    continuation_count = 3;
    code_point = first & 0x07;
  } else {
    return first;
  }

  for (usize continuation_index = 1; continuation_index <= continuation_count;
       continuation_index++)
  {
    if (start + continuation_index >= arg.count()) return first;

    let const continuation_byte =
        static_cast<unsigned char>(arg[start + continuation_index]);
    if ((continuation_byte & 0xC0) != 0x80) return first;

    code_point = (code_point << 6) | (continuation_byte & 0x3F);
  }

  return code_point;
}

/* is_valid is false when a byte follows the number, matching bash. An argument
   that opens with a quote yields the next character's code point. */
fn parse_printf_number(const String &arg) throws -> printf_number
{
  if (!arg.is_empty() && (arg[0] == '\'' || arg[0] == '"')) {
    return {arg.count() > 1 ? decode_utf8_code_point(arg, 1) : 0, true, false,
            false};
  }

  usize i = 0;
  while (i < arg.count() && (arg[i] == ' ' || arg[i] == '\t'))
    i++;
  let const number_start = i;
  if (i < arg.count() && (arg[i] == '+' || arg[i] == '-')) i++;

  let const is_hexadecimal = i + 1 < arg.count() && arg[i] == '0' &&
                             (arg[i + 1] == 'x' || arg[i + 1] == 'X');
  let const is_octal = !is_hexadecimal && i < arg.count() && arg[i] == '0' &&
                       i + 1 < arg.count();
  usize digit_start = i;
  if (is_hexadecimal) {
    i += 2;
    digit_start = i;
    while (i < arg.count() && utils::hex_digit_value(arg[i]).has_value())
      i++;
  } else if (is_octal) {
    while (i < arg.count() && arg[i] >= '0' && arg[i] <= '7')
      i++;
  } else {
    while (i < arg.count() && arg[i] >= '0' && arg[i] <= '9')
      i++;
  }
  let const number_end = i;

  let const number_text =
      arg.view().substring_of_length(number_start, number_end - number_start);
  let is_out_of_range = false;
  let const parsed =
      is_hexadecimal ? utils::parse_integer_in_base(number_text, int_base::hex,
                                                    &is_out_of_range)
      : is_octal ? utils::parse_integer_in_base(number_text, int_base::octal,
                                                &is_out_of_range)
                 : utils::parse_decimal_i64(number_text, &is_out_of_range);
  let const has_digits = number_end > digit_start;
  return {parsed.is_error() ? 0 : parsed.value(),
          has_digits && number_end == arg.count(), is_hexadecimal,
          is_out_of_range};
}

fn parse_printf_integer(const String &arg) throws -> i64
{
  return parse_printf_number(arg).value;
}

fn append_simple_escape(String &out, char e,
                        bool should_use_bash_escapes) throws -> void
{
  switch (e) {
  case 'n': out += '\n'; break;
  case 't': out += '\t'; break;
  case 'r': out += '\r'; break;
  case 'a': out += '\a'; break;
  case 'b': out += '\b'; break;
  case 'f': out += '\f'; break;
  case 'v': out += '\v'; break;
  case 'e':
  case 'E':
    if (should_use_bash_escapes) {
      out += '\x1b';
    } else {
      out += '\\';
      out += e;
    }
    break;
  case '\\': out += '\\'; break;
  default:
    out += '\\';
    out += e;
    break;
  }
}

fn append_escape(String &out, const String &fmt, usize &i,
                 bool should_use_bash_escapes) throws -> void
{
  ASSERT(i < fmt.length());

  let const e = fmt[i];

  /* The index is left on the last digit consumed, the caller advances past it.
   */
  if (e >= '0' && e <= '7') {
    i32 value = e - '0';
    usize digit_count = 1;
    while (digit_count < 3 && i + 1 < fmt.length() && fmt[i + 1] >= '0' &&
           fmt[i + 1] <= '7')
    {
      i++;
      value = value * 8 + (fmt[i] - '0');
      digit_count++;
    }
    out += static_cast<char>(value);
    return;
  }

  if (e == 'x' && i + 1 < fmt.length() &&
      utils::hex_digit_value(fmt[i + 1]).has_value())
  {
    i32 value = 0;
    usize digit_count = 0;
    while (digit_count < 2 && i + 1 < fmt.length() &&
           utils::hex_digit_value(fmt[i + 1]).has_value())
    {
      i++;
      value = value * 16 + *utils::hex_digit_value(fmt[i]);
      digit_count++;
    }
    out += static_cast<char>(value);
    return;
  }

  if ((e == 'u' || e == 'U') && i + 1 < fmt.length() &&
      utils::hex_digit_value(fmt[i + 1]).has_value())
  {
    usize max_digit_count = e == 'u' ? 4 : 8;
    u32 code_point = 0;
    usize digit_count = 0;
    while (digit_count < max_digit_count && i + 1 < fmt.length() &&
           utils::hex_digit_value(fmt[i + 1]).has_value())
    {
      i++;
      code_point = code_point * 16 + *utils::hex_digit_value(fmt[i]);
      digit_count++;
    }

    utils::append_utf8(out, code_point);
    return;
  }

  append_simple_escape(out, e, should_use_bash_escapes);
}

/* Returns true when a \c was seen so the caller can abort the whole printf. */
fn append_b_argument(String &out, const String &arg,
                     bool should_use_bash_escapes) throws -> bool
{
  for (usize i = 0; i < arg.length(); i++) {
    if (arg[i] != '\\' || i + 1 >= arg.length()) {
      out += arg[i];
      continue;
    }
    let const e = arg[i + 1];
    if (e == 'c') return true;
    if (e == 'x' && i + 2 < arg.length() &&
        utils::hex_digit_value(arg[i + 2]).has_value())
    {
      usize digit_index = i + 2;
      i32 value = 0;
      usize digit_count = 0;
      while (digit_count < 2 && digit_index < arg.length() &&
             utils::hex_digit_value(arg[digit_index]).has_value())
      {
        value = value * 16 + *utils::hex_digit_value(arg[digit_index]);
        digit_index++;
        digit_count++;
      }
      out += static_cast<char>(value);
      i = digit_index - 1;
      continue;
    }
    if (e == '0' || (e >= '1' && e <= '7')) {
      /* A leading zero does not count toward the three octal digits. */
      usize digit_index = i + 1;
      if (arg[digit_index] == '0') digit_index++;
      i32 value = 0;
      usize digit_count = 0;
      while (digit_count < 3 && digit_index < arg.length() &&
             arg[digit_index] >= '0' && arg[digit_index] <= '7')
      {
        value = value * 8 + (arg[digit_index] - '0');
        digit_index++;
        digit_count++;
      }
      out += static_cast<char>(value);
      i = digit_index - 1;
      continue;
    }
    i++;
    append_simple_escape(out, e, should_use_bash_escapes);
  }
  return false;
}

fn is_q_safe_byte(char c) throws -> bool
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
         c == '/' || c == ':' || c == '%' || c == '+' || c == '@' || c == '=';
}

fn append_q_argument(String &out, const String &arg) throws -> void
{
  if (utils::append_ansi_c_quote_if_needed(out, arg.view())) return;

  for (usize i = 0; i < arg.count(); i++) {
    let const c = arg[i];
    if (!is_q_safe_byte(c)) out += '\\';
    out += c;
  }
}

fn report_invalid_number(ExecContext &ec, EvalContext &cxt, const String &arg,
                         bool is_hex, i32 &exit_status,
                         Allocator allocator) throws -> void
{
  let message = String{allocator, arg.view()};
  message += is_hex ? ": invalid hex number" : ": invalid number";
  report_soft_builtin_error(ec, cxt, message.view());
  exit_status = 1;
}

fn report_out_of_range(ExecContext &ec, EvalContext &cxt, const String &arg,
                       i32 &exit_status, Allocator allocator) throws -> void
{
  let message = String{allocator, arg.view()};
  message += ": Numerical result out of range";
  report_soft_builtin_error(ec, cxt, message.view());
  exit_status = 1;
}

fn append_conversion(String &out, const String &spec, char conv,
                     const String &arg, bool is_missing_argument,
                     ExecContext &ec, EvalContext &cxt, i32 &exit_status,
                     Allocator allocator) throws -> void
{
  char buffer[256];

  /* A conversion whose result is wider than the stack buffer is rewritten into
     a heap buffer sized from the length snprintf reports. */
  let const do_append_formatted = [&](const char *format, auto value) throws {
    const int needed = std::snprintf(buffer, sizeof(buffer), format, value);
    if (needed >= 0 && static_cast<usize>(needed) < sizeof(buffer)) {
      out += buffer;
    } else if (needed > 0) {
      let const size = static_cast<usize>(needed) + 1;
      char *const heap_buffer = static_cast<char *>(std::malloc(size));
      if (heap_buffer != nullptr) {
        std::snprintf(heap_buffer, size, format, value);
        out += StringView{heap_buffer, static_cast<usize>(needed)};
        std::free(heap_buffer);
      }
    }
  };

  switch (conv) {
  case 'q': append_q_argument(out, arg); break;
  case 's': {
    if (spec == "%") {
      /* A plain %s stops at an embedded NUL the way snprintf on the c_str
         does, so the output matches the reference shell. */
      let const value = arg.view();
      let const printed = value.find_character('\0').value_or(value.length);
      out.append(value.substring_of_length(0, printed));
    } else {
      String with_s = spec.clone();
      with_s.push('s');
      do_append_formatted(with_s.c_str(), arg.c_str());
    }
  } break;
  case 'c': {
    if (spec == "%") {
      out += arg.is_empty() ? '\0' : arg[0];
    } else {
      String with_c = spec.clone();
      with_c.push('c');
      do_append_formatted(with_c.c_str(), arg.is_empty() ? '\0' : arg[0]);
    }
  } break;
  case 'd':
  case 'i': {
    let const number = parse_printf_number(arg);
    if (!number.is_valid && !is_missing_argument)
      report_invalid_number(ec, cxt, arg, number.is_hex, exit_status,
                            allocator);
    else if (number.is_out_of_range && !is_missing_argument)
      report_out_of_range(ec, cxt, arg, exit_status, allocator);
    let const with_ll = spec + "lld";
    do_append_formatted(with_ll.c_str(), static_cast<long long>(number.value));
  } break;
  case 'x':
  case 'X':
  case 'o':
  case 'u': {
    let const number = parse_printf_number(arg);
    if (!number.is_valid && !is_missing_argument)
      report_invalid_number(ec, cxt, arg, number.is_hex, exit_status,
                            allocator);
    else if (number.is_out_of_range && !is_missing_argument)
      report_out_of_range(ec, cxt, arg, exit_status, allocator);
    String with_ll = spec + "ll";
    with_ll.push(conv);
    do_append_formatted(with_ll.c_str(),
                        static_cast<unsigned long long>(number.value));
  } break;
  case 'f':
  case 'e':
  case 'E':
  case 'g':
  case 'G': {
    String with_conv = spec.clone();
    with_conv.push(conv);
    let const value = std::strtod(arg.c_str(), nullptr);
    do_append_formatted(with_conv.c_str(), value);
  } break;
  default:
    out += spec;
    out += conv;
    break;
  }
}

} /* namespace */

Printf::Printf() = default;

pure fn Printf::kind() const wontthrow -> Builtin::Kind { return Kind::Printf; }

fn Printf::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help")
    SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (ec.args().count() < 2) return 0;

  LOG(All, "printf formatting %zu arguments", ec.args().count() - 1);

  /* bash printf -v NAME stores the result in NAME, riding every mood but POSIX.
   */
  usize format_index = 1;
  Maybe<String> store_variable;
  if (cxt.bash_additions_enabled() && ec.args()[1] == "-v" &&
      ec.args().count() >= 3)
  {
    store_variable = ec.args()[2];
    format_index = 3;
  }

  if (format_index < ec.args().count() && ec.args()[format_index] == "--") {
    format_index++;
  }

  if (format_index >= ec.args().count()) return 0;

  let const &args = ec.args();
  let const &fmt = args[format_index];
  let const operand_base = format_index + 1;
  let const operand_count = args.count() - operand_base;
  let const empty_operand = String{cxt.scratch_allocator()};
  let const do_operand_at = [&](usize index) wontthrow -> const String & {
    return index < operand_count ? args[operand_base + index] : empty_operand;
  };

  let out = String{cxt.scratch_allocator()};
  i32 exit_status = 0;
  usize operand_index = 0;
  bool has_consumed_a_conversion = false;
  bool should_stop = false;

  let const do_consume_star = [&](String &spec) throws {
    char number_text[24];
    spec.append(utils::int_to_text_into(
        parse_printf_integer(do_operand_at(operand_index)), number_text,
        sizeof(number_text)));
    operand_index++;
    has_consumed_a_conversion = true;
  };

  let const should_use_bash_escapes = !cxt.is_posix_mode();

  do {
    has_consumed_a_conversion = false;
    for (usize i = 0; i < fmt.length(); i++) {
      if (fmt[i] == '\\' && i + 1 < fmt.length()) {
        i++;
        append_escape(out, fmt, i, should_use_bash_escapes);
        continue;
      }
      if (fmt[i] != '%') {
        out += fmt[i];
        continue;
      }

      String spec{cxt.scratch_allocator(), "%"};
      i++;

      while (i < fmt.length() && std::strchr("-+ 0#", fmt[i]) != nullptr)
        spec.push(fmt[i++]);
      if (i < fmt.length() && fmt[i] == '*') {
        do_consume_star(spec);
        i++;
      } else {
        while (i < fmt.length() && fmt[i] >= '0' && fmt[i] <= '9')
          spec.push(fmt[i++]);
      }
      if (i < fmt.length() && fmt[i] == '.') {
        spec.push(fmt[i++]);
        if (i < fmt.length() && fmt[i] == '*') {
          do_consume_star(spec);
          i++;
        } else {
          while (i < fmt.length() && fmt[i] >= '0' && fmt[i] <= '9')
            spec.push(fmt[i++]);
        }
      }

      /* bash %(datefmt)T formats a time. The format sits in parentheses where
         the conversion letter would be, followed by T, and the operand is the
         epoch seconds, -1 for now and -2 for the shell start. */
      if (cxt.bash_additions_enabled() && i < fmt.length() && fmt[i] == '(') {
        usize close = i + 1;
        while (close < fmt.length() && fmt[close] != ')')
          close++;
        if (close + 1 < fmt.length() && fmt[close + 1] == 'T') {
          let const date_format =
              fmt.view().substring_of_length(i + 1, close - i - 1);
          let const has_operand = operand_index < operand_count;
          let const epoch =
              has_operand ? parse_printf_integer(do_operand_at(operand_index))
                          : -1;
          let const formatted = os::format_local_time(date_format, epoch);
          if (spec == "%")
            out += formatted;
          else
            append_conversion(out, spec, 's', formatted, false, ec, cxt,
                              exit_status, cxt.scratch_allocator());
          operand_index++;
          has_consumed_a_conversion = true;
          i = close + 1;
          continue;
        }
      }
      if (i >= fmt.length()) {
        out += spec;
        break;
      }

      let const conv = fmt[i];
      if (conv == '%') {
        out += '%';
        continue;
      }
      /* A '(' that did not open a valid %(fmt)T is emitted literally and
         consumes no operand. */
      if (conv == '(') {
        out += spec;
        out += '(';
        continue;
      }

      let const is_missing_argument = operand_index >= operand_count;
      let const &arg = do_operand_at(operand_index);
      if (conv == 'b') {
        should_stop = append_b_argument(out, arg, should_use_bash_escapes);
        operand_index++;
        has_consumed_a_conversion = true;
        if (should_stop) break;
        continue;
      }
      append_conversion(out, spec, conv, arg, is_missing_argument, ec, cxt,
                        exit_status, cxt.scratch_allocator());
      operand_index++;
      has_consumed_a_conversion = true;
    }
  } while (!should_stop && operand_index < operand_count &&
           has_consumed_a_conversion);

  if (store_variable.has_value()) {
    let const target = store_variable->view();
    let const open_bracket = target.find_character('[');
    if (open_bracket.has_value() && *open_bracket > 0 &&
        target.length >= *open_bracket + 2 && target[target.length - 1] == ']')
    {
      let const array_name = target.substring_of_length(0, *open_bracket);
      let const subscript = target.substring_of_length(
          *open_bracket + 1, target.length - *open_bracket - 2);
      cxt.assign_array_element(array_name, subscript, out.view(), false);
      return exit_status;
    }
    cxt.set_shell_variable(target, out.view());
    return exit_status;
  }

  ec.print_to_stdout(out);
  return exit_status;
}

} /* namespace koshka */
