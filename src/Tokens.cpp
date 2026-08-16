#include "Tokens.hpp"

#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Trace.hpp"

namespace koshka {

Token::Token(SourceLocation location) : m_location(location) {}

pure fn Token::source_location() const wontthrow -> SourceLocation
{
  return m_location;
}

fn Token::operator delete(opaque *pointer) wontthrow -> void
{
  if (is_arena_pointer(pointer)) return;
  ::operator delete(pointer);
}

cold fn Token::to_ast_string() const throws -> String { return raw_string(); }

fn Token::raw_view() const wontthrow -> Maybe<StringView> { return None; }

pure fn WordSegment::is_split_eligible() const wontthrow -> bool
{
  return kind == Kind::UnquotedText ||
         (kind == Kind::VariableReference && !is_in_double_quotes);
}

pure fn WordSegment::has_live_glob_chars() const wontthrow -> bool
{
  return kind == Kind::UnquotedText;
}

pure fn WordSegment::is_tilde_candidate() const wontthrow -> bool
{
  return kind == Kind::UnquotedText;
}

pure fn WordSegment::has_glob_metacharacter() const wontthrow -> bool
{
  return optimizer::word_segment_has_glob_metacharacter(*this);
}

pure fn Word::is_empty() const wontthrow -> bool { return segments.is_empty(); }

hot fn Word::to_literal_string() const throws -> String
{
  let result = String{heap_allocator()};
  for (let const &segment : segments) {
    switch (segment.kind) {
    case WordSegment::Kind::CommandSubstitution:
      result += "$(";
      result += segment.text;
      result += ")";
      continue;
    case WordSegment::Kind::FunctionSubstitution:
      result += "${ ";
      result += segment.text;
      result += " }";
      continue;
    case WordSegment::Kind::ArithmeticExpansion:
      result += "$((";
      result += segment.text;
      result += "))";
      continue;
    case WordSegment::Kind::VariableReference: result += '$'; break;
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::ProcessSubstitution: break;
    }
    result += segment.text;
  }
  return result;
}

pure fn Word::plain_literal_kind() const wontthrow -> PlainLiteral
{
  if (!m_has_cached_plain_kind) {
    m_cached_plain_kind = optimizer::classify_plain_literal(*this);
    m_has_cached_plain_kind = true;
  }
  return m_cached_plain_kind;
}

fn Word::constant_value() const throws -> StringView
{
  if (segments.count() == 1) return segments[0].text.view();
  if (!m_has_constant_value) {
    for (let const &segment : segments)
      m_constant_value.append(segment.text.view());
    m_has_constant_value = true;
  }
  return m_constant_value.view();
}

pure fn Word::is_all_ascii_digits() const wontthrow -> bool
{
  if (segments.is_empty()) return false;
  bool has_seen_digit = false;
  for (let const &segment : segments) {
    switch (segment.kind) {
    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::ArithmeticExpansion:
    case WordSegment::Kind::FunctionSubstitution: return false;
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::ProcessSubstitution: break;
    }

    for (usize i = 0; i < segment.text.count(); i++) {
      const char c = segment.text[i];
      if (c < '0' || c > '9') return false;
      has_seen_digit = true;
    }
  }
  return has_seen_digit;
}

pure fn Word::fd_allocation_name() const wontthrow -> Maybe<StringView>
{
  if (segments.count() != 1) return None;

  let const &segment = segments[0];
  if (segment.kind != WordSegment::Kind::UnquotedText &&
      segment.kind != WordSegment::Kind::LiteralText)
  {
    return None;
  }

  let const text = segment.text.view();
  if (text.length < 3 || text[0] != '{' || text[text.length - 1] != '}') {
    return None;
  }

  let const name = text.substring_of_length(1, text.length - 2);
  if (!lexer::is_variable_name_start(name[0])) return None;
  for (usize i = 1; i < name.length; i++)
    if (!lexer::is_variable_name(name[i])) return None;

  return name;
}

pure fn Word::runs_substitution() const wontthrow -> bool
{
  for (let const &segment : segments) {
    switch (segment.kind) {
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::FunctionSubstitution: return true;
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::ProcessSubstitution:
    case WordSegment::Kind::ArithmeticExpansion: break;
    }
  }
  return false;
}

cold fn Word::to_pretty_string() const throws -> String
{
  let result = String{"[Word"};
  for (let const &segment : segments) {
    result += "\n  ";
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText: result += "Literal"; break;
    case WordSegment::Kind::UnquotedText: result += "Unquoted"; break;
    case WordSegment::Kind::DoubleQuotedText: result += "DoubleQuoted"; break;
    case WordSegment::Kind::VariableReference: result += "Variable"; break;
    case WordSegment::Kind::CommandSubstitution:
      result += "CommandSubstitution";
      break;
    case WordSegment::Kind::ProcessSubstitution:
      result += "ProcessSubstitution";
      break;
    case WordSegment::Kind::ArithmeticExpansion:
      result += "ArithmeticExpansion";
      break;
    case WordSegment::Kind::FunctionSubstitution:
      result += "FunctionSubstitution";
      break;
    }
    result += " \"";
    result += segment.text;
    result += '"';
  }
  result += "\n]";
  return result;
}

static fn append_subscript_segment_source(const WordSegment &segment,
                                          String &out) throws -> bool
{
  switch (segment.kind) {
  case WordSegment::Kind::VariableReference:
    out.append("${");
    out.append(segment.text.view());
    out.push('}');
    return true;
  case WordSegment::Kind::ArithmeticExpansion:
    out.append("$((");
    out.append(segment.text.view());
    out.append("))");
    return true;
  case WordSegment::Kind::CommandSubstitution:
    out.append("$(");
    out.append(segment.text.view());
    out.push(')');
    return true;
  default: return false;
  }
}

/* An array element assignment whose subscript holds an expansion, the $k in
   v[$k]=1, splits across segments since the = lands after the ] in a later
   segment. */
static fn
array_element_assignment_split(const ArrayList<WordSegment> &segments) throws
    -> Maybe<word_assignment_split>
{
  const WordSegment &first = segments[0];
  if (first.text.is_empty() || !lexer::is_variable_name_start(first.text[0])) {
    return koshka::None;
  }

  usize name_end = 1;
  while (name_end < first.text.count() &&
         lexer::is_variable_name(first.text[name_end]))
    name_end++;
  if (name_end >= first.text.count() || first.text[name_end] != '[') {
    return koshka::None;
  }

  let subscript = String{heap_allocator()};
  /* A ] in the rest of segment 0 means the = sits in segment 0 too, already
     ruled out, so this is not an assignment. */
  const StringView head = first.text.substring(name_end + 1);
  if (head.find_character(']').has_value()) return koshka::None;
  subscript.append(head);

  for (usize i = 1; i < segments.count(); i++) {
    let const &segment = segments[i];
    const bool is_text = segment.kind == WordSegment::Kind::UnquotedText ||
                         segment.kind == WordSegment::Kind::DoubleQuotedText ||
                         segment.kind == WordSegment::Kind::LiteralText;
    if (!is_text) {
      if (!append_subscript_segment_source(segment, subscript))
        return koshka::None;
      continue;
    }

    let const close = segment.text.find_character(']');
    if (!close.has_value()) {
      subscript.append(segment.text.view());
      continue;
    }

    subscript.append(segment.text.substring_of_length(0, *close));
    const StringView after = segment.text.substring(*close + 1);
    bool is_append = false;
    usize value_start = 0;
    if (after.starts_with("+=")) {
      is_append = true;
      value_start = 2;
    } else if (after.starts_with("=")) {
      value_start = 1;
    } else {
      return koshka::None;
    }

    let key = String{first.text.substring_of_length(0, name_end)};
    key.push('[');
    key.append(subscript.view());
    key.push(']');

    LOG(All, "folding the subscript into array element key '%s'", key.c_str());

    let value = Word{};
    value.segments.push(WordSegment{WordSegment::Kind::UnquotedText,
                                    String{after.substring(value_start)},
                                    false});
    for (usize j = i + 1; j < segments.count(); j++)
      value.segments.push(segments[j]);

    return word_assignment_split{steal(key), steal(value), is_append};
  }

  return koshka::None;
}

hot fn Word::get_assignment_split() const throws -> Maybe<word_assignment_split>
{
  if (segments.is_empty()) return koshka::None;

  const WordSegment &first = segments[0];
  if (first.kind != WordSegment::Kind::UnquotedText) return koshka::None;

  let const equals_position = first.text.find_character('=');
  if (!equals_position.has_value()) {
    if (first.text.find_character('[').has_value())
      return array_element_assignment_split(segments);
    return koshka::None;
  }
  if (*equals_position == 0) return koshka::None;

  ASSERT(*equals_position <= first.text.count());

  const bool is_append = first.text[*equals_position - 1] == '+';
  const usize name_length = is_append ? *equals_position - 1 : *equals_position;
  if (name_length == 0) return koshka::None;

  if (!lexer::is_variable_name_start(first.text[0])) return koshka::None;
  usize name_cursor = 1;
  while (name_cursor < name_length &&
         lexer::is_variable_name(first.text[name_cursor]))
    name_cursor++;
  if (name_cursor < name_length && first.text[name_cursor] == '[') {
    if (first.text[name_length - 1] != ']' || name_length - name_cursor < 3) {
      return koshka::None;
    }
    name_cursor = name_length;
  }
  if (name_cursor != name_length) return koshka::None;

  let const name_view = first.text.substring_of_length(0, name_length);
  let name = String{name_view};

  let value = Word{};
  /* The value always begins with an unquoted segment, even when empty, so that
     FOO= produces one empty field rather than no field at all. */
  value.segments.push(WordSegment{WordSegment::Kind::UnquotedText,
                                  first.text.substring(*equals_position + 1),
                                  false});
  for (usize i = 1; i < segments.count(); i++)
    value.segments.push(segments[i]);

  return word_assignment_split{steal(name), steal(value), is_append};
}

namespace tokens {

#define KEYWORD_TOKEN_DECLS(t, s)                                              \
  t::t(SourceLocation location) : Token(location) {}                           \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow { return Token::Flag::Keyword; }     \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }

KEYWORD_TOKEN_DECLS(If, "if");
KEYWORD_TOKEN_DECLS(Then, "then");
KEYWORD_TOKEN_DECLS(Else, "else");
KEYWORD_TOKEN_DECLS(Elif, "elif");
KEYWORD_TOKEN_DECLS(Fi, "fi");
KEYWORD_TOKEN_DECLS(For, "for");
KEYWORD_TOKEN_DECLS(While, "while");
KEYWORD_TOKEN_DECLS(Until, "until");
KEYWORD_TOKEN_DECLS(Do, "do");
KEYWORD_TOKEN_DECLS(Done, "done");
KEYWORD_TOKEN_DECLS(Case, "case");
KEYWORD_TOKEN_DECLS(When, "when");
KEYWORD_TOKEN_DECLS(Esac, "esac");
KEYWORD_TOKEN_DECLS(Time, "time");
KEYWORD_TOKEN_DECLS(Function, "function");

#define SENTINEL_TOKEN_DECLS_COMPOUND(t, s)                                    \
  t::t(SourceLocation location) : Token(location) {}                           \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow                                      \
  {                                                                            \
    return Token::Flag::Sentinel | Token::Flag::CompoundList;                  \
  }                                                                            \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }

SENTINEL_TOKEN_DECLS_COMPOUND(Newline, "newline");
SENTINEL_TOKEN_DECLS_COMPOUND(Semicolon, ";");

#define SENTINEL_TOKEN_DECLS(t, s)                                             \
  t::t(SourceLocation location) : Token(location) {}                           \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow { return Token::Flag::Sentinel; }    \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }

SENTINEL_TOKEN_DECLS(EndOfFile, "end of input");
SENTINEL_TOKEN_DECLS(DoubleSemicolon, ";;");
SENTINEL_TOKEN_DECLS(SemicolonAmpersand, ";&");
SENTINEL_TOKEN_DECLS(DoubleSemicolonAmpersand, ";;&");
SENTINEL_TOKEN_DECLS(AmpersandGreater, "&>");
SENTINEL_TOKEN_DECLS(AmpersandDoubleGreater, "&>>");
SENTINEL_TOKEN_DECLS(PipeAmpersand, "|&");
SENTINEL_TOKEN_DECLS(TripleLess, "<<<");
SENTINEL_TOKEN_DECLS(Dot, ".");

SENTINEL_TOKEN_DECLS(LeftParen, "(");
SENTINEL_TOKEN_DECLS(RightParen, ")");
SENTINEL_TOKEN_DECLS(LeftBracket, "{");
SENTINEL_TOKEN_DECLS(RightBracket, "}");

Value::Value(SourceLocation location, StringView sv)
    : Token(location), m_value(sv)
{}

fn Value::raw_string() const throws -> String { return m_value; }

Assignment::Assignment(SourceLocation location, StringView key, Word value,
                       bool is_append)
    : Token(location), m_key(key), m_value(steal(value)), m_is_append(is_append)
{}

fn Assignment::kind() const wontthrow -> Token::Kind
{
  return Token::Kind::Assignment;
}

fn Assignment::flags() const wontthrow -> Token::Flags
{
  return Token::Flag::Special;
}

fn Assignment::raw_string() const throws -> String
{
  let result = m_key.clone();
  result += m_is_append ? "+=" : "=";
  result += m_value.to_literal_string();
  return result;
}

pure fn Assignment::key() const wontthrow -> const String & { return m_key; }

pure fn Assignment::is_append() const wontthrow -> bool { return m_is_append; }

pure fn Assignment::value_word() const wontthrow -> const Word &
{
  return m_value;
}

WordToken::WordToken(SourceLocation location, Word word)
    : Value(location, ""), m_word(steal(word))
{
  m_value = m_word.to_literal_string();
}

fn WordToken::kind() const wontthrow -> Token::Kind
{
  return Token::Kind::Word;
}

fn WordToken::flags() const wontthrow -> Token::Flags
{
  return Token::Flag::Value;
}

pure fn WordToken::word() const wontthrow -> const Word & { return m_word; }

Redirection::Redirection(SourceLocation location, StringView what_fd,
                         StringView to_file)
    : Token(location), m_from_fd(what_fd), m_to_file(to_file)
{}

fn Redirection::kind() const wontthrow -> Token::Kind
{
  return Token::Kind::Redirection;
}

fn Redirection::flags() const wontthrow -> Token::Flags
{
  return Token::Flag::Special;
}

pure fn Redirection::from_fd() const wontthrow -> const String &
{
  return m_from_fd;
}

pure fn Redirection::to_file() const wontthrow -> const String &
{
  return m_to_file;
}

Operator::Operator(SourceLocation location) : Token(location) {}

fn Operator::left_precedence() const wontthrow -> u8 { return 0; }

fn Operator::unary_precedence() const wontthrow -> u8 { return 0; }

fn Operator::binary_left_associative() const wontthrow -> bool { return true; }

fn Operator::construct_binary_expression(const Expression *lhs,
                                         const Expression *rhs) const throws
    -> Expression *
{
  unused(lhs);
  unused(rhs);
  unreachable("Invalid binary operator construction of type %d", ENUM(kind()));
}

fn Operator::construct_unary_expression(const Expression *rhs) const throws
    -> Expression *
{
  unused(rhs);
  unreachable("Invalid unary operator construction of type %d", ENUM(kind()));
}

#define BINARY_UNARY_OPERATOR_TOKEN_DECLS(t, s, up, bp, uexpr, bexpr)          \
  t::t(SourceLocation location) : Operator(location) {}                        \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow                                      \
  {                                                                            \
    return Token::Flag::BinaryOperator | Token::Flag::UnaryOperator;           \
  }                                                                            \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }    \
  u8 t::left_precedence() const wontthrow { return bp; }                       \
  u8 t::unary_precedence() const wontthrow { return up; }                      \
  Expression *t::construct_binary_expression(                                  \
      const Expression *lhs, const Expression *rhs) const throws               \
  {                                                                            \
    ASSERT(AST_ARENA != nullptr);                                              \
    return AST_ARENA->create<expressions::bexpr>(source_location(), lhs, rhs); \
  }                                                                            \
  Expression *t::construct_unary_expression(const Expression *rhs)             \
      const throws                                                             \
  {                                                                            \
    ASSERT(AST_ARENA != nullptr);                                              \
    return AST_ARENA->create<expressions::uexpr>(source_location(), rhs);      \
  }

BINARY_UNARY_OPERATOR_TOKEN_DECLS(Plus, "+", 13, 11, Unnegate, Add);
BINARY_UNARY_OPERATOR_TOKEN_DECLS(Minus, "-", 13, 11, Negate, Subtract);

#define BINARY_OPERATOR_TOKEN_DECLS_COMPOUND(t, s, bp, bexpr)                  \
  t::t(SourceLocation location) : Operator(location) {}                        \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow                                      \
  {                                                                            \
    return Token::Flag::BinaryOperator | Token::Flag::CompoundList;            \
  }                                                                            \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }    \
  u8 t::left_precedence() const wontthrow { return bp; }                       \
  Expression *t::construct_binary_expression(                                  \
      const Expression *lhs, const Expression *rhs) const throws               \
  {                                                                            \
    ASSERT(AST_ARENA != nullptr);                                              \
    return AST_ARENA->create<expressions::bexpr>(source_location(), lhs, rhs); \
  }

#define BINARY_OPERATOR_TOKEN_DECLS(t, s, bp, bexpr)                           \
  t::t(SourceLocation location) : Operator(location) {}                        \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow                                      \
  {                                                                            \
    return Token::Flag::BinaryOperator;                                        \
  }                                                                            \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }    \
  u8 t::left_precedence() const wontthrow { return bp; }                       \
  Expression *t::construct_binary_expression(                                  \
      const Expression *lhs, const Expression *rhs) const throws               \
  {                                                                            \
    ASSERT(AST_ARENA != nullptr);                                              \
    return AST_ARENA->create<expressions::bexpr>(source_location(), lhs, rhs); \
  }

BINARY_OPERATOR_TOKEN_DECLS_COMPOUND(DoublePipe, "||", 4, LogicalOr);
BINARY_OPERATOR_TOKEN_DECLS_COMPOUND(Ampersand, "&", 7, BinaryAnd);
BINARY_OPERATOR_TOKEN_DECLS_COMPOUND(DoubleAmpersand, "&&", 4, LogicalAnd);

BINARY_OPERATOR_TOKEN_DECLS(Slash, "/", 12, Divide);
BINARY_OPERATOR_TOKEN_DECLS(Asterisk, "*", 12, Multiply);
BINARY_OPERATOR_TOKEN_DECLS(Percent, "%", 12, Module);
BINARY_OPERATOR_TOKEN_DECLS(Greater, ">", 8, GreaterThan);
BINARY_OPERATOR_TOKEN_DECLS(DoubleGreater, ">>", 8, RightShift);
BINARY_OPERATOR_TOKEN_DECLS(GreaterEquals, ">=", 8, GreaterOrEqual);
BINARY_OPERATOR_TOKEN_DECLS(Less, "<", 8, LessThan);
BINARY_OPERATOR_TOKEN_DECLS(DoubleLess, "<<", 8, LeftShift);
BINARY_OPERATOR_TOKEN_DECLS(LessEquals, "<=", 8, LessOrEqual);
BINARY_OPERATOR_TOKEN_DECLS(Pipe, "|", 5, BinaryOr);
BINARY_OPERATOR_TOKEN_DECLS(Cap, "^", 9, Xor);
BINARY_OPERATOR_TOKEN_DECLS(Equals, "=", 3, BinaryDummyExpression);
BINARY_OPERATOR_TOKEN_DECLS(DoubleEquals, "==", 3, Equal);
BINARY_OPERATOR_TOKEN_DECLS(ExclamationEquals, "!=", 3, NotEqual);

#define UNARY_OPERATOR_TOKEN_DECLS(t, s, up, uexpr)                            \
  t::t(SourceLocation location) : Operator(location) {}                        \
  Token::Kind t::kind() const wontthrow { return Token::Kind::t; }             \
  Token::Flags t::flags() const wontthrow                                      \
  {                                                                            \
    return Token::Flag::UnaryOperator;                                         \
  }                                                                            \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }    \
  u8 t::unary_precedence() const wontthrow { return up; }                      \
  Expression *t::construct_unary_expression(const Expression *rhs)             \
      const throws                                                             \
  {                                                                            \
    ASSERT(AST_ARENA != nullptr);                                              \
    return AST_ARENA->create<expressions::uexpr>(source_location(), rhs);      \
  }

UNARY_OPERATOR_TOKEN_DECLS(ExclamationMark, "!", 13, LogicalNot);
UNARY_OPERATOR_TOKEN_DECLS(Tilde, "~", 13, BinaryComplement);

} /* namespace tokens */

} /* namespace koshka */
