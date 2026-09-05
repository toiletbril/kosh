#include "Parser.hpp"

#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Optimizer.hpp"
#include "ParserInternal.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

using namespace tokens;
using namespace expressions;
using internal::is_unquoted_word;
using internal::throw_unterminated;
using internal::token_kind_mask;

hot pure static fn get_sequence_kind(Token::Kind tk) wontthrow
    -> CompoundListCondition::Kind
{
  switch (tk) {
  case Token::Kind::Newline:
  case Token::Kind::EndOfFile:
  case Token::Kind::Ampersand:
  case Token::Kind::Semicolon:
  case Token::Kind::DoubleSemicolon: return CompoundListCondition::Kind::None;
  case Token::Kind::DoubleAmpersand: return CompoundListCondition::Kind::And;
  case Token::Kind::DoublePipe: return CompoundListCondition::Kind::Or;

  default: unreachable("Invalid shell sequence token: %d", ENUM(tk));
  }
}

Parser::Parser(Lexer &&lexer) : m_lexer(steal(lexer)) {}

Parser::~Parser() = default;

pure fn Parser::debug_words() const wontthrow -> const ArrayList<Word> &
{
  return m_lexer.debug_words();
}

fn Parser::take_shellcheck_suppressions() throws
    -> ArrayList<shellcheck_suppression>
{
  return steal(m_shellcheck_suppressions);
}

fn Parser::take_analysis_scope_definitions() throws
    -> ArrayList<analysis_scope_definition>
{
  return steal(m_analysis_scope_definitions);
}

fn Parser::record_analysis_scope_definition(StringView name,
                                            bool is_alias) throws -> void
{
  if (!m_should_collect_analysis_scopes) return;

  m_analysis_scope_definitions.push(
      analysis_scope_definition{String{name}, is_alias});
}

fn Parser::record_analysis_alias_definitions(
    const ArrayList<const Token *> &args) throws -> void
{
  if (!m_should_collect_analysis_scopes || args.is_empty()) {
    return;
  }

  let const command_view = args[0]->raw_view();
  if (!command_view.has_value() || *command_view != "alias") {
    return;
  }

  for (usize i = 1; i < args.count(); i++) {
    let const text = args[i]->raw_string();
    let const equals_position = text.find_character('=');
    if (equals_position.has_value() && *equals_position > 0)
      record_analysis_scope_definition(
          StringView{text.data(), *equals_position}, true);
  }
}

mustuse fn Parser::open_analysis_scope() const wontthrow -> usize
{
  return m_analysis_scope_definitions.count();
}

fn Parser::close_analysis_scope(usize scope_mark) throws
    -> ArrayList<analysis_scope_definition>
{
  let harvested = ArrayList<analysis_scope_definition>{heap_allocator()};
  for (usize i = scope_mark; i < m_analysis_scope_definitions.count(); i++)
    harvested.push(steal(m_analysis_scope_definitions[i]));

  while (m_analysis_scope_definitions.count() > scope_mark)
    m_analysis_scope_definitions.pop_back();

  return harvested;
}

static_assert(static_cast<u8>(Token::Kind::Function) < 64);

/* A brace is a reserved word only when a token is exactly '{' or '}' as a
   single unquoted segment, so a quoted or escaped brace is rejected. */
/* [[ and ]] arrive from the lexer as ordinary single unquoted words. */
hot pure static fn get_unquoted_word_text(const Token *token) wontthrow
    -> const SegmentText *
{
  if (token == nullptr || token->kind() != Token::Kind::Word) return nullptr;
  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  if (word.segments.count() != 1 ||
      word.segments[0].kind != WordSegment::Kind::UnquotedText)
  {
    return nullptr;
  }

  return &word.segments[0].text;
}

hot pure fn internal::is_unquoted_word(const Token *token,
                                       StringView text) wontthrow -> bool
{
  let const *unquoted_text = get_unquoted_word_text(token);
  return unquoted_text != nullptr && *unquoted_text == text;
}

/* RightBracket in the terminator set stands for a standalone '}' word, the
   close of a brace group. */
hot pure static fn is_list_terminator(const Token *token,
                                      u64 terminator_mask) wontthrow -> bool
{
  ASSERT(token != nullptr);
  return (terminator_mask & (u64{1} << static_cast<u8>(token->kind()))) != 0 ||
         ((terminator_mask &
           (u64{1} << static_cast<u8>(Token::Kind::RightBracket))) != 0 &&
          is_unquoted_word(token, "}"));
}

/* The byte location of the keyword as a whole word in the source, so a missing
   terminator can point the caret straight at the keyword read as an argument.
 */
cold pure static fn find_standalone_keyword(StringView source,
                                            StringView keyword) wontthrow
    -> Maybe<SourceLocation>
{
  let const do_is_boundary = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0 || c == ';' ||
           c == '&' || c == '|';
  };

  if (keyword.length == 0 || keyword.length > source.length) {
    return koshka::None;
  }

  for (usize pos = 0; pos + keyword.length <= source.length; pos++) {
    if (source.substring_of_length(pos, keyword.length) != keyword) continue;
    let const end_position = pos + keyword.length;
    let const left_ok = pos == 0 || do_is_boundary(source[pos - 1]);
    let const right_ok =
        end_position == source.length || do_is_boundary(source[end_position]);
    if (left_ok && right_ok) {
      return SourceLocation{pos, keyword.length};
    }
  }
  return koshka::None;
}

cold [[noreturn]] fn
internal::throw_unterminated(const SourceLocation &opener, StringView what,
                             StringView source, StringView keyword,
                             SourceLocation fallback) throws -> void
{
  if (Maybe<SourceLocation> found = find_standalone_keyword(source, keyword);
      found.has_value())
  {
    found->source_name_index = opener.source_name_index;
    throw ErrorWithLocationAndDetails{
        opener, what, *found,
        "this '" + keyword +
            "' was read as an argument, so put a ';' or a newline before it"};
  }
  fallback.source_name_index = opener.source_name_index;
  throw ErrorWithLocationAndDetails{opener, what, fallback,
                                    "expected '" + keyword + "'"};
}

cold static fn unexpected_command_token_message(const Token *token) throws
    -> String
{
  ASSERT(token != nullptr);
  if (is_unquoted_word(token, "}")) return "'}' has no matching '{'";
  switch (token->kind()) {
  case Token::Kind::Then:
  case Token::Kind::Else:
  case Token::Kind::Elif:
  case Token::Kind::Fi: {
    let const ast = token->to_ast_string();
    return "'" + ast.view() + "' has no matching 'if'";
  }
  case Token::Kind::Do:
  case Token::Kind::Done: {
    let const ast = token->to_ast_string();
    return "'" + ast.view() + "' has no matching 'while', 'until', or 'for'";
  }
  case Token::Kind::Esac: return "'esac' has no matching 'case'";
  case Token::Kind::DoubleSemicolon:
    return "';;' is only valid between the arms of a 'case'";
  case Token::Kind::RightParen: return "')' has no matching '('";
  case Token::Kind::RightBracket: return "'}' has no matching '{'";
  case Token::Kind::Pipe: return "'|' has no command before it to pipe from";
  default: {
    let const ast = token->to_ast_string();
    return "Expected a command, found '" + ast.view() + "'";
  }
  }
}

hot pure static fn is_compound_terminator(Token::Kind kind) wontthrow -> bool
{
  switch (kind) {
  case Token::Kind::RightParen:
  case Token::Kind::RightBracket:
  case Token::Kind::DoubleSemicolon:
  case Token::Kind::Then:
  case Token::Kind::Do:
  case Token::Kind::Done:
  case Token::Kind::Fi:
  case Token::Kind::Else:
  case Token::Kind::Elif:
  case Token::Kind::Esac: return true;
  default: return false;
  }
}

hot pure static fn is_compound_list_separator(Token::Kind kind) wontthrow
    -> bool
{
  switch (kind) {
  case Token::Kind::Newline:
  case Token::Kind::Semicolon:
  case Token::Kind::Ampersand:
  case Token::Kind::DoubleAmpersand:
  case Token::Kind::DoublePipe: return true;
  default: return false;
  }
}

flatten fn Parser::construct_ast() throws -> Expression *
{
  return parse_command_list(0);
}

fn Parser::construct_next_top_level_ast() throws -> Expression *
{
  if (m_should_collect_analysis_metadata)
    m_lexer.set_should_collect_shellcheck_directives(true);
  let const first_token = m_lexer.peek_shell_token();
  if (m_should_collect_analysis_metadata)
    m_lexer.set_should_collect_shellcheck_directives(false);
  if (first_token->kind() == Token::Kind::EndOfFile) return nullptr;

  m_should_stop_after_top_level_unit = true;
  defer { m_should_stop_after_top_level_unit = false; };

  return parse_command_list(0);
}

pure fn Parser::is_at_end() const wontthrow -> bool
{
  return m_lexer.is_at_source_end();
}

fn Parser::skip_newlines_after_pipe() throws -> void
{
  while (m_lexer.peek_shell_token()->kind() == Token::Kind::Newline)
    m_lexer.advance_past_last_peek();
}

fn Parser::skip_semicolons_and_newlines() throws -> void
{
  loop
  {
    Token *t = m_lexer.peek_shell_token();
    ASSERT(t != nullptr);
    if (t->kind() != Token::Kind::Semicolon &&
        t->kind() != Token::Kind::Newline)
      break;
    m_lexer.advance_past_last_peek();
  }
}

/* Skip to the next statement boundary so parsing resumes after a syntax error.
   At least one token is always consumed, so the offending token cannot stall
   the loop. */
cold fn Parser::recover_to_next_statement() throws -> void
{
  LOG(Debug, "skipping tokens to the next statement boundary");
  bool has_consumed_token = false;
  loop
  {
    Token *token = m_lexer.peek_shell_token();
    ASSERT(token != nullptr);

    if (token->kind() == Token::Kind::EndOfFile) return;

    let const is_boundary = token->kind() == Token::Kind::Newline ||
                            token->kind() == Token::Kind::Semicolon;

    if (is_boundary && has_consumed_token) {
      m_lexer.advance_past_last_peek();
      return;
    }

    m_lexer.advance_past_last_peek();
    has_consumed_token = true;
  }
}

cold fn Parser::record_detailed_parse_error(
    const ErrorWithLocationAndDetails &error, ArrayList<String> &errors,
    EvalContext *context, ArrayList<source_diagnostic> *diagnostic_sink) throws
    -> void
{
  LOG(Debug, "recording a detailed parse error and recovering: %s",
      error.message().c_str());
  errors.push(error.to_string(m_lexer.source(), context));
  errors.push(error.details_to_string(m_lexer.source(), context));
  if (diagnostic_sink == nullptr) return;

  let const location = error.location();
  let source_name = String{heap_allocator()};
  if (let const name = location.get_filename(); name.has_value())
    source_name = String{*name};
  let const details_location = error.details_location();
  let related_source_name = String{heap_allocator()};
  if (let const related_name = details_location.get_filename();
      related_name.has_value())
  {
    related_source_name = String{*related_name};
  }
  let const related_location = error.details_message().is_empty()
                                   ? Maybe<SourceLocation>{None}
                                   : Maybe<SourceLocation>{details_location};

  diagnostic_sink->push(source_diagnostic{
      None, error_severity::Error, location, steal(source_name),
      error.message().clone(), String{error.detail_message()}, related_location,
      steal(related_source_name), String{error.details_message()},
      ArrayList<source_fix>{heap_allocator()}});
}

cold fn Parser::record_parse_error(
    const ErrorWithLocation &error, ArrayList<String> &errors,
    EvalContext *context, ArrayList<source_diagnostic> *diagnostic_sink) throws
    -> void
{
  LOG(Debug, "recording a parse error and recovering: %s",
      error.message().c_str());
  errors.push(error.to_string(m_lexer.source(), context));
  if (diagnostic_sink == nullptr) return;

  let const location = error.location();
  let source_name = String{heap_allocator()};
  if (let const name = location.get_filename(); name.has_value())
    source_name = String{*name};

  diagnostic_sink->push(source_diagnostic{
      None, error_severity::Error, location, steal(source_name),
      error.message().clone(), String{error.detail_message()}, None,
      String{heap_allocator()}, String{heap_allocator()},
      ArrayList<source_fix>{heap_allocator()}});
}

/* Parse every top-level command, recovering from a syntax error instead of
   aborting at the first. */
cold fn Parser::construct_ast(
    ArrayList<String> &errors, EvalContext *context,
    ArrayList<source_diagnostic> *diagnostic_sink) throws -> Expression *
{
  Expression *first_piece = nullptr;
  let last_location = SourceLocation{};

  loop
  {
    /* An unterminated quote or here-document is raised by the token read
       itself, so the scan for the next command records it and stops. */
    Token *token = nullptr;
    if (m_should_collect_analysis_metadata)
      m_lexer.set_should_collect_shellcheck_directives(true);
    try {
      token = m_lexer.peek_shell_token();
    } catch (const ErrorWithLocationAndDetails &e) {
      record_detailed_parse_error(e, errors, context, diagnostic_sink);
    } catch (const ErrorWithLocation &e) {
      record_parse_error(e, errors, context, diagnostic_sink);
    }
    if (m_should_collect_analysis_metadata)
      m_lexer.set_should_collect_shellcheck_directives(false);
    if (token == nullptr) break;

    last_location = token->source_location();
    if (token->kind() == Token::Kind::EndOfFile) break;

    let did_parse_fail = false;
    try {
      Expression *piece = parse_command_list(0);
      ASSERT(piece != nullptr);
      if (first_piece == nullptr) first_piece = piece;
    } catch (const ErrorWithLocationAndDetails &e) {
      record_detailed_parse_error(e, errors, context, diagnostic_sink);
      did_parse_fail = true;
    } catch (const ErrorWithLocation &e) {
      record_parse_error(e, errors, context, diagnostic_sink);
      did_parse_fail = true;
    }
    if (!did_parse_fail) continue;

    /* The recovery scan is reached only once the parse error is recorded, and
       a lexical error stops that scan at the same place. */
    let did_recovery_fail = false;
    try {
      recover_to_next_statement();
    } catch (const ErrorWithLocation &) {
      did_recovery_fail = true;
    }
    if (did_recovery_fail) break;
  }

  if (first_piece == nullptr)
    return m_lexer.arena().create<DummyExpression>(last_location);

  return first_piece;
}

cold fn Parser::construct_next_top_level_ast(
    ArrayList<String> &errors, EvalContext *context,
    ArrayList<source_diagnostic> *diagnostic_sink) throws -> Expression *
{
  m_should_stop_after_top_level_unit = true;
  defer { m_should_stop_after_top_level_unit = false; };

  loop
  {
    Token *token = nullptr;
    if (m_should_collect_analysis_metadata)
      m_lexer.set_should_collect_shellcheck_directives(true);
    try {
      token = m_lexer.peek_shell_token();
    } catch (const ErrorWithLocationAndDetails &e) {
      record_detailed_parse_error(e, errors, context, diagnostic_sink);
    } catch (const ErrorWithLocation &e) {
      record_parse_error(e, errors, context, diagnostic_sink);
    }
    if (m_should_collect_analysis_metadata)
      m_lexer.set_should_collect_shellcheck_directives(false);

    if (token == nullptr || token->kind() == Token::Kind::EndOfFile)
      return nullptr;

    try {
      Expression *piece = parse_command_list(0);
      ASSERT(piece != nullptr);
      return piece;
    } catch (const ErrorWithLocationAndDetails &e) {
      record_detailed_parse_error(e, errors, context, diagnostic_sink);
    } catch (const ErrorWithLocation &e) {
      record_parse_error(e, errors, context, diagnostic_sink);
    }

    let did_recovery_fail = false;
    try {
      recover_to_next_statement();
    } catch (const ErrorWithLocation &) {
      did_recovery_fail = true;
    }
    if (did_recovery_fail) return nullptr;
  }
}

fn Parser::reject_empty_loop_body(const Expression *body) throws -> void
{
  if (!body->is_dummy()) return;
  Token *terminator = m_lexer.peek_shell_token();
  ASSERT(terminator != nullptr);
  throw koshka::ErrorWithLocationAndDetails{
      terminator->source_location(), "Unable to parse the loop",
      "The body between 'do' and 'done' is empty, a command is required"};
}

hot fn Parser::parse_command_list(u64 terminator_mask) throws -> Expression *
{
  /* Every nested compound command recurses through this list. A source nested
     past the limit throws here instead of overflowing the native stack. */
  m_command_depth++;
  defer { m_command_depth--; };
  if (m_command_depth > MAX_COMMAND_DEPTH) {
    Token *token = m_lexer.peek_shell_token();
    ASSERT(token != nullptr);
    throw koshka::ErrorWithLocation{
        token->source_location(),
        "Compound command nested deeper than " +
            String::from(static_cast<i64>(MAX_COMMAND_DEPTH),
                         heap_allocator())};
  }

  Command *lhs = nullptr;

  CompoundList *compound_list = m_lexer.arena().create<CompoundList>();
  CompoundListCondition::Kind next_cond = CompoundListCondition::Kind::None;

  bool should_parse_command = true;
  bool should_negate_pending = false;
  bool should_time_pending = false;
  bool is_time_posix_format = false;
  bool should_time_report_rss = false;
  SourceLocation time_location{};
  Maybe<usize> active_shellcheck_suppression{};

  let const do_finish_shellcheck_suppression = [&](usize end_position) {
    if (!active_shellcheck_suppression.has_value()) return;
    m_shellcheck_suppressions[*active_shellcheck_suppression].end_position =
        end_position;
    active_shellcheck_suppression = None;
  };

  let const do_finish_pending = [&](Command *pending, const Token *at) throws {
    if (should_negate_pending) {
      pending->set_negated();
      should_negate_pending = false;
    }
    if (should_time_pending) {
      pending->set_timed(is_time_posix_format, should_time_report_rss,
                         time_location);
      should_time_pending = false;
      is_time_posix_format = false;
      should_time_report_rss = false;
    }
    compound_list->append_node(m_lexer.arena().create<CompoundListCondition>(
        at->source_location(), next_cond, pending));
  };

  loop
  {
    if (should_parse_command) {
      /* A leading time keyword times the command or pipeline that follows. bash
         allows it before the ! negation, and -p or --posix selects the POSIX
         report. */
      Token *maybe_time = nullptr;
      if (m_should_collect_analysis_metadata) {
        let const should_collect_directives =
            next_cond == CompoundListCondition::Kind::None;
        m_lexer.set_should_collect_shellcheck_directives(
            should_collect_directives);
        maybe_time = m_lexer.peek_shell_token();
        let directives = m_lexer.take_shellcheck_directives();
        m_lexer.set_should_collect_shellcheck_directives(false);
        while (!directives.is_empty() &&
               maybe_time->kind() == Token::Kind::Newline)
        {
          m_lexer.advance_past_last_peek();
          m_lexer.set_should_collect_shellcheck_directives(true);
          maybe_time = m_lexer.peek_shell_token();
          let following_directives = m_lexer.take_shellcheck_directives();
          m_lexer.set_should_collect_shellcheck_directives(false);
          for (let const &directive : following_directives)
            directives.push(directive);
        }
        let const is_source_command =
            maybe_time->kind() != Token::Kind::Newline &&
            maybe_time->kind() != Token::Kind::EndOfFile;
        let const is_first_source_command =
            is_source_command && !m_has_parsed_source_command;
        if (is_source_command) m_has_parsed_source_command = true;
        if (!directives.is_empty()) {
          let const source = m_lexer.source();
          let selectors = ArrayList<shellcheck_selector>{heap_allocator()};
          for (let const &directive : directives)
            collect_shellcheck_selectors(source, directive, selectors);
          m_shellcheck_suppressions.push(shellcheck_suppression{
              maybe_time->source_location().position,
              is_first_source_command ? static_cast<usize>(-1)
                                      : maybe_time->source_location().position,
              steal(selectors)});
          if (!is_first_source_command)
            active_shellcheck_suppression =
                m_shellcheck_suppressions.count() - 1;
        }
      } else {
        maybe_time = m_lexer.peek_shell_token();
      }
      ASSERT(maybe_time != nullptr);
      const Token *leading_command_token = nullptr;
      if (maybe_time->kind() == Token::Kind::Time) {
        time_location = maybe_time->source_location();
        m_lexer.advance_past_last_peek();
        Token *maybe_option = m_lexer.peek_shell_token();
        if (m_lexer.mood() == mimic_mood::Default &&
            is_unquoted_word(maybe_option, "--help"))
        {
          leading_command_token = maybe_time;
        } else {
          should_time_pending = true;
          loop
          {
            if (is_unquoted_word(maybe_option, "-p") ||
                is_unquoted_word(maybe_option, "--posix"))
            {
              is_time_posix_format = true;
            } else if (is_unquoted_word(maybe_option, "-R")) {
              should_time_report_rss = true;
            } else {
              break;
            }
            m_lexer.advance_past_last_peek();
            maybe_option = m_lexer.peek_shell_token();
          }
        }
      }
      Token *maybe_negation = m_lexer.peek_shell_token();
      ASSERT(maybe_negation != nullptr);
      if (is_unquoted_word(maybe_negation, "!")) {
        m_lexer.advance_past_last_peek();
        should_negate_pending = true;
      }
      lhs = parse_simple_command(leading_command_token);
    } else {
      should_parse_command = true;
    }

    Token *token = m_lexer.peek_shell_token();
    ASSERT(token != nullptr);

    /* A terminator keyword is left for the caller to consume. */
    if (is_list_terminator(token, terminator_mask)) {
      do_finish_shellcheck_suppression(token->source_location().position);
      if (lhs != nullptr) {
        do_finish_pending(lhs, token);
      } else if (next_cond != CompoundListCondition::Kind::None) {
        throw koshka::ErrorWithLocation{token->source_location(),
                                        "Expected a command after an operator"};
      }
      if (compound_list->is_empty()) {
        return m_lexer.arena().create<DummyExpression>(
            token->source_location());
      }
      return compound_list;
    }

    switch (token->kind()) {
    case Token::Kind::Ampersand:
      if (lhs != nullptr) lhs->make_async();
      [[fallthrough]];
    case Token::Kind::DoublePipe:
    case Token::Kind::DoubleAmpersand:
      if (lhs == nullptr) {
        let const ast = token->to_ast_string();
        String msg = "Expected a command ";
        msg += compound_list->is_empty() ? "before" : "after";
        msg += " operator, found '";
        msg += ast.view();
        msg += "'";
        throw koshka::ErrorWithLocation{token->source_location(), msg};
      }
      [[fallthrough]];
    case Token::Kind::Newline:
    case Token::Kind::EndOfFile:
    case Token::Kind::Semicolon: {
      if (token->kind() != Token::Kind::DoublePipe &&
          token->kind() != Token::Kind::DoubleAmpersand)
      {
        do_finish_shellcheck_suppression(token->source_location().position);
      }
      m_lexer.advance_past_last_peek();

      if (lhs != nullptr) {
        do_finish_pending(lhs, token);
        next_cond = get_sequence_kind(token->kind());
      }

      if (token->kind() == Token::Kind::Newline &&
          m_should_stop_after_top_level_unit && m_command_depth == 1 &&
          next_cond == CompoundListCondition::Kind::None &&
          !compound_list->is_empty())
      {
        return compound_list;
      }

      if (token->kind() == Token::Kind::EndOfFile) {
        if (next_cond != CompoundListCondition::Kind::None) {
          throw koshka::ErrorWithLocation{
              token->source_location(), "Expected a command after an operator"};
        }

        if (compound_list->is_empty()) {
          return m_lexer.arena().create<DummyExpression>(
              token->source_location());
        }

        return compound_list;
      }
    } break;

    case Token::Kind::Pipe:
    case Token::Kind::PipeAmpersand: {
      if (lhs == nullptr) {
        throw koshka::ErrorWithLocation{token->source_location(),
                                        "Expected a command before the pipe"};
      }

      /* A |& pipe routes the left command's stderr into the pipe too, the
         shorthand for 2>&1 |. */
      let const has_left_stderr_pipe =
          token->kind() == Token::Kind::PipeAmpersand;
      m_lexer.advance_past_last_peek();
      skip_newlines_after_pipe();

      Pipeline *pipeline =
          m_lexer.arena().create<Pipeline>(token->source_location());
      pipeline->append_command(
          has_left_stderr_pipe ? wrap_with_stderr_to_stdout(lhs) : lhs);

      Token *last_pipe_token = token;

      loop
      {
        Command *rhs = parse_simple_command();
        if (rhs == nullptr) {
          /* An ampersand glued to the pipe under POSIX mode is the bash |&
             stderr pipe read as | then &. */
          Token *after = m_lexer.peek_shell_token();
          if (m_lexer.is_posix_mode() && after != nullptr &&
              after->kind() == Token::Kind::Ampersand &&
              after->source_location().position ==
                  last_pipe_token->source_location().position + 1)
          {
            throw koshka::ErrorWithLocationAndDetails{
                last_pipe_token->source_location(),
                "Unable to build the pipeline because no command follows "
                "the pipe. The |& stderr pipe is a bashism that POSIX mode "
                "does not read",
                "Use 2>&1 | instead"};
          }
          throw koshka::ErrorWithLocation{
              last_pipe_token->source_location(),
              "Unable to build the pipeline because no command follows the "
              "pipe to receive the output"};
        }

        last_pipe_token = m_lexer.peek_shell_token();
        ASSERT(last_pipe_token != nullptr);
        const bool has_another_pipe =
            last_pipe_token->kind() == Token::Kind::Pipe ||
            last_pipe_token->kind() == Token::Kind::PipeAmpersand;
        const bool should_pipe_standard_error =
            last_pipe_token->kind() == Token::Kind::PipeAmpersand;
        pipeline->append_command(has_another_pipe && should_pipe_standard_error
                                     ? wrap_with_stderr_to_stdout(rhs)
                                     : rhs);
        if (has_another_pipe) {
          m_lexer.advance_past_last_peek();
          skip_newlines_after_pipe();
          continue;
        }
        break;
      }

      lhs = pipeline;

      should_parse_command = false;
    } break;

    default:
      throw ErrorWithLocation{token->source_location(),
                              unexpected_command_token_message(token)};
    }
  }

  unreachable(
      "the command-list parser loop terminated without returning or throwing");
}

static fn stderr_to_stdout_dup() wontthrow -> expressions::Redirection
{
  expressions::Redirection dup{};
  dup.fd = 2;
  dup.target = nullptr;
  dup.kind = expressions::Redirection::Kind::DuplicateOutput;
  dup.dup_fd = 1;
  return dup;
}

/* A & touching the operator means a descriptor duplication, n>&m, otherwise a
   filename word follows. */
fn Parser::build_file_or_dup_redirection(
    i32 fd, Token::Kind op_kind, const SourceLocation &op_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out, bool fd_was_explicit,
    const Token *fd_allocation_name_token) throws -> void
{
  if (!first_location) first_location = op_location;

  expressions::Redirection redir{};
  redir.fd = fd;
  redir.target = nullptr;
  redir.dup_fd = -1;
  redir.fd_allocation_name_token = fd_allocation_name_token;

  {
    Token *after = m_lexer.peek_shell_token();
    ASSERT(after != nullptr);
    if (after->kind() == Token::Kind::Ampersand &&
        after->source_location().position ==
            op_location.position + op_location.length)
    {
      m_lexer.advance_past_last_peek();
      Token *from = m_lexer.next_shell_token();
      if (from->kind() != Token::Kind::Word) {
        throw ErrorWithLocation{from->source_location(),
                                "Expected a descriptor after '&'"};
      }
      let const &from_word = static_cast<tokens::WordToken *>(from)->word();

      redir.kind = (op_kind == Token::Kind::Less)
                       ? expressions::Redirection::Kind::DuplicateInput
                       : expressions::Redirection::Kind::DuplicateOutput;

      let const literal = from_word.to_literal_string();

      /* The close form >&- and <&- closes fd outright, the dash arriving as
         part of the following word. */
      if (literal == "-") {
        redir.dup_fd = expressions::Redirection::DUP_FD_CLOSE;
        out.push(redir);
        return;
      }

      /* A wholly-digit word names the descriptor at parse time, anything else
         such as $4 or ${fd} resolves when the redirection runs. */
      if (literal.view().is_all_decimal_digits()) {
        let const parsed_descriptor = literal.to<i64>();
        if (parsed_descriptor.is_error()) {
          throw ErrorWithLocation{from->source_location(),
                                  parsed_descriptor.error().message()};
        }
        redir.dup_fd = static_cast<i32>(parsed_descriptor.value());
        out.push(redir);
        return;
      }

      /* A bare >&word in every mood but POSIX may be the csh both-streams
         spelling, cmd >&/dev/null, decided after the expansion. An explicit
         descriptor as in 2>&word keeps the strict error. */
      redir.target = from;
      redir.is_dup_filename_allowed = op_kind == Token::Kind::Greater &&
                                      !fd_was_explicit &&
                                      !m_lexer.is_posix_mode();
      out.push(redir);
      return;
    }
  }

  {
    Token *after = m_lexer.peek_shell_token();
    ASSERT(after != nullptr);
    /* The second character must touch the operator, so a real pipe in cmd >file
       | next stays separate from >| and <>. */
    let const is_adjacent = after->source_location().position ==
                            op_location.position + op_location.length;

    /* >| truncates the target even under noclobber, the explicit override. */
    if (op_kind == Token::Kind::Greater && after->kind() == Token::Kind::Pipe &&
        is_adjacent)
    {
      m_lexer.advance_past_last_peek();
      Token *target = m_lexer.next_shell_token();
      if (target->kind() != Token::Kind::Word) {
        throw ErrorWithLocation{target->source_location(),
                                "Expected a filename after '>|'"};
      }
      redir.kind = expressions::Redirection::Kind::TruncateOutputOverride;
      redir.target = target;
      out.push(redir);
      return;
    }

    /* <> opens the target for reading and writing, creating it if absent. */
    if (op_kind == Token::Kind::Less && after->kind() == Token::Kind::Greater &&
        is_adjacent)
    {
      m_lexer.advance_past_last_peek();
      Token *target = m_lexer.next_shell_token();
      if (target->kind() != Token::Kind::Word) {
        throw ErrorWithLocation{target->source_location(),
                                "Expected a filename after '<>'"};
      }
      redir.kind = expressions::Redirection::Kind::ReadWrite;
      redir.target = target;
      out.push(redir);
      return;
    }
  }

  Token *target = m_lexer.next_shell_token();
  ASSERT(target != nullptr);
  if (target->kind() != Token::Kind::Word) {
    throw ErrorWithLocation{target->source_location(),
                            "Expected a filename after the redir"};
  }
  switch (op_kind) {
  case Token::Kind::Greater:
    redir.kind = expressions::Redirection::Kind::TruncateOutput;
    break;
  case Token::Kind::DoubleGreater:
    redir.kind = expressions::Redirection::Kind::AppendOutput;
    break;
  case Token::Kind::Less:
    redir.kind = expressions::Redirection::Kind::ReadInput;
    break;
  default:
    unreachable("the file redirection builder received token kind %d",
                ENUM(op_kind));
  }
  redir.target = target;
  out.push(redir);
}

fn Parser::build_both_streams_redirection(
    bool is_append, const SourceLocation &op_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> void
{
  build_file_or_dup_redirection(
      1, is_append ? Token::Kind::DoubleGreater : Token::Kind::Greater,
      op_location, first_location, out, /*fd_was_explicit=*/true);
  out.back().is_both_streams_spelling = true;
  out.push(stderr_to_stdout_dup());
}

fn Parser::build_here_string_redirection(
    const SourceLocation &op_location, Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> void
{
  if (!first_location) first_location = op_location;

  Token *word = m_lexer.next_shell_token();
  ASSERT(word != nullptr);
  if (word->kind() != Token::Kind::Word) {
    throw ErrorWithLocation{word->source_location(),
                            "Expected a word after '<<<'"};
  }

  expressions::Redirection redir{};
  redir.fd = 0;
  redir.kind = expressions::Redirection::Kind::HereString;
  redir.target = word;
  redir.dup_fd = -1;
  redir.heredoc = nullptr;
  redir.should_expand_heredoc = false;
  out.push(redir);
}

mustuse fn Parser::wrap_with_stderr_to_stdout(Command *command) throws
    -> Command *
{
  ASSERT(command != nullptr);
  let redirections = ArrayList<expressions::Redirection>{heap_allocator()};
  redirections.push(stderr_to_stdout_dup());
  let redirected = m_lexer.arena().create<RedirectedCommand>(
      command->source_location(), command, steal(redirections));
  redirected->set_source_end_position(command->source_end_position());
  return redirected;
}

fn Parser::build_heredoc_redirection(
    i32 fd, const SourceLocation &op_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> void
{
  if (!first_location) first_location = op_location;

  Token *delimiter_token = m_lexer.next_shell_token();
  ASSERT(delimiter_token != nullptr);
  if (delimiter_token->kind() != Token::Kind::Word) {
    /* A <<<word in POSIX mode tokenizes as << then <word, so a stray < here is
       the bash here-string in a mode that does not read it. */
    if (delimiter_token->kind() == Token::Kind::Less) {
      throw ErrorWithLocationAndDetails{
          delimiter_token->source_location(),
          "Expected a heredoc delimiter. The <<< here-string is a bashism "
          "that POSIX mode does not read",
          "Use a heredoc instead"};
    }
    throw ErrorWithLocation{delimiter_token->source_location(),
                            "Expected a heredoc delimiter"};
  }
  const Word &delimiter_word =
      static_cast<tokens::WordToken *>(delimiter_token)->word();

  let const delimiter_literal = delimiter_word.to_literal_string();
  let delimiter = delimiter_literal.view();
  bool should_strip_tabs = false;
  /* <<- strips leading tabs. The dash counts only when unquoted, so <<'-EOF'
     keeps the dash in the delimiter and terminates on -EOF. */
  let const has_unquoted_leading_dash =
      !delimiter_word.segments.is_empty() &&
      delimiter_word.segments[0].kind == WordSegment::Kind::UnquotedText &&
      !delimiter_word.segments[0].text.is_empty() &&
      delimiter_word.segments[0].text.view()[0] == '-';
  if (has_unquoted_leading_dash && !delimiter.is_empty() &&
      delimiter[0] == '-' &&
      delimiter_token->source_location().position ==
          op_location.position + op_location.length)
  {
    should_strip_tabs = true;
    delimiter = delimiter.substring(1);
  }

  LOG(Debug, "registering a heredoc redirection with delimiter '%.*s'",
      static_cast<int>(delimiter.length), delimiter.data);

  /* A quoted delimiter, such as <<'EOF', keeps the body literal. */
  bool should_expand = true;
  for (let const &segment : delimiter_word.segments) {
    if (segment.kind != WordSegment::Kind::UnquotedText) {
      should_expand = false;
      break;
    }
  }

  expressions::Redirection redir{};
  redir.fd = fd;
  redir.kind = expressions::Redirection::Kind::Heredoc;
  redir.target = nullptr;
  redir.dup_fd = -1;
  redir.heredoc = m_lexer.register_heredoc(delimiter, should_strip_tabs);
  redir.should_expand_heredoc = should_expand;
  out.push(redir);
}

mustuse fn Parser::try_parse_descriptor_prefixed_redirection(
    const tokens::WordToken *word_token, const SourceLocation &word_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> bool
{
  m_lexer.advance_past_last_peek();
  Token *next = m_lexer.peek_shell_token();
  ASSERT(next != nullptr);
  let const nk = next->kind();
  if ((nk == Token::Kind::Greater || nk == Token::Kind::DoubleGreater ||
       nk == Token::Kind::Less || nk == Token::Kind::DoubleLess) &&
      next->source_location().position ==
          word_location.position + word_location.length)
  {
    let const op_location = next->source_location();
    m_lexer.advance_past_last_peek();

    let const allocation_name = word_token->word().fd_allocation_name();
    if (allocation_name.has_value()) {
      if (nk == Token::Kind::DoubleLess) {
        throw ErrorWithLocation{word_location,
                                "A heredoc descriptor cannot be allocated"};
      }
      build_file_or_dup_redirection(-1, nk, op_location, first_location, out,
                                    /*fd_was_explicit=*/true, word_token);
      return true;
    }

    let const literal = word_token->word().to_literal_string();
    let const parsed_descriptor = literal.to<i64>();
    if (parsed_descriptor.is_error()) {
      throw ErrorWithLocation{word_location,
                              parsed_descriptor.error().message()};
    }
    let const fd = static_cast<i32>(parsed_descriptor.value());
    if (nk == Token::Kind::DoubleLess) {
      build_heredoc_redirection(fd, op_location, first_location, out);
    } else {
      build_file_or_dup_redirection(fd, nk, op_location, first_location, out,
                                    /*fd_was_explicit=*/true);
    }
    return true;
  }
  return false;
}

/* A digit word touching a redirect operator is a descriptor prefix, such as the
   2 in 2>file. */
mustuse fn Parser::try_parse_trailing_redirection(
    ArrayList<expressions::Redirection> &out) throws -> bool
{
  Maybe<SourceLocation> ignored_first_location;

  Token *token = m_lexer.peek_shell_token();
  ASSERT(token != nullptr);

  switch (token->kind()) {
  case Token::Kind::Greater:
  case Token::Kind::DoubleGreater:
  case Token::Kind::Less: {
    let const op_kind = token->kind();
    let const op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_file_or_dup_redirection((op_kind == Token::Kind::Less) ? 0 : 1,
                                  op_kind, op_location, ignored_first_location,
                                  out, /*fd_was_explicit=*/false);
    return true;
  }

  case Token::Kind::AmpersandGreater:
  case Token::Kind::AmpersandDoubleGreater: {
    let const op_kind = token->kind();
    let const op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_both_streams_redirection(op_kind ==
                                       Token::Kind::AmpersandDoubleGreater,
                                   op_location, ignored_first_location, out);
    return true;
  }

  case Token::Kind::DoubleLess: {
    let const op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_heredoc_redirection(0, op_location, ignored_first_location, out);
    return true;
  }

  case Token::Kind::TripleLess: {
    let const op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_here_string_redirection(op_location, ignored_first_location, out);
    return true;
  }

  case Token::Kind::Word: {
    const tokens::WordToken *word_token =
        static_cast<tokens::WordToken *>(token);
    if (!word_token->word().is_all_ascii_digits() &&
        !word_token->word().fd_allocation_name().has_value())
    {
      return false;
    }

    let const word_location = token->source_location();
    if (try_parse_descriptor_prefixed_redirection(word_token, word_location,
                                                  ignored_first_location, out))
    {
      return true;
    }

    throw ErrorWithLocationAndDetails{
        word_location, "Unexpected word after a compound command",
        "A compound command takes no extra words before its terminator"};
  }

  default: return false;
  }
}

mustuse fn Parser::attach_trailing_redirections(Command *compound) throws
    -> Command *
{
  ASSERT(compound != nullptr);

  /* The wrapper location is the compound's opening token, so the end is taken
     from the lexer after each redirection is consumed. Without it the span
     would close after that one token and a function body would print as `{`. */
  let end_position = compound->source_end_position();
  let redirections = ArrayList<expressions::Redirection>{heap_allocator()};
  while (try_parse_trailing_redirection(redirections))
    end_position = m_lexer.cursor_position();

  if (redirections.is_empty()) return compound;

  let redirected = m_lexer.arena().create<RedirectedCommand>(
      compound->source_location(), compound, steal(redirections));
  redirected->set_source_end_position(end_position);

  return redirected;
}

enum class command_position_word : u8
{
  None,
  BraceOpen,
  BraceClose,
  Conditional,
  Select,
};

/* Returns a command, a compound command, or nullptr when a list terminator is
   next. A reserved word or a group opener in command position starts a compound
   command. */
hot fn Parser::parse_simple_command(const Token *leading_token) throws
    -> Command *
{
  Maybe<SourceLocation> source_location;
  let const arena_allocator = bump_allocator(m_lexer.arena());
  ArrayList<const Token *> args_accumulator{heap_allocator()};
  let local_vars = ArrayList<PrefixAssignment>{heap_allocator()};
  let array_args = ArrayList<array_builtin_assignment>{heap_allocator()};
  let redirections = ArrayList<expressions::Redirection>{heap_allocator()};

  if (leading_token != nullptr) {
    source_location = leading_token->source_location();
    args_accumulator.push(leading_token);
  }

  let const do_build_command = [&]() -> Command * {
    if (!source_location) return nullptr;

    record_analysis_alias_definitions(args_accumulator);

    args_accumulator.move_to_allocator(arena_allocator);
    local_vars.move_to_allocator(arena_allocator);
    array_args.move_to_allocator(arena_allocator);
    redirections.move_to_allocator(arena_allocator);

    SimpleCommand *c = m_lexer.arena().create<SimpleCommand>(
        *source_location, steal(args_accumulator));
    if (local_vars.count() != 0) c->set_local_vars(steal(local_vars));
    if (!array_args.is_empty()) c->set_array_args(steal(array_args));
    if (!redirections.is_empty()) c->set_redirections(steal(redirections));
    return c;
  };

  let const do_add_redirection = [&](i32 fd, Token::Kind op_kind,
                                     const SourceLocation &op_location,
                                     bool fd_was_explicit) {
    build_file_or_dup_redirection(fd, op_kind, op_location, source_location,
                                  redirections, fd_was_explicit);
  };

  loop
  {
    Token *token = m_lexer.peek_shell_token();
    ASSERT(token != nullptr);

    if (!source_location) {
      let position_word = command_position_word::None;
      if (let const *text = get_unquoted_word_text(token); text != nullptr) {
        let const view = text->view();
        if (!view.is_empty()) {
          switch (view[0]) {
          case '{':
            if (view.length == 1)
              position_word = command_position_word::BraceOpen;
            break;
          case '}':
            if (view.length == 1)
              position_word = command_position_word::BraceClose;
            break;
          case '[':
            if (view.length == 2 && view[1] == '[')
              position_word = command_position_word::Conditional;
            break;
          case 's':
            if (view.length == 6 && view == "select")
              position_word = command_position_word::Select;
            break;
          default: break;
          }
        }
      }

      /* A standalone '{' opens a brace group, a standalone '}' closes one, both
         arriving as words. A '}' with no open group is left for the caller. */
      switch (position_word) {
      case command_position_word::BraceOpen:
        return attach_trailing_redirections(parse_brace_group());
      case command_position_word::BraceClose: return nullptr;
      case command_position_word::Conditional:
        /* The sh mood is POSIX, where [[ is not a keyword, so the conditional
           is rejected there. */
        if (m_lexer.is_posix_mode()) {
          throw ErrorWithLocation{token->source_location(),
                                  "The [[ conditional is a bash extension that "
                                  "the sh mood does not "
                                  "provide"};
        }
        return attach_trailing_redirections(parse_conditional_command());
      case command_position_word::Select:
        /* Select is not a reserved word in the lexer, so it is matched on the
           text in bash mode. */
        if (m_lexer.is_bash_compatible())
          return attach_trailing_redirections(parse_select());
        break;
      case command_position_word::None: break;
      }

      switch (token->kind()) {
      case Token::Kind::If: return attach_trailing_redirections(parse_if());
      case Token::Kind::While:
        return attach_trailing_redirections(parse_while_or_until(false));
      case Token::Kind::Until:
        return attach_trailing_redirections(parse_while_or_until(true));
      case Token::Kind::For: return attach_trailing_redirections(parse_for());
      case Token::Kind::Case: return attach_trailing_redirections(parse_case());
      case Token::Kind::LeftParen:
        return attach_trailing_redirections(parse_paren_command());

      case Token::Kind::Then:
      case Token::Kind::Do:
      case Token::Kind::Done:
      case Token::Kind::Fi:
      case Token::Kind::Else:
      case Token::Kind::Elif:
      case Token::Kind::Esac:
      case Token::Kind::RightParen:
      case Token::Kind::DoubleSemicolon: return nullptr;

      default: break;
      }
    }

    switch (token->kind()) {
    /* A reserved word out of command position is an ordinary word. */
    case Token::Kind::Word:
    case Token::Kind::If:
    case Token::Kind::Then:
    case Token::Kind::Else:
    case Token::Kind::Elif:
    case Token::Kind::Fi:
    case Token::Kind::While:
    case Token::Kind::Until:
    case Token::Kind::For:
    case Token::Kind::Do:
    case Token::Kind::Done:
    case Token::Kind::Case:
    case Token::Kind::Esac:
    case Token::Kind::Time:
    case Token::Kind::When: {
      /* A run of digits touching a redir operator is a descriptor prefix, such
         as the 2 in 2>file, not an argument. */
      if (token->kind() == Token::Kind::Word) {
        const tokens::WordToken *word_token =
            static_cast<tokens::WordToken *>(token);
        if (word_token->word().is_all_ascii_digits() ||
            word_token->word().fd_allocation_name().has_value())
        {
          let const word_location = token->source_location();
          if (try_parse_descriptor_prefixed_redirection(
                  word_token, word_location, source_location, redirections))
          {
            break;
          }
          if (!source_location) source_location = word_location;
          args_accumulator.push(token);
          break;
        }
      }
      m_lexer.advance_past_last_peek();
      if (!source_location) source_location = token->source_location();
      args_accumulator.push(token);
    } break;

    case Token::Kind::Function:
      /* The bash function keyword begins a definition only when it leads the
         command. */
      if (args_accumulator.is_empty() && local_vars.count() == 0) {
        m_lexer.advance_past_last_peek();
        return parse_keyword_function_definition();
      }
      m_lexer.advance_past_last_peek();
      if (!source_location) source_location = token->source_location();
      args_accumulator.push(token);
      break;

    case Token::Kind::LeftParen:
      if (args_accumulator.count() == 1 && local_vars.count() == 0 &&
          args_accumulator[0]->kind() == Token::Kind::Word)
      {
        return parse_function_definition(args_accumulator[0]);
      }
      return do_build_command();

    case Token::Kind::Assignment: {
      m_lexer.advance_past_last_peek();
      if (!source_location) source_location = token->source_location();

      Assignment *a = static_cast<Assignment *>(token);

      Token *next = m_lexer.peek_shell_token();
      ASSERT(next != nullptr);

      let const is_array_assignment =
          next->kind() == Token::Kind::LeftParen &&
          next->source_location().position ==
              a->source_location().position + a->source_location().length;

      /* Once a command word is present, an assignment-looking token is an
         ordinary argument, except an array assignment given to a builtin such
         as local. */
      if (!args_accumulator.is_empty()) {
        if (is_array_assignment) {
          let const command_name = args_accumulator[0]->raw_string();
          if (classify_assignment_builtin(command_name.view()) !=
              assignment_builtin::None)
          {
            ArrayList<const Token *> elements = consume_bash_array_assignment();
            array_args.push(
                array_builtin_assignment{a->key().clone(), steal(elements),
                                         a->source_location(), a->is_append()});
            break;
          }
        }
        args_accumulator.push(token);
        break;
      }

      /* NAME=(...) leading the command is captured in every mood. POSIX mode
         downgrades it to an empty scalar at evaluation. */
      if (is_array_assignment) {
        ArrayList<const Token *> elements = consume_bash_array_assignment();
        array_args.push(
            array_builtin_assignment{a->key().clone(), steal(elements),
                                     a->source_location(), a->is_append()});
        break;
      }

      if (local_vars.count() == 0 &&
          (is_compound_list_separator(next->kind()) ||
           next->kind() == Token::Kind::EndOfFile ||
           is_compound_terminator(next->kind())))
      {
        return m_lexer.arena().create<AssignCommand>(*source_location, a);
      } else {
        /* Kept in source order so a later assignment sees an earlier one and a
           repeated name accumulates, which a map would lose. */
        local_vars.push(PrefixAssignment{a});
      }
    } break;

    case Token::Kind::Greater:
    case Token::Kind::DoubleGreater:
    case Token::Kind::Less: {
      let const op_kind = token->kind();
      let const op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      do_add_redirection((op_kind == Token::Kind::Less) ? 0 : 1, op_kind,
                         op_location, /*fd_was_explicit=*/false);
    } break;

    case Token::Kind::AmpersandGreater:
    case Token::Kind::AmpersandDoubleGreater: {
      let const op_kind = token->kind();
      let const op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      build_both_streams_redirection(
          op_kind == Token::Kind::AmpersandDoubleGreater, op_location,
          source_location, redirections);
    } break;

    case Token::Kind::DoubleLess: {
      let const op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      build_heredoc_redirection(0, op_location, source_location, redirections);
    } break;

    case Token::Kind::TripleLess: {
      let const op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      build_here_string_redirection(op_location, source_location, redirections);
    } break;

    default: return do_build_command();
    }
  }

  unreachable("the simple-command parser loop terminated without returning");
}

fn Parser::finish_function_body(const SourceLocation &location,
                                StringView name) throws -> Command *
{
  skip_newlines_after_pipe();

  record_analysis_scope_definition(name, false);
  let const scope_mark = open_analysis_scope();

  let body_storage = FunctionBodyHandle::create();
  BumpArena &per_command_arena = m_lexer.arena();
  BumpArena *previous_function_arena = FUNCTION_ARENA;
  m_lexer.set_arena(*body_storage.get_arena());
  FUNCTION_ARENA = body_storage.get_arena();
  Command *body = nullptr;
  try {
    body = parse_simple_command();
  } catch (...) {
    FUNCTION_ARENA = previous_function_arena;
    m_lexer.set_arena(per_command_arena);
    throw;
  }
  FUNCTION_ARENA = previous_function_arena;
  m_lexer.set_arena(per_command_arena);

  if (body == nullptr) {
    throw ErrorWithLocation{location,
                            "Expected a compound command as the function body"};
  }
  body_storage.set_body(body);

  /* The span ends where the body ends so declare -f can print the definition
     text from the source. */
  let definition = m_lexer.arena().create<FunctionDefinition>(
      location, name, steal(body_storage));
  definition->set_analysis_scope_definitions(close_analysis_scope(scope_mark));
  definition->set_source_end_position(body->source_end_position());
  return definition;
}

hot fn Parser::parse_function_definition(const Token *name_token) throws
    -> Command *
{
  ASSERT(name_token != nullptr);
  let const location = name_token->source_location();
  let const name = name_token->raw_string();

  LOG(Debug, "parsing a function definition for '%s'", name.c_str());

  m_lexer.advance_past_last_peek();
  Token *close = m_lexer.next_shell_token();
  ASSERT(close != nullptr);
  if (close->kind() != Token::Kind::RightParen) {
    throw ErrorWithLocation{close->source_location(),
                            "Expected ')' in a function definition"};
  }

  return finish_function_body(location, name.view());
}

fn Parser::parse_keyword_function_definition() throws -> Command *
{
  Token *name_token = m_lexer.next_shell_token();
  ASSERT(name_token != nullptr);
  if (name_token->kind() != Token::Kind::Word) {
    throw ErrorWithLocation{name_token->source_location(),
                            "Expected a name after the 'function' keyword"};
  }
  let const location = name_token->source_location();
  let const name = name_token->raw_string();

  LOG(Debug, "parsing a keyword function definition for '%s'", name.c_str());

  /* An empty () pair may follow the name in the bash function form. */
  Token *after_name = m_lexer.peek_shell_token();
  ASSERT(after_name != nullptr);
  if (after_name->kind() == Token::Kind::LeftParen) {
    m_lexer.advance_past_last_peek();
    Token *close = m_lexer.next_shell_token();
    ASSERT(close != nullptr);
    if (close->kind() != Token::Kind::RightParen) {
      throw ErrorWithLocation{close->source_location(),
                              "Expected ')' in a function definition"};
    }
  }

  return finish_function_body(location, name.view());
}

fn Parser::consume_bash_array_assignment() throws -> ArrayList<const Token *>
{
  Token *open = m_lexer.next_shell_token();
  ASSERT(open != nullptr);
  ASSERT(open->kind() == Token::Kind::LeftParen);

  /* Every token inside the outermost pair is kept so bash mode expands them as
     array elements, while POSIX mode discards the list. */
  ArrayList<const Token *> elements{heap_allocator()};
  usize depth = 1;
  loop
  {
    Token *t = m_lexer.next_shell_token();
    ASSERT(t != nullptr);
    if (t->kind() == Token::Kind::EndOfFile) {
      throw ErrorWithLocation{open->source_location(),
                              "Unterminated array assignment, expected ')'"};
    }
    if (t->kind() == Token::Kind::LeftParen) {
      depth++;
      elements.push(t);
    } else if (t->kind() == Token::Kind::RightParen) {
      depth--;
      if (depth == 0) break;
      elements.push(t);
    } else {
      if (t->kind() != Token::Kind::Newline) elements.push(t);
    }
  }
  return elements;
}

} /* namespace koshka */
