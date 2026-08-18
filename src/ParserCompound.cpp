#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"
#include "ParserInternal.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

using namespace tokens;
using namespace expressions;
using parser_internal::is_unquoted_word;
using parser_internal::throw_unterminated;
using parser_internal::token_kind_mask;

hot fn Parser::parse_if() throws -> Command *
{
  Token *if_token = m_lexer.next_shell_token();
  ASSERT(if_token != nullptr);
  ASSERT(if_token->kind() == Token::Kind::If);
  let const location = if_token->source_location();

  LOG(Debug, "parsing an if clause at byte %u", location.position);

  let branches = ArrayList<if_branch>{heap_allocator()};
  const Expression *otherwise = nullptr;

  loop
  {
    Expression *condition =
        parse_command_list(token_kind_mask(Token::Kind::Then));
    Token *then_token = m_lexer.next_shell_token();
    ASSERT(then_token != nullptr);
    if (then_token->kind() != Token::Kind::Then) {
      let const detail = condition->is_dummy()
                             ? "Expected a command for the condition"
                             : "Expected 'then' after the condition";
      throw ErrorWithLocationAndDetails{location, "Unterminated if",
                                        then_token->source_location(), detail};
    }

    Expression *body = parse_command_list(
        token_kind_mask(Token::Kind::Elif, Token::Kind::Else, Token::Kind::Fi));
    branches.push(if_branch{condition, body});

    Token *after = m_lexer.next_shell_token();
    ASSERT(after != nullptr);
    if (after->kind() == Token::Kind::Elif) {
      continue;
    } else if (after->kind() == Token::Kind::Else) {
      otherwise = parse_command_list(token_kind_mask(Token::Kind::Fi));
      Token *fi_token = m_lexer.next_shell_token();
      ASSERT(fi_token != nullptr);
      if (fi_token->kind() != Token::Kind::Fi) {
        throw_unterminated(location, "Unterminated if", m_lexer.source(), "fi",
                           fi_token->source_location());
      }
      break;
    } else if (after->kind() == Token::Kind::Fi) {
      break;
    } else {
      throw_unterminated(location, "Unterminated if", m_lexer.source(), "fi",
                         after->source_location());
    }
  }

  return m_lexer.arena().create<IfClause>(location, steal(branches), otherwise);
}

hot fn Parser::parse_while_or_until(bool is_until) throws -> Command *
{
  Token *keyword = m_lexer.next_shell_token();
  ASSERT(keyword != nullptr);
  let const location = keyword->source_location();

  LOG(Debug, "parsing a %s loop at byte %u", is_until ? "until" : "while",
      location.position);

  Expression *condition = parse_command_list(token_kind_mask(Token::Kind::Do));
  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    let const detail = condition->is_dummy()
                           ? "Expected a command for the loop condition"
                           : "Expected 'do'";
    throw ErrorWithLocationAndDetails{location, "Unterminated loop",
                                      do_token->source_location(), detail};
  }

  let const parsed_body = parse_loop_body(location, "Unterminated loop");

  let loop_node = m_lexer.arena().create<WhileLoop>(location, condition,
                                                    parsed_body.body, is_until);
  loop_node->set_source_end_position(parsed_body.done_location.position +
                                     parsed_body.done_location.length);
  return loop_node;
}

static fn word_token_from_assignment(BumpArena &arena,
                                     const Assignment *a) throws
    -> tokens::WordToken *;

/* Whether the (( opening before body_start_position closes with two adjacent
   right parens at depth zero, separating an arithmetic command from a subshell
   whose first child is a subshell. */
static fn double_paren_closes_adjacent(StringView source,
                                       usize body_start_position) wontthrow
    -> bool
{
  usize depth = 0;
  char quote = 0;
  for (usize i = body_start_position; i < source.length; i++) {
    let const c = source[i];
    if (quote == '\'') {
      if (c == '\'') quote = 0;
      continue;
    }
    if (c == '\\') {
      i++;
      continue;
    }
    if (quote == '"') {
      if (c == '"') quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(') {
      depth++;
      continue;
    }
    if (c == ')') {
      if (depth > 0) {
        depth--;
        continue;
      }
      return i + 1 < source.length && source[i + 1] == ')';
    }
  }
  return false;
}

static fn word_token_from_raw(BumpArena &arena, StringView text,
                              SourceLocation location) throws
    -> tokens::WordToken *;

fn Parser::parse_optional_in_clause_words(
    ArrayList<const Token *> &words) throws -> bool
{
  /* The word 'in' is not a keyword token. */
  Token *peeked = m_lexer.peek_shell_token();
  ASSERT(peeked != nullptr);
  if (!is_unquoted_word(peeked, "in")) {
    if (peeked->kind() == Token::Kind::Word && peeked->raw_string() == "in") {
      ErrorWithLocation error{peeked->source_location(),
                              "Expected an unquoted 'in'"};
      error.set_command_status(2);
      throw error;
    }
    return false;
  }

  m_lexer.advance_past_last_peek();
  loop
  {
    Token *word = m_lexer.peek_shell_token();
    ASSERT(word != nullptr);
    if (word->kind() == Token::Kind::Assignment) {
      m_lexer.advance_past_last_peek();
      words.push(word_token_from_assignment(m_lexer.arena(),
                                            static_cast<Assignment *>(word)));
      continue;
    }
    if (word->kind() != Token::Kind::Word) {
      /* A non-keyword separator or operator ends the list. */
      let const raw = word->raw_string();
      if (!KEYWORDS.find(raw.view()).has_value()) break;
      m_lexer.advance_past_last_peek();
      words.push(word_token_from_raw(m_lexer.arena(), raw.view(),
                                     word->source_location()));
      continue;
    }
    m_lexer.advance_past_last_peek();
    words.push(word);
  }
  return true;
}

hot fn Parser::parse_for() throws -> Command *
{
  Token *keyword = m_lexer.next_shell_token();
  ASSERT(keyword != nullptr);
  let const location = keyword->source_location();

  LOG(Debug, "parsing a for loop at byte %u", location.position);

  /* A for header opening with (( is the bash C-style loop, riding every mood
     but POSIX where the bare-name reading holds. */
  if (!m_lexer.is_posix_mode()) {
    Token *peeked = m_lexer.peek_shell_token();
    ASSERT(peeked != nullptr);
    if (peeked->kind() == Token::Kind::LeftParen) {
      Token *first_paren = m_lexer.next_shell_token();
      Token *second = m_lexer.peek_shell_token();
      ASSERT(second != nullptr);
      if (second->kind() == Token::Kind::LeftParen &&
          second->source_location().position ==
              first_paren->source_location().position + 1)
      {
        return parse_c_style_for(location, first_paren);
      }
      throw ErrorWithLocation{first_paren->source_location(),
                              "Expected '((' or a variable name after 'for'"};
    }
  }

  Token *name_token = m_lexer.next_shell_token();
  ASSERT(name_token != nullptr);
  if (name_token->kind() != Token::Kind::Word) {
    let const raw = name_token->raw_string();
    if (KEYWORDS.find(raw.view()).has_value())
      name_token = word_token_from_raw(m_lexer.arena(), raw.view(),
                                       name_token->source_location());
  }
  if (name_token->kind() != Token::Kind::Word) {
    /* A (( in the name slot under POSIX mode is the bash C-style loop in a mode
       that keeps the dash reading. */
    if (m_lexer.is_posix_mode() && name_token->kind() == Token::Kind::LeftParen)
    {
      throw ErrorWithLocationAndDetails{
          name_token->source_location(),
          "Expected a variable name after 'for'. The for ((...)) C-style "
          "loop is a bashism that POSIX mode does not read",
          "Use a while loop instead"};
    }
    throw ErrorWithLocation{name_token->source_location(),
                            "Expected a variable name after 'for'"};
  }

  /* The loop variable must be a plain name, so a $ expansion such as for $f, a
     quoted word, or a non-identifier is rejected. */
  let const &name_word =
      static_cast<const tokens::WordToken *>(name_token)->word();
  let is_name_plain =
      name_word.segments.count() == 1 &&
      name_word.segments[0].kind == WordSegment::Kind::UnquotedText;
  if (is_name_plain) {
    is_name_plain =
        optimizer::is_plain_variable_name(name_word.segments[0].text.view());
  }
  if (!is_name_plain) {
    throw ErrorWithLocationAndDetails{name_token->source_location(),
                                      StringView{"Bad for loop variable, '"} +
                                          name_token->raw_string() +
                                          "' is not a plain name",
                                      "Drop the '$' and any quotes"};
  }

  let const variable_name = name_token->raw_string();

  ArrayList<const Token *> words{heap_allocator()};
  let const has_in_clause = parse_optional_in_clause_words(words);

  skip_semicolons_and_newlines();

  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    String detail = "Expected 'do'";
    if (!has_in_clause) {
      detail = "Expected 'do', or 'in WORDS' before it; without 'in' the loop "
               "walks the positional parameters";
    }
    throw ErrorWithLocationAndDetails{location, "Unterminated for loop",
                                      do_token->source_location(), detail};
  }

  let const parsed_body = parse_loop_body(location, "Unterminated for loop");

  let loop_node = m_lexer.arena().create<ForLoop>(
      location, name_token->source_location(), variable_name.view(),
      steal(words), has_in_clause, parsed_body.body);
  loop_node->set_source_end_position(parsed_body.done_location.position +
                                     parsed_body.done_location.length);
  return loop_node;
}

/* A bash select loop, select name in words; do BODY; done. It shares the for
   header shape, printing a numbered menu and reading a choice at run time. */
hot fn Parser::parse_select() throws -> Command *
{
  Token *keyword = m_lexer.next_shell_token();
  ASSERT(keyword != nullptr);
  ASSERT(is_unquoted_word(keyword, "select"));
  let const location = keyword->source_location();

  LOG(Debug, "parsing a select loop at byte %u", location.position);

  Token *name_token = m_lexer.next_shell_token();
  ASSERT(name_token != nullptr);
  if (name_token->kind() != Token::Kind::Word) {
    let const raw = name_token->raw_string();
    if (KEYWORDS.find(raw.view()).has_value())
      name_token = word_token_from_raw(m_lexer.arena(), raw.view(),
                                       name_token->source_location());
  }
  if (name_token->kind() != Token::Kind::Word) {
    throw ErrorWithLocation{name_token->source_location(),
                            "Expected a variable name after 'select'"};
  }
  let const variable_name = name_token->raw_string();

  ArrayList<const Token *> words{heap_allocator()};
  let const has_in_clause = parse_optional_in_clause_words(words);

  skip_semicolons_and_newlines();

  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    throw ErrorWithLocationAndDetails{location, "Unterminated select loop",
                                      do_token->source_location(),
                                      "expected 'do'"};
  }

  let const parsed_body = parse_loop_body(location, "Unterminated select loop");

  return m_lexer.arena().create<SelectLoop>(
      location, name_token->source_location(), variable_name.view(),
      steal(words), has_in_clause, parsed_body.body);
}

/* In a case word or pattern a NAME=VALUE token is a plain word, rebuilt into a
   word token that keeps the expansion segments after the NAME= prefix. */
static fn word_token_from_assignment(BumpArena &arena,
                                     const Assignment *a) throws
    -> tokens::WordToken *
{
  let word = Word{};
  let prefix = a->key().clone();
  prefix += a->is_append() ? "+=" : "=";
  word.segments.push(
      WordSegment{WordSegment::Kind::UnquotedText, steal(prefix), false});
  for (let const &segment : a->value_word().segments)
    word.segments.push(segment.clone());
  return arena.create<tokens::WordToken>(a->source_location(), steal(word));
}

static fn word_token_from_raw(BumpArena &arena, StringView text,
                              SourceLocation location) throws
    -> tokens::WordToken *
{
  let word = Word{};
  word.segments.push(
      WordSegment{WordSegment::Kind::UnquotedText, String{text}, false});
  return arena.create<tokens::WordToken>(steal(location), steal(word));
}

hot fn Parser::parse_case() throws -> Command *
{
  Token *keyword = m_lexer.next_shell_token();
  ASSERT(keyword != nullptr);
  let const location = keyword->source_location();

  LOG(Debug, "parsing a case clause at byte %u", location.position);

  Token *word = m_lexer.next_shell_token();
  ASSERT(word != nullptr);
  if (word->kind() == Token::Kind::Assignment) {
    word = word_token_from_assignment(m_lexer.arena(),
                                      static_cast<Assignment *>(word));
  } else if (word->kind() != Token::Kind::Word) {
    let const raw = word->raw_string();
    if (KEYWORDS.find(raw.view()).has_value())
      word = word_token_from_raw(m_lexer.arena(), raw.view(),
                                 word->source_location());
    else
      throw ErrorWithLocation{word->source_location(),
                              "Expected a word to match on after 'case'"};
  }

  while (m_lexer.peek_shell_token()->kind() == Token::Kind::Newline)
    m_lexer.advance_past_last_peek();

  Token *in_token = m_lexer.next_shell_token();
  ASSERT(in_token != nullptr);
  if (!is_unquoted_word(in_token, "in")) {
    ErrorWithLocation error{in_token->source_location(),
                            "Expected an unquoted 'in' after the case word"};
    error.set_command_status(2);
    throw error;
  }

  let items = ArrayList<case_item>{heap_allocator()};

  loop
  {
    Token *t = m_lexer.peek_shell_token();
    ASSERT(t != nullptr);

    if (t->kind() == Token::Kind::Newline ||
        t->kind() == Token::Kind::Semicolon)
    {
      m_lexer.advance_past_last_peek();
      continue;
    }
    if (t->kind() == Token::Kind::Esac) {
      m_lexer.advance_past_last_peek();
      break;
    }

    if (t->kind() == Token::Kind::LeftParen) m_lexer.advance_past_last_peek();

    ArrayList<const Token *> patterns{heap_allocator()};

    loop
    {
      Token *pattern = m_lexer.next_shell_token();
      ASSERT(pattern != nullptr);

      if (pattern->kind() == Token::Kind::Assignment) {
        pattern = word_token_from_assignment(
            m_lexer.arena(), static_cast<Assignment *>(pattern));
      } else if (pattern->kind() != Token::Kind::Word) {
        /* A keyword used as a literal pattern, the way ble.sh writes (done), is
           taken by its source text. */
        let const pattern_location = pattern->source_location();
        let const text = m_lexer.source().substring_of_length(
            pattern_location.position, pattern_location.length);
        if (KEYWORDS.find(text).has_value()) {
          pattern =
              word_token_from_raw(m_lexer.arena(), text, pattern_location);
        } else {
          throw ErrorWithLocationAndDetails{
              location, "Unterminated case", pattern_location,
              "expected a pattern to start an arm, or 'esac' to end the case"};
        }
      }

      patterns.push(pattern);

      Token *separator = m_lexer.next_shell_token();
      ASSERT(separator != nullptr);

      if (separator->kind() == Token::Kind::Pipe) continue;
      if (separator->kind() == Token::Kind::RightParen) break;

      throw ErrorWithLocation{separator->source_location(),
                              "Expected '|' or ')' in a case pattern"};
    }

    Expression *body = parse_command_list(token_kind_mask(
        Token::Kind::DoubleSemicolon, Token::Kind::SemicolonAmpersand,
        Token::Kind::DoubleSemicolonAmpersand, Token::Kind::Esac));

    Token *after = m_lexer.peek_shell_token();
    ASSERT(after != nullptr);
    let terminator = case_terminator::Break;
    let is_last_arm = false;
    switch (after->kind()) {
    case Token::Kind::DoubleSemicolon: m_lexer.advance_past_last_peek(); break;
    case Token::Kind::SemicolonAmpersand:
      terminator = case_terminator::FallThrough;
      m_lexer.advance_past_last_peek();
      break;
    case Token::Kind::DoubleSemicolonAmpersand:
      terminator = case_terminator::ContinueMatch;
      m_lexer.advance_past_last_peek();
      break;
    case Token::Kind::Esac:
      m_lexer.advance_past_last_peek();
      is_last_arm = true;
      break;
    default: break;
    }

    items.push(case_item{steal(patterns), body, terminator});
    if (is_last_arm) break;
  }

  return m_lexer.arena().create<CaseClause>(location, word, steal(items));
}

hot fn Parser::parse_brace_group() throws -> Command *
{
  Token *open = m_lexer.next_shell_token();
  ASSERT(open != nullptr);
  ASSERT(is_unquoted_word(open, "{"));

  LOG(Debug, "parsing a brace group at byte %u",
      open->source_location().position);

  Expression *body =
      parse_command_list(token_kind_mask(Token::Kind::RightBracket));

  Token *close = m_lexer.next_shell_token();
  ASSERT(close != nullptr);
  if (!is_unquoted_word(close, "}")) {
    /* The closing '}' is a reserved word only at the start of a command, so
       without a ';' or newline before it the group never closes. */
    throw_unterminated(open->source_location(), "Unterminated brace group",
                       m_lexer.source(), "}", close->source_location());
  }

  BraceGroup *group =
      m_lexer.arena().create<BraceGroup>(open->source_location(), body);
  let const close_location = close->source_location();
  group->set_source_end_position(close_location.position +
                                 close_location.length);
  return group;
}

hot fn Parser::parse_paren_command() throws -> Command *
{
  Token *open = m_lexer.next_shell_token();
  ASSERT(open != nullptr);
  ASSERT(open->kind() == Token::Kind::LeftParen);

  /* Two opening parens are a nested subshell in POSIX, so POSIX keeps that
     reading while bash and default take the arithmetic command. A (( that
     closes with a lone ) at depth zero, such as ((cmd; cmd); cmd), is a
     subshell whose first child is a subshell, decided by a quote-aware scan. */
  Token *next = m_lexer.peek_shell_token();
  ASSERT(next != nullptr);
  if (!m_lexer.is_posix_mode() && next->kind() == Token::Kind::LeftParen &&
      next->source_location().position ==
          open->source_location().position + 1 &&
      double_paren_closes_adjacent(m_lexer.source(),
                                   next->source_location().position + 1))
  {
    return parse_arithmetic_command(open);
  }
  return parse_subshell(open);
}

hot fn Parser::parse_subshell(Token *open) throws -> Command *
{
  ASSERT(open != nullptr);
  ASSERT(open->kind() == Token::Kind::LeftParen);

  LOG(Debug, "parsing a subshell at byte %u", open->source_location().position);

  let const scope_mark = open_analysis_scope();
  Expression *body =
      parse_command_list(token_kind_mask(Token::Kind::RightParen));

  Token *close = m_lexer.next_shell_token();
  ASSERT(close != nullptr);
  if (close->kind() != Token::Kind::RightParen) {
    throw ErrorWithLocationAndDetails{open->source_location(),
                                      "Unterminated subshell",
                                      close->source_location(), "expected ')'"};
  }

  let subshell =
      m_lexer.arena().create<Subshell>(open->source_location(), body);
  subshell->set_analysis_scope_definitions(close_analysis_scope(scope_mark));
  let const close_location = close->source_location();
  subshell->set_source_end_position(close_location.position +
                                    close_location.length);
  return subshell;
}

/* Read the body of a (( )) construct, returning a view of the source between
   the two pairs. Shared by the arithmetic command and the C-style for header.
 */
hot fn Parser::capture_double_paren_body(Token *open) throws -> StringView
{
  ASSERT(open != nullptr);
  Token *second = m_lexer.next_shell_token();
  ASSERT(second != nullptr);
  ASSERT(second->kind() == Token::Kind::LeftParen);

  let const body_start_position = second->source_location().position + 1;
  usize body_end_position;
  usize depth = 0;
  loop
  {
    Token *t = m_lexer.next_shell_token();
    ASSERT(t != nullptr);
    if (t->kind() == Token::Kind::EndOfFile) {
      throw ErrorWithLocationAndDetails{open->source_location(),
                                        "Unterminated '(('",
                                        t->source_location(), "expected '))'"};
    }
    if (t->kind() == Token::Kind::LeftParen) {
      depth++;
      continue;
    }
    if (t->kind() == Token::Kind::RightParen) {
      if (depth > 0) {
        depth--;
        continue;
      }
      Token *closing = m_lexer.peek_shell_token();
      ASSERT(closing != nullptr);
      if (closing->kind() == Token::Kind::RightParen &&
          closing->source_location().position ==
              t->source_location().position + 1)
      {
        body_end_position = t->source_location().position;
        m_lexer.advance_past_last_peek();
        break;
      }
      throw ErrorWithLocationAndDetails{open->source_location(),
                                        "Unterminated '(('",
                                        t->source_location(), "expected '))'"};
    }
  }

  return m_lexer.source().substring_of_length(
      body_start_position, body_end_position - body_start_position);
}

hot fn Parser::parse_arithmetic_command(Token *open) throws -> Command *
{
  LOG(Debug, "parsing an arithmetic command at byte %u",
      open->source_location().position);

  let const body = capture_double_paren_body(open);
  /* The location spans the whole (( body )) so a runtime error underlines the
     entire expression. */
  let const open_location = open->source_location();
  const SourceLocation full_location{open_location.position, body.length + 4,
                                     open_location.filename};
  return m_lexer.arena().create<expressions::ArithmeticCommand>(
      full_location, String{bump_allocator(m_lexer.arena()), body});
}

/* A bash C-style for, for (( init; cond; step )); do BODY; done. The header is
   split on its two top-level semicolons into three arithmetic clauses. */
hot fn Parser::parse_c_style_for(const SourceLocation &location,
                                 Token *open) throws -> Command *
{
  LOG(Debug, "parsing a c-style for header at byte %u", location.position);

  let const header = capture_double_paren_body(open);

  /* The clause separators are the semicolons at paren depth zero, so a grouped
     subexpression in a clause is skipped. */
  usize separators[2] = {0, 0};
  usize separator_count = 0;
  usize depth = 0;
  for (usize i = 0; i < header.length; i++) {
    let const c = header[i];
    if (c == '(') {
      depth++;
    } else if (c == ')') {
      if (depth > 0) depth--;
    } else if (c == ';' && depth == 0) {
      if (separator_count < 2) separators[separator_count] = i;
      separator_count++;
    }
  }
  if (separator_count != 2) {
    throw ErrorWithLocation{
        location, "Expected '(( init; condition; step ))' in a C-style for"};
  }

  let const allocator = bump_allocator(m_lexer.arena());
  let init = String{allocator, header.substring_of_length(0, separators[0])};
  let condition = String{
      allocator, header.substring_of_length(separators[0] + 1,
                                            separators[1] - separators[0] - 1)};
  let step = String{allocator, header.substring(separators[1] + 1)};

  skip_semicolons_and_newlines();

  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    throw ErrorWithLocationAndDetails{location, "Unterminated for loop",
                                      do_token->source_location(),
                                      "expected 'do'"};
  }

  let const parsed_body = parse_loop_body(location, "Unterminated for loop");

  return m_lexer.arena().create<expressions::CStyleForLoop>(
      location, open->source_location().position + 2, steal(init),
      steal(condition), steal(step), parsed_body.body);
}

fn Parser::parse_loop_body(const SourceLocation &location,
                           StringView unterminated_message) throws
    -> parsed_loop_body
{
  Expression *body = parse_command_list(token_kind_mask(Token::Kind::Done));
  reject_empty_loop_body(body);
  Token *done_token = m_lexer.next_shell_token();
  ASSERT(done_token != nullptr);
  if (done_token->kind() != Token::Kind::Done) {
    throw_unterminated(location, unterminated_message, m_lexer.source(), "done",
                       done_token->source_location());
  }

  return parsed_loop_body{body, done_token->source_location()};
}

hot fn Parser::parse_conditional_command() throws -> Command *
{
  Token *open = m_lexer.next_shell_token();
  ASSERT(open != nullptr);
  ASSERT(is_unquoted_word(open, "[["));

  LOG(Debug, "parsing a conditional command at byte %u",
      open->source_location().position);

  /* The tokens between [[ and ]] are collected raw rather than run through the
     command parser, so a < or > inside is a string comparison and not a
     redirection. The operand words are kept for the evaluator to expand without
     field splitting. */
  let elements = ArrayList<conditional_element>{heap_allocator()};
  usize close_end_position;
  loop
  {
    Token *t = m_lexer.next_shell_token();
    ASSERT(t != nullptr);
    if (is_unquoted_word(t, "]]")) {
      close_end_position =
          t->source_location().position + t->source_location().length;
      break;
    }
    if (t->kind() == Token::Kind::EndOfFile) {
      throw ErrorWithLocationAndDetails{open->source_location(),
                                        "Unterminated '[['",
                                        t->source_location(), "expected ']]'"};
    }

    using Kind = conditional_element::Kind;
    switch (t->kind()) {
    case Token::Kind::DoubleAmpersand:
      elements.push({Kind::And, nullptr, false, None});
      break;
    case Token::Kind::DoublePipe:
      elements.push({Kind::Or, nullptr, false, None});
      break;
    case Token::Kind::LeftParen:
      elements.push({Kind::OpenParen, nullptr, false, None});
      break;
    case Token::Kind::RightParen:
      elements.push({Kind::CloseParen, nullptr, false, None});
      break;
    case Token::Kind::Less:
      elements.push({Kind::Less, nullptr, false, t->source_location()});
      break;
    case Token::Kind::Greater:
      elements.push({Kind::Greater, nullptr, false, t->source_location()});
      break;
    case Token::Kind::Newline: continue;
    case Token::Kind::Word: {
      let const is_bare_unquoted =
          static_cast<tokens::WordToken *>(t)->word().plain_literal_kind() ==
          Word::PlainLiteral::PlainUnquotedOneSegment;
      if (is_unquoted_word(t, "!")) {
        elements.push({Kind::Not, nullptr, false, None});
        break;
      }
      elements.push({Kind::Operand, t, is_bare_unquoted, None});

      if (is_unquoted_word(t, "=~")) {
        Token *peek = m_lexer.peek_shell_token();
        if (peek != nullptr && !is_unquoted_word(peek, "]]") &&
            peek->kind() != Token::Kind::EndOfFile &&
            peek->kind() != Token::Kind::DoubleAmpersand &&
            peek->kind() != Token::Kind::DoublePipe &&
            peek->kind() != Token::Kind::RightParen)
        {
          m_lexer.advance_past_last_peek();
          Token *const first = peek;
          usize end_position = first->source_location().position +
                               first->source_location().length;
          usize regex_parenthesis_depth = 0;
          Word regex_word{};
          let const do_append_segments = [&](Token *tok) throws {
            if (tok->kind() == Token::Kind::Word) {
              for (let const &segment :
                   static_cast<const tokens::WordToken *>(tok)->word().segments)
                regex_word.segments.push(segment);
            } else {
              regex_word.segments.push(WordSegment{
                  WordSegment::Kind::UnquotedText, tok->raw_string(), false});
            }
            if (tok->kind() == Token::Kind::LeftParen)
              ++regex_parenthesis_depth;
            if (tok->kind() == Token::Kind::RightParen &&
                regex_parenthesis_depth > 0)
            {
              --regex_parenthesis_depth;
            }
          };
          do_append_segments(first);
          loop
          {
            Token *next = m_lexer.peek_shell_token();
            if (next == nullptr || is_unquoted_word(next, "]]") ||
                next->kind() == Token::Kind::EndOfFile)
            {
              break;
            }
            if (regex_parenthesis_depth == 0 &&
                (next->kind() == Token::Kind::DoubleAmpersand ||
                 next->kind() == Token::Kind::DoublePipe ||
                 next->kind() == Token::Kind::RightParen))
            {
              break;
            }
            if (next->source_location().position != end_position) {
              if (regex_parenthesis_depth == 0) break;
              regex_word.segments.push(WordSegment{
                  WordSegment::Kind::UnquotedText,
                  m_lexer.source().substring_of_length(
                      end_position,
                      next->source_location().position - end_position),
                  false});
            }
            m_lexer.advance_past_last_peek();
            end_position = next->source_location().position +
                           next->source_location().length;
            do_append_segments(next);
          }
          SourceLocation regex_location = first->source_location();
          regex_location.length =
              static_cast<u32>(end_position - regex_location.position);
          elements.push({Kind::Operand,
                         m_lexer.arena().create<tokens::WordToken>(
                             regex_location, steal(regex_word)),
                         false, None});
        }
      }
      break;
    }
    default: elements.push({Kind::Operand, t, false, None}); break;
    }
  }

  let const node = m_lexer.arena().create<expressions::ConditionalCommand>(
      open->source_location(), steal(elements));
  node->set_source_end_position(close_end_position);
  return node;
}

} /* namespace koshka */
