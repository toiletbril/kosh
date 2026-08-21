#include "Common.hpp"
#include "Debug.hpp"
#include "ErrorOr.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "EvalArithmeticInternal.hpp"
#include "Lexer.hpp"
#include "Maybe.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace {

#if T__HAS_GCC_EXTENSIONS
using wide_int = i128;
using wide_uint = u128;
#else
using wide_int = i64;
using wide_uint = u64;
#endif

static pure fn parse_wide_operand(StringView text) wontthrow -> wide_int
{
  let body = text;
  bool is_negative = false;
  if (body.length > 0 && (body[0] == '+' || body[0] == '-')) {
    is_negative = body[0] == '-';
    body = body.substring(1);
  }

  let const detected = arithmetic_internal::detect_radix_prefix(body);
  let const radix = detected.radix;
  usize i = detected.prefix_length;

  wide_uint value = 0;
  for (; i < body.length; i++) {
    let const c = body[i];
    u32 digit;
    if (c >= '0' && c <= '9')
      digit = static_cast<u32>(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = static_cast<u32>(c - 'a') + 10;
    else if (c >= 'A' && c <= 'F')
      digit = static_cast<u32>(c - 'A') + 10;
    else
      break;
    if (digit >= radix) break;
    value = value * radix + digit;
  }

  return static_cast<wide_int>(is_negative ? wide_uint{0} - value : value);
}

static pure fn wide_divide(wide_int lhs, wide_int rhs) wontthrow -> wide_int
{
  constexpr wide_int WIDE_MINIMUM = static_cast<wide_int>(wide_uint{1} << 127u);
  if (lhs == WIDE_MINIMUM && rhs == -1) {
    return WIDE_MINIMUM;
  }
  return lhs / rhs;
}

static pure fn wide_modulo(wide_int lhs, wide_int rhs) wontthrow -> wide_int
{
  constexpr wide_int WIDE_MINIMUM = static_cast<wide_int>(wide_uint{1} << 127u);
  if (lhs == WIDE_MINIMUM && rhs == -1) {
    return 0;
  }
  return lhs % rhs;
}

static fn lex_wide_number(StringView from, wide_int *out_value) throws -> usize
{
  usize consumed;
  if (from.length >= 2 && from[0] == '0' && (from[1] == 'x' || from[1] == 'X'))
    consumed =
        2 + arithmetic_internal::count_leading_digits(from.substring(2), 16);
  else if (from.length >= 2 && from[0] == '0' &&
           (from[1] == 'b' || from[1] == 'B'))
    consumed =
        2 + arithmetic_internal::count_leading_digits(from.substring(2), 2);
  else if (from.length >= 1 && from[0] == '0')
    consumed = arithmetic_internal::count_leading_digits(from, 8);
  else
    consumed = arithmetic_internal::count_leading_digits(from, 10);
  if (consumed == 0) consumed = 1;
  *out_value = parse_wide_operand(from.substring_of_length(0, consumed));
  return consumed;
}

static fn format_wide(wide_int value) throws -> String
{
  let const is_negative = value < 0;
  /* Negating the minimum would overflow, so the magnitude is taken in the
     unsigned domain where the wrap is defined. */
  wide_uint magnitude =
      is_negative ? static_cast<wide_uint>(0) - static_cast<wide_uint>(value)
                  : static_cast<wide_uint>(value);
  char buffer[64];
  usize position = sizeof(buffer);
  do {
    buffer[--position] =
        static_cast<char>('0' + static_cast<int>(magnitude % 10));
    magnitude /= 10;
  } while (magnitude != 0);

  String text{heap_allocator()};
  if (is_negative) text.push('-');
  text.append(StringView{buffer + position, sizeof(buffer) - position});
  return text;
}

static fn evaluate_wide_expression(EvalContext *context, StringView expression,
                                   usize depth) throws -> wide_int;

/* A recursive-descent evaluator over 128-bit integers for the calc builtin.
   A variable read evaluates the stored expression text lazily. */
class WideArithmeticParser
{
public:
  EvalContext *context;
  StringView source;
  usize pos;
  usize depth{0};
  /* A nested parser reads a stored formula, so it reports unlocated. */
  bool is_top_level{false};
  /* The untaken arm of a ternary parses to advance the cursor but takes no
     side effect and raises no fault. */
  bool m_is_skipping{false};
  static constexpr usize MAX_DEPTH = 128;

  [[noreturn]] cold fn fail(StringView message, StringView note = {}) throws
      -> void
  {
    const SourceLocation location{pos, 1};
    if (note.is_empty()) throw ErrorWithLocation{location, message};
    throw ErrorWithLocationAndDetails{location, message, note};
  }

  fn skip_spaces() wontthrow -> void
  {
    arithmetic_internal::skip_spaces(source, pos);
  }

  fn starts_with(StringView op) wontthrow -> bool
  {
    return arithmetic_internal::starts_with(source, pos, op);
  }

  fn consume(StringView op) wontthrow -> bool
  {
    return arithmetic_internal::consume(source, pos, op);
  }

  fn read_variable(StringView name, usize name_position) const throws
      -> wide_int
  {
    ASSERT(context != nullptr);

    String value{context->scratch_allocator()};
    bool was_found = false;
    if (let const *stored = context->lookup_shell_variable(name);
        stored != nullptr)
    {
      value = String{stored->view()};
      was_found = true;
    } else if (let const fetched = context->get_variable_value(name);
               fetched.has_value())
    {
      value = String{fetched->view()};
      was_found = true;
    }

    if (!was_found) {
      if (m_is_skipping) return 0;
      /* calc treats an unset variable as an error, $((...)) reads it as zero.
       */
      let message = "The variable '" + String{name} + "' is not set";
      if (is_top_level)
        throw ErrorWithLocation{
            SourceLocation{name_position, name.length},
            steal(message)
        };
      throw Error{steal(message)};
    }

    if (value.count() == 0) return 0;

    /* A reference cycle such as x=x grows the depth without end, the cap
       reports instead. */
    if (depth >= MAX_DEPTH) {
      let message = "The variable '" + String{name} + "' refers to itself";
      if (is_top_level)
        throw ErrorWithLocation{
            SourceLocation{name_position, name.length},
            steal(message)
        };
      throw Error{steal(message)};
    }

    return evaluate_wide_expression(context, value.view(), depth + 1);
  }

  static fn wrap_add(wide_int a, wide_int b) wontthrow -> wide_int
  {
    return static_cast<wide_int>(static_cast<wide_uint>(a) +
                                 static_cast<wide_uint>(b));
  }
  static fn wrap_sub(wide_int a, wide_int b) wontthrow -> wide_int
  {
    return static_cast<wide_int>(static_cast<wide_uint>(a) -
                                 static_cast<wide_uint>(b));
  }
  static fn wrap_mul(wide_int a, wide_int b) wontthrow -> wide_int
  {
    return static_cast<wide_int>(static_cast<wide_uint>(a) *
                                 static_cast<wide_uint>(b));
  }
  fn wrap_power(wide_int base, wide_int exponent) throws -> wide_int
  {
    if (exponent < 0) {
      if (m_is_skipping) return 0;
      fail("Exponent less than 0", "'**' requires a non-negative exponent");
    }
    wide_uint result = 1;
    wide_uint factor = static_cast<wide_uint>(base);
    wide_uint remaining = static_cast<wide_uint>(exponent);
    while (remaining > 0) {
      if ((remaining & 1u) != 0) result *= factor;
      factor *= factor;
      remaining >>= 1;
    }
    return static_cast<wide_int>(result);
  }

  fn parse() throws -> wide_int
  {
    skip_spaces();
    if (pos == source.length) return 0;
    let const result = parse_comma();
    skip_spaces();
    if (pos != source.length) {
      throw ErrorWithLocationAndDetails{
          SourceLocation{pos, source.length - pos},
          "Unexpected '" + String{source.substring(pos)}
          +
              "' after the expression",
          "An operator is missing between two values"
      };
    }
    return result;
  }

  fn parse_comma() throws -> wide_int
  {
    wide_int result = parse_assignment();
    while (consume(","))
      result = parse_assignment();
    return result;
  }

  /* The variable binds to its right-side expression text so a later read
     re-evaluates it against the current context. */
  fn write_variable(StringView name, StringView expression_text) const throws
      -> void
  {
    if (m_is_skipping) return;
    ASSERT(context != nullptr);
    context->set_shell_variable(name, expression_text);
  }

  /* The flag is saved and restored so a nested skip inside an already-skipped
     region stays skipped. */
  fn parse_skipped(wide_int (WideArithmeticParser::*parse_branch)()) throws
      -> wide_int
  {
    let const was_skipping = m_is_skipping;
    m_is_skipping = true;
    defer { m_is_skipping = was_skipping; };
    return (this->*parse_branch)();
  }

  fn parse_assignment() throws -> wide_int
  {
    let const save = pos;
    skip_spaces();
    if (pos < source.length && lexer::is_variable_name_start(source[pos])) {
      let const name_start = pos;
      while (pos < source.length && lexer::is_variable_name(source[pos]))
        pos++;
      let const name = source.substring_of_length(name_start, pos - name_start);

      skip_spaces();
      if (starts_with("=") && !starts_with("==")) {
        consume("=");
        skip_spaces();
        let const right_start = pos;
        let const right_value = parse_assignment();
        write_variable(
            name, source.substring_of_length(right_start, pos - right_start));
        return right_value;
      }
      pos = save;
    }
    return parse_ternary();
  }

  fn parse_ternary() throws -> wide_int
  {
    let const condition = parse_binary(1);
    if (consume("?")) {
      if (condition != 0) {
        let const if_true = parse_assignment();
        if (!consume(":"))
          fail("Expected ':' in a conditional", "A '?' needs a matching ':'");
        let const if_false =
            parse_skipped(&WideArithmeticParser::parse_ternary);
        unused(if_false);
        return if_true;
      }
      let const if_true =
          parse_skipped(&WideArithmeticParser::parse_assignment);
      unused(if_true);
      if (!consume(":"))
        fail("Expected ':' in a conditional", "A '?' needs a matching ':'");
      return parse_ternary();
    }
    return condition;
  }

  struct binary_operator
  {
    char kind;
    u8 precedence;
    u8 length;
  };

  fn peek_binary_operator() wontthrow -> binary_operator
  {
    skip_spaces();
    if (pos >= source.length) return {0, 0, 0};
    let const first_byte = source[pos];
    let const second_byte = pos + 1 < source.length ? source[pos + 1] : '\0';
    switch (first_byte) {
    case '*':
      return second_byte == '*' ? binary_operator{'P', 11, 2}
                                : binary_operator{'*', 10, 1};
    case '/': return {'/', 10, 1};
    case '%': return {'%', 10, 1};
    case '+': return {'+', 9, 1};
    case '-': return {'-', 9, 1};
    case '<':
      if (second_byte == '<') return {'L', 8, 2};
      if (second_byte == '=') return {'l', 7, 2};
      return {'<', 7, 1};
    case '>':
      if (second_byte == '>') return {'R', 8, 2};
      if (second_byte == '=') return {'g', 7, 2};
      return {'>', 7, 1};
    case '=':
      return second_byte == '=' ? binary_operator{'e', 6, 2}
                                : binary_operator{0, 0, 0};
    case '!':
      return second_byte == '=' ? binary_operator{'n', 6, 2}
                                : binary_operator{0, 0, 0};
    case '&':
      return second_byte == '&' ? binary_operator{'A', 2, 2}
                                : binary_operator{'&', 5, 1};
    case '^': return {'^', 4, 1};
    case '|':
      return second_byte == '|' ? binary_operator{'O', 1, 2}
                                : binary_operator{'|', 3, 1};
    default: return {0, 0, 0};
    }
  }

  fn parse_binary(u8 min_precedence) throws -> wide_int
  {
    let lhs = parse_unary();
    loop
    {
      let const op = peek_binary_operator();
      if (op.precedence < min_precedence) return lhs;
      pos += op.length;
      let const rhs =
          parse_binary(op.kind == 'P' ? op.precedence : op.precedence + 1);
      switch (op.kind) {
      case 'P': lhs = wrap_power(lhs, rhs); break;
      case '*': lhs = wrap_mul(lhs, rhs); break;
      case '/':
        if (rhs == 0) {
          if (m_is_skipping) {
            lhs = 0;
            break;
          }
          fail("Division by zero", "The right operand evaluated to 0");
        }
        lhs = wide_divide(lhs, rhs);
        break;
      case '%':
        if (rhs == 0) {
          if (m_is_skipping) {
            lhs = 0;
            break;
          }
          fail("Division by zero", "The right operand evaluated to 0");
        }
        lhs = wide_modulo(lhs, rhs);
        break;
      case '+': lhs = wrap_add(lhs, rhs); break;
      case '-': lhs = wrap_sub(lhs, rhs); break;
      case 'L':
        lhs = static_cast<wide_int>(static_cast<wide_uint>(lhs)
                                    << (static_cast<wide_uint>(rhs) & 127u));
        break;
      case 'R': lhs = lhs >> (static_cast<wide_uint>(rhs) & 127u); break;
      case '<': lhs = lhs < rhs ? 1 : 0; break;
      case 'l': lhs = lhs <= rhs ? 1 : 0; break;
      case '>': lhs = lhs > rhs ? 1 : 0; break;
      case 'g': lhs = lhs >= rhs ? 1 : 0; break;
      case 'e': lhs = lhs == rhs ? 1 : 0; break;
      case 'n': lhs = lhs != rhs ? 1 : 0; break;
      case '&': lhs = lhs & rhs; break;
      case '^': lhs = lhs ^ rhs; break;
      case '|': lhs = lhs | rhs; break;
      case 'A': lhs = (lhs != 0 && rhs != 0) ? 1 : 0; break;
      case 'O': lhs = (lhs != 0 || rhs != 0) ? 1 : 0; break;
      default:
        unreachable("the wide arithmetic parser received invalid binary "
                    "operator '%c'",
                    op.kind);
      }
    }
  }

  fn parse_unary() throws -> wide_int
  {
    skip_spaces();
    let const first = pos < source.length ? source[pos] : '\0';
    if (first == '+') {
      pos++;
      return parse_unary();
    }
    if (first == '-') {
      pos++;
      return wrap_sub(0, parse_unary());
    }
    if (first == '!') {
      pos++;
      return parse_unary() == 0 ? 1 : 0;
    }
    if (first == '~') {
      pos++;
      return ~parse_unary();
    }
    return parse_primary();
  }

  fn parse_primary() throws -> wide_int
  {
    depth++;
    defer { depth--; };
    if (depth > MAX_DEPTH)
      fail("Expression nested too deeply",
           "A variable may reference itself, like `x=x`");

    skip_spaces();
    if (consume("(")) {
      let const value = parse_comma();
      if (!consume(")")) fail("Expected ')'", "An opening '(' is unmatched");
      return value;
    }
    if (pos < source.length && lexer::is_number(source[pos])) {
      wide_int value = 0;
      pos += lex_wide_number(source.substring(pos), &value);
      return value;
    }
    if (pos < source.length && lexer::is_variable_name_start(source[pos])) {
      let const name_start = pos;
      while (pos < source.length && lexer::is_variable_name(source[pos]))
        pos++;
      return read_variable(
          source.substring_of_length(name_start, pos - name_start), name_start);
    }
    if (pos >= source.length)
      fail("Unfinished expression", "An operand is missing");
    fail("Unexpected character in the arithmetic expression",
         "This is not a valid operator or operand");
  }
};

static fn evaluate_wide_expression(EvalContext *context, StringView expression,
                                   usize depth) throws -> wide_int
{
  WideArithmeticParser sub{context, expression, 0};
  sub.depth = depth;
  return sub.parse();
}

} /* namespace */

fn EvalContext::evaluate_arithmetic_wide(StringView expression,
                                         bool &out_nonzero) throws -> String
{
  String expanded{scratch_allocator()};
  StringView to_parse = expression;
  if (expression.find_character('$').has_value() ||
      expression.find_character('`').has_value())
  {
    expanded = expand_modifier_word(expression);
    to_parse = expanded.view();
  }

  WideArithmeticParser parser{this, to_parse, 0};
  parser.is_top_level = true;
  let const value = parser.parse();

  /* The default mood prints the full 128-bit value, the bash and posix moods
     wrap it to 64 bits. */
  if (mood() != mimic_mood::Default) {
    let const wrapped = static_cast<i64>(value);
    out_nonzero = wrapped != 0;
    return String::from(wrapped, heap_allocator());
  }

  out_nonzero = value != 0;
  return format_wide(value);
}

} /* namespace koshka */
