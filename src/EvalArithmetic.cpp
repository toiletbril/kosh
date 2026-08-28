#include "ArbitraryArithmetic.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "ErrorOr.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Lexer.hpp"
#include "Maybe.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace {

using arithmetic_internal::ArithmeticValue;

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
static fn lex_exact_arith_number(StringView from, ArithmeticValue *out_value,
                                 Allocator allocator) throws -> usize;

hot static fn arith_apply_binop(char kind, const ArithmeticValue &lhs,
                                const ArithmeticValue &rhs, bool is_exact,
                                Allocator allocator) throws -> ArithmeticValue;

/* A recursive-descent evaluator for $((...)) following C operator precedence.
 */
class ArithmeticParser
{
public:
  ArithmeticParser(EvalContext *context_value, StringView source_value,
                   bool is_exact_value, Allocator allocator_value,
                   usize depth_value = 0, bool is_skipping = false)
      : context{context_value}, source{source_value}, pos{0},
        is_exact{is_exact_value}, depth{depth_value},
        m_is_skipping{is_skipping}, allocator{allocator_value}
  {}

  /* Null only on the analyze-time constant fold, where no variable read and no
     assignment path that dereferences the context is reached. */
  EvalContext *context;
  StringView source;
  usize pos;
  bool is_exact{false};

  usize depth{0};
  static constexpr usize MAX_DEPTH = 128;

  /* The dead operand of a short-circuited || or && and the untaken ternary arm
     are parsed to consume their tokens, this flag makes their assignments skip
     the store. */
  bool m_is_skipping{false};
  bool should_error_unset{false};

  Maybe<SourceLocation> precise_base{};
  Allocator allocator;

  [[noreturn]] cold fn fail(StringView message, StringView note = {}) throws
      -> void
  {
    if (precise_base.has_value()) {
      let const error_position = should_error_unset    ? pos
                                 : pos < source.length ? pos
                                 : source.is_empty()   ? 0
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
    if (should_error_unset) fail(message, note);

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

  fn read_variable_value(StringView name, usize name_position) throws
      -> ArithmeticValue
  {
    if (m_is_skipping) return ArithmeticValue{};

    ASSERT(context != nullptr);
    if (let const *stored = context->lookup_shell_variable(name);
        stored != nullptr)
    {
      return evaluate_operand_value(stored->view());
    }

    let const value = context->get_variable_value(name);
    if (!value.has_value()) {
      if (should_error_unset && !m_is_skipping) {
        let const message = "The variable '" + String{name} + "' is not set";
        if (precise_base.has_value()) {
          throw ErrorWithLocation{
              SourceLocation{precise_base->position + name_position,
                             name.length, precise_base->source_name_index},
              message.view()
          };
        }
        throw Error{steal(message)};
      }
      /* An unset name reports under the strict mood, a skipped ternary branch
         never does. */
      if (!m_is_skipping) context->report_unset_reference(name);
      return ArithmeticValue{};
    }
    return evaluate_operand_value(value->view());
  }

  fn evaluate_operand_value(StringView value) throws -> ArithmeticValue
  {
    if (value.is_empty()) return ArithmeticValue{};
    if (!is_exact) {
      if (let const literal = try_parse_single_integer_literal(value);
          literal.has_value())
        return ArithmeticValue{literal.value()};
    }

    if (depth >= MAX_DEPTH)
      fail("The variable value recurses too deeply",
           "A variable value refers back to itself");

    ArithmeticParser nested{context,   value,     is_exact,
                            allocator, depth + 1, m_is_skipping};
    nested.should_error_unset = should_error_unset;
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

  fn write_variable(StringView name, const ArithmeticValue &value) const throws
      -> void
  {
    if (m_is_skipping) return;
    ASSERT(context != nullptr);
    let const text = value.to_string(context->scratch_allocator());
    context->set_shell_variable(name, text.view());
  }

  struct lvalue
  {
    StringView name;
    Maybe<StringView> subscript;
    usize name_position;
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
    skip_spaces();
    let const name_position = pos;
    let const name = read_lvalue_name();
    if (name.is_empty()) return lvalue{name, None, name_position};
    return lvalue{name, read_optional_subscript(), name_position};
  }

  fn read_lvalue_value(const lvalue &target) throws -> ArithmeticValue
  {
    if (m_is_skipping) return ArithmeticValue{};

    if (target.subscript.has_value()) {
      ASSERT(context != nullptr);
      let const value = context->read_array_element_arithmetic_text(
          target.name, *target.subscript);
      return evaluate_operand_value(value.view());
    }
    return read_variable_value(target.name, target.name_position);
  }

  fn write_lvalue(const lvalue &target,
                  const ArithmeticValue &value) const throws -> void
  {
    if (m_is_skipping) return;
    if (target.subscript.has_value()) {
      ASSERT(context != nullptr);
      let const text = value.to_string(context->scratch_allocator());
      context->assign_array_element(target.name, *target.subscript, text.view(),
                                    false);
      return;
    }
    write_variable(target.name, value);
  }

  fn prefix_step(i64 delta, usize operator_position) throws -> ArithmeticValue
  {
    const lvalue target = read_lvalue();
    if (target.name.is_empty())
      fail_span(operator_position, operator_position + 2,
                "Expected a variable after '++' or '--'",
                "'++' and '--' step a variable, not a value");
    let const updated =
        arith_apply_binop('+', read_lvalue_value(target),
                          ArithmeticValue{delta}, is_exact, allocator);
    write_lvalue(target, updated);
    return updated;
  }

  fn postfix_step(const lvalue &target, i64 delta) throws -> ArithmeticValue
  {
    let const original = read_lvalue_value(target);
    write_lvalue(target,
                 arith_apply_binop('+', original, ArithmeticValue{delta},
                                   is_exact, allocator));
    return original;
  }

  fn parse() throws -> ArithmeticValue
  {
    skip_spaces();
    if (pos == source.length) return ArithmeticValue{};
    let const result = parse_comma();
    skip_spaces();
    if (pos != source.length) {
      if (should_error_unset && precise_base.has_value()) {
        throw ErrorWithLocationAndDetails{
            SourceLocation{precise_base->position + pos, source.length - pos,
                           precise_base->source_name_index},
            "Unexpected '" + String{source.substring(pos)}
            +
                "' after the expression",
            "An operator is missing between two values"
        };
      }
      fail("Unexpected '" + String{source.substring(pos)} +
               "' after the expression",
           "An operator is missing between two values");
    }
    return result;
  }

  fn parse_comma() throws -> ArithmeticValue
  {
    let result = parse_assignment();
    while (consume(","))
      result = parse_assignment();
    return result;
  }

  fn parse_assignment() throws -> ArithmeticValue
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
      const lvalue target{name, read_optional_subscript(), name_start};

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
            let const result = arith_apply_binop(
                kind, read_lvalue_value(target), rhs, is_exact, allocator);
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
  fn parse_skipped(ArithmeticValue (ArithmeticParser::*parse_branch)()) throws
      -> ArithmeticValue
  {
    let const was_skipping = m_is_skipping;
    m_is_skipping = true;
    defer { m_is_skipping = was_skipping; };
    return (this->*parse_branch)();
  }

  fn parse_ternary() throws -> ArithmeticValue
  {
    let const condition = parse_binary(1);
    if (consume("?")) {
      if (!condition.is_zero()) {
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
  fn parse_binary(u8 min_precedence) throws -> ArithmeticValue
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
        let const lhs_decides = (op.kind == 'A') == lhs.is_zero();
        let rhs = ArithmeticValue{};
        if (lhs_decides) {
          let const was_skipping = m_is_skipping;
          m_is_skipping = true;
          defer { m_is_skipping = was_skipping; };
          rhs = parse_binary(op.precedence + 1);
        } else {
          rhs = parse_binary(op.precedence + 1);
        }
        lhs = ArithmeticValue{
            op.kind == 'A' ? ((!lhs.is_zero() && !rhs.is_zero()) ? 1 : 0)
                           : ((!lhs.is_zero() || !rhs.is_zero()) ? 1 : 0)};
        continue;
      }

      /* ** is right-associative so it re-enters at its own precedence. */
      let const rhs =
          parse_binary(op.kind == 'P' ? op.precedence : op.precedence + 1);
      if (m_is_skipping) {
        lhs = ArithmeticValue{};
        continue;
      }
      switch (op.kind) {
      case 'P':
        if (rhs.is_negative()) {
          if (m_is_skipping) {
            lhs = ArithmeticValue{};
            break;
          }
          fail_span(lhs_start, pos, "Exponent less than 0",
                    "'**' requires a non-negative exponent");
        }
        lhs = arith_apply_binop('P', lhs, rhs, is_exact, allocator);
        break;
      case '/':
        if (rhs.is_zero()) {
          if (m_is_skipping) {
            lhs = ArithmeticValue{};
            break;
          }
          fail_span(lhs_start, pos, "Division by zero",
                    "The right operand evaluated to 0");
        }
        lhs = arith_apply_binop('/', lhs, rhs, is_exact, allocator);
        break;
      case '%':
        if (rhs.is_zero()) {
          if (m_is_skipping) {
            lhs = ArithmeticValue{};
            break;
          }
          fail_span(lhs_start, pos, "Division by zero",
                    "The right operand evaluated to 0");
        }
        lhs = arith_apply_binop('%', lhs, rhs, is_exact, allocator);
        break;
      default:
        lhs = arith_apply_binop(op.kind, lhs, rhs, is_exact, allocator);
        break;
      }
    }
  }

  fn parse_unary() throws -> ArithmeticValue
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
      return arith_apply_binop('-', ArithmeticValue{}, parse_unary(), is_exact,
                               allocator);
    }
    if (first == '!') {
      pos++;
      return ArithmeticValue{parse_unary().is_zero() ? 1 : 0};
    }
    if (first == '~') {
      pos++;
      let const value = parse_unary();
      return is_exact ? ArithmeticValue::bit_not(value, allocator)
                      : ArithmeticValue{~value.wrapped_i64()};
    }
    return parse_primary();
  }

  fn parse_primary() throws -> ArithmeticValue
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
      if (is_exact) {
        let value = ArithmeticValue{};
        pos += lex_exact_arith_number(source.substring(pos), &value, allocator);
        return value;
      }
      i64 value = 0;
      pos += lex_arith_number(source.substring(pos), &value);
      return ArithmeticValue{value};
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

static fn lex_exact_arith_number(StringView from, ArithmeticValue *out_value,
                                 Allocator allocator) throws -> usize
{
  let const do_count_digits = [](StringView text, u32 radix)
                                  wontthrow -> usize {
    usize digit_count = 0;

    while (digit_count < text.length) {
      let const byte = text[digit_count];
      i32 digit = -1;
      if (byte >= '0' && byte <= '9')
        digit = byte - '0';
      else if (byte >= 'a' && byte <= 'z')
        digit = byte - 'a' + 10;
      else if (byte >= 'A' && byte <= 'Z')
        digit = radix <= 36 ? byte - 'A' + 10 : byte - 'A' + 36;
      else if (byte == '@')
        digit = 62;
      else if (byte == '_')
        digit = 63;
      if (digit < 0 || static_cast<u32>(digit) >= radix) break;
      digit_count++;
    }

    return digit_count;
  };

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

    let const digit_count = do_count_digits(from.substring(base_length + 1),
                                            static_cast<u32>(base));
    let const consumed = base_length + 1 + digit_count;
    *out_value = ArithmeticValue::parse(
        from.substring_of_length(base_length + 1, digit_count),
        static_cast<u32>(base), allocator);
    return consumed;
  }

  let const decimal_integer_count =
      arithmetic_internal::count_leading_digits(from, 10);
  if (decimal_integer_count < from.length && from[decimal_integer_count] == '.')
  {
    let const fractional_count = arithmetic_internal::count_leading_digits(
        from.substring(decimal_integer_count + 1), 10);
    let const consumed = decimal_integer_count + 1 + fractional_count;
    *out_value = ArithmeticValue::parse_decimal(
        from.substring_of_length(0, consumed), allocator);
    return consumed;
  }

  let const detected = arithmetic_internal::detect_radix_prefix(from);
  let digit_count = arithmetic_internal::count_leading_digits(
      from.substring(detected.prefix_length), detected.radix);
  if (digit_count == 0 && detected.prefix_length == 0) digit_count = 1;
  let const consumed = detected.prefix_length + digit_count;
  *out_value = ArithmeticValue::parse(
      from.substring_of_length(detected.prefix_length, digit_count),
      detected.radix, allocator);
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
      let consumed = lex_arith_number(src.substring(i), &value);
      if (i + consumed < src.length && src[i + consumed] == '.') {
        consumed++;
        consumed += arithmetic_internal::count_leading_digits(
            src.substring(i + consumed), 10);
      }
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
    if (t.k == arith_token::kind::number &&
        t.text.find_character('.').has_value())
      return false;
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
hot static fn arith_apply_binop(char kind, const ArithmeticValue &lhs,
                                const ArithmeticValue &rhs, bool is_exact,
                                Allocator allocator) throws -> ArithmeticValue
{
  if (!is_exact) {
    let const left = lhs.wrapped_i64();
    let const right = rhs.wrapped_i64();
    switch (kind) {
    case 'P':
      if (right < 0) {
        throw ErrorWithDetails{"Exponent less than 0",
                               "'**' requires a non-negative exponent"};
      }
      return ArithmeticValue{arithmetic_power(left, right)};
    case '*': return ArithmeticValue{arithmetic_multiply(left, right)};
    case '/':
      if (right == 0) {
        throw ErrorWithDetails{"Division by zero",
                               "The right operand evaluated to 0"};
      }
      return ArithmeticValue{arithmetic_divide(left, right)};
    case '%':
      if (right == 0) {
        throw ErrorWithDetails{"Division by zero",
                               "The right operand evaluated to 0"};
      }
      return ArithmeticValue{arithmetic_modulo(left, right)};
    case '+': return ArithmeticValue{arithmetic_add(left, right)};
    case '-': return ArithmeticValue{arithmetic_subtract(left, right)};
    case 'L': return ArithmeticValue{arithmetic_shift_left(left, right)};
    case 'R': return ArithmeticValue{arithmetic_shift_right(left, right)};
    case '<': return ArithmeticValue{left < right ? 1 : 0};
    case 'l': return ArithmeticValue{left <= right ? 1 : 0};
    case '>': return ArithmeticValue{left > right ? 1 : 0};
    case 'g': return ArithmeticValue{left >= right ? 1 : 0};
    case 'e': return ArithmeticValue{left == right ? 1 : 0};
    case 'n': return ArithmeticValue{left != right ? 1 : 0};
    case '&': return ArithmeticValue{left & right};
    case '^': return ArithmeticValue{left ^ right};
    case '|': return ArithmeticValue{left | right};
    default:
      unreachable("the cached arithmetic evaluator received invalid binary "
                  "operator '%c'",
                  kind);
    }
  }

  switch (kind) {
  case 'P': return ArithmeticValue::power(lhs, rhs, allocator);
  case '*': return ArithmeticValue::multiply(lhs, rhs, allocator);
  case '/':
    if (rhs.is_zero()) {
      throw ErrorWithDetails{"Division by zero",
                             "The right operand evaluated to 0"};
    }
    return ArithmeticValue::divide(lhs, rhs, allocator);
  case '%':
    if (rhs.is_zero()) {
      throw ErrorWithDetails{"Division by zero",
                             "The right operand evaluated to 0"};
    }
    return ArithmeticValue::modulo(lhs, rhs, allocator);
  case '+': return ArithmeticValue::add(lhs, rhs, allocator);
  case '-': return ArithmeticValue::subtract(lhs, rhs, allocator);
  case 'L': return ArithmeticValue::shift_left(lhs, rhs, allocator);
  case 'R': return ArithmeticValue::shift_right(lhs, rhs, allocator);
  case '<': return ArithmeticValue{lhs.compare(rhs, allocator) < 0 ? 1 : 0};
  case 'l': return ArithmeticValue{lhs.compare(rhs, allocator) <= 0 ? 1 : 0};
  case '>': return ArithmeticValue{lhs.compare(rhs, allocator) > 0 ? 1 : 0};
  case 'g': return ArithmeticValue{lhs.compare(rhs, allocator) >= 0 ? 1 : 0};
  case 'e': return ArithmeticValue{lhs.compare(rhs, allocator) == 0 ? 1 : 0};
  case 'n': return ArithmeticValue{lhs.compare(rhs, allocator) != 0 ? 1 : 0};
  case '&': return ArithmeticValue::bit_and(lhs, rhs, allocator);
  case '^': return ArithmeticValue::bit_xor(lhs, rhs, allocator);
  case '|': return ArithmeticValue::bit_or(lhs, rhs, allocator);
  default:
    unreachable("the cached arithmetic evaluator received invalid binary "
                "operator '%c'",
                kind);
  }
}

static fn evaluate_named_value_operand(EvalContext *context, StringView value,
                                       bool is_exact,
                                       Allocator allocator) throws
    -> ArithmeticValue
{
  if (value.is_empty()) return ArithmeticValue{};
  if (!is_exact) {
    if (let const literal = try_parse_single_integer_literal(value);
        literal.has_value())
      return ArithmeticValue{literal.value()};
  }

  ArithmeticParser nested{context, value, is_exact, allocator};
  return nested.parse();
}

static fn arith_read_variable(EvalContext *context, StringView name,
                              bool is_exact, Allocator allocator) throws
    -> ArithmeticValue
{
  ASSERT(context != nullptr);
  if (let const *stored = context->lookup_shell_variable(name);
      stored != nullptr)
  {
    return evaluate_named_value_operand(context, stored->view(), is_exact,
                                        allocator);
  }
  let const value = context->get_variable_value(name);
  if (!value.has_value()) {
    context->report_unset_reference(name);
    return ArithmeticValue{};
  }

  return evaluate_named_value_operand(context, value->view(), is_exact,
                                      allocator);
}

/* A precedence-climbing evaluator over the cached token stream for a simple
   expression with no assignment, ternary, comma, or short-circuit. */
class ArithmeticTokenEvaluator
{
public:
  ArithmeticTokenEvaluator(EvalContext *context_value,
                           const ArrayList<arith_token> &tokens_value,
                           bool is_exact_value, Allocator allocator_value)
      : context{context_value}, toks{tokens_value}, is_exact{is_exact_value},
        allocator{allocator_value}
  {}

  EvalContext *context;
  const ArrayList<arith_token> &toks;
  usize ti{0};
  usize depth{0};
  bool is_exact{false};
  Allocator allocator;
  static constexpr usize MAX_DEPTH = 128;

  pure fn at_op(StringView s) wontthrow -> bool
  {
    return ti < toks.count() && toks[ti].k == arith_token::kind::op &&
           toks[ti].text == s;
  }

  hot flatten fn parse_operand() throws -> ArithmeticValue
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
      return arith_apply_binop('-', ArithmeticValue{}, parse_operand(),
                               is_exact, allocator);
    }
    if (at_op("!")) {
      ti++;
      return ArithmeticValue{parse_operand().is_zero() ? 1 : 0};
    }
    if (at_op("~")) {
      ti++;
      let const value = parse_operand();
      return is_exact ? ArithmeticValue::bit_not(value, allocator)
                      : ArithmeticValue{~value.wrapped_i64()};
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
      let value = ArithmeticValue{toks[ti].value};
      if (is_exact) lex_exact_arith_number(toks[ti].text, &value, allocator);
      ti++;
      return value;
    }
    if (ti < toks.count() && toks[ti].k == arith_token::kind::name) {
      let const name = toks[ti].text;
      ti++;
      return arith_read_variable(context, name, is_exact, allocator);
    }
    if (ti >= toks.count())
      throw ErrorWithDetails{"Unfinished expression", "An operand is missing"};
    throw ErrorWithDetails{"Unexpected character in the arithmetic expression",
                           "This is not a valid operator or operand"};
  }

  fn parse_binary(u8 min_precedence) throws -> ArithmeticValue
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
      lhs = arith_apply_binop(op.kind, lhs, rhs, is_exact, allocator);
    }
  }

  fn run() throws -> ArithmeticValue
  {
    if (toks.is_empty()) return ArithmeticValue{};
    let const result = parse_binary(1);
    if (ti != toks.count()) {
      throw ErrorWithDetails{"Unexpected '" + String{toks[ti].text} +
                                 "' after the expression",
                             "An operator is missing between two values"};
    }
    return result;
  }
};

} // namespace

fn EvalContext::read_array_element_arithmetic_text(StringView name,
                                                   StringView subscript) throws
    -> String
{
  return apply_array_subscript(name, subscript);
}

namespace {

fn evaluate_arithmetic_value(EvalContext *context, StringView expression,
                             const SourceLocation *expression_base,
                             bool is_exact, Allocator allocator,
                             bool should_error_unset = false) throws
    -> ArithmeticValue
{
  LOG(All, "evaluating the arithmetic expression of %zu bytes",
      expression.length);

  if (!expression.find_character('$').has_value() &&
      !expression.find_character('`').has_value())
  {
    let parser = ArithmeticParser{context, expression, is_exact, allocator};
    parser.should_error_unset = should_error_unset;
    if (expression_base != nullptr) parser.precise_base = *expression_base;
    return parser.parse();
  }

  LOG(All, "expanding parameters inside the arithmetic before the parse");
  let const expanded_word =
      context->expand_modifier_word(expression, true, true, expression_base);
  let parser =
      ArithmeticParser{context, expanded_word.view(), is_exact, allocator};
  parser.should_error_unset = should_error_unset;
  return parser.parse();
}

} /* namespace */

fn EvalContext::evaluate_arithmetic(
    StringView expression, const SourceLocation *expression_base) throws -> i64
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  let const value = evaluate_arithmetic_value(this, expression, expression_base,
                                              is_exact, scratch_allocator());
  return is_exact ? value.checked_i64() : value.wrapped_i64();
}

fn EvalContext::evaluate_arithmetic_text(
    StringView expression, const SourceLocation *expression_base) throws
    -> String
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  let const value = evaluate_arithmetic_value(this, expression, expression_base,
                                              is_exact, scratch_allocator());
  return value.to_string(heap_allocator());
}

fn EvalContext::evaluate_calculator_arithmetic_text(
    StringView expression) throws -> String
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  const SourceLocation expression_base{0, 0};
  let const value = evaluate_arithmetic_value(
      this, expression, &expression_base, is_exact, scratch_allocator(), true);
  return value.to_string(heap_allocator());
}

fn EvalContext::evaluate_arithmetic_nonzero(
    StringView expression, const SourceLocation *expression_base) throws -> bool
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  return !evaluate_arithmetic_value(this, expression, expression_base, is_exact,
                                    scratch_allocator())
              .is_zero();
}

fn EvalContext::compare_arithmetic(StringView left, StringView right) throws
    -> i32
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  let const left_value = evaluate_arithmetic_value(
      this, left, nullptr, is_exact, scratch_allocator());
  let const right_value = evaluate_arithmetic_value(
      this, right, nullptr, is_exact, scratch_allocator());
  return left_value.compare(right_value, scratch_allocator());
}

namespace {

fn evaluate_arithmetic_cached_value(EvalContext *context, StringView expression,
                                    ArrayList<arith_token> &tokens,
                                    bool &is_tokenized, bool &is_simple,
                                    const SourceLocation *source_location,
                                    bool is_exact, Allocator allocator) throws
    -> ArithmeticValue
{
  if (!is_tokenized) {
    if (expression.find_character('$').has_value() ||
        expression.find_character('`').has_value())
    {
      return evaluate_arithmetic_value(context, expression, source_location,
                                       is_exact, allocator);
    }

    tokens.clear();
    try {
      tokenize_arithmetic(expression, tokens);
    } catch (...) {
      tokens.clear();
      is_tokenized = true;
      is_simple = false;
      return evaluate_arithmetic_value(context, expression, source_location,
                                       is_exact, allocator);
    }
    is_tokenized = true;
    is_simple = arith_tokens_are_simple(tokens);
  }

  if (!is_simple)
    return evaluate_arithmetic_value(context, expression, source_location,
                                     is_exact, allocator);

  ArithmeticTokenEvaluator evaluator{context, tokens, is_exact, allocator};
  return evaluator.run();
}

} /* namespace */

fn EvalContext::evaluate_arithmetic_cached_text(
    const WordSegment &segment) throws -> String
{
  let const source_location =
      segment.get_source_location(m_current_location.source_name_index);
  let cache_arena = segment.is_substitution_cache_in_function_arena
                        ? FUNCTION_ARENA
                        : AST_ARENA;
  if (cache_arena == nullptr)
    return evaluate_arithmetic_text(
        segment.text.view(),
        source_location.has_value() ? &*source_location : nullptr);

  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let &cache = segment.get_eval_cache();
  if (cache.arith == nullptr ||
      !cache_arena->is_lifetime_valid(cache.arithmetic_lifetime))
  {
    cache.arith = cache_arena->create<arith_token_cache>();
    cache.arithmetic_lifetime = cache_arena->register_lifetime();
  }

  let const is_exact = mood() == mimic_mood::Default;
  if (is_exact && cache.arith->has_exact_constant_text) {
    return String{heap_allocator(), cache.arith->exact_constant_text.view()};
  }
  let const value = evaluate_arithmetic_cached_value(
      this, segment.text.view(), cache.arith->tokens, cache.arith->is_tokenized,
      cache.arith->is_simple,
      source_location.has_value() ? &*source_location : nullptr, is_exact,
      scratch_allocator());
  let result = value.to_string(heap_allocator());
  if (is_exact && cache.arith->is_tokenized && cache.arith->is_simple) {
    let is_constant = true;

    for (let const &token : cache.arith->tokens) {
      if (token.k == arith_token::kind::name ||
          token.k == arith_token::kind::subscript)
      {
        is_constant = false;
        break;
      }
    }

    if (is_constant) {
      cache.arith->exact_constant_text =
          String{heap_allocator(), result.view()};
      cache.arith->has_exact_constant_text = true;
    }
  }

  return result;
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

  let &cache = segment.get_eval_cache();
  if (cache.arith == nullptr ||
      !cache_arena->is_lifetime_valid(cache.arithmetic_lifetime))
  {
    cache.arith = cache_arena->create<arith_token_cache>();
    cache.arithmetic_lifetime = cache_arena->register_lifetime();
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
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  let const value = evaluate_arithmetic_cached_value(
      this, expression, tokens, is_tokenized, is_simple, source_location,
      is_exact, scratch_allocator());
  return is_exact ? value.checked_i64() : value.wrapped_i64();
}

fn EvalContext::evaluate_arithmetic_cached_clause_nonzero(
    StringView expression, ArrayList<arith_token> &tokens, bool &is_tokenized,
    bool &is_simple, const SourceLocation *source_location) throws -> bool
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = mood() == mimic_mood::Default;
  return !evaluate_arithmetic_cached_value(
              this, expression, tokens, is_tokenized, is_simple,
              source_location, is_exact, scratch_allocator())
              .is_zero();
}

namespace {

fn get_constant_arithmetic_arena() wontthrow -> BumpArena &
{
  static thread_local BumpArena arena;

  return arena;
}

} /* namespace */

fn evaluate_constant_arithmetic(StringView expression) throws -> i64
{
  /* The optimizer has proven the expression holds no variable and no
     assignment, so a null context is never dereferenced. */
  let &arena = get_constant_arithmetic_arena();
  let const scratch = arena.mark();
  defer { arena.release(scratch); };
  let parser =
      ArithmeticParser{nullptr, expression, false, bump_allocator(arena)};
  return parser.parse().wrapped_i64();
}

fn evaluate_constant_arithmetic_nonzero(StringView expression,
                                        bool is_exact) throws -> bool
{
  let &arena = get_constant_arithmetic_arena();
  let const scratch = arena.mark();
  defer { arena.release(scratch); };
  let parser =
      ArithmeticParser{nullptr, expression, is_exact, bump_allocator(arena)};
  return !parser.parse().is_zero();
}

} /* namespace koshka */
