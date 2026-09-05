#include "Common.hpp"
#include "Debug.hpp"
#include "ErrorOr.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "EvalExtendedArithmetic.hpp"
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
                                 BumpArena &arena) throws -> usize;

static pure fn is_leading_decimal_number(StringView source,
                                         usize position) wontthrow -> bool
{
  return position + 1 < source.length && source[position] == '.' &&
         lexer::is_number(source[position + 1]);
}

hot static fn arith_apply_binop(char kind, const ArithmeticValue &lhs,
                                const ArithmeticValue &rhs, bool is_exact,
                                BumpArena &arena,
                                Maybe<u32> bc_scale = {}) throws
    -> ArithmeticValue;

/* A recursive-descent evaluator for $((...)) following C operator precedence.
 */
class ArithmeticParser
{
public:
  ArithmeticParser(EvalContext *context_value, StringView source_value,
                   bool is_exact_value, BumpArena &arena_value,
                   usize depth_value = 0, bool is_skipping = false,
                   Maybe<u32> bc_scale_value = {})
      : context{context_value}, source{source_value}, pos{0},
        is_exact{is_exact_value}, depth{depth_value},
        m_is_skipping{is_skipping}, bc_scale{bc_scale_value}, arena{arena_value}
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
  Maybe<u32> bc_scale{};

  Maybe<SourceLocation> precise_base{};
  BumpArena &arena;
  u32 cached_pi_scale{0};
  ArithmeticValue cached_pi{};
  usize calculator_function_position{0};

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

    ArithmeticParser nested{context,   value,         is_exact, arena,
                            depth + 1, m_is_skipping, bc_scale};
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
                          ArithmeticValue{delta}, is_exact, arena, bc_scale);
    write_lvalue(target, updated);
    return updated;
  }

  fn postfix_step(const lvalue &target, i64 delta) throws -> ArithmeticValue
  {
    let const original = read_lvalue_value(target);
    write_lvalue(target,
                 arith_apply_binop('+', original, ArithmeticValue{delta},
                                   is_exact, arena, bc_scale));
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
          {"**=", 'P'},
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
            skip_spaces();
            let const rhs_start = pos;
            let const rhs = parse_assignment();
            let const lhs = read_lvalue_value(target);
            let result = ArithmeticValue{};
            try {
              result =
                  arith_apply_binop(kind, lhs, rhs, is_exact, arena, bc_scale);
            } catch (const ErrorWithLocation &) {
              throw;
            } catch (const ErrorBase &error) {
              fail_span(rhs_start, pos, error.message().view(),
                        error.detail_message());
            }
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
      skip_spaces();
      let const rhs_start = pos;

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
      try {
        switch (op.kind) {
        case 'P':
          if (rhs.is_negative() && !bc_scale.has_value()) {
            if (m_is_skipping) {
              lhs = ArithmeticValue{};
              break;
            }
            fail_span(rhs_start, pos, "Exponent less than 0",
                      "'**' requires a non-negative exponent");
          }
          lhs = arith_apply_binop('P', lhs, rhs, is_exact, arena, bc_scale);
          break;
        case '/':
          if (rhs.is_zero()) {
            if (m_is_skipping) {
              lhs = ArithmeticValue{};
              break;
            }
            fail_span(rhs_start, pos, "Division by zero",
                      "The right operand evaluated to 0");
          }
          lhs = arith_apply_binop('/', lhs, rhs, is_exact, arena, bc_scale);
          break;
        case '%':
          if (rhs.is_zero()) {
            if (m_is_skipping) {
              lhs = ArithmeticValue{};
              break;
            }
            fail_span(rhs_start, pos, "Division by zero",
                      "The right operand evaluated to 0");
          }
          lhs = arith_apply_binop('%', lhs, rhs, is_exact, arena, bc_scale);
          break;
        default:
          lhs = arith_apply_binop(op.kind, lhs, rhs, is_exact, arena, bc_scale);
          break;
        }
      } catch (const ErrorWithLocation &) {
        throw;
      } catch (const ErrorBase &error) {
        fail_span(op.kind == 'P' || op.kind == '/' || op.kind == '%'
                      ? rhs_start
                      : lhs_start,
                  pos, error.message().view(), error.detail_message());
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
                               arena, bc_scale);
    }
    if (first == '!') {
      pos++;
      return ArithmeticValue{parse_unary().is_zero() ? 1 : 0};
    }
    if (first == '~') {
      pos++;
      let const value = parse_unary();
      return is_exact ? ArithmeticValue::bit_not(value, arena)
                      : ArithmeticValue{~value.wrapped_i64()};
    }
    return parse_primary();
  }

  enum class calculator_function : u8
  {
    Abs,
    Atan,
    Ceil,
    Cmp,
    Cos,
    Exp,
    Fact,
    Fib,
    Floor,
    Frac,
    Gcd,
    Int,
    IsEven,
    IsInt,
    IsOdd,
    Length,
    Lcm,
    Ln,
    Max,
    Min,
    Scale,
    Sgn,
    Sin,
    Sqrt,
  };

  struct calculator_argument : ArithmeticValue
  {
    calculator_argument(ArithmeticValue value, usize start_position,
                        usize end_position)
        : ArithmeticValue(steal(value)), start(start_position),
          end(end_position)
    {}
    usize start;
    usize end;
  };

  fn calculator_function_arguments(StringView name) throws
      -> ArrayList<calculator_argument>
  {
    let arguments = ArrayList<calculator_argument>{bump_allocator(arena)};
    if (consume(")")) return arguments;

    loop
    {
      skip_spaces();
      let const argument_start = pos;
      let argument = parse_assignment();
      arguments.push(calculator_argument{steal(argument), argument_start, pos});
      if (consume(")")) return arguments;
      if (!consume(","))
        fail("Expected ',' or ')' after an argument",
             "The call to '" + String{name} + "' is unfinished");
    }
  }

  fn require_argument_count(StringView name,
                            const ArrayList<calculator_argument> &arguments,
                            usize minimum_count, usize maximum_count) throws
      -> void
  {
    if (arguments.count() >= minimum_count &&
        arguments.count() <= maximum_count)
      return;
    let const allocator = bump_allocator(arena);
    let detail = String{allocator, "The function accepts "};
    if (maximum_count == ~usize{0}) {
      detail += "at least ";
      detail += String::from(minimum_count, allocator);
      detail += minimum_count == 1 ? " argument" : " arguments";
    } else if (minimum_count == maximum_count) {
      detail += String::from(minimum_count, allocator);
      detail += minimum_count == 1 ? " argument" : " arguments";
    } else {
      detail += String::from(minimum_count, allocator);
      detail += " to ";
      detail += String::from(maximum_count, allocator);
      detail += " arguments";
    }
    fail_span(calculator_function_position,
              calculator_function_position + name.length,
              "Wrong number of arguments for '" + String{name} + "'", detail);
  }

  fn require_integer_argument(StringView name,
                              const calculator_argument &argument) throws -> i64
  {
    if (!argument.has_integer_value(arena))
      fail_span(argument.start, argument.end,
                "The argument to '" + String{name} + "' is not an integer",
                "This function requires an integer value");
    try {
      return ArithmeticValue::integer_part(argument, arena).checked_i64();
    } catch (const ErrorWithLocation &) {
      throw;
    } catch (const ErrorBase &error) {
      fail_span(argument.start, argument.end, error.message().view(),
                error.detail_message());
    }
  }

  fn calculator_gcd(ArithmeticValue left, ArithmeticValue right) throws
      -> ArithmeticValue
  {
    left = ArithmeticValue::absolute(left, arena);
    right = ArithmeticValue::absolute(right, arena);
    while (!right.is_zero()) {
      let const remainder = ArithmeticValue::modulo(left, right, arena);
      left = right;
      right = remainder;
    }
    return left;
  }

  fn calculator_fibonacci(u64 index) throws -> ArithmeticValue
  {
    let current = ArithmeticValue{};
    let next = ArithmeticValue{1};
    if (index == 0) return current;
    let bit = u64{1} << (63u - static_cast<u32>(__builtin_clzll(index)));
    while (bit != 0) {
      let const doubled_next = ArithmeticValue::add(next, next, arena);
      let const c = ArithmeticValue::multiply(
          current, ArithmeticValue::subtract(doubled_next, current, arena),
          arena);
      let const d = ArithmeticValue::add(
          ArithmeticValue::multiply(current, current, arena),
          ArithmeticValue::multiply(next, next, arena), arena);
      if ((index & bit) == 0) {
        current = c;
        next = d;
      } else {
        current = d;
        next = ArithmeticValue::add(c, d, arena);
      }
      bit >>= 1u;
    }
    return current;
  }

  fn calculator_requested_scale(
      StringView name, const ArrayList<calculator_argument> &arguments) throws
      -> u32
  {
    u32 requested_scale = 20;
    if (arguments.count() == 2) {
      let const parsed = require_integer_argument(name, arguments[1]);
      if (parsed < 0 || parsed > 100000)
        fail_span(arguments[1].start, arguments[1].end,
                  "Invalid scale for '" + String{name} + "'",
                  "The scale must be between 0 and 100000");
      requested_scale = static_cast<u32>(parsed);
    }
    return requested_scale;
  }

  fn calculator_work_scale(u32 requested_scale) throws -> u32
  {
    if (requested_scale > ~u32{0} - 12) throw std::bad_alloc{};
    return requested_scale + 12;
  }

  fn calculator_pi(u32 decimal_scale) throws -> ArithmeticValue
  {
    if (cached_pi_scale < decimal_scale) {
      let const quarter_pi = calculator_atan(ArithmeticValue{1}, decimal_scale);
      let const half_pi = ArithmeticValue::add(quarter_pi, quarter_pi, arena);
      cached_pi = ArithmeticValue::add(half_pi, half_pi, arena);
      cached_pi_scale = decimal_scale;
    }
    if (cached_pi_scale == decimal_scale) return cached_pi;

    return ArithmeticValue::rescale(cached_pi, decimal_scale, arena);
  }

  fn calculator_fixed_multiply(const ArithmeticValue &left,
                               const ArithmeticValue &right, u32 decimal_scale,
                               BumpArena &result_arena) throws
      -> ArithmeticValue
  {
    return ArithmeticValue::rescale(
        ArithmeticValue::multiply(left, right, result_arena), decimal_scale,
        result_arena);
  }

  fn calculator_fixed_divide(const ArithmeticValue &left,
                             const ArithmeticValue &right, u32 decimal_scale,
                             BumpArena &result_arena) throws -> ArithmeticValue
  {
    if (right.is_zero())
      fail("Division by zero", "The right operand evaluated to 0");
    u32 prepared_scale = left.get_decimal_scale();
    if (decimal_scale > prepared_scale) prepared_scale = decimal_scale;
    let const prepared =
        ArithmeticValue::rescale(left, prepared_scale, result_arena);
    return ArithmeticValue::rescale(
        ArithmeticValue::divide(prepared, right, result_arena), decimal_scale,
        result_arena);
  }

  fn calculator_atan(ArithmeticValue value, u32 requested_scale) throws
      -> ArithmeticValue
  {
    let const work_scale = calculator_work_scale(requested_scale);
    let const is_negative = value.is_negative();
    value = ArithmeticValue::rescale(ArithmeticValue::absolute(value, arena),
                                     work_scale, arena);
    let const threshold = ArithmeticValue::rescale(
        ArithmeticValue::parse_decimal("0.1", arena), work_scale, arena);
    BumpArena iteration_arenas[2];
    usize state_position = 0;
    value = ArithmeticValue::add(value, ArithmeticValue{}, iteration_arenas[0]);
    u32 doubling_count = 0;
    while (value.compare(threshold, bump_allocator(arena)) > 0) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      let const square = calculator_fixed_multiply(
          value, value, work_scale, iteration_arenas[next_position]);
      let const root = ArithmeticValue::square_root(
          ArithmeticValue::add(ArithmeticValue{1}, square,
                               iteration_arenas[next_position]),
          work_scale, iteration_arenas[next_position]);
      value = calculator_fixed_divide(
          value,
          ArithmeticValue::add(ArithmeticValue{1}, root,
                               iteration_arenas[next_position]),
          work_scale, iteration_arenas[next_position]);
      state_position = next_position;
      doubling_count++;
      if (doubling_count > 64) throw std::bad_alloc{};
    }

    value = ArithmeticValue::add(value, ArithmeticValue{}, arena);
    iteration_arenas[0].reset();
    iteration_arenas[1].reset();
    let const square =
        calculator_fixed_multiply(value, value, work_scale, arena);
    let term =
        ArithmeticValue::add(value, ArithmeticValue{}, iteration_arenas[0]);
    let sum = term;
    state_position = 0;
    let const iteration_count = static_cast<usize>(work_scale) * 2 + 128;
    for (usize index = 1; index < iteration_count; index++) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      let next_term = calculator_fixed_multiply(
          term, square, work_scale, iteration_arenas[next_position]);
      let const denominator = ArithmeticValue{static_cast<i64>(index * 2 + 1)};
      let const contribution = calculator_fixed_divide(
          next_term, denominator, work_scale, iteration_arenas[next_position]);
      if (contribution.is_zero()) break;
      let next_sum =
          (index & 1u) != 0
              ? ArithmeticValue::subtract(sum, contribution,
                                          iteration_arenas[next_position])
              : ArithmeticValue::add(sum, contribution,
                                     iteration_arenas[next_position]);
      term = next_term;
      sum = next_sum;
      state_position = next_position;
    }
    sum = ArithmeticValue::add(sum, ArithmeticValue{}, arena);
    for (u32 count = 0; count < doubling_count; count++)
      sum = ArithmeticValue::add(sum, sum, arena);
    if (is_negative)
      sum = ArithmeticValue::subtract(ArithmeticValue{}, sum, arena);
    return ArithmeticValue::rescale(sum, requested_scale, arena);
  }

  fn calculator_sin_cos(ArithmeticValue value, u32 requested_scale,
                        bool is_cosine) throws -> ArithmeticValue
  {
    let const work_scale = calculator_work_scale(requested_scale);
    value = ArithmeticValue::rescale(value, work_scale, arena);
    let const pi = calculator_pi(work_scale);
    let const half_pi =
        calculator_fixed_divide(pi, ArithmeticValue{2}, work_scale, arena);
    let const two_pi = ArithmeticValue::add(pi, pi, arena);
    value = ArithmeticValue::modulo(value, two_pi, arena);
    if (value.compare(pi, bump_allocator(arena)) > 0)
      value = ArithmeticValue::subtract(value, two_pi, arena);
    let const negative_pi =
        ArithmeticValue::subtract(ArithmeticValue{}, pi, arena);
    if (value.compare(negative_pi, bump_allocator(arena)) < 0)
      value = ArithmeticValue::add(value, two_pi, arena);
    let should_negate = false;
    if (value.compare(half_pi, bump_allocator(arena)) > 0) {
      value = ArithmeticValue::subtract(pi, value, arena);
      should_negate = is_cosine;
    } else {
      let const negative_half_pi =
          ArithmeticValue::subtract(ArithmeticValue{}, half_pi, arena);
      if (value.compare(negative_half_pi, bump_allocator(arena)) < 0) {
        value = ArithmeticValue::subtract(negative_pi, value, arena);
        should_negate = is_cosine;
      }
    }
    let const square =
        calculator_fixed_multiply(value, value, work_scale, arena);
    BumpArena iteration_arenas[2];
    let term = ArithmeticValue::add(is_cosine ? ArithmeticValue{1} : value,
                                    ArithmeticValue{}, iteration_arenas[0]);
    let sum = term;
    usize state_position = 0;
    let const iteration_count = static_cast<usize>(work_scale) * 2 + 128;
    for (usize index = 1; index < iteration_count; index++) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      let const first = is_cosine ? index * 2 - 1 : index * 2;
      let const second = is_cosine ? index * 2 : index * 2 + 1;
      let const denominator = ArithmeticValue{static_cast<i64>(first * second)};
      let next_term = calculator_fixed_divide(
          calculator_fixed_multiply(term, square, work_scale,
                                    iteration_arenas[next_position]),
          denominator, work_scale, iteration_arenas[next_position]);
      if (next_term.is_zero()) break;
      let next_sum = (index & 1u) != 0
                         ? ArithmeticValue::subtract(
                               sum, next_term, iteration_arenas[next_position])
                         : ArithmeticValue::add(
                               sum, next_term, iteration_arenas[next_position]);
      term = next_term;
      sum = next_sum;
      state_position = next_position;
    }
    sum = ArithmeticValue::add(sum, ArithmeticValue{}, arena);
    if (should_negate)
      sum = ArithmeticValue::subtract(ArithmeticValue{}, sum, arena);
    return ArithmeticValue::rescale(sum, requested_scale, arena);
  }

  fn calculator_exp(ArithmeticValue value, u32 requested_scale) throws
      -> ArithmeticValue
  {
    let const work_scale = calculator_work_scale(requested_scale);
    let const is_negative = value.is_negative();
    value = ArithmeticValue::rescale(ArithmeticValue::absolute(value, arena),
                                     work_scale, arena);
    let const reduction_limit = ArithmeticValue::rescale(
        ArithmeticValue::parse_decimal("0.5", arena), work_scale, arena);
    BumpArena iteration_arenas[2];
    usize state_position = 0;
    value = ArithmeticValue::add(value, ArithmeticValue{}, iteration_arenas[0]);
    u32 squaring_count = 0;
    while (value.compare(reduction_limit, bump_allocator(arena)) > 0) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      value = calculator_fixed_divide(value, ArithmeticValue{2}, work_scale,
                                      iteration_arenas[next_position]);
      state_position = next_position;
      squaring_count++;
      if (squaring_count > 4096)
        fail("The operand to 'exp' is too large",
             "Use an absolute value below 2 ** 4096");
    }
    value = ArithmeticValue::add(value, ArithmeticValue{}, arena);
    iteration_arenas[0].reset();
    iteration_arenas[1].reset();
    let term = ArithmeticValue::rescale(ArithmeticValue{1}, work_scale,
                                        iteration_arenas[0]);
    let sum = term;
    state_position = 0;
    let const iteration_count = static_cast<usize>(work_scale) * 4 + 256;
    for (usize index = 1; index < iteration_count; index++) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      let next_term = calculator_fixed_divide(
          calculator_fixed_multiply(term, value, work_scale,
                                    iteration_arenas[next_position]),
          ArithmeticValue{static_cast<i64>(index)}, work_scale,
          iteration_arenas[next_position]);
      if (next_term.is_zero()) break;
      let next_sum =
          ArithmeticValue::add(sum, next_term, iteration_arenas[next_position]);
      term = next_term;
      sum = next_sum;
      state_position = next_position;
    }
    for (u32 count = 0; count < squaring_count; count++) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      sum = calculator_fixed_multiply(sum, sum, work_scale,
                                      iteration_arenas[next_position]);
      state_position = next_position;
    }
    if (is_negative)
      sum = calculator_fixed_divide(ArithmeticValue{1}, sum, work_scale, arena);
    return ArithmeticValue::rescale(sum, requested_scale, arena);
  }

  fn calculator_ln(ArithmeticValue value, u32 requested_scale) throws
      -> ArithmeticValue
  {
    let const work_scale = calculator_work_scale(requested_scale);
    value = ArithmeticValue::rescale(value, work_scale, arena);
    let const lower = ArithmeticValue::rescale(
        ArithmeticValue::parse_decimal("0.8", arena), work_scale, arena);
    let const upper = ArithmeticValue::rescale(
        ArithmeticValue::parse_decimal("1.25", arena), work_scale, arena);
    BumpArena iteration_arenas[2];
    usize state_position = 0;
    value = ArithmeticValue::add(value, ArithmeticValue{}, iteration_arenas[0]);
    u32 root_count = 0;
    while (value.compare(lower, bump_allocator(arena)) < 0 ||
           value.compare(upper, bump_allocator(arena)) > 0)
    {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      value = ArithmeticValue::square_root(value, work_scale,
                                           iteration_arenas[next_position]);
      state_position = next_position;
      root_count++;
      if (root_count > 64) throw std::bad_alloc{};
    }
    value = ArithmeticValue::add(value, ArithmeticValue{}, arena);
    iteration_arenas[0].reset();
    iteration_arenas[1].reset();
    let const numerator =
        ArithmeticValue::subtract(value, ArithmeticValue{1}, arena);
    let const denominator =
        ArithmeticValue::add(value, ArithmeticValue{1}, arena);
    let const reduced =
        calculator_fixed_divide(numerator, denominator, work_scale, arena);
    let const square =
        calculator_fixed_multiply(reduced, reduced, work_scale, arena);
    let term =
        ArithmeticValue::add(reduced, ArithmeticValue{}, iteration_arenas[0]);
    let sum = term;
    state_position = 0;
    let const iteration_count = static_cast<usize>(work_scale) * 2 + 128;
    for (usize index = 1; index < iteration_count; index++) {
      let const next_position = 1 - state_position;
      iteration_arenas[next_position].reset();
      let next_term = calculator_fixed_multiply(
          term, square, work_scale, iteration_arenas[next_position]);
      let const contribution = calculator_fixed_divide(
          next_term, ArithmeticValue{static_cast<i64>(index * 2 + 1)},
          work_scale, iteration_arenas[next_position]);
      if (contribution.is_zero()) break;
      let next_sum = ArithmeticValue::add(sum, contribution,
                                          iteration_arenas[next_position]);
      term = next_term;
      sum = next_sum;
      state_position = next_position;
    }
    sum = ArithmeticValue::add(sum, ArithmeticValue{}, arena);
    sum = ArithmeticValue::add(sum, sum, arena);
    for (u32 count = 0; count < root_count; count++)
      sum = ArithmeticValue::add(sum, sum, arena);
    return ArithmeticValue::rescale(sum, requested_scale, arena);
  }

  fn apply_calculator_function(StringView name, usize name_position) throws
      -> ArithmeticValue
  {
    static constexpr static_string_entry<calculator_function> ENTRIES[] = {
        {SSK("abs"),    calculator_function::Abs   },
        {SSK("atan"),   calculator_function::Atan  },
        {SSK("ceil"),   calculator_function::Ceil  },
        {SSK("cmp"),    calculator_function::Cmp   },
        {SSK("cos"),    calculator_function::Cos   },
        {SSK("exp"),    calculator_function::Exp   },
        {SSK("fact"),   calculator_function::Fact  },
        {SSK("fib"),    calculator_function::Fib   },
        {SSK("floor"),  calculator_function::Floor },
        {SSK("frac"),   calculator_function::Frac  },
        {SSK("gcd"),    calculator_function::Gcd   },
        {SSK("int"),    calculator_function::Int   },
        {SSK("iseven"), calculator_function::IsEven},
        {SSK("isint"),  calculator_function::IsInt },
        {SSK("isodd"),  calculator_function::IsOdd },
        {SSK("length"), calculator_function::Length},
        {SSK("lcm"),    calculator_function::Lcm   },
        {SSK("ln"),     calculator_function::Ln    },
        {SSK("max"),    calculator_function::Max   },
        {SSK("min"),    calculator_function::Min   },
        {SSK("scale"),  calculator_function::Scale },
        {SSK("sgn"),    calculator_function::Sgn   },
        {SSK("sin"),    calculator_function::Sin   },
        {SSK("sqrt"),   calculator_function::Sqrt  },
    };
    static constexpr StaticStringMap FUNCTIONS{ENTRIES};
    let const previous_function_position = calculator_function_position;
    calculator_function_position = name_position;
    defer { calculator_function_position = previous_function_position; };

    let const function = FUNCTIONS.find(name);
    if (!function.has_value())
      fail_span(name_position, name_position + name.length,
                "Unknown calculator function '" + String{name} + "'",
                "Use a supported numeric function name");

    let const arguments = calculator_function_arguments(name);
    switch (*function) {
    case calculator_function::Abs:
      require_argument_count(name, arguments, 1, 1);
      return ArithmeticValue::absolute(arguments[0], arena);
    case calculator_function::Atan:
      require_argument_count(name, arguments, 1, 2);
      return calculator_atan(arguments[0],
                             calculator_requested_scale(name, arguments));
    case calculator_function::Ceil:
    case calculator_function::Floor:
    case calculator_function::Frac:
    case calculator_function::Int: {
      require_argument_count(name, arguments, 1, 1);
      let const integer = ArithmeticValue::integer_part(arguments[0], arena);
      if (*function == calculator_function::Int) return integer;
      let const fraction =
          ArithmeticValue::subtract(arguments[0], integer, arena);
      if (*function == calculator_function::Frac) return fraction;
      if (fraction.is_zero()) return integer;
      if (*function == calculator_function::Floor)
        return arguments[0].is_negative()
                   ? ArithmeticValue::subtract(integer, ArithmeticValue{1},
                                               arena)
                   : integer;
      return arguments[0].is_negative()
                 ? integer
                 : ArithmeticValue::add(integer, ArithmeticValue{1}, arena);
    }
    case calculator_function::Cmp:
      require_argument_count(name, arguments, 2, 2);
      return ArithmeticValue{
          arguments[0].compare(arguments[1], bump_allocator(arena))};
    case calculator_function::Cos:
      require_argument_count(name, arguments, 1, 2);
      return calculator_sin_cos(
          arguments[0], calculator_requested_scale(name, arguments), true);
    case calculator_function::Exp:
      require_argument_count(name, arguments, 1, 2);
      return calculator_exp(arguments[0],
                            calculator_requested_scale(name, arguments));
    case calculator_function::Fact: {
      require_argument_count(name, arguments, 1, 1);
      let const count = require_integer_argument(name, arguments[0]);
      if (count < 0)
        fail_span(arguments[0].start, arguments[0].end,
                  "The argument to 'fact' is negative",
                  "Factorial requires a non-negative integer");
      let result = ArithmeticValue{1};
      for (i64 factor = 2; factor <= count; factor++)
        result =
            ArithmeticValue::multiply(result, ArithmeticValue{factor}, arena);
      return result;
    }
    case calculator_function::Fib: {
      require_argument_count(name, arguments, 1, 1);
      let const index = require_integer_argument(name, arguments[0]);
      if (index < 0)
        fail_span(arguments[0].start, arguments[0].end,
                  "The argument to 'fib' is negative",
                  "Fibonacci requires a non-negative integer");
      return calculator_fibonacci(static_cast<u64>(index));
    }
    case calculator_function::Gcd:
    case calculator_function::Lcm: {
      require_argument_count(name, arguments, 1, ~usize{0});
      if (!arguments[0].has_integer_value(arena))
        fail_span(arguments[0].start, arguments[0].end,
                  "An argument to '" + String{name} + "' is not an integer",
                  "This function requires integer values");
      let result = ArithmeticValue::absolute(
          ArithmeticValue::integer_part(arguments[0], arena), arena);
      for (usize index = 1; index < arguments.count(); index++) {
        if (!arguments[index].has_integer_value(arena))
          fail_span(arguments[index].start, arguments[index].end,
                    "An argument to '" + String{name} + "' is not an integer",
                    "This function requires integer values");
        let const argument =
            ArithmeticValue::integer_part(arguments[index], arena);
        let const divisor = calculator_gcd(result, argument);
        if (*function == calculator_function::Gcd) {
          result = divisor;
        } else if (result.is_zero() || argument.is_zero()) {
          result = ArithmeticValue{};
        } else {
          result = ArithmeticValue::absolute(
              ArithmeticValue::multiply(
                  ArithmeticValue::divide(result, divisor, arena), argument,
                  arena),
              arena);
        }
      }
      return result;
    }
    case calculator_function::IsEven:
    case calculator_function::IsOdd: {
      require_argument_count(name, arguments, 1, 1);
      require_integer_argument(name, arguments[0]);
      let const remainder = ArithmeticValue::modulo(
          ArithmeticValue::absolute(
              ArithmeticValue::integer_part(arguments[0], arena), arena),
          ArithmeticValue{2}, arena);
      let const is_odd = !remainder.is_zero();
      return ArithmeticValue{
          (*function == calculator_function::IsOdd) == is_odd ? 1 : 0};
    }
    case calculator_function::IsInt:
      require_argument_count(name, arguments, 1, 1);
      return ArithmeticValue{arguments[0].has_integer_value(arena) ? 1 : 0};
    case calculator_function::Length: {
      require_argument_count(name, arguments, 1, 1);
      let const text = arguments[0].to_string(bump_allocator(arena));
      usize digit_count = 0;
      for (usize position = 0; position < text.count(); position++) {
        if (text[position] >= '0' && text[position] <= '9') {
          digit_count++;
        }
      }
      let const body_start = text[0] == '-' ? usize{1} : usize{0};
      if (text.count() > body_start + 1 && text[body_start] == '0' &&
          text[body_start + 1] == '.')
        digit_count--;
      return ArithmeticValue{static_cast<i64>(digit_count)};
    }
    case calculator_function::Ln:
      require_argument_count(name, arguments, 1, 2);
      if (arguments[0].is_zero() || arguments[0].is_negative())
        fail_span(arguments[0].start, arguments[0].end,
                  "Logarithm operand is not positive",
                  "ln requires a value greater than 0");
      return calculator_ln(arguments[0],
                           calculator_requested_scale(name, arguments));
    case calculator_function::Max:
    case calculator_function::Min: {
      require_argument_count(name, arguments, 1, ~usize{0});
      let result = arguments[0];
      for (usize index = 1; index < arguments.count(); index++) {
        let const ordering =
            result.compare(arguments[index], bump_allocator(arena));
        if ((*function == calculator_function::Max && ordering < 0) ||
            (*function == calculator_function::Min && ordering > 0))
          result = arguments[index];
      }
      return result;
    }
    case calculator_function::Scale:
      require_argument_count(name, arguments, 1, 1);
      return ArithmeticValue{
          static_cast<i64>(arguments[0].get_decimal_scale())};
    case calculator_function::Sgn:
      require_argument_count(name, arguments, 1, 1);
      return ArithmeticValue{
          arguments[0].is_zero() ? 0 : (arguments[0].is_negative() ? -1 : 1)};
    case calculator_function::Sin:
      require_argument_count(name, arguments, 1, 2);
      return calculator_sin_cos(
          arguments[0], calculator_requested_scale(name, arguments), false);
    case calculator_function::Sqrt: {
      require_argument_count(name, arguments, 1, 2);
      if (arguments[0].is_negative())
        fail_span(arguments[0].start, arguments[0].end,
                  "Square root operand is negative",
                  "sqrt requires a value greater than or equal to 0");
      u32 decimal_scale = arguments[0].get_decimal_scale() > 20
                              ? arguments[0].get_decimal_scale()
                              : 20;
      if (arguments.count() == 2) {
        let const requested_scale =
            require_integer_argument(name, arguments[1]);
        if (requested_scale < 0 || requested_scale > 100000)
          fail_span(arguments[1].start, arguments[1].end,
                    "Invalid scale for 'sqrt'",
                    "The scale must be between 0 and 100000");
        decimal_scale = static_cast<u32>(requested_scale);
      }
      return ArithmeticValue::square_root(arguments[0], decimal_scale, arena);
    }
    }
    unreachable();
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
    let const has_leading_decimal_point =
        is_exact && is_leading_decimal_number(source, pos);
    if (pos < source.length &&
        (lexer::is_number(source[pos]) || has_leading_decimal_point))
    {
      if (is_exact) {
        let value = ArithmeticValue{};
        pos += lex_exact_arith_number(source.substring(pos), &value, arena);
        return value;
      }
      i64 value = 0;
      pos += lex_arith_number(source.substring(pos), &value);
      return ArithmeticValue{value};
    }
    if (pos < source.length && lexer::is_variable_name_start(source[pos])) {
      const lvalue target = read_lvalue();
      if (should_error_unset && !target.subscript.has_value() && consume("("))
        return apply_calculator_function(target.name, target.name_position);
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
                                 BumpArena &arena) throws -> usize
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
        static_cast<u32>(base), arena);
    return consumed;
  }

  let const decimal_integer_count =
      arithmetic_internal::count_leading_digits(from, 10);
  if (decimal_integer_count < from.length && from[decimal_integer_count] == '.')
  {
    let const fractional_count = arithmetic_internal::count_leading_digits(
        from.substring(decimal_integer_count + 1), 10);
    if (decimal_integer_count == 0 && fractional_count == 0) {
      *out_value = ArithmeticValue{};
      return 0;
    }
    let const consumed = decimal_integer_count + 1 + fractional_count;
    *out_value = ArithmeticValue::parse_decimal(
        from.substring_of_length(0, consumed), arena);
    return consumed;
  }

  let const detected = arithmetic_internal::detect_radix_prefix(from);
  let digit_count = arithmetic_internal::count_leading_digits(
      from.substring(detected.prefix_length), detected.radix);
  if (digit_count == 0 && detected.prefix_length == 0) digit_count = 1;
  let const consumed = detected.prefix_length + digit_count;
  *out_value = ArithmeticValue::parse(
      from.substring_of_length(detected.prefix_length, digit_count),
      detected.radix, arena);
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
    let const has_leading_decimal_point = is_leading_decimal_number(src, i);
    if (lexer::is_number(current_byte) || has_leading_decimal_point) {
      i64 value = 0;
      usize consumed;
      if (has_leading_decimal_point) {
        consumed = 1 + arithmetic_internal::count_leading_digits(
                           src.substring(i + 1), 10);
      } else {
        consumed = lex_arith_number(src.substring(i), &value);
        if (i + consumed < src.length && src[i + consumed] == '.') {
          consumed++;
          consumed += arithmetic_internal::count_leading_digits(
              src.substring(i + consumed), 10);
        }
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

static fn bc_apply_binop(char kind, const ArithmeticValue &lhs,
                         const ArithmeticValue &rhs, u32 scale,
                         BumpArena &arena) throws -> ArithmeticValue
{
  if ((kind == '/' || kind == '%') && rhs.is_zero())
    throw ErrorWithDetails{"Division by zero",
                           "The right operand evaluated to 0"};

  switch (kind) {
  case '*': {
    let const combined_scale =
        static_cast<u64>(lhs.get_decimal_scale()) + rhs.get_decimal_scale();
    let desired_scale = lhs.get_decimal_scale() > rhs.get_decimal_scale()
                            ? lhs.get_decimal_scale()
                            : rhs.get_decimal_scale();
    if (scale > desired_scale) desired_scale = scale;
    if (combined_scale < desired_scale)
      desired_scale = static_cast<u32>(combined_scale);
    return ArithmeticValue::rescale(ArithmeticValue::multiply(lhs, rhs, arena),
                                    desired_scale, arena);
  }
  case '/': {
    u32 prepared_scale = lhs.get_decimal_scale();
    if (scale > prepared_scale) prepared_scale = scale;
    let const prepared = ArithmeticValue::rescale(lhs, prepared_scale, arena);
    return ArithmeticValue::rescale(
        ArithmeticValue::divide(prepared, rhs, arena), scale, arena);
  }
  case '%': {
    let const quotient = bc_apply_binop('/', lhs, rhs, scale, arena);
    let const product = ArithmeticValue::multiply(quotient, rhs, arena);
    let const remainder = ArithmeticValue::subtract(lhs, product, arena);
    let const desired_scale_wide =
        static_cast<u64>(scale) + rhs.get_decimal_scale();
    if (desired_scale_wide > ~u32{0}) throw std::bad_alloc{};
    let desired_scale = static_cast<u32>(desired_scale_wide);
    if (lhs.get_decimal_scale() > desired_scale)
      desired_scale = lhs.get_decimal_scale();
    return ArithmeticValue::rescale(remainder, desired_scale, arena);
  }
  case 'P': {
    if (!rhs.has_integer_value(arena))
      throw ErrorWithDetails{"Exponent is not an integer",
                             "'^' requires an integer exponent"};
    let const exponent = ArithmeticValue::integer_part(rhs, arena);
    let const is_negative = exponent.is_negative();
    let const magnitude = ArithmeticValue::absolute(exponent, arena);
    let const powered = ArithmeticValue::power(lhs, magnitude, arena);
    if (is_negative)
      return bc_apply_binop('/', ArithmeticValue{1}, powered, scale, arena);
    let const exponent_count = magnitude.checked_i64();
    let const combined_scale = static_cast<u64>(lhs.get_decimal_scale()) *
                               static_cast<u64>(exponent_count);
    let desired_scale =
        lhs.get_decimal_scale() > scale ? lhs.get_decimal_scale() : scale;
    if (combined_scale < desired_scale)
      desired_scale = static_cast<u32>(combined_scale);
    return ArithmeticValue::rescale(powered, desired_scale, arena);
  }
  default: return arith_apply_binop(kind, lhs, rhs, true, arena);
  }
}

/* Uses the same helpers as the char parser's ladder so the fast path and the
   full parser agree. */
hot static fn arith_apply_binop(char kind, const ArithmeticValue &lhs,
                                const ArithmeticValue &rhs, bool is_exact,
                                BumpArena &arena, Maybe<u32> bc_scale) throws
    -> ArithmeticValue
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

  if (bc_scale.has_value() &&
      (kind == '*' || kind == '/' || kind == '%' || kind == 'P'))
    return bc_apply_binop(kind, lhs, rhs, *bc_scale, arena);

  let const allocator = bump_allocator(arena);

  switch (kind) {
  case 'P': return ArithmeticValue::power(lhs, rhs, arena);
  case '*': return ArithmeticValue::multiply(lhs, rhs, arena);
  case '/':
    if (rhs.is_zero()) {
      throw ErrorWithDetails{"Division by zero",
                             "The right operand evaluated to 0"};
    }
    return ArithmeticValue::divide(lhs, rhs, arena);
  case '%':
    if (rhs.is_zero()) {
      throw ErrorWithDetails{"Division by zero",
                             "The right operand evaluated to 0"};
    }
    return ArithmeticValue::modulo(lhs, rhs, arena);
  case '+': return ArithmeticValue::add(lhs, rhs, arena);
  case '-': return ArithmeticValue::subtract(lhs, rhs, arena);
  case 'L': return ArithmeticValue::shift_left(lhs, rhs, arena);
  case 'R': return ArithmeticValue::shift_right(lhs, rhs, arena);
  case '<': return ArithmeticValue{lhs.compare(rhs, allocator) < 0 ? 1 : 0};
  case 'l': return ArithmeticValue{lhs.compare(rhs, allocator) <= 0 ? 1 : 0};
  case '>': return ArithmeticValue{lhs.compare(rhs, allocator) > 0 ? 1 : 0};
  case 'g': return ArithmeticValue{lhs.compare(rhs, allocator) >= 0 ? 1 : 0};
  case 'e': return ArithmeticValue{lhs.compare(rhs, allocator) == 0 ? 1 : 0};
  case 'n': return ArithmeticValue{lhs.compare(rhs, allocator) != 0 ? 1 : 0};
  case '&': return ArithmeticValue::bit_and(lhs, rhs, arena);
  case '^': return ArithmeticValue::bit_xor(lhs, rhs, arena);
  case '|': return ArithmeticValue::bit_or(lhs, rhs, arena);
  default:
    unreachable("the cached arithmetic evaluator received invalid binary "
                "operator '%c'",
                kind);
  }
}

static fn evaluate_named_value_operand(EvalContext *context, StringView value,
                                       bool is_exact, BumpArena &arena) throws
    -> ArithmeticValue
{
  if (value.is_empty()) return ArithmeticValue{};
  if (!is_exact) {
    if (let const literal = try_parse_single_integer_literal(value);
        literal.has_value())
      return ArithmeticValue{literal.value()};
  }

  ArithmeticParser nested{context, value, is_exact, arena};
  return nested.parse();
}

static fn arith_read_variable(EvalContext *context, StringView name,
                              bool is_exact, BumpArena &arena) throws
    -> ArithmeticValue
{
  ASSERT(context != nullptr);
  if (let const *stored = context->lookup_shell_variable(name);
      stored != nullptr)
  {
    return evaluate_named_value_operand(context, stored->view(), is_exact,
                                        arena);
  }
  let const value = context->get_variable_value(name);
  if (!value.has_value()) {
    context->report_unset_reference(name);
    return ArithmeticValue{};
  }

  return evaluate_named_value_operand(context, value->view(), is_exact, arena);
}

/* A precedence-climbing evaluator over the cached token stream for a simple
   expression with no assignment, ternary, comma, or short-circuit. */
class ArithmeticTokenEvaluator
{
public:
  ArithmeticTokenEvaluator(EvalContext *context_value,
                           const ArrayList<arith_token> &tokens_value,
                           bool is_exact_value, BumpArena &arena_value)
      : context{context_value}, toks{tokens_value}, is_exact{is_exact_value},
        arena{arena_value}
  {}

  EvalContext *context;
  const ArrayList<arith_token> &toks;
  usize ti{0};
  usize depth{0};
  bool is_exact{false};
  BumpArena &arena;
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
                               is_exact, arena);
    }
    if (at_op("!")) {
      ti++;
      return ArithmeticValue{parse_operand().is_zero() ? 1 : 0};
    }
    if (at_op("~")) {
      ti++;
      let const value = parse_operand();
      return is_exact ? ArithmeticValue::bit_not(value, arena)
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
      if (is_exact) lex_exact_arith_number(toks[ti].text, &value, arena);
      ti++;
      return value;
    }
    if (ti < toks.count() && toks[ti].k == arith_token::kind::name) {
      let const name = toks[ti].text;
      ti++;
      return arith_read_variable(context, name, is_exact, arena);
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
      lhs = arith_apply_binop(op.kind, lhs, rhs, is_exact, arena);
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
                             bool is_exact, BumpArena &arena,
                             bool should_error_unset = false,
                             Maybe<u32> bc_scale = {}) throws -> ArithmeticValue
{
  LOG(All, "evaluating the arithmetic expression of %zu bytes",
      expression.length);

  if (!expression.find_character('$').has_value() &&
      !expression.find_character('`').has_value())
  {
    let parser = ArithmeticParser{context, expression, is_exact, arena,
                                  0,       false,      bc_scale};
    parser.should_error_unset = should_error_unset;
    if (expression_base != nullptr) parser.precise_base = *expression_base;
    return parser.parse();
  }

  LOG(All, "expanding parameters inside the arithmetic before the parse");
  let const expanded_word =
      context->expand_modifier_word(expression, true, true, expression_base);
  let parser = ArithmeticParser{
      context, expanded_word.view(), is_exact, arena, 0, false, bc_scale};
  parser.should_error_unset = should_error_unset;
  return parser.parse();
}

} /* namespace */

fn EvalContext::evaluate_arithmetic(
    StringView expression, const SourceLocation *expression_base) throws -> i64
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = is_extended_arithmetic_enabled();
  let const value = evaluate_arithmetic_value(this, expression, expression_base,
                                              is_exact, m_scratch_arena);
  return is_exact ? value.checked_i64() : value.wrapped_i64();
}

fn EvalContext::evaluate_arithmetic_text(
    StringView expression, const SourceLocation *expression_base) throws
    -> String
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = is_extended_arithmetic_enabled();
  let const value = evaluate_arithmetic_value(this, expression, expression_base,
                                              is_exact, m_scratch_arena);
  return value.to_string(heap_allocator());
}

fn EvalContext::evaluate_calculator_arithmetic_text(
    StringView expression, const SourceLocation *expression_base) throws
    -> String
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = is_extended_arithmetic_enabled();
  const SourceLocation synthetic_base{0, 0};
  let const base =
      expression_base != nullptr ? expression_base : &synthetic_base;
  let const value = evaluate_arithmetic_value(this, expression, base, is_exact,
                                              m_scratch_arena, true);
  return value.to_string(heap_allocator());
}

fn EvalContext::evaluate_bc_arithmetic_text(StringView expression,
                                            u32 scale) throws -> String
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  const SourceLocation expression_base{0, 0};
  let const value = evaluate_arithmetic_value(
      this, expression, &expression_base, true, m_scratch_arena, true, scale);
  return value.to_string(heap_allocator());
}

fn EvalContext::evaluate_arithmetic_nonzero(
    StringView expression, const SourceLocation *expression_base) throws -> bool
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = is_extended_arithmetic_enabled();
  return !evaluate_arithmetic_value(this, expression, expression_base, is_exact,
                                    m_scratch_arena)
              .is_zero();
}

fn EvalContext::compare_arithmetic(StringView left, StringView right) throws
    -> i32
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = is_extended_arithmetic_enabled();
  let const left_value =
      evaluate_arithmetic_value(this, left, nullptr, is_exact, m_scratch_arena);
  let const right_value = evaluate_arithmetic_value(this, right, nullptr,
                                                    is_exact, m_scratch_arena);
  return left_value.compare(right_value, scratch_allocator());
}

namespace {

fn evaluate_arithmetic_cached_value(EvalContext *context, StringView expression,
                                    ArrayList<arith_token> &tokens,
                                    bool &is_tokenized, bool &is_simple,
                                    const SourceLocation *source_location,
                                    bool is_exact, BumpArena &arena) throws
    -> ArithmeticValue
{
  if (!is_tokenized) {
    if (expression.find_character('$').has_value() ||
        expression.find_character('`').has_value())
    {
      return evaluate_arithmetic_value(context, expression, source_location,
                                       is_exact, arena);
    }

    tokens.clear();
    try {
      tokenize_arithmetic(expression, tokens);
    } catch (...) {
      tokens.clear();
      is_tokenized = true;
      is_simple = false;
      return evaluate_arithmetic_value(context, expression, source_location,
                                       is_exact, arena);
    }
    is_tokenized = true;
    is_simple = arith_tokens_are_simple(tokens);
  }

  if (!is_simple)
    return evaluate_arithmetic_value(context, expression, source_location,
                                     is_exact, arena);

  ArithmeticTokenEvaluator evaluator{context, tokens, is_exact, arena};
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

  let &cache = segment.get_eval_cache();
  if (cache.arith == nullptr ||
      !cache_arena->is_lifetime_valid(cache.arithmetic_lifetime))
  {
    cache.arith =
        cache_arena->create<arith_token_cache>(bump_allocator(*cache_arena));
    cache.arithmetic_lifetime = cache_arena->register_lifetime();
  }

  let const is_exact = is_extended_arithmetic_enabled();
  if (is_exact && cache.arith->has_exact_constant_text) {
    return String{scratch_allocator(), cache.arith->exact_constant_text.view()};
  }
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const value = evaluate_arithmetic_cached_value(
      this, segment.text.view(), cache.arith->tokens, cache.arith->is_tokenized,
      cache.arith->is_simple,
      source_location.has_value() ? &*source_location : nullptr, is_exact,
      m_scratch_arena);
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
          String{bump_allocator(*cache_arena), result.view()};
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
  let const is_exact = is_extended_arithmetic_enabled();
  let const value = evaluate_arithmetic_cached_value(
      this, expression, tokens, is_tokenized, is_simple, source_location,
      is_exact, m_scratch_arena);
  return is_exact ? value.checked_i64() : value.wrapped_i64();
}

fn EvalContext::evaluate_arithmetic_cached_clause_nonzero(
    StringView expression, ArrayList<arith_token> &tokens, bool &is_tokenized,
    bool &is_simple, const SourceLocation *source_location) throws -> bool
{
  let const scratch = scratch_mark();
  defer { scratch_release(scratch); };
  let const is_exact = is_extended_arithmetic_enabled();
  return !evaluate_arithmetic_cached_value(
              this, expression, tokens, is_tokenized, is_simple,
              source_location, is_exact, m_scratch_arena)
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
  let parser = ArithmeticParser{nullptr, expression, false, arena};
  return parser.parse().wrapped_i64();
}

fn evaluate_constant_arithmetic_nonzero(StringView expression,
                                        bool is_exact) throws -> bool
{
  let &arena = get_constant_arithmetic_arena();
  let const scratch = arena.mark();
  defer { arena.release(scratch); };
  let parser = ArithmeticParser{nullptr, expression, is_exact, arena};
  return !parser.parse().is_zero();
}

fn evaluate_constant_arithmetic_text(StringView expression,
                                     Allocator allocator) throws -> String
{
  let &arena = get_constant_arithmetic_arena();
  let const scratch = arena.mark();
  defer { arena.release(scratch); };
  let parser = ArithmeticParser{nullptr, expression, true, arena};
  return parser.parse().to_string(allocator);
}

pure fn obvious_xor_power_operator_position(StringView expression) wontthrow
    -> Maybe<usize>
{
  usize position = 0;
  while (position < expression.length &&
         lexer::is_whitespace(expression[position]))
    position++;
  let const left_start = position;
  while (position < expression.length && lexer::is_number(expression[position]))
    position++;
  if (position == left_start) return None;
  while (position < expression.length &&
         lexer::is_whitespace(expression[position]))
    position++;
  if (position >= expression.length || expression[position] != '^') return None;
  let const operator_position = position++;
  if (position < expression.length && expression[position] == '=') return None;
  while (position < expression.length &&
         lexer::is_whitespace(expression[position]))
    position++;
  let const right_start = position;
  while (position < expression.length && lexer::is_number(expression[position]))
    position++;
  if (position == right_start) return None;
  while (position < expression.length &&
         lexer::is_whitespace(expression[position]))
    position++;
  if (position != expression.length) return None;

  return operator_position;
}

} /* namespace koshka */
