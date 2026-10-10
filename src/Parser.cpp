/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file parses simple commands, functions, pipelines, command lists,
 * redirections, and heredocs into syntax-tree nodes. It also collects parser
 * diagnostics and analysis scopes. ParserCompound.cpp owns larger recursive
 * grammar productions.
 */

#include "Parser.hpp"

#include "Errors.hpp"
#include "Expressions.hpp"
#include "Optimizer.hpp"
#include "ParserInternal.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

using namespace tokens;
using namespace expressions;
using internal::get_unquoted_word_text;
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

alwaysinline fn Parser::next_token_of_kind(Token::Kind kind,
                                           StringView missing_message) throws
    -> Token *
{
  Token *token = m_lexer.next_shell_token();
  ASSERT(token != nullptr);
  if (token->kind() != kind) {
    throw ErrorWithLocation{token->source_location(), missing_message};
  }

  return token;
}

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

fn Parser::take_analysis_directives() throws -> analysis_directives
{
  analysis_directives directives{};
  directives.shellcheck_suppressions = take_shellcheck_suppressions();
  directives.scope_definitions = take_analysis_scope_definitions();
  directives.directive_spans = take_shellcheck_directive_spans();
  directives.heredoc_misses = take_heredoc_terminator_misses();
  directives.source_name_index = m_lexer.source_name_index();

  return directives;
}

fn Parser::record_analysis_scope_definition(
    StringView name, analysis_scope_definition_kind kind) throws -> void
{
  if (m_analysis_scope_collection_mode !=
      analysis_metadata_collection_mode::Enabled)
    return;

  m_analysis_scope_definitions.push(
      analysis_scope_definition{String{name}, kind});
}

fn Parser::record_analysis_alias_definitions(
    const ArrayList<const Token *> &args) throws -> void
{
  if (m_analysis_scope_collection_mode !=
          analysis_metadata_collection_mode::Enabled ||
      args.is_empty())
  {
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
          StringView{text.data(), *equals_position},
          analysis_scope_definition_kind::Alias);
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

hot pure fn internal::get_unquoted_word_text(const Token *token) wontthrow
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

hot pure static fn is_list_terminator(const Token *token,
                                      u64 terminator_mask) wontthrow -> bool
{
  ASSERT(token != nullptr);
  return (terminator_mask & (u64{1} << static_cast<u8>(token->kind()))) != 0 ||
         ((terminator_mask &
           (u64{1} << static_cast<u8>(Token::Kind::RightBracket))) != 0 &&
          is_unquoted_word(token, "}"));
}

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

cold wontreturn fn internal::throw_unterminated(
    const SourceLocation &opener, StringView what, StringView source,
    StringView keyword, SourceLocation fallback) throws -> void
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
  if (m_analysis_metadata_collection_mode ==
      analysis_metadata_collection_mode::Enabled)
    m_lexer.set_shellcheck_directive_collection_mode(
        shellcheck_directive_collection_mode::Enabled);
  let const first_token = m_lexer.peek_shell_token();
  if (m_analysis_metadata_collection_mode ==
      analysis_metadata_collection_mode::Enabled)
    m_lexer.set_shellcheck_directive_collection_mode(
        shellcheck_directive_collection_mode::Disabled);
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

alwaysinline static fn
get_location_filename(const SourceLocation &location) throws -> String
{
  let const name = location.get_filename();
  return name.has_value() ? String{*name} : String{heap_allocator()};
}

cold fn Parser::record_detailed_parse_error(
    const ErrorWithLocationAndDetails &error, ArrayList<String> &errors,
    EvalContext *context, ArrayList<source_diagnostic> *diagnostic_sink) throws
    -> void
{
  LOG(Debug, "recording a detailed parse error and recovering: %s",
      error.message().c_str());
  if (m_error_collection != nullptr) {
    m_error_collection->push(error);

    return;
  }

  errors.push(error.to_string(m_lexer.source(), context));
  errors.push(error.details_to_string(m_lexer.source(), context));
  if (diagnostic_sink == nullptr) return;

  let const location = error.location();
  let const details_location = error.details_location();
  let const related_location = error.details_message().is_empty()
                                   ? Maybe<SourceLocation>{None}
                                   : Maybe<SourceLocation>{details_location};

  diagnostic_sink->push(source_diagnostic{
      None, error_severity::error, location, get_location_filename(location),
      error.message().clone(), String{error.detail_message()}, related_location,
      get_location_filename(details_location), String{error.details_message()},
      ArrayList<source_fix>{heap_allocator()}});
}

cold fn Parser::record_parse_error(
    const ErrorWithLocation &error, ArrayList<String> &errors,
    EvalContext *context, ArrayList<source_diagnostic> *diagnostic_sink) throws
    -> void
{
  LOG(Debug, "recording a parse error and recovering: %s",
      error.message().c_str());
  if (m_error_collection != nullptr) {
    m_error_collection->push(ErrorWithLocationAndDetails{
        error.location(), error.message().view(), error.detail_message()});

    return;
  }

  errors.push(error.to_string(m_lexer.source(), context));
  if (diagnostic_sink == nullptr) return;

  let const location = error.location();
  diagnostic_sink->push(source_diagnostic{
      None, error_severity::error, location, get_location_filename(location),
      error.message().clone(), String{error.detail_message()}, None,
      String{heap_allocator()}, String{heap_allocator()},
      ArrayList<source_fix>{heap_allocator()}});
}

cold fn Parser::record_substitution_errors(
    ArrayList<String> &errors, EvalContext *context,
    ArrayList<source_diagnostic> *diagnostic_sink) throws -> void
{
  if (!m_lexer.has_substitution_errors()) return;

  let const substitution_errors = m_lexer.take_substitution_errors();
  for (let const &error : substitution_errors) {
    if (error.details_message().is_empty()) {
      record_parse_error(error, errors, context, diagnostic_sink);
    } else {
      record_detailed_parse_error(error, errors, context, diagnostic_sink);
    }
  }
}

cold fn Parser::record_error(
    ArrayList<String> &errors, EvalContext *context,
    ArrayList<source_diagnostic> *diagnostic_sink) throws -> void
{
  record_substitution_errors(errors, context, diagnostic_sink);

  try {
    throw;
  } catch (const ErrorWithLocationAndDetails &e) {
    record_detailed_parse_error(e, errors, context, diagnostic_sink);
  } catch (const ErrorWithLocation &e) {
    record_parse_error(e, errors, context, diagnostic_sink);
  }
}

alwaysinline fn Parser::peek_top_level_token(
    ArrayList<String> &errors, EvalContext *context,
    ArrayList<source_diagnostic> *diagnostic_sink) throws -> Token *
{
  let const is_collecting_directives =
      m_analysis_metadata_collection_mode ==
      analysis_metadata_collection_mode::Enabled;
  if (is_collecting_directives)
    m_lexer.set_shellcheck_directive_collection_mode(
        shellcheck_directive_collection_mode::Enabled);

  Token *token = nullptr;
  try {
    token = m_lexer.peek_shell_token();
  } catch (const ErrorWithLocation &) {
    record_error(errors, context, diagnostic_sink);
  }

  if (is_collecting_directives)
    m_lexer.set_shellcheck_directive_collection_mode(
        shellcheck_directive_collection_mode::Disabled);
  record_substitution_errors(errors, context, diagnostic_sink);
  return token;
}

cold fn Parser::construct_ast(
    ArrayList<String> &errors, EvalContext *context,
    ArrayList<source_diagnostic> *diagnostic_sink) throws -> Expression *
{
  Expression *first_piece = nullptr;
  let last_location = SourceLocation{};
  usize failed_statement_count = 0;

  loop
  {
    Token *token = peek_top_level_token(errors, context, diagnostic_sink);
    if (token == nullptr) break;

    last_location = token->source_location();
    if (token->kind() == Token::Kind::EndOfFile) break;

    let did_parse_fail = false;
    try {
      Expression *piece = parse_command_list(0);
      ASSERT(piece != nullptr);
      if (first_piece == nullptr) first_piece = piece;
      record_substitution_errors(errors, context, diagnostic_sink);
    } catch (const ErrorWithLocation &) {
      record_error(errors, context, diagnostic_sink);
      did_parse_fail = true;
    }
    if (!did_parse_fail) continue;

    failed_statement_count++;
    if (failed_statement_count >= MAX_RECOVERED_STATEMENT_COUNT) break;

    let did_recovery_fail = false;
    try {
      recover_to_next_statement();
    } catch (const ErrorWithLocation &) {
      did_recovery_fail = true;
    }
    record_substitution_errors(errors, context, diagnostic_sink);
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
    Token *token = peek_top_level_token(errors, context, diagnostic_sink);
    if (token == nullptr || token->kind() == Token::Kind::EndOfFile)
      return nullptr;

    try {
      Expression *piece = parse_command_list(0);
      ASSERT(piece != nullptr);
      record_substitution_errors(errors, context, diagnostic_sink);
      return piece;
    } catch (const ErrorWithLocation &) {
      record_error(errors, context, diagnostic_sink);
    }

    let did_recovery_fail = false;
    try {
      recover_to_next_statement();
    } catch (const ErrorWithLocation &) {
      did_recovery_fail = true;
    }
    record_substitution_errors(errors, context, diagnostic_sink);
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
  SourceLocation and_or_start{};
  usize and_or_first_index = 0;

  bool should_parse_command = true;
  bool should_negate_pending = false;
  bool should_time_pending = false;
  time_format_mode time_format = time_format_mode::Default;
  time_rss_mode time_rss = time_rss_mode::Omit;
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
      pending->set_timed(time_location, time_format, time_rss);
      should_time_pending = false;
      time_format = time_format_mode::Default;
      time_rss = time_rss_mode::Omit;
    }
    compound_list->append_node(m_lexer.arena().create<CompoundListCondition>(
        at->source_location(), next_cond, pending));
  };

  loop
  {
    if (should_parse_command) {
      Token *maybe_time = nullptr;
      if (m_analysis_metadata_collection_mode ==
          analysis_metadata_collection_mode::Enabled)
      {
        let const should_collect_directives =
            next_cond == CompoundListCondition::Kind::None;
        m_lexer.set_shellcheck_directive_collection_mode(
            should_collect_directives
                ? shellcheck_directive_collection_mode::Enabled
                : shellcheck_directive_collection_mode::Disabled);
        maybe_time = m_lexer.peek_shell_token();
        let directives = m_lexer.take_shellcheck_directives();
        m_lexer.set_shellcheck_directive_collection_mode(
            shellcheck_directive_collection_mode::Disabled);
        while (!directives.is_empty() &&
               maybe_time->kind() == Token::Kind::Newline)
        {
          m_lexer.advance_past_last_peek();
          m_lexer.set_shellcheck_directive_collection_mode(
              shellcheck_directive_collection_mode::Enabled);
          maybe_time = m_lexer.peek_shell_token();
          let following_directives = m_lexer.take_shellcheck_directives();
          m_lexer.set_shellcheck_directive_collection_mode(
              shellcheck_directive_collection_mode::Disabled);
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
      if (next_cond == CompoundListCondition::Kind::None) {
        and_or_start = maybe_time->source_location();
        and_or_first_index = compound_list->node_count();
      }
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
              time_format = time_format_mode::Posix;
            } else if (is_unquoted_word(maybe_option, "-R")) {
              time_rss = time_rss_mode::Include;
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
      if (lhs != nullptr) {
        if (next_cond != CompoundListCondition::Kind::None) {
          do_finish_pending(lhs, token);

          let const source = m_lexer.source();
          let end_position = token->source_location().position;
          while (end_position > and_or_start.position &&
                 (source[end_position - 1] == ' ' ||
                  source[end_position - 1] == '\t'))
          {
            end_position--;
          }

          CompoundList *and_or_list = m_lexer.arena().create<CompoundList>();
          compound_list->move_nodes_from(and_or_first_index, *and_or_list);
          BraceGroup *group =
              m_lexer.arena().create<BraceGroup>(and_or_start, and_or_list);
          group->set_source_end_position(end_position);
          next_cond = CompoundListCondition::Kind::None;
          lhs = group;
        }
        lhs->make_async();
      }
      fallthru;
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
      fallthru;
    case Token::Kind::Newline:
    case Token::Kind::EndOfFile:
    case Token::Kind::Semicolon: {
      let const is_stray_semicolon = token->kind() == Token::Kind::Semicolon &&
                                     lhs == nullptr && !should_time_pending;
      if (is_stray_semicolon) {
        throw koshka::ErrorWithLocation{token->source_location(),
                                        "Expected a command before ';'"};
      }

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
          Token *after = m_lexer.peek_shell_token();
          if (m_lexer.is_posix_mode() &&
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

fn Parser::build_file_or_dup_redirection(
    i32 fd, Token::Kind op_kind, const SourceLocation &op_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out,
    const Token *fd_allocation_name_token,
    redirection_descriptor_spelling descriptor_spelling) throws -> void
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
      Token *from = next_token_of_kind(Token::Kind::Word,
                                       "Expected a descriptor after '&'");
      let const &from_word = static_cast<tokens::WordToken *>(from)->word();

      redir.kind = (op_kind == Token::Kind::Less)
                       ? expressions::Redirection::Kind::DuplicateInput
                       : expressions::Redirection::Kind::DuplicateOutput;

      let const literal = from_word.to_literal_string();

      if (literal == "-") {
        redir.dup_fd = expressions::Redirection::DUP_FD_CLOSE;
        out.push(redir);
        return;
      }

      let const is_move =
          literal.count() > 1 && literal.view()[literal.count() - 1] == '-';
      let const descriptor_text =
          is_move ? literal.view().substring_of_length(0, literal.count() - 1)
                  : literal.view();
      if (descriptor_text.is_all_decimal_digits()) {
        let const parsed_descriptor = descriptor_text.to<i64>();
        if (parsed_descriptor.is_error()) {
          throw ErrorWithLocation{from->source_location(),
                                  parsed_descriptor.error().message()};
        }

        if (parsed_descriptor.value() <= INT32_MAX) {
          redir.dup_fd = static_cast<i32>(parsed_descriptor.value());
          out.push(redir);
          if (is_move) {
            redir.fd = redir.dup_fd;
            redir.dup_fd = expressions::Redirection::DUP_FD_CLOSE;
            redir.fd_allocation_name_token = nullptr;
            out.push(redir);
          }

          return;
        }
      }

      redir.target = from;
      redir.is_dup_filename_allowed =
          op_kind == Token::Kind::Greater &&
          descriptor_spelling == redirection_descriptor_spelling::Implicit &&
          !m_lexer.is_posix_mode();
      out.push(redir);
      return;
    }
  }

  {
    Token *after = m_lexer.peek_shell_token();
    ASSERT(after != nullptr);
    let const is_adjacent = after->source_location().position ==
                            op_location.position + op_location.length;

    if (op_kind == Token::Kind::Greater && after->kind() == Token::Kind::Pipe &&
        is_adjacent)
    {
      m_lexer.advance_past_last_peek();
      redir.kind = expressions::Redirection::Kind::TruncateOutputOverride;
      redir.target = next_token_of_kind(Token::Kind::Word,
                                        "Expected a filename after '>|'");
      out.push(redir);
      return;
    }

    if (op_kind == Token::Kind::Less && after->kind() == Token::Kind::Greater &&
        is_adjacent)
    {
      m_lexer.advance_past_last_peek();
      redir.kind = expressions::Redirection::Kind::ReadWrite;
      redir.target = next_token_of_kind(Token::Kind::Word,
                                        "Expected a filename after '<>'");
      out.push(redir);
      return;
    }
  }

  Token *target = next_token_of_kind(Token::Kind::Word,
                                     "Expected a filename after the redir");
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
    const SourceLocation &op_location, Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out,
    assignment_update_mode update_mode) throws -> void
{
  build_file_or_dup_redirection(1,
                                update_mode == assignment_update_mode::Append
                                    ? Token::Kind::DoubleGreater
                                    : Token::Kind::Greater,
                                op_location, first_location, out, nullptr,
                                redirection_descriptor_spelling::Explicit);
  out.back().is_both_streams_spelling = true;
  out.push(stderr_to_stdout_dup());
}

fn Parser::build_here_string_redirection(
    i32 fd, const SourceLocation &op_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> void
{
  if (!first_location) first_location = op_location;

  expressions::Redirection redir{};
  redir.fd = fd;
  redir.kind = expressions::Redirection::Kind::HereString;
  redir.target =
      next_token_of_kind(Token::Kind::Word, "Expected a word after '<<<'");
  redir.dup_fd = -1;
  redir.heredoc = nullptr;
  redir.should_expand_heredoc = false;
  out.push(redir);
}

mustuse fn Parser::wrap_with_stderr_to_stdout(Command *command) throws
    -> Command *
{
  ASSERT(command != nullptr);
  let const arena_allocator = bump_allocator(m_lexer.arena());

  if (command->is_simple_command()) {
    static_cast<SimpleCommand *>(command)->append_redirection(
        stderr_to_stdout_dup(), arena_allocator);
    return command;
  }

  let redirections = ArrayList<expressions::Redirection>{arena_allocator};
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
  let is_separated_strip_operator = false;
  if (delimiter_token->kind() == Token::Kind::Word &&
      delimiter_token->source_location().position ==
          op_location.position + op_location.length)
  {
    let const &word =
        static_cast<tokens::WordToken *>(delimiter_token)->word();
    if (word.segments.count() == 1 &&
        word.segments[0].kind == WordSegment::Kind::UnquotedText &&
        word.segments[0].text.view() == "-")
    {
      is_separated_strip_operator = true;
      delimiter_token = m_lexer.next_shell_token();
      ASSERT(delimiter_token != nullptr);
    }
  }
  if (delimiter_token->kind() != Token::Kind::Word) {
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
  heredoc_tab_policy tab_policy = is_separated_strip_operator
                                      ? heredoc_tab_policy::Strip
                                      : heredoc_tab_policy::Preserve;
  let const has_unquoted_leading_dash =
      !is_separated_strip_operator &&
      !delimiter_word.segments.is_empty() &&
      delimiter_word.segments[0].kind == WordSegment::Kind::UnquotedText &&
      !delimiter_word.segments[0].text.is_empty() &&
      delimiter_word.segments[0].text.view()[0] == '-';
  if (has_unquoted_leading_dash && !delimiter.is_empty() &&
      delimiter[0] == '-' &&
      delimiter_token->source_location().position ==
          op_location.position + op_location.length)
  {
    tab_policy = heredoc_tab_policy::Strip;
    delimiter = delimiter.substring(1);
  }

  LOG(Debug, "registering a heredoc redirection with delimiter '%.*s'",
      static_cast<int>(delimiter.length), delimiter.data);

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
  redir.heredoc_delimiter = delimiter_token;
  redir.dup_fd = -1;
  redir.heredoc =
      m_lexer.register_heredoc(delimiter, tab_policy, should_expand);
  redir.should_expand_heredoc = should_expand;
  redir.should_strip_heredoc_tabs = tab_policy == heredoc_tab_policy::Strip;
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
  let const is_descriptor_in_range = [&]() throws -> bool {
    if (word_token->word().fd_allocation_name().has_value()) return true;

    let const parsed = word_token->word().to_literal_string().to<i64>();
    return !parsed.is_error() && parsed.value() <= INT32_MAX;
  };
  if ((nk == Token::Kind::Greater || nk == Token::Kind::DoubleGreater ||
       nk == Token::Kind::Less || nk == Token::Kind::DoubleLess ||
       nk == Token::Kind::TripleLess) &&
      next->source_location().position ==
          word_location.position + word_location.length &&
      is_descriptor_in_range())
  {
    let const op_location = next->source_location();
    m_lexer.advance_past_last_peek();

    let const allocation_name = word_token->word().fd_allocation_name();
    if (allocation_name.has_value()) {
      if (nk == Token::Kind::DoubleLess) {
        build_heredoc_redirection(-1, op_location, first_location, out);
        out.back().fd_allocation_name_token = word_token;
      } else if (nk == Token::Kind::TripleLess) {
        build_here_string_redirection(-1, op_location, first_location, out);
        out.back().fd_allocation_name_token = word_token;
      } else {
        build_file_or_dup_redirection(
            -1, nk, op_location, first_location, out, word_token,
            redirection_descriptor_spelling::Explicit);
      }
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
    } else if (nk == Token::Kind::TripleLess) {
      build_here_string_redirection(fd, op_location, first_location, out);
    } else {
      build_file_or_dup_redirection(fd, nk, op_location, first_location, out,
                                    nullptr,
                                    redirection_descriptor_spelling::Explicit);
    }
    return true;
  }
  return false;
}

alwaysinline fn Parser::try_build_operator_redirection(
    const Token *token, Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> bool
{
  let const op_kind = token->kind();
  let const op_location = token->source_location();

  switch (op_kind) {
  case Token::Kind::Greater:
  case Token::Kind::DoubleGreater:
  case Token::Kind::Less:
    m_lexer.advance_past_last_peek();
    build_file_or_dup_redirection(op_kind == Token::Kind::Less ? 0 : 1, op_kind,
                                  op_location, first_location, out, nullptr,
                                  redirection_descriptor_spelling::Implicit);
    return true;

  case Token::Kind::AmpersandGreater:
  case Token::Kind::AmpersandDoubleGreater:
    m_lexer.advance_past_last_peek();
    build_both_streams_redirection(op_location, first_location, out,
                                   op_kind ==
                                           Token::Kind::AmpersandDoubleGreater
                                       ? assignment_update_mode::Append
                                       : assignment_update_mode::Replace);
    return true;

  case Token::Kind::DoubleLess:
    m_lexer.advance_past_last_peek();
    build_heredoc_redirection(0, op_location, first_location, out);
    return true;

  case Token::Kind::TripleLess:
    m_lexer.advance_past_last_peek();
    build_here_string_redirection(0, op_location, first_location, out);
    return true;

  default: return false;
  }
}

mustuse fn Parser::try_parse_trailing_redirection(
    ArrayList<expressions::Redirection> &out) throws -> bool
{
  Maybe<SourceLocation> ignored_first_location;

  Token *token = m_lexer.peek_shell_token();
  ASSERT(token != nullptr);

  if (try_build_operator_redirection(token, ignored_first_location, out)) {
    return true;
  }
  if (token->kind() != Token::Kind::Word) return false;

  const tokens::WordToken *word_token = static_cast<tokens::WordToken *>(token);
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

mustuse fn Parser::attach_trailing_redirections(Command *compound) throws
    -> Command *
{
  ASSERT(compound != nullptr);

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
  Coproc,
};

hot fn Parser::parse_simple_command(const Token *leading_token) throws
    -> Command *
{
  Maybe<SourceLocation> source_location;
  let const arena_allocator = bump_allocator(m_lexer.arena());
  ArrayList<const Token *> args_accumulator{arena_allocator};
  let local_vars = ArrayList<PrefixAssignment>{arena_allocator};
  let array_args = ArrayList<array_builtin_assignment>{arena_allocator};
  let redirections = ArrayList<expressions::Redirection>{arena_allocator};
  let full_end_position = usize{0};

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
    c->set_full_source_end_position(full_end_position);
    return c;
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
          case 'c':
            if (view.length == 6 && view == "coproc")
              position_word = command_position_word::Coproc;
            break;
          default: break;
          }
        }
      }

      switch (position_word) {
      case command_position_word::BraceOpen:
        return attach_trailing_redirections(parse_brace_group());
      case command_position_word::BraceClose: return nullptr;
      case command_position_word::Conditional:
        if (m_lexer.is_posix_mode()) {
          throw ErrorWithLocation{token->source_location(),
                                  "The [[ conditional is a bash extension that "
                                  "the sh mood does not "
                                  "provide"};
        }
        return attach_trailing_redirections(parse_conditional_command());
      case command_position_word::Select:
        if (m_lexer.is_bash_compatible())
          return attach_trailing_redirections(parse_select());
        break;
      case command_position_word::Coproc:
        if (m_lexer.bash_additions_enabled())
          return attach_trailing_redirections(parse_coproc());
        break;
      case command_position_word::None: break;
      }

      switch (token->kind()) {
      case Token::Kind::If: return attach_trailing_redirections(parse_if());
      case Token::Kind::While:
        return attach_trailing_redirections(
            parse_while_or_until(loop_kind::While));
      case Token::Kind::Until:
        return attach_trailing_redirections(
            parse_while_or_until(loop_kind::Until));
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

      let const operator_length =
          a->get_update_mode() == assignment_update_mode::Append ? 2 : 1;
      let const has_empty_value =
          a->source_location().length == a->key().count() + operator_length;
      let const is_array_assignment =
          next->kind() == Token::Kind::LeftParen && has_empty_value &&
          next->source_location().position ==
              a->source_location().position + a->source_location().length;

      if (!args_accumulator.is_empty()) {
        if (is_array_assignment) {
          let const command_name = args_accumulator[0]->raw_string();
          if (classify_assignment_builtin(command_name.view()) !=
              assignment_builtin::None)
          {
            ArrayList<const Token *> elements = consume_bash_array_assignment();
            let const assignment_end_position =
                static_cast<u32>(m_lexer.cursor_position());
            array_args.push(array_builtin_assignment{
                a->key().clone(), steal(elements), a->source_location(),
                assignment_end_position, a->get_update_mode()});
            break;
          }
        }
        args_accumulator.push(token);
        break;
      }

      if (is_array_assignment) {
        ArrayList<const Token *> elements = consume_bash_array_assignment();
        let const assignment_end_position =
            static_cast<u32>(m_lexer.cursor_position());
        array_args.push(array_builtin_assignment{
            a->key().clone(), steal(elements), a->source_location(),
            assignment_end_position, a->get_update_mode()});
        break;
      }

      let const is_lone_assignment =
          local_vars.count() == 0 && redirections.is_empty() &&
          array_args.is_empty() &&
          (is_compound_list_separator(next->kind()) ||
           next->kind() == Token::Kind::EndOfFile ||
           is_compound_terminator(next->kind()));
      if (is_lone_assignment) {
        return m_lexer.arena().create<AssignCommand>(*source_location, a);
      } else {
        local_vars.push(PrefixAssignment{a});
      }
    } break;

    case Token::Kind::Greater:
    case Token::Kind::DoubleGreater:
    case Token::Kind::Less:
    case Token::Kind::AmpersandGreater:
    case Token::Kind::AmpersandDoubleGreater:
    case Token::Kind::DoubleLess:
    case Token::Kind::TripleLess:
      try_build_operator_redirection(token, source_location, redirections);
      break;

    default: return do_build_command();
    }

    full_end_position = m_lexer.cursor_position();
  }

  unreachable("the simple-command parser loop terminated without returning");
}

fn Parser::finish_function_body(const SourceLocation &location,
                                StringView name) throws -> Command *
{
  skip_newlines_after_pipe();

  record_analysis_scope_definition(name,
                                   analysis_scope_definition_kind::Function);
  let const scope_mark = open_analysis_scope();

  let const rest_of_source = m_lexer.source().substring(location.position);
  let const line_length = rest_of_source.find_character('\n');
  let body_storage = FunctionBodyHandle::create(
      line_length.has_value() ? line_length.value() : rest_of_source.length);
  let &previous_arena = m_lexer.arena();
  let const previous_arena_kind = m_lexer.arena_kind();
  Command *body = nullptr;
  {
    let const pending_heredoc_count = m_lexer.get_pending_heredoc_count();
    m_lexer.set_arena(*body_storage.get_arena(),
                      ParseSession::AllocationKind::FunctionBody);
    defer { m_lexer.set_arena(previous_arena, previous_arena_kind); };
    try {
      body = parse_simple_command();
    } catch (...) {
      m_lexer.drop_pending_heredocs_after(pending_heredoc_count);
      throw;
    }
  }

  if (body == nullptr) {
    throw ErrorWithLocation{location,
                            "Expected a compound command as the function body"};
  }
  body_storage.set_body(body);

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
  unused(next_token_of_kind(Token::Kind::RightParen,
                            "Expected ')' in a function definition"));

  return finish_function_body(location, name.view());
}

fn Parser::parse_keyword_function_definition() throws -> Command *
{
  Token *name_token = next_token_of_kind(
      Token::Kind::Word, "Expected a name after the 'function' keyword");
  let const location = name_token->source_location();
  let const name = name_token->raw_string();

  LOG(Debug, "parsing a keyword function definition for '%s'", name.c_str());

  Token *after_name = m_lexer.peek_shell_token();
  ASSERT(after_name != nullptr);
  if (after_name->kind() == Token::Kind::LeftParen) {
    m_lexer.advance_past_last_peek();
    unused(next_token_of_kind(Token::Kind::RightParen,
                              "Expected ')' in a function definition"));
  }

  return finish_function_body(location, name.view());
}

fn Parser::consume_bash_array_assignment() throws -> ArrayList<const Token *>
{
  Token *open = m_lexer.next_shell_token();
  ASSERT(open != nullptr);
  ASSERT(open->kind() == Token::Kind::LeftParen);

  ArrayList<const Token *> elements{heap_allocator()};
  m_lexer.set_is_lexing_array_literal(true);
  try {
    consume_bash_array_elements(open, elements);
  } catch (...) {
    m_lexer.set_is_lexing_array_literal(false);
    throw;
  }
  m_lexer.set_is_lexing_array_literal(false);

  return elements;
}

fn Parser::consume_bash_array_elements(const Token *open,
                                       ArrayList<const Token *> &elements)
    throws -> void
{
  loop
  {
    Token *t = m_lexer.next_shell_token();
    ASSERT(t != nullptr);
    switch (t->kind()) {
    case Token::Kind::EndOfFile:
      throw ErrorWithLocation{open->source_location(),
                              "Unterminated array assignment, expected ')'"};
    case Token::Kind::RightParen: return;
    case Token::Kind::Newline: break;
    case Token::Kind::LeftParen:
    case Token::Kind::Semicolon:
    case Token::Kind::DoubleSemicolon:
    case Token::Kind::SemicolonAmpersand:
    case Token::Kind::DoubleSemicolonAmpersand:
    case Token::Kind::Ampersand:
    case Token::Kind::DoubleAmpersand:
    case Token::Kind::Pipe:
    case Token::Kind::DoublePipe:
    case Token::Kind::PipeAmpersand:
    case Token::Kind::Greater:
    case Token::Kind::DoubleGreater:
    case Token::Kind::Less:
    case Token::Kind::DoubleLess:
    case Token::Kind::TripleLess:
    case Token::Kind::AmpersandGreater:
    case Token::Kind::AmpersandDoubleGreater: {
      let const ast = t->to_ast_string();
      let const message = "Unexpected '" + ast.view() +
                          "' in an array assignment, expected a word";
      let const is_arithmetic_shape =
          elements.is_empty() && t->kind() == Token::Kind::LeftParen &&
          t->source_location().position == open->source_location().position + 1;
      if (is_arithmetic_shape) {
        throw ErrorWithLocationAndDetails{
            t->source_location(), message.view(),
            "Write `(( name = expression ))` to assign arithmetic"};
      }
      throw ErrorWithLocation{t->source_location(), message.view()};
    }
    default: elements.push(t); break;
    }
  }
}

} /* namespace koshka */
