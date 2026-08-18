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

pure fn fold_leading_digits(StringView text, u32 radix) wontthrow -> u64
{
  u64 magnitude = 0;

  for (usize length = 0; length < text.length; length++) {
    let const digit = utils::hex_digit_value(text[length]);
    if (!digit.has_value() || *digit >= radix) break;

    magnitude = magnitude * radix + *digit;
  }

  return magnitude;
}

pure fn parse_arithmetic_operand(StringView text) wontthrow -> i64
{
  let body = text;
  let is_negative = false;
  if (body.length > 0 && (body[0] == '+' || body[0] == '-')) {
    is_negative = body[0] == '-';
    body = body.substring(1);
  }

  let const detected = arithmetic_internal::detect_radix_prefix(body);
  let const magnitude = fold_leading_digits(
      body.substring(detected.prefix_length), detected.radix);

  return static_cast<i64>(is_negative ? -magnitude : magnitude);
}

pure alwaysinline fn try_parse_single_integer_literal(StringView text) wontthrow
    -> Maybe<i64>
{
  usize i = 0;
  let is_negative = false;
  if (i < text.length && (text[i] == '+' || text[i] == '-')) {
    is_negative = text[i] == '-';
    i++;
  }
  let const detected =
      arithmetic_internal::detect_radix_prefix(text.substring(i));
  i += detected.prefix_length;
  let const digits = text.substring(i);
  let const digit_count =
      arithmetic_internal::count_leading_digits(digits, detected.radix);
  if (digit_count == 0 || i + digit_count != text.length) {
    return None;
  }

  let const magnitude = fold_leading_digits(digits, detected.radix);
  return static_cast<i64>(is_negative ? -magnitude : magnitude);
}

/* The add, subtract, and multiply run in u64 where overflow is defined, a
   direct i64 overflow is undefined and trips UBSan in the dbg build. */
pure fn arithmetic_add(i64 lhs, i64 rhs) wontthrow -> i64
{
  return static_cast<i64>(static_cast<u64>(lhs) + static_cast<u64>(rhs));
}

pure fn arithmetic_subtract(i64 lhs, i64 rhs) wontthrow -> i64
{
  return static_cast<i64>(static_cast<u64>(lhs) - static_cast<u64>(rhs));
}

pure fn arithmetic_multiply(i64 lhs, i64 rhs) wontthrow -> i64
{
  return static_cast<i64>(static_cast<u64>(lhs) * static_cast<u64>(rhs));
}

/* Runs in u64 so the result wraps in 64 bits. The caller rejects a negative
   exponent. */
pure fn arithmetic_power(i64 base, i64 exponent) wontthrow -> i64
{
  let result = static_cast<u64>(1);
  let factor = static_cast<u64>(base);
  let remaining = static_cast<u64>(exponent);
  while (remaining > 0) {
    if ((remaining & 1u) != 0) result *= factor;
    factor *= factor;
    remaining >>= 1;
  }
  return static_cast<i64>(result);
}

/* INT64_MIN / -1 and INT64_MIN % -1 overflow the signed result and trap on
   x86, so the two's-complement wrap of INT64_MIN and 0 is returned directly. */
pure fn arithmetic_divide(i64 lhs, i64 rhs) wontthrow -> i64
{
  if (lhs == INT64_MIN && rhs == -1) {
    return INT64_MIN;
  }
  return lhs / rhs;
}

pure fn arithmetic_modulo(i64 lhs, i64 rhs) wontthrow -> i64
{
  if (lhs == INT64_MIN && rhs == -1) {
    return 0;
  }
  return lhs % rhs;
}

/* The count is masked to the low 6 bits the way dash does, the shift runs in
   u64 where a shift below the width is defined. */
pure fn arithmetic_shift_left(i64 lhs, i64 rhs) wontthrow -> i64
{
  let const count = static_cast<u64>(rhs) & 63u;
  return static_cast<i64>(static_cast<u64>(lhs) << count);
}

pure fn arithmetic_shift_right(i64 lhs, i64 rhs) wontthrow -> i64
{
  let const count = static_cast<u64>(rhs) & 63u;
  let const is_negative = lhs < 0;
  let value = static_cast<u64>(lhs) >> count;
  if (is_negative && count > 0) {
    value |= ~(~static_cast<u64>(0) >> count);
  }
  return static_cast<i64>(value);
}

static fn lex_arith_number(StringView from, i64 *out_value) throws -> usize;

hot static fn arith_apply_binop(char kind, i64 lhs, i64 rhs) throws -> i64;

/* A recursive-descent evaluator for $((...)) following C operator precedence.
 */
class ArithmeticParser
{
public:
  /* Null only on the analyze-time constant fold, where no variable read and no
     assignment path that dereferences the context is reached. */
  EvalContext *context;
  StringView source;
  usize pos;

  usize depth{0};
  static constexpr usize MAX_DEPTH = 512;

  /* The dead operand of a short-circuited || or && and the untaken ternary arm
     are parsed to consume their tokens, this flag makes their assignments skip
     the store. */
  bool m_is_skipping{false};

  Maybe<SourceLocation> precise_base{};

  [[noreturn]] cold fn fail(StringView message, StringView note = {}) throws
      -> void
  {
    if (precise_base.has_value()) {
      let const error_position = pos < source.length ? pos
                                 : source.is_empty() ? 0
                                                     : source.length - 1;
      const SourceLocation location{precise_base->position + error_position, 1,
                                    precise_base->source_name_index};
      if (note.is_empty()) throw ErrorWithLocation{location, message};
      throw ErrorWithLocationAndDetails{location, message, note};
    }

    if (note.is_empty()) throw Error{String{message}};
    throw ErrorWithDetails{message, note};
  }

  [[noreturn]] cold fn fail_span(usize start_position, usize end_position,
                                 StringView message,
                                 StringView note = {}) throws -> void
  {
    while (end_position > start_position && (source[end_position - 1] == ' ' ||
                                             source[end_position - 1] == '\t' ||
                                             source[end_position - 1] == '\n' ||
                                             source[end_position - 1] == '\r'))
    {
      end_position--;
    }

    if (precise_base.has_value()) {
      const SourceLocation location{precise_base->position + start_position,
                                    end_position - start_position,
                                    precise_base->source_name_index};
      if (note.is_empty()) throw ErrorWithLocation{location, message};
      throw ErrorWithLocationAndDetails{location, message, note};
    }

    if (note.is_empty()) throw Error{String{message}};
    throw ErrorWithDetails{message, note};
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

  fn read_variable_value(StringView name) throws -> i64
  {
    ASSERT(context != nullptr);
    if (let const *stored = context->lookup_shell_variable(name);
        stored != nullptr)
    {
      return evaluate_operand_value(stored->view());
    }

    let const value = context->get_variable_value(name);
    if (!value.has_value()) {
      /* An unset name reports under the strict mood, a skipped ternary branch
         never does. */
      if (!m_is_skipping) context->report_unset_reference(name);
      return 0;
    }
    return evaluate_operand_value(value->view());
  }

  fn evaluate_operand_value(StringView value) throws -> i64
  {
    if (value.is_empty()) return 0;
    if (let const literal = try_parse_single_integer_literal(value);
        literal.has_value())
      return literal.value();

    if (depth >= MAX_DEPTH)
      fail("The variable value recurses too deeply",
           "A variable value refers back to itself");

    ArithmeticParser nested{context, value, 0, depth + 1, m_is_skipping};
    return nested.parse();
  }

  fn read_lvalue_name() wontthrow -> StringView
  {
    skip_spaces();
    if (pos >= source.length || !lexer::is_variable_name_start(source[pos])) {
      return StringView{};
    }
    let const name_start = pos;
    while (pos < source.length && lexer::is_variable_name(source[pos]))
      pos++;
    return source.substring_of_length(name_start, pos - name_start);
  }

  fn write_variable(StringView name, i64 value) const throws -> void
  {
    if (m_is_skipping) return;
    ASSERT(context != nullptr);
    char buffer[24];
    context->set_shell_variable(
        name, utils::int_to_text_into(value, buffer, sizeof(buffer)));
  }

  struct lvalue
  {
    StringView name;
    Maybe<StringView> subscript;
  };

  /* Nested brackets are balanced so a[b[0]] reads the whole inner expression.
   */
  fn read_optional_subscript() throws -> Maybe<StringView>
  {
    if (pos >= source.length || source[pos] != '[') {
      return None;
    }
    pos++;
    let const inner_start = pos;
    usize depth = 1;
    while (pos < source.length && depth > 0) {
      if (source[pos] == '[')
        depth++;
      else if (source[pos] == ']' && --depth == 0)
        break;
      pos++;
    }
    if (depth != 0)
      fail("Expected ']' after an array subscript",
           "The subscript '[' was never closed");

    let const subscript =
        source.substring_of_length(inner_start, pos - inner_start);
    pos++;
    return subscript;
  }

  fn read_lvalue() throws -> lvalue
  {
    let const name = read_lvalue_name();
    if (name.is_empty()) return lvalue{name, None};
    return lvalue{name, read_optional_subscript()};
  }

  fn read_lvalue_value(const lvalue &target) throws -> i64
  {
    if (target.subscript.has_value()) {
      ASSERT(context != nullptr);
      let const value = context->read_array_element_arithmetic_text(
          target.name, *target.subscript);
      return evaluate_operand_value(value.view());
    }
    return read_variable_value(target.name);
  }

  fn write_lvalue(const lvalue &target, i64 value) const throws -> void
  {
    if (m_is_skipping) return;
    if (target.subscript.has_value()) {
      ASSERT(context != nullptr);
      char buffer[24];
      context->assign_array_element(
          target.name, *target.subscript,
          utils::int_to_text_into(value, buffer, sizeof(buffer)), false);
      return;
    }
    write_variable(target.name, value);
  }

  fn prefix_step(i64 delta, usize operator_position) throws -> i64
  {
    const lvalue target = read_lvalue();
    if (target.name.is_empty())
      fail_span(operator_position, operator_position + 2,
                "Expected a variable after '++' or '--'",
                "'++' and '--' step a variable, not a value");
    const i64 updated = arithmetic_add(read_lvalue_value(target), delta);
    write_lvalue(target, updated);
    return updated;
  }

  fn postfix_step(const lvalue &target, i64 delta) throws -> i64
  {
    const i64 original = read_lvalue_value(target);
    write_lvalue(target, arithmetic_add(original, delta));
    return original;
  }

  fn parse() throws -> i64
  {
    skip_spaces();
    if (pos == source.length) return 0;
    let const result = parse_comma();
    skip_spaces();
    if (pos != source.length) {
      fail("Unexpected '" + String{source.substring(pos)} +
               "' after the expression",
           "An operator is missing between two values");
    }
    return result;
  }

  fn parse_comma() throws -> i64
  {
    i64 result = parse_assignment();
    while (consume(","))
      result = parse_assignment();
    return result;
  }

  fn parse_assignment() throws -> i64
  {
    /* Try a bare name on the left and rewind when no assignment operator
       follows it. */
    let const save = pos;
    skip_spaces();
    if (pos < source.length && lexer::is_variable_name_start(source[pos])) {
      let const name_start = pos;
      while (pos < source.length && lexer::is_variable_name(source[pos]))
        pos++;
      let const name = source.substring_of_length(name_start, pos - name_start);
      const lvalue target{name, read_optional_subscript()};

      struct compound_operator
      {
        StringView token;
        char kind;
      };
      static const compound_operator compound_operators[] = {
          {"<<=", 'L'},
          {">>=", 'R'},
          {"+=",  '+'},
          {"-=",  '-'},
          {"*=",  '*'},
          {"/=",  '/'},
          {"%=",  '%'},
          {"&=",  '&'},
          {"|=",  '|'},
          {"^=",  '^'},
      };
      skip_spaces();
      let const next_byte = pos < source.length ? source[pos] : '\0';
      if (next_byte == '<' || next_byte == '>' || next_byte == '+' ||
          next_byte == '-' || next_byte == '*' || next_byte == '/' ||
          next_byte == '%' || next_byte == '&' || next_byte == '|' ||
          next_byte == '^')
      {
        for (let const &[ op, kind ] : compound_operators) {
          if (consume(op)) {
            let const rhs = parse_assignment();
            let const result =
                arith_apply_binop(kind, read_lvalue_value(target), rhs);
            write_lvalue(target, result);
            return result;
          }
        }
      }
      if (starts_with("=") && !starts_with("==")) {
        consume("=");
        let const rhs = parse_assignment();
        write_lvalue(target, rhs);
        return rhs;
      }
      pos = save;
    }
    return parse_ternary();
  }

  /* The flag is saved and restored so a nested skip inside an already-skipped
     region stays skipped. */
  fn parse_skipped(i64 (ArithmeticParser::*parse_branch)()) throws -> i64
  {
    let const was_skipping = m_is_skipping;
    m_is_skipping = true;
    defer { m_is_skipping = was_skipping; };
    return (this->*parse_branch)();
  }

  fn parse_ternary() throws -> i64
  {
    let const condition = parse_binary(1);
    if (consume("?")) {
      if (condition != 0) {
        let const if_true = parse_assignment();
        if (!consume(":"))
          fail("Expected ':' in a conditional", "A '?' needs a matching ':'");
        let const if_false = parse_skipped(&ArithmeticParser::parse_ternary);
        unused(if_false);
        return if_true;
      }
      let const if_true = parse_skipped(&ArithmeticParser::parse_assignment);
      unused(if_true);
      if (!consume(":"))
        fail("Expected ':' in a conditional", "A '?' needs a matching ':'");
      return parse_ternary();
    }
    return condition;
  }

  /* Precedence runs 11 for the tightest ** to 1 for the loosest ||. Precedence
     0 means no binary operator opens here. */
  struct binary_operator
  {
    char kind;
    u8 precedence;
    u8 length;
  };

  /* A compound assignment suffix such as += or <<= answers no operator, the
     assignment level above the ladder owns those. */
  fn peek_binary_operator() wontthrow -> binary_operator
  {
    skip_spaces();
    if (pos >= source.length) return {0, 0, 0};
    let const first_byte = source[pos];
    let const second_byte = pos + 1 < source.length ? source[pos + 1] : '\0';
    let const third_byte = pos + 2 < source.length ? source[pos + 2] : '\0';
    switch (first_byte) {
    case '*':
      if (second_byte == '*') return {'P', 11, 2};
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'*', 10, 1};
    case '/':
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'/', 10, 1};
    case '%':
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'%', 10, 1};
    case '+':
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'+', 9, 1};
    case '-':
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'-', 9, 1};
    case '<':
      if (second_byte == '<')
        return third_byte == '=' ? binary_operator{0, 0, 0}
                                 : binary_operator{'L', 8, 2};
      if (second_byte == '=') return {'l', 7, 2};
      return {'<', 7, 1};
    case '>':
      if (second_byte == '>')
        return third_byte == '=' ? binary_operator{0, 0, 0}
                                 : binary_operator{'R', 8, 2};
      if (second_byte == '=') return {'g', 7, 2};
      return {'>', 7, 1};
    case '=':
      return second_byte == '=' ? binary_operator{'e', 6, 2}
                                : binary_operator{0, 0, 0};
    case '!':
      return second_byte == '=' ? binary_operator{'n', 6, 2}
                                : binary_operator{0, 0, 0};
    case '&':
      if (second_byte == '&') return {'A', 2, 2};
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'&', 5, 1};
    case '^':
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'^', 4, 1};
    case '|':
      if (second_byte == '|') return {'O', 1, 2};
      return second_byte == '=' ? binary_operator{0, 0, 0}
                                : binary_operator{'|', 3, 1};
    default: return {0, 0, 0};
    }
  }

  /* One precedence-climbing loop, one frame for a whole run of operators. The
     nine cascade levels showed up whole in the profile. */
  fn parse_binary(u8 min_precedence) throws -> i64
  {
    skip_spaces();
    let const lhs_start = pos;
    let lhs = parse_unary();
    loop
    {
      let const op = peek_binary_operator();
      if (op.precedence < min_precedence) return lhs;
      pos += op.length;

      if (op.kind == 'A' || op.kind == 'O') {
        let const lhs_decides = (op.kind == 'A') == (lhs == 0);
        i64 rhs = 0;
        if (lhs_decides) {
          let const was_skipping = m_is_skipping;
          m_is_skipping = true;
          defer { m_is_skipping = was_skipping; };
          rhs = parse_binary(op.precedence + 1);
        } else {
          rhs = parse_binary(op.precedence + 1);
        }
        lhs = op.kind == 'A' ? ((lhs != 0 && rhs != 0) ? 1 : 0)
                             : ((lhs != 0 || rhs != 0) ? 1 : 0);
        continue;
      }

      /* ** is right-associative so it re-enters at its own precedence. */
      let const rhs =
          parse_binary(op.kind == 'P' ? op.precedence : op.precedence + 1);
      switch (op.kind) {
      case 'P':
        if (rhs < 0) {
          if (m_is_skipping) {
            lhs = 0;
            break;
          }
          fail_span(lhs_start, pos, "Exponent less than 0",
                    "'**' requires a non-negative exponent");
        }
        lhs = arithmetic_power(lhs, rhs);
        break;
      case '/':
        if (rhs == 0) {
          if (m_is_skipping) {
            lhs = 0;
            break;
          }
          fail_span(lhs_start, pos, "Division by zero",
                    "The right operand evaluated to 0");
        }
        lhs = arithmetic_divide(lhs, rhs);
        break;
      case '%':
        if (rhs == 0) {
          if (m_is_skipping) {
            lhs = 0;
            break;
          }
          fail_span(lhs_start, pos, "Division by zero",
                    "The right operand evaluated to 0");
        }
        lhs = arithmetic_modulo(lhs, rhs);
        break;
      default: lhs = arith_apply_binop(op.kind, lhs, rhs); break;
      }
    }
  }

  fn parse_unary() throws -> i64
  {
    /* The doubled operators are checked before single + and - so a leading ++
       or -- is read as one prefix step. */
    skip_spaces();
    let const first = pos < source.length ? source[pos] : '\0';
    if (first == '+') {
      let const operator_position = pos;
      if (consume("++")) return prefix_step(1, operator_position);
      pos++;
      return parse_unary();
    }
    if (first == '-') {
      let const operator_position = pos;
      if (consume("--")) return prefix_step(-1, operator_position);
      pos++;
      return arithmetic_subtract(0, parse_unary());
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

  fn parse_primary() throws -> i64
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
      i64 value = 0;
      pos += lex_arith_number(source.substring(pos), &value);
      return value;
    }
    if (pos < source.length && lexer::is_variable_name_start(source[pos])) {
      const lvalue target = read_lvalue();
      if (consume("++")) return postfix_step(target, 1);
      if (consume("--")) return postfix_step(target, -1);
      return read_lvalue_value(target);
    }
    if (pos >= source.length)
      fail("Unfinished expression", "An operand is missing");
    fail("Unexpected character in the arithmetic expression",
         "This is not a valid operator or operand");
  }
};

static fn lex_arith_number(StringView from, i64 *out_value) throws -> usize
{
  if (let const base_length =
          arithmetic_internal::count_leading_digits(from, 10);
      base_length > 0 && base_length < from.length && from[base_length] == '#')
  {
    let const base =
        parse_arithmetic_operand(from.substring_of_length(0, base_length));
    if (base < 2 || base > 64) {
      throw ErrorWithDetails{"The arithmetic base must be between 2 and 64",
                             "Use `base#digits` with a base from 2 to 64"};
    }
    let const do_digit_value = [base](char c) -> i64 {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'Z') {
        return base <= 36 ? c - 'A' + 10 : c - 'A' + 36;
      }
      if (c == '@') return 62;
      if (c == '_') return 63;
      return -1;
    };
    u64 value = 0;
    usize i = base_length + 1;
    while (i < from.length) {
      let const digit = do_digit_value(from[i]);
      if (digit < 0 || digit >= base) {
        break;
      }
      /* The accumulation wraps in the unsigned domain so an oversized base#
         literal does not trigger signed-overflow. */
      value = value * static_cast<u64>(base) + static_cast<u64>(digit);
      i++;
    }
    *out_value = static_cast<i64>(value);
    return i;
  }

  usize consumed;
  if (from.length >= 2 && from[0] == '0' && (from[1] == 'x' || from[1] == 'X'))
  {
    consumed =
        2 + arithmetic_internal::count_leading_digits(from.substring(2), 16);
  } else if (from.length >= 2 && from[0] == '0' &&
             (from[1] == 'b' || from[1] == 'B'))
  {
    consumed =
        2 + arithmetic_internal::count_leading_digits(from.substring(2), 2);
  } else if (from.length >= 1 && from[0] == '0') {
    consumed = arithmetic_internal::count_leading_digits(from, 8);
  } else {
    consumed = arithmetic_internal::count_leading_digits(from, 10);
  }
  if (consumed == 0) consumed = 1;
  *out_value = parse_arithmetic_operand(from.substring_of_length(0, consumed));
  return consumed;
}

/* Longest first so the scan munches maximally, <<= before << before <. */
static const StringView ARITH_OPERATORS[] = {
    "<<=", ">>=", "**", "<<", ">>", "<=", ">=", "==", "!=", "&&",
    "||",  "++",  "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
    "^=",  "(",   ")",  ",",  "?",  ":",  "+",  "-",  "*",  "/",
    "%",   "<",   ">",  "=",  "&",  "|",  "^",  "!",  "~",
};

static fn tokenize_arithmetic(StringView src,
                              ArrayList<arith_token> &out) throws -> void
{
  usize i = 0;
  while (i < src.length) {
    let const current_byte = src[i];
    if (current_byte == ' ' || current_byte == '\t' || current_byte == '\n' ||
        current_byte == '\r')
    {
      i++;
      continue;
    }
    if (lexer::is_number(current_byte)) {
      i64 value = 0;
      let const consumed = lex_arith_number(src.substring(i), &value);
      out.push(arith_token{arith_token::kind::number, value,
                           src.substring_of_length(i, consumed)});
      i += consumed;
      continue;
    }
    if (lexer::is_variable_name_start(current_byte)) {
      let const name_start = i;
      i++;
      while (i < src.length && lexer::is_variable_name(src[i]))
        i++;
      out.push(
          arith_token{arith_token::kind::name, 0,
                      src.substring_of_length(name_start, i - name_start)});
      if (i < src.length && src[i] == '[') {
        i++;
        let const inner_start = i;
        usize depth = 1;
        while (i < src.length && depth > 0) {
          if (src[i] == '[')
            depth++;
          else if (src[i] == ']' && --depth == 0)
            break;
          i++;
        }
        if (depth != 0) {
          throw ErrorWithDetails{"Expected ']' after an array subscript",
                                 "The subscript '[' was never closed"};
        }
        out.push(
            arith_token{arith_token::kind::subscript, 0,
                        src.substring_of_length(inner_start, i - inner_start)});
        i++;
      }
      continue;
    }
    bool is_matched = false;
    for (let const &op : ARITH_OPERATORS) {
      if (i + op.length <= src.length &&
          src.substring_of_length(i, op.length) == op)
      {
        out.push(arith_token{arith_token::kind::op, 0,
                             src.substring_of_length(i, op.length)});
        i += op.length;
        is_matched = true;
        break;
      }
    }
    if (!is_matched) {
      out.push(
          arith_token{arith_token::kind::op, 0, src.substring_of_length(i, 1)});
      i++;
    }
  }
}

/* An operator that assigns, steps, short-circuits, or branches forces the full
   char parser, the token fast path keeps no side-effect ordering. */
static pure fn arith_op_is_complex(StringView t) wontthrow -> bool
{
  static constexpr PackedStringKey KEYS[] = {
      SSK("="),  SSK("+="), SSK("-="), SSK("*="),  SSK("/="),  SSK("%="),
      SSK("&="), SSK("|="), SSK("^="), SSK("<<="), SSK(">>="), SSK("?"),
      SSK(":"),  SSK(","),  SSK("++"), SSK("--"),  SSK("&&"),  SSK("||"),
  };
  static constexpr StaticStringSet COMPLEX_OPS{KEYS};
  return COMPLEX_OPS.contains(t);
}

static pure fn
arith_tokens_are_simple(const ArrayList<arith_token> &toks) wontthrow -> bool
{
  for (let const &t : toks) {
    if (t.k == arith_token::kind::subscript) return false;
    if (t.k == arith_token::kind::op && arith_op_is_complex(t.text)) {
      return false;
    }
  }
  return true;
}

struct arith_binop
{
  char kind;
  u8 precedence;
};

/* Mirrors peek_binary_operator. The short-circuit pair is excluded, a simple
   expression never holds it. */
static pure fn arith_classify_binop(StringView t) wontthrow -> arith_binop
{
  static constexpr static_string_entry<arith_binop> ENTRIES[] = {
      {SSK("**"), {'P', 11}},
      {SSK("*"),  {'*', 10}},
      {SSK("/"),  {'/', 10}},
      {SSK("%"),  {'%', 10}},
      {SSK("+"),  {'+', 9} },
      {SSK("-"),  {'-', 9} },
      {SSK("<<"), {'L', 8} },
      {SSK(">>"), {'R', 8} },
      {SSK("<"),  {'<', 7} },
      {SSK("<="), {'l', 7} },
      {SSK(">"),  {'>', 7} },
      {SSK(">="), {'g', 7} },
      {SSK("=="), {'e', 6} },
      {SSK("!="), {'n', 6} },
      {SSK("&"),  {'&', 5} },
      {SSK("^"),  {'^', 4} },
      {SSK("|"),  {'|', 3} },
  };
  static constexpr StaticStringMap BINOPS{ENTRIES};
  if (let const found = BINOPS.find(t); found.has_value()) return *found;
  return {0, 0};
}

/* Uses the same helpers as the char parser's ladder so the fast path and the
   full parser agree. */
hot static fn arith_apply_binop(char kind, i64 lhs, i64 rhs) throws -> i64
{
  switch (kind) {
  case 'P':
    if (rhs < 0) {
      throw ErrorWithDetails{"Exponent less than 0",
                             "'**' requires a non-negative exponent"};
    }
    return arithmetic_power(lhs, rhs);
  case '*': return arithmetic_multiply(lhs, rhs);
  case '/':
    if (rhs == 0) {
      throw ErrorWithDetails{"Division by zero",
                             "The right operand evaluated to 0"};
    }
    return arithmetic_divide(lhs, rhs);
  case '%':
    if (rhs == 0) {
      throw ErrorWithDetails{"Division by zero",
                             "The right operand evaluated to 0"};
    }
    return arithmetic_modulo(lhs, rhs);
  case '+': return arithmetic_add(lhs, rhs);
  case '-': return arithmetic_subtract(lhs, rhs);
  case 'L': return arithmetic_shift_left(lhs, rhs);
  case 'R': return arithmetic_shift_right(lhs, rhs);
  case '<': return lhs < rhs ? 1 : 0;
  case 'l': return lhs <= rhs ? 1 : 0;
  case '>': return lhs > rhs ? 1 : 0;
  case 'g': return lhs >= rhs ? 1 : 0;
  case 'e': return lhs == rhs ? 1 : 0;
  case 'n': return lhs != rhs ? 1 : 0;
  case '&': return lhs & rhs;
  case '^': return lhs ^ rhs;
  case '|': return lhs | rhs;
  default:
    unreachable("the cached arithmetic evaluator received invalid binary "
                "operator '%c'",
                kind);
  }
}

static fn evaluate_named_value_operand(EvalContext *context,
                                       StringView value) throws -> i64
{
  if (value.is_empty()) return 0;
  if (let const literal = try_parse_single_integer_literal(value);
      literal.has_value())
    return literal.value();

  ArithmeticParser nested{context, value, 0};
  return nested.parse();
}

static fn arith_read_variable(EvalContext *context, StringView name) throws
    -> i64
{
  ASSERT(context != nullptr);
  if (let const *stored = context->lookup_shell_variable(name);
      stored != nullptr)
  {
    return evaluate_named_value_operand(context, stored->view());
  }
  let const value = context->get_variable_value(name);
  if (!value.has_value()) {
    context->report_unset_reference(name);
    return 0;
  }

  return evaluate_named_value_operand(context, value->view());
}

/* A precedence-climbing evaluator over the cached token stream for a simple
   expression with no assignment, ternary, comma, or short-circuit. */
class ArithmeticTokenEvaluator
{
public:
  EvalContext *context;
  const ArrayList<arith_token> &toks;
  usize ti{0};
  usize depth{0};
  static constexpr usize MAX_DEPTH = 512;

  pure fn at_op(StringView s) wontthrow -> bool
  {
    return ti < toks.count() && toks[ti].k == arith_token::kind::op &&
           toks[ti].text == s;
  }

  hot flatten fn parse_operand() throws -> i64
  {
    depth++;
    defer { depth--; };
    if (depth > MAX_DEPTH) {
      throw ErrorWithDetails{"Expression nested too deeply",
                             "A variable may reference itself, like `x=x`"};
    }

    if (at_op("+")) {
      ti++;
      return parse_operand();
    }
    if (at_op("-")) {
      ti++;
      return arithmetic_subtract(0, parse_operand());
    }
    if (at_op("!")) {
      ti++;
      return parse_operand() == 0 ? 1 : 0;
    }
    if (at_op("~")) {
      ti++;
      return ~parse_operand();
    }
    if (at_op("(")) {
      ti++;
      let const value = parse_binary(1);
      if (!at_op(")")) {
        throw ErrorWithDetails{"Expected ')'", "An opening '(' is unmatched"};
      }
      ti++;
      return value;
    }
    if (ti < toks.count() && toks[ti].k == arith_token::kind::number) {
      let const value = toks[ti].value;
      ti++;
      return value;
    }
    if (ti < toks.count() && toks[ti].k == arith_token::kind::name) {
      let const name = toks[ti].text;
      ti++;
      return arith_read_variable(context, name);
    }
    if (ti >= toks.count())
      throw ErrorWithDetails{"Unfinished expression", "An operand is missing"};
    throw ErrorWithDetails{"Unexpected character in the arithmetic expression",
                           "This is not a valid operator or operand"};
  }

  fn parse_binary(u8 min_precedence) throws -> i64
  {
    let lhs = parse_operand();
    loop
    {
      if (ti >= toks.count() || toks[ti].k != arith_token::kind::op) {
        return lhs;
      }
      let const op = arith_classify_binop(toks[ti].text);
      if (op.precedence < min_precedence) return lhs;
      ti++;
      let const rhs =
          parse_binary(op.kind == 'P' ? op.precedence : op.precedence + 1);
      lhs = arith_apply_binop(op.kind, lhs, rhs);
    }
  }

  fn run() throws -> i64
  {
    if (toks.is_empty()) return 0;
    let const result = parse_binary(1);
    if (ti != toks.count()) {
      throw ErrorWithDetails{"Unexpected '" + String{toks[ti].text} +
                                 "' after the expression",
                             "An operator is missing between two values"};
    }
    return result;
  }
};

} /* namespace */

fn EvalContext::read_array_element_arithmetic_text(StringView name,
                                                   StringView subscript) throws
    -> String
{
  return apply_array_subscript(name, subscript);
}

fn EvalContext::evaluate_arithmetic(
    StringView expression, const SourceLocation *expression_base) throws -> i64
{
  LOG(All, "evaluating the arithmetic expression of %zu bytes",
      expression.length);

  /* A source with no parameter to expand, the d=$((d+1)) hot loop case, skips
     the expansion copy and parses directly. */
  if (!expression.find_character('$').has_value() &&
      !expression.find_character('`').has_value())
  {
    let parser = ArithmeticParser{this, expression, 0};
    if (expression_base != nullptr) parser.precise_base = *expression_base;
    return parser.parse();
  }

  /* The expanded word owns the bytes the parser views, so it outlives the
     parser. */
  LOG(All, "expanding parameters inside the arithmetic before the parse");
  let const expanded_word =
      expand_modifier_word(expression, true, true, expression_base);
  let parser = ArithmeticParser{this, expanded_word.view(), 0};
  return parser.parse();
}

fn EvalContext::evaluate_arithmetic_cached(const WordSegment &segment) throws
    -> i64
{
  let const source_location =
      segment.get_source_location(m_current_location.source_name_index);

  let cache_arena = segment.is_substitution_cache_in_function_arena
                        ? FUNCTION_ARENA
                        : AST_ARENA;
  if (cache_arena == nullptr) {
    return evaluate_arithmetic(segment.text.view(), source_location.has_value()
                                                        ? &*source_location
                                                        : nullptr);
  }

  let const generation = cache_arena->reset_generation();
  let &cache = segment.get_eval_cache();
  if (cache.arith == nullptr || cache.arena_generation != generation) {
    cache.arith = cache_arena->create<arith_token_cache>();
    cache.arena_generation = generation;
  }

  return evaluate_arithmetic_cached_clause(
      segment.text.view(), cache.arith->tokens, cache.arith->is_tokenized,
      cache.arith->is_simple,
      source_location.has_value() ? &*source_location : nullptr);
}

fn EvalContext::evaluate_arithmetic_cached_clause(
    StringView expression, ArrayList<arith_token> &tokens, bool &is_tokenized,
    bool &is_simple, const SourceLocation *source_location) throws -> i64
{
  if (!is_tokenized) {
    if (expression.find_character('$').has_value() ||
        expression.find_character('`').has_value())
    {
      return evaluate_arithmetic(expression, source_location);
    }

    tokens.clear();
    try {
      tokenize_arithmetic(expression, tokens);
    } catch (...) {
      tokens.clear();
      is_tokenized = true;
      is_simple = false;
      return evaluate_arithmetic(expression, source_location);
    }
    is_tokenized = true;
    is_simple = arith_tokens_are_simple(tokens);
  }

  if (!is_simple) return evaluate_arithmetic(expression, source_location);

  ArithmeticTokenEvaluator evaluator{this, tokens};
  return evaluator.run();
}

fn evaluate_constant_arithmetic(StringView expression) throws -> i64
{
  /* The optimizer has proven the expression holds no variable and no
     assignment, so a null context is never dereferenced. */
  let parser = ArithmeticParser{nullptr, expression, 0};
  return parser.parse();
}

} /* namespace koshka */
