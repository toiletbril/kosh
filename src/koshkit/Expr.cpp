#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("expression");

HELP_DESCRIPTION_DECL("The expr utility evaluates an operand expression.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Expr);

namespace koshka::koshkit {

static pure fn expr_value_is_true(StringView value) wontthrow -> bool
{
  return !value.is_empty() && value != "0";
}

class ExprParser
{
public:
  ExprParser(const ArrayList<String> &tokens, Allocator allocator)
      : m_tokens(tokens), m_allocator(allocator)
  {}

  fn parse() throws -> String
  {
    let result = parse_or();
    if (m_position != m_tokens.count()) throw Error{"expr: syntax error"};
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
    if (m_position == m_tokens.count()) throw Error{"expr: missing operand"};
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
      if ((operation.view() == "/" || operation.view() == "%") &&
          right_number == 0)
        throw Error{"expr: division by zero"};

      i128 result = 0;
      if (operation.view() == "*")
        result = static_cast<i128>(left_number) * right_number;
      else if (operation.view() == "/") {
        if (left_number == INT64_MIN && right_number == -1)
          throw Error{"expr: integer overflow"};
        result = left_number / right_number;
      } else {
        result = left_number % right_number;
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
      let const pattern = parse_primary();
      String anchored{m_allocator, "^"};
      anchored += pattern.view();
      os::compiled_regex compiled;
      if (os::compile_basic_regex(anchored.view(),
                                  os::case_sensitivity::Sensitive,
                                  compiled) != os::regex_compile_result::Ok)
        throw Error{"expr: invalid regular expression"};
      defer { os::free_regex(compiled); };

      let spans = ArrayList<os::regex_span>{m_allocator};
      String error_message{m_allocator};
      let const result = os::execute_regex(compiled, left.view(), spans,
                                           error_message, m_allocator);
      if (result == os::regex_match_result::Error)
        throw Error{"expr: " + error_message};
      if (result == os::regex_match_result::NoMatch) {
        left = String{m_allocator, "0"};
      } else if (spans.count() > 1) {
        if (spans[1].start < 0)
          left = String{m_allocator};
        else
          left = String{m_allocator,
                        left.view().substring_of_length(
                            static_cast<usize>(spans[1].start),
                            static_cast<usize>(spans[1].end - spans[1].start))};
      } else {
        left = String::from(spans[0].end - spans[0].start, m_allocator);
      }
    }
    return left;
  }

  fn parse_primary() throws -> String
  {
    if (peek("(")) {
      m_position++;
      let result = parse_or();
      if (!peek(")")) throw Error{"expr: missing closing parenthesis"};
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
      throw Error{"expr: expected integer, got '" + String{value} + "'"};
    return parsed.value();
  }

  fn number_string(i128 value) const throws -> String
  {
    if (value < INT64_MIN || value > INT64_MAX)
      throw Error{"expr: integer overflow"};
    return String::from(static_cast<i64>(value), m_allocator);
  }

  const ArrayList<String> &m_tokens;
  Allocator m_allocator;
  usize m_position{0};
};

Expr::Expr() = default;

pure fn Expr::kind() const wontthrow -> Utility::Kind { return Kind::Expr; }

fn Expr::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, nullptr, true);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  ExprParser parser{operands, cxt.scratch_allocator()};
  let result = parser.parse();
  let const status = expr_value_is_true(result.view()) ? 0 : 1;
  result += '\n';
  ec.print_to_stdout(result);
  return status;
}

} // namespace koshka::koshkit
