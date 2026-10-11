/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the expr utility. Its recursive-descent parser evaluates
 * boolean, comparison, arithmetic, regular-expression, and grouping operators
 * over command operands.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

KOSHKIT_UTIL_DECL("expression",
                  "The expr utility evaluates an operand expression.");

REGISTER_KOSHKIT_UTIL_FLAGS(Expr);

namespace koshka::koshkit {

static pure fn expr_value_is_true(StringView value) wontthrow -> bool
{
  return !value.is_empty() && value != "0";
}

class ExprParser
{
public:
  ExprParser(const ArrayList<String> &tokens,
             const ArrayList<SourceLocation> &token_locations,
             Allocator allocator)
      : m_tokens(tokens), m_token_locations(token_locations),
        m_allocator(allocator)
  {}

  fn parse() throws -> String
  {
    let result = parse_or();
    if (m_position != m_tokens.count()) throw Error{"syntax error"};
    return result;
  }

private:
  enum class ComparisonOperatorKind : uchar
  {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
  };

  pure fn peek_comparison_operator() const wontthrow
      -> Maybe<ComparisonOperatorKind>
  {
    if (m_position == m_tokens.count()) return None;
    let const token = m_tokens[m_position].view();
    if (token.length == 1) {
      switch (token[0]) {
      case '=': return ComparisonOperatorKind::Equal;
      case '<': return ComparisonOperatorKind::Less;
      case '>': return ComparisonOperatorKind::Greater;
      default: return None;
      }
    }
    if (token.length != 2 || token[1] != '=') return None;
    switch (token[0]) {
    case '!': return ComparisonOperatorKind::NotEqual;
    case '<': return ComparisonOperatorKind::LessEqual;
    case '>': return ComparisonOperatorKind::GreaterEqual;
    default: return None;
    }
  }

  pure fn peek(StringView token) const wontthrow -> bool
  {
    return m_position < m_tokens.count() &&
           m_tokens[m_position].view() == token;
  }

  fn take() throws -> String
  {
    if (m_position == m_tokens.count()) throw Error{"missing operand"};
    return String{m_allocator, m_tokens[m_position++].view()};
  }

  fn parse_or() throws -> String
  {
    let left = parse_and();
    while (peek("|")) {
      m_position++;
      let right = parse_and();
      if (!expr_value_is_true(left.view())) left = steal(right);
    }
    return left;
  }

  fn parse_and() throws -> String
  {
    let left = parse_comparison();
    while (peek("&")) {
      m_position++;
      let right = parse_comparison();
      if (!expr_value_is_true(left.view()) || !expr_value_is_true(right.view()))
        left = String{m_allocator, "0"};
    }
    return left;
  }

  fn parse_comparison() throws -> String
  {
    let left = parse_addition();
    loop
    {
      let const operation = peek_comparison_operator();
      if (!operation.has_value()) break;
      m_position++;
      let right = parse_addition();
      let const left_number = left.view().to<i64>();
      let const right_number = right.view().to<i64>();
      int order = 0;
      if (!left_number.is_error() && !right_number.is_error())
        order = (left_number.value() > right_number.value()) -
                (left_number.value() < right_number.value());
      else
        order = left.view() < right.view()   ? -1
                : right.view() < left.view() ? 1
                                             : 0;

      bool matches = false;
      switch (*operation) {
      case ComparisonOperatorKind::Equal: matches = order == 0; break;
      case ComparisonOperatorKind::NotEqual: matches = order != 0; break;
      case ComparisonOperatorKind::Less: matches = order < 0; break;
      case ComparisonOperatorKind::LessEqual: matches = order <= 0; break;
      case ComparisonOperatorKind::Greater: matches = order > 0; break;
      case ComparisonOperatorKind::GreaterEqual: matches = order >= 0; break;
      }
      left = String{m_allocator, matches ? "1" : "0"};
    }
    return left;
  }

  fn parse_addition() throws -> String
  {
    let left = parse_multiplication();
    while (peek("+") || peek("-")) {
      let const operation = take();
      let const right = parse_multiplication();
      let const left_number = require_number(left.view());
      let const right_number = require_number(right.view());
      let const result = operation.view() == "+"
                             ? static_cast<i128>(left_number) + right_number
                             : static_cast<i128>(left_number) - right_number;
      left = number_string(result);
    }
    return left;
  }

  fn parse_multiplication() throws -> String
  {
    let left = parse_match();
    while (peek("*") || peek("/") || peek("%")) {
      let const operation = take();
      let const right = parse_match();
      let const left_number = require_number(left.view());
      let const right_number = require_number(right.view());
      let const operator_byte = operation.view()[0];
      if ((operator_byte == '/' || operator_byte == '%') && right_number == 0) {
        throw Error{"division by zero"};
      }

      i128 result = 0;
      switch (operator_byte) {
      case '*': result = static_cast<i128>(left_number) * right_number; break;
      case '/':
        if (left_number == INT64_MIN && right_number == -1)
          throw Error{"integer overflow"};
        result = left_number / right_number;
        break;
      default:
        result = right_number == -1 ? 0 : left_number % right_number;
        break;
      }
      left = number_string(result);
    }
    return left;
  }

  fn parse_match() throws -> String
  {
    let left = parse_primary();
    while (peek(":")) {
      m_position++;
      let const pattern_position = m_position;
      let const pattern = parse_primary();
      String anchored{m_allocator, "^"};
      anchored += pattern.view();
      let compiled = os::compile_basic_regex(anchored.view(),
                                             os::case_sensitivity::Sensitive);
      if (!compiled.has_value()) {
        throw ErrorWithLocationAndDetails{
            m_token_locations[pattern_position], "invalid regular expression",
            "use a valid basic regular expression"};
      }
      defer { os::free_regex(*compiled); };

      let const match = os::execute_regex(
          *compiled, os::regex_execution_options{left.view(), m_allocator});
      if (match.result == os::regex_match_result::Error)
        throw Error{"" + match.error_message};
      if (match.result == os::regex_match_result::NoMatch) {
        left = String{m_allocator, "0"};
      } else if (match.spans.count() > 1) {
        if (match.spans[1].start < 0)
          left = String{m_allocator};
        else
          left = String{m_allocator,
                        left.view().substring_of_length(
                            static_cast<usize>(match.spans[1].start),
                            static_cast<usize>(match.spans[1].end -
                                               match.spans[1].start))};
      } else {
        left = String::from(match.spans[0].end - match.spans[0].start,
                            m_allocator);
      }
    }
    return left;
  }

  fn parse_primary() throws -> String
  {
    if (peek("(")) {
      m_position++;
      let result = parse_or();
      if (!peek(")")) throw Error{"missing closing parenthesis"};
      m_position++;
      return result;
    }
    if (peek("+")) {
      m_position++;
      return take();
    }
    return take();
  }

  fn require_number(StringView value) const throws -> i64
  {
    let const parsed = value.to<i64>();
    if (parsed.is_error())
      throw Error{"expected integer, got '" + String{value} + "'"};
    return parsed.value();
  }

  fn number_string(i128 value) const throws -> String
  {
    if (value < INT64_MIN || value > INT64_MAX) throw Error{"integer overflow"};
    return String::from(static_cast<i64>(value), m_allocator);
  }

  const ArrayList<String> &m_tokens;
  const ArrayList<SourceLocation> &m_token_locations;
  Allocator m_allocator;
  usize m_position{0};
};

fn Expr::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  KOSHKIT_PARSE_OPERANDS_OR_HELP(
      args, arg_locations, {.should_accept_negative_number_operand = true});

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  ExprParser parser{operands, operand_locations, cxt.scratch_allocator()};
  let result = parser.parse();
  let const status = expr_value_is_true(result.view()) ? 0 : 1;
  result += '\n';
  ec.print_to_stdout(result);
  return status;
}

} /* namespace koshka::koshkit */
