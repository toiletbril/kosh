#include "Parser.hpp"

#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Optimizer.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

using namespace tokens;
using namespace expressions;

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
  if (!m_should_collect_analysis_scopes || args.is_empty()) return;

  let const command_view = args[0]->raw_view();
  if (!command_view.has_value() || *command_view != "alias") return;

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

hot pure static fn kind_in(Token::Kind kind,
                           std::initializer_list<Token::Kind> set) wontthrow
    -> bool
{
  for (let k : set) {
    if (k == kind) return true;
  }
  return false;
}

/* A brace is a reserved word only when a token is exactly '{' or '}' as a
   single unquoted segment, so a quoted or escaped brace is rejected. */
/* [[ and ]] arrive from the lexer as ordinary single unquoted words. */
hot pure static fn is_unquoted_word(const Token *token,
                                    StringView text) wontthrow -> bool
{
  if (token == nullptr || token->kind() != Token::Kind::Word) {
    return false;
  }
  const Word &word = static_cast<const tokens::WordToken *>(token)->word();
  if (word.segments.count() != 1 ||
      word.segments[0].kind != WordSegment::Kind::UnquotedText)
  {
    return false;
  }
  return word.segments[0].text == text;
}

/* RightBracket in the terminator set stands for a standalone '}' word, the
   close of a brace group. */
hot pure static fn
is_list_terminator(const Token *token,
                   std::initializer_list<Token::Kind> terminators) wontthrow
    -> bool
{
  ASSERT(token != nullptr);
  return kind_in(token->kind(), terminators) ||
         (kind_in(Token::Kind::RightBracket, terminators) &&
          is_unquoted_word(token, "}"));
}

/* The byte location of the keyword as a whole word in the source, so a missing
   terminator can point the caret straight at the keyword read as an argument.
 */
cold pure static fn find_standalone_keyword(StringView source,
                                            StringView keyword) wontthrow
    -> Maybe<SourceLocation>
{
  let do_is_boundary = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0 || c == ';' ||
           c == '&' || c == '|';
  };

  if (keyword.length == 0 || keyword.length > source.length) {
    return koshka::None;
  }

  for (usize pos = 0; pos + keyword.length <= source.length; pos++) {
    if (source.substring_of_length(pos, keyword.length) != keyword) continue;
    const let end_position = pos + keyword.length;
    const let left_ok = pos == 0 || do_is_boundary(source[pos - 1]);
    const let right_ok =
        end_position == source.length || do_is_boundary(source[end_position]);
    if (left_ok && right_ok) {
      return SourceLocation{pos, keyword.length};
    }
  }
  return koshka::None;
}

cold [[noreturn]] static fn
throw_unterminated(SourceLocation opener, StringView what, StringView source,
                   StringView keyword, SourceLocation fallback) throws -> void
{
  if (Maybe<SourceLocation> found = find_standalone_keyword(source, keyword);
      found.has_value())
  {
    found->filename = opener.filename;
    throw ErrorWithLocationAndDetails{
        opener, what, *found,
        "This '" + keyword +
            "' was read as an argument, put a ';' or a newline before it"};
  }
  fallback.filename = opener.filename;
  throw ErrorWithLocationAndDetails{opener, what, fallback,
                                    "Expected '" + keyword + "'"};
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
    const let ast = token->to_ast_string();
    return "'" + ast.view() + "' has no matching 'if'";
  }
  case Token::Kind::Do:
  case Token::Kind::Done: {
    const let ast = token->to_ast_string();
    return "'" + ast.view() + "' has no matching 'while', 'until', or 'for'";
  }
  case Token::Kind::Esac: return "'esac' has no matching 'case'";
  case Token::Kind::DoubleSemicolon:
    return "';;' is only valid between the arms of a 'case'";
  case Token::Kind::RightParen: return "')' has no matching '('";
  case Token::Kind::RightBracket: return "'}' has no matching '{'";
  case Token::Kind::Pipe: return "'|' has no command before it to pipe from";
  default: {
    const let ast = token->to_ast_string();
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

flatten fn Parser::construct_ast() throws -> Expression *
{
  return parse_command_list({});
}

fn Parser::construct_next_top_level_ast() throws -> Expression *
{
  m_lexer.set_should_collect_shellcheck_directives(true);
  let const first_token = m_lexer.peek_shell_token();
  m_lexer.set_should_collect_shellcheck_directives(false);
  if (first_token->kind() == Token::Kind::EndOfFile) return nullptr;

  m_should_stop_after_top_level_unit = true;
  defer { m_should_stop_after_top_level_unit = false; };

  return parse_command_list({});
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

    const bool is_boundary = token->kind() == Token::Kind::Newline ||
                             token->kind() == Token::Kind::Semicolon;

    if (is_boundary && has_consumed_token) {
      m_lexer.advance_past_last_peek();
      return;
    }

    m_lexer.advance_past_last_peek();
    has_consumed_token = true;
  }
}

/* Parse every top-level command, recovering from a syntax error instead of
   aborting at the first. */
cold fn Parser::construct_ast(ArrayList<String> &errors,
                              EvalContext *context) throws -> Expression *
{
  Expression *first_piece = nullptr;
  let last_location = SourceLocation{};

  loop
  {
    m_lexer.set_should_collect_shellcheck_directives(true);
    Token *token = m_lexer.peek_shell_token();
    m_lexer.set_should_collect_shellcheck_directives(false);
    ASSERT(token != nullptr);
    last_location = token->source_location();
    if (token->kind() == Token::Kind::EndOfFile) break;

    try {
      Expression *piece = parse_command_list({});
      ASSERT(piece != nullptr);
      if (first_piece == nullptr) first_piece = piece;
    } catch (const ErrorWithLocationAndDetails &e) {
      /* Render both parts here, since the detail note would be sliced off a
         base-class copy. */
      LOG(Debug, "recording a detailed parse error and recovering: %s",
          e.message().c_str());
      errors.push(e.to_string(m_lexer.source(), context));
      errors.push(e.details_to_string(m_lexer.source(), context));
      recover_to_next_statement();
    } catch (const ErrorWithLocation &e) {
      LOG(Debug, "recording a parse error and recovering: %s",
          e.message().c_str());
      errors.push(e.to_string(m_lexer.source(), context));
      recover_to_next_statement();
    }
  }

  if (first_piece == nullptr)
    return m_lexer.arena().create<DummyExpression>(last_location);

  return first_piece;
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

hot fn Parser::parse_command_list(
    std::initializer_list<Token::Kind> terminators) throws -> Expression *
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
  Maybe<usize> active_shellcheck_suppression{};

  let do_finish_shellcheck_suppression = [&](usize end_position) {
    if (!active_shellcheck_suppression.has_value()) return;
    m_shellcheck_suppressions[*active_shellcheck_suppression].end_position =
        end_position;
    active_shellcheck_suppression = None;
  };

  let do_finish_pending = [&](Command *pending, const Token *at) throws {
    if (should_negate_pending) {
      pending->set_negated();
      should_negate_pending = false;
    }
    if (should_time_pending) {
      pending->set_timed(is_time_posix_format);
      should_time_pending = false;
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
      let const should_collect_directives =
          next_cond == CompoundListCondition::Kind::None;
      m_lexer.set_should_collect_shellcheck_directives(
          should_collect_directives);
      Token *maybe_time = m_lexer.peek_shell_token();
      let directives = m_lexer.take_shellcheck_directives();
      m_lexer.set_should_collect_shellcheck_directives(false);
      ASSERT(maybe_time != nullptr);
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
          active_shellcheck_suppression = m_shellcheck_suppressions.count() - 1;
      }
      if (maybe_time->kind() == Token::Kind::Time) {
        m_lexer.advance_past_last_peek();
        should_time_pending = true;
        Token *maybe_posix = m_lexer.peek_shell_token();
        if (is_unquoted_word(maybe_posix, "-p") ||
            is_unquoted_word(maybe_posix, "--posix"))
        {
          m_lexer.advance_past_last_peek();
          is_time_posix_format = true;
        }
      }
      Token *maybe_negation = m_lexer.peek_shell_token();
      ASSERT(maybe_negation != nullptr);
      if (is_unquoted_word(maybe_negation, "!")) {
        m_lexer.advance_past_last_peek();
        should_negate_pending = true;
      }
      lhs = parse_simple_command();
    } else {
      should_parse_command = true;
    }

    Token *token = m_lexer.peek_shell_token();
    ASSERT(token != nullptr);

    /* A terminator keyword is left for the caller to consume. */
    if (is_list_terminator(token, terminators)) {
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
        const let ast = token->to_ast_string();
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
      let const does_left_pipe_stderr =
          token->kind() == Token::Kind::PipeAmpersand;
      m_lexer.advance_past_last_peek();
      skip_newlines_after_pipe();

      Pipeline *pipeline =
          m_lexer.arena().create<Pipeline>(token->source_location());
      pipeline->append_command(
          does_left_pipe_stderr ? wrap_with_stderr_to_stdout(lhs) : lhs);

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
        const bool does_this_pipe_stderr =
            last_pipe_token->kind() == Token::Kind::PipeAmpersand;
        pipeline->append_command(has_another_pipe && does_this_pipe_stderr
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

  unreachable();
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
    i32 fd, Token::Kind op_kind, SourceLocation op_location,
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
        const let parsed_descriptor = literal.to<i64>();
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
      redir.can_dup_be_filename = op_kind == Token::Kind::Greater &&
                                  !fd_was_explicit && !m_lexer.is_posix_mode();
      out.push(redir);
      return;
    }
  }

  {
    Token *after = m_lexer.peek_shell_token();
    ASSERT(after != nullptr);
    /* The second character must touch the operator, so a real pipe in cmd >file
       | next stays separate from >| and <>. */
    const bool is_adjacent = after->source_location().position ==
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
  default: redir.kind = expressions::Redirection::Kind::ReadInput; break;
  }
  redir.target = target;
  out.push(redir);
}

fn Parser::build_both_streams_redirection(
    bool is_append, SourceLocation op_location,
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
    SourceLocation op_location, Maybe<SourceLocation> &first_location,
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
    i32 fd, SourceLocation op_location, Maybe<SourceLocation> &first_location,
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

  const let delimiter_literal = delimiter_word.to_literal_string();
  let delimiter = delimiter_literal.view();
  bool should_strip_tabs = false;
  /* <<- strips leading tabs. The dash counts only when unquoted, so <<'-EOF'
     keeps the dash in the delimiter and terminates on -EOF. */
  const let has_unquoted_leading_dash =
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
    const tokens::WordToken *word_token, SourceLocation word_location,
    Maybe<SourceLocation> &first_location,
    ArrayList<expressions::Redirection> &out) throws -> bool
{
  m_lexer.advance_past_last_peek();
  Token *next = m_lexer.peek_shell_token();
  ASSERT(next != nullptr);
  const let nk = next->kind();
  if ((nk == Token::Kind::Greater || nk == Token::Kind::DoubleGreater ||
       nk == Token::Kind::Less || nk == Token::Kind::DoubleLess) &&
      next->source_location().position ==
          word_location.position + word_location.length)
  {
    const let op_location = next->source_location();
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

    const let literal = word_token->word().to_literal_string();
    const let parsed_descriptor = literal.to<i64>();
    if (parsed_descriptor.is_error()) {
      throw ErrorWithLocation{word_location,
                              parsed_descriptor.error().message()};
    }
    const let fd = static_cast<i32>(parsed_descriptor.value());
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
    const let op_kind = token->kind();
    const let op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_file_or_dup_redirection((op_kind == Token::Kind::Less) ? 0 : 1,
                                  op_kind, op_location, ignored_first_location,
                                  out, /*fd_was_explicit=*/false);
    return true;
  }

  case Token::Kind::AmpersandGreater:
  case Token::Kind::AmpersandDoubleGreater: {
    const let op_kind = token->kind();
    const let op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_both_streams_redirection(op_kind ==
                                       Token::Kind::AmpersandDoubleGreater,
                                   op_location, ignored_first_location, out);
    return true;
  }

  case Token::Kind::DoubleLess: {
    const let op_location = token->source_location();
    m_lexer.advance_past_last_peek();
    build_heredoc_redirection(0, op_location, ignored_first_location, out);
    return true;
  }

  case Token::Kind::TripleLess: {
    const let op_location = token->source_location();
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

    const let word_location = token->source_location();
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

  let redirections = ArrayList<expressions::Redirection>{heap_allocator()};
  while (try_parse_trailing_redirection(redirections)) {}

  if (redirections.is_empty()) return compound;

  return m_lexer.arena().create<RedirectedCommand>(
      compound->source_location(), compound, steal(redirections));
}

/* The bash assignment builtins that parse a NAME=(...) argument as an array
   assignment. */
static pure fn is_assignment_builtin_name(StringView name) wontthrow -> bool
{
  static constexpr PackedStringKey KEYS[] = {
      SSK("local"),    SSK("declare"), SSK("typeset"),
      SSK("readonly"), SSK("export"),
  };
  static constexpr StaticStringSet ASSIGNMENT_BUILTINS{KEYS};
  return ASSIGNMENT_BUILTINS.contains(name);
}

/* Returns a command, a compound command, or nullptr when a list terminator is
   next. A reserved word or a group opener in command position starts a compound
   command. */
hot fn Parser::parse_simple_command() throws -> Command *
{
  Maybe<SourceLocation> source_location;
  ArrayList<const Token *> args_accumulator{heap_allocator()};
  let local_vars = ArrayList<prefix_assignment>{heap_allocator()};
  let array_args = ArrayList<array_builtin_assignment>{heap_allocator()};
  let redirections = ArrayList<expressions::Redirection>{heap_allocator()};

  let do_build_command = [&]() -> Command * {
    if (!source_location) return nullptr;

    record_analysis_alias_definitions(args_accumulator);

    SimpleCommand *c = m_lexer.arena().create<SimpleCommand>(
        *source_location, steal(args_accumulator));
    if (local_vars.count() != 0) c->set_local_vars(steal(local_vars));
    if (!array_args.is_empty()) c->set_array_args(steal(array_args));
    if (!redirections.is_empty()) c->set_redirections(steal(redirections));
    return c;
  };

  let do_add_redirection = [&](i32 fd, Token::Kind op_kind,
                               SourceLocation op_location,
                               bool fd_was_explicit) {
    build_file_or_dup_redirection(fd, op_kind, op_location, source_location,
                                  redirections, fd_was_explicit);
  };

  loop
  {
    Token *token = m_lexer.peek_shell_token();
    ASSERT(token != nullptr);

    if (args_accumulator.is_empty() && local_vars.count() == 0 &&
        array_args.is_empty())
    {
      /* A standalone '{' opens a brace group, a standalone '}' closes one, both
         arriving as words. A '}' with no open group is left for the caller. */
      if (is_unquoted_word(token, "{")) {
        return attach_trailing_redirections(parse_brace_group());
      }
      if (is_unquoted_word(token, "}")) return nullptr;

      /* The sh mood is POSIX, where [[ is not a keyword, so the conditional is
         rejected there. */
      if (is_unquoted_word(token, "[[")) {
        if (m_lexer.is_posix_mode()) {
          throw ErrorWithLocation{token->source_location(),
                                  "The [[ conditional is a bash extension that "
                                  "the sh mood does not "
                                  "provide"};
        }
        return attach_trailing_redirections(parse_conditional_command());
      }

      /* select is not a reserved word in the lexer, so it is matched on the
         text in bash mode. */
      if (m_lexer.is_bash_compatible() && is_unquoted_word(token, "select")) {
        return attach_trailing_redirections(parse_select());
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
          const let word_location = token->source_location();
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
          if (is_assignment_builtin_name(command_name.view())) {
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
          (next->flags() & Token::Flag::CompoundList ||
           next->kind() == Token::Kind::EndOfFile ||
           is_compound_terminator(next->kind())))
      {
        return m_lexer.arena().create<AssignCommand>(*source_location, a);
      } else {
        /* Kept in source order so a later assignment sees an earlier one and a
           repeated name accumulates, which a map would lose. */
        local_vars.push(
            prefix_assignment{a->key().clone(), Word{a->value_word()},
                              a->source_location(), a->is_append()});
      }
    } break;

    case Token::Kind::Greater:
    case Token::Kind::DoubleGreater:
    case Token::Kind::Less: {
      const let op_kind = token->kind();
      const let op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      do_add_redirection((op_kind == Token::Kind::Less) ? 0 : 1, op_kind,
                         op_location, /*fd_was_explicit=*/false);
    } break;

    case Token::Kind::AmpersandGreater:
    case Token::Kind::AmpersandDoubleGreater: {
      const let op_kind = token->kind();
      const let op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      build_both_streams_redirection(
          op_kind == Token::Kind::AmpersandDoubleGreater, op_location,
          source_location, redirections);
    } break;

    case Token::Kind::DoubleLess: {
      const let op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      build_heredoc_redirection(0, op_location, source_location, redirections);
    } break;

    case Token::Kind::TripleLess: {
      const let op_location = token->source_location();
      m_lexer.advance_past_last_peek();
      build_here_string_redirection(op_location, source_location, redirections);
    } break;

    default: return do_build_command();
    }
  }

  unreachable();
}

hot fn Parser::parse_if() throws -> Command *
{
  Token *if_token = m_lexer.next_shell_token();
  ASSERT(if_token != nullptr);
  ASSERT(if_token->kind() == Token::Kind::If);
  const let location = if_token->source_location();

  LOG(Debug, "parsing an if clause at byte %zu", location.position);

  let branches = ArrayList<if_branch>{heap_allocator()};
  const Expression *otherwise = nullptr;

  loop
  {
    Expression *condition = parse_command_list({Token::Kind::Then});
    Token *then_token = m_lexer.next_shell_token();
    ASSERT(then_token != nullptr);
    if (then_token->kind() != Token::Kind::Then) {
      const let detail = condition->is_dummy()
                             ? "Expected a command for the condition"
                             : "Expected 'then' after the condition";
      throw ErrorWithLocationAndDetails{location, "Unterminated if",
                                        then_token->source_location(), detail};
    }

    Expression *body = parse_command_list(
        {Token::Kind::Elif, Token::Kind::Else, Token::Kind::Fi});
    branches.push(if_branch{condition, body});

    Token *after = m_lexer.next_shell_token();
    ASSERT(after != nullptr);
    if (after->kind() == Token::Kind::Elif) {
      continue;
    } else if (after->kind() == Token::Kind::Else) {
      otherwise = parse_command_list({Token::Kind::Fi});
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
  const let location = keyword->source_location();

  LOG(Debug, "parsing a %s loop at byte %zu", is_until ? "until" : "while",
      location.position);

  Expression *condition = parse_command_list({Token::Kind::Do});
  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    const let detail = condition->is_dummy()
                           ? "Expected a command for the loop condition"
                           : "Expected 'do'";
    throw ErrorWithLocationAndDetails{location, "Unterminated loop",
                                      do_token->source_location(), detail};
  }

  Expression *body = parse_command_list({Token::Kind::Done});
  reject_empty_loop_body(body);
  Token *done_token = m_lexer.next_shell_token();
  ASSERT(done_token != nullptr);
  if (done_token->kind() != Token::Kind::Done) {
    throw_unterminated(location, "Unterminated loop", m_lexer.source(), "done",
                       done_token->source_location());
  }

  let loop_node =
      m_lexer.arena().create<WhileLoop>(location, condition, body, is_until);
  const SourceLocation done_location = done_token->source_location();
  loop_node->set_source_end_position(done_location.position +
                                     done_location.length);
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
  if (peeked->kind() != Token::Kind::Word || peeked->raw_string() != "in") {
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
      const String raw = word->raw_string();
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
  const let location = keyword->source_location();

  LOG(Debug, "parsing a for loop at byte %zu", location.position);

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
    const String raw = name_token->raw_string();
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

  const let variable_name = name_token->raw_string();

  ArrayList<const Token *> words{heap_allocator()};
  const bool has_in_clause = parse_optional_in_clause_words(words);

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

  Expression *body = parse_command_list({Token::Kind::Done});
  reject_empty_loop_body(body);
  Token *done_token = m_lexer.next_shell_token();
  ASSERT(done_token != nullptr);
  if (done_token->kind() != Token::Kind::Done) {
    throw_unterminated(location, "Unterminated for loop", m_lexer.source(),
                       "done", done_token->source_location());
  }

  let loop_node = m_lexer.arena().create<ForLoop>(
      location, name_token->source_location(), variable_name.view(),
      steal(words), has_in_clause, body);
  const SourceLocation done_location = done_token->source_location();
  loop_node->set_source_end_position(done_location.position +
                                     done_location.length);
  return loop_node;
}

/* A bash select loop, select name in words; do BODY; done. It shares the for
   header shape, printing a numbered menu and reading a choice at run time. */
hot fn Parser::parse_select() throws -> Command *
{
  Token *keyword = m_lexer.next_shell_token();
  ASSERT(keyword != nullptr);
  ASSERT(is_unquoted_word(keyword, "select"));
  const let location = keyword->source_location();

  LOG(Debug, "parsing a select loop at byte %zu", location.position);

  Token *name_token = m_lexer.next_shell_token();
  ASSERT(name_token != nullptr);
  if (name_token->kind() != Token::Kind::Word) {
    const String raw = name_token->raw_string();
    if (KEYWORDS.find(raw.view()).has_value())
      name_token = word_token_from_raw(m_lexer.arena(), raw.view(),
                                       name_token->source_location());
  }
  if (name_token->kind() != Token::Kind::Word) {
    throw ErrorWithLocation{name_token->source_location(),
                            "Expected a variable name after 'select'"};
  }
  const let variable_name = name_token->raw_string();

  ArrayList<const Token *> words{heap_allocator()};
  const bool has_in_clause = parse_optional_in_clause_words(words);

  skip_semicolons_and_newlines();

  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    throw ErrorWithLocationAndDetails{location, "Unterminated select loop",
                                      do_token->source_location(),
                                      "expected 'do'"};
  }

  Expression *body = parse_command_list({Token::Kind::Done});
  reject_empty_loop_body(body);
  Token *done_token = m_lexer.next_shell_token();
  ASSERT(done_token != nullptr);
  if (done_token->kind() != Token::Kind::Done) {
    throw_unterminated(location, "Unterminated select loop", m_lexer.source(),
                       "done", done_token->source_location());
  }

  return m_lexer.arena().create<SelectLoop>(location, variable_name.view(),
                                            steal(words), has_in_clause, body);
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
  return arena.create<tokens::WordToken>(location, steal(word));
}

hot fn Parser::parse_case() throws -> Command *
{
  Token *keyword = m_lexer.next_shell_token();
  ASSERT(keyword != nullptr);
  const let location = keyword->source_location();

  LOG(Debug, "parsing a case clause at byte %zu", location.position);

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

  Token *in_token = m_lexer.next_shell_token();
  ASSERT(in_token != nullptr);
  if (!(in_token->kind() == Token::Kind::Word &&
        in_token->raw_string() == "in"))
  {
    throw ErrorWithLocation{in_token->source_location(),
                            "Expected 'in' after the case word"};
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

    Expression *body = parse_command_list(
        {Token::Kind::DoubleSemicolon, Token::Kind::SemicolonAmpersand,
         Token::Kind::DoubleSemicolonAmpersand, Token::Kind::Esac});

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

  LOG(Debug, "parsing a brace group at byte %zu",
      open->source_location().position);

  Expression *body = parse_command_list({Token::Kind::RightBracket});

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
  const SourceLocation close_location = close->source_location();
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

  LOG(Debug, "parsing a subshell at byte %zu",
      open->source_location().position);

  let const scope_mark = open_analysis_scope();
  Expression *body = parse_command_list({Token::Kind::RightParen});

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
  const SourceLocation close_location = close->source_location();
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

  const usize body_start_position = second->source_location().position + 1;
  usize body_end_position = body_start_position;
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
  LOG(Debug, "parsing an arithmetic command at byte %zu",
      open->source_location().position);

  const StringView body = capture_double_paren_body(open);
  /* The location spans the whole (( body )) so a runtime error underlines the
     entire expression. */
  const SourceLocation open_location = open->source_location();
  const SourceLocation full_location{open_location.position, body.length + 4,
                                     open_location.filename};
  return m_lexer.arena().create<expressions::ArithmeticCommand>(
      full_location, String{bump_allocator(m_lexer.arena()), body});
}

/* A bash C-style for, for (( init; cond; step )); do BODY; done. The header is
   split on its two top-level semicolons into three arithmetic clauses. */
hot fn Parser::parse_c_style_for(SourceLocation location, Token *open) throws
    -> Command *
{
  LOG(Debug, "parsing a c-style for header at byte %zu", location.position);

  const StringView header = capture_double_paren_body(open);

  /* The clause separators are the semicolons at paren depth zero, so a grouped
     subexpression in a clause is skipped. */
  usize separators[2] = {0, 0};
  usize separator_count = 0;
  usize depth = 0;
  for (usize i = 0; i < header.length; i++) {
    const char c = header[i];
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
  let const init =
      String{allocator, header.substring_of_length(0, separators[0])};
  let const condition = String{
      allocator, header.substring_of_length(separators[0] + 1,
                                            separators[1] - separators[0] - 1)};
  let const step = String{allocator, header.substring(separators[1] + 1)};

  skip_semicolons_and_newlines();

  Token *do_token = m_lexer.next_shell_token();
  ASSERT(do_token != nullptr);
  if (do_token->kind() != Token::Kind::Do) {
    throw ErrorWithLocationAndDetails{location, "Unterminated for loop",
                                      do_token->source_location(),
                                      "expected 'do'"};
  }

  Expression *body = parse_command_list({Token::Kind::Done});
  reject_empty_loop_body(body);
  Token *done_token = m_lexer.next_shell_token();
  ASSERT(done_token != nullptr);
  if (done_token->kind() != Token::Kind::Done) {
    throw_unterminated(location, "Unterminated for loop", m_lexer.source(),
                       "done", done_token->source_location());
  }

  return m_lexer.arena().create<expressions::CStyleForLoop>(
      location, steal(init), steal(condition), steal(step), body);
}

hot fn Parser::parse_conditional_command() throws -> Command *
{
  Token *open = m_lexer.next_shell_token();
  ASSERT(open != nullptr);
  ASSERT(is_unquoted_word(open, "[["));

  LOG(Debug, "parsing a conditional command at byte %zu",
      open->source_location().position);

  /* The tokens between [[ and ]] are collected raw rather than run through the
     command parser, so a < or > inside is a string comparison and not a
     redirection. The operand words are kept for the evaluator to expand without
     field splitting. */
  let elements = ArrayList<conditional_element>{heap_allocator()};
  usize close_end_position =
      open->source_location().position + open->source_location().length;
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
      elements.push({Kind::And, nullptr});
      break;
    case Token::Kind::DoublePipe: elements.push({Kind::Or, nullptr}); break;
    case Token::Kind::LeftParen:
      elements.push({Kind::OpenParen, nullptr});
      break;
    case Token::Kind::RightParen:
      elements.push({Kind::CloseParen, nullptr});
      break;
    case Token::Kind::Less: elements.push({Kind::Less, nullptr}); break;
    case Token::Kind::Greater: elements.push({Kind::Greater, nullptr}); break;
    case Token::Kind::Newline: continue;
    case Token::Kind::Word: {
      const String word_literal =
          static_cast<tokens::WordToken *>(t)->word().to_literal_string();
      if (word_literal == "!") {
        elements.push({Kind::Not, nullptr});
        break;
      }
      elements.push({Kind::Operand, t});

      if (word_literal == "=~") {
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
          regex_location.length = end_position - regex_location.position;
          elements.push(
              {Kind::Operand, m_lexer.arena().create<tokens::WordToken>(
                                  regex_location, steal(regex_word))});
        }
      }
      break;
    }
    default: elements.push({Kind::Operand, t}); break;
    }
  }

  let const node = m_lexer.arena().create<expressions::ConditionalCommand>(
      open->source_location(), steal(elements));
  node->set_source_end_position(close_end_position);
  return node;
}

fn Parser::finish_function_body(SourceLocation location, StringView name) throws
    -> Command *
{
  skip_newlines_after_pipe();

  record_analysis_scope_definition(name, false);
  let const scope_mark = open_analysis_scope();

  /* The body is parsed into the persistent function arena so it outlives the
     per-command arena reset. */
  BumpArena &per_command_arena = m_lexer.arena();
  if (FUNCTION_ARENA != nullptr) m_lexer.set_arena(*FUNCTION_ARENA);
  Command *body = parse_simple_command();
  m_lexer.set_arena(per_command_arena);

  if (body == nullptr) {
    throw ErrorWithLocation{location,
                            "Expected a compound command as the function body"};
  }

  /* The span ends where the body ends so declare -f can print the definition
     text from the source. */
  let definition =
      m_lexer.arena().create<FunctionDefinition>(location, name, body);
  definition->set_analysis_scope_definitions(close_analysis_scope(scope_mark));
  definition->set_source_end_position(body->source_end_position());
  return definition;
}

hot fn Parser::parse_function_definition(const Token *name_token) throws
    -> Command *
{
  ASSERT(name_token != nullptr);
  const let location = name_token->source_location();
  const let name = name_token->raw_string();

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
  const let location = name_token->source_location();
  const let name = name_token->raw_string();

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

hot fn Parser::parse_expression(u8 min_precedence) throws -> Expression *
{
  m_recursion_depth++;
  defer { m_recursion_depth--; };

  Token *t = m_lexer.next_expression_token();
  ASSERT(t != nullptr);

  if (m_recursion_depth > MAX_RECURSION_DEPTH) {
    throw ErrorWithLocation{
        t->source_location(),
        "Expression nesting level exceeded maximum of " +
            String::from(static_cast<i64>(MAX_RECURSION_DEPTH),
                         heap_allocator())};
  }

  Expression *lhs = nullptr;

  switch (t->kind()) {
  case Token::Kind::Number: {
    const let parsed_number = t->raw_string().to<i64>();
    if (parsed_number.is_error())
      throw ErrorWithLocation{t->source_location(),
                              parsed_number.error().message()};
    lhs = m_lexer.arena().create<ConstantNumber>(t->source_location(),
                                                 parsed_number.value());
  } break;

  case Token::Kind::If: {
    m_if_condition_depth++;

    Expression *condition = parse_expression();

    Token *after = m_lexer.next_expression_token();
    ASSERT(after != nullptr);
    if (after->kind() == Token::Kind::Semicolon) {
      after = m_lexer.next_expression_token();
    }
    if (after->kind() != Token::Kind::Then) {
      const let ast = after->to_ast_string();
      throw ErrorWithLocation{after->source_location(),
                              "Expected 'Then' after the condition, found '" +
                                  ast.view() + "'"};
    }

    Expression *then = parse_expression();

    Expression *otherwise = nullptr;
    after = m_lexer.next_expression_token();

    if (after->kind() == Token::Kind::Else) {
      after = m_lexer.peek_expression_token();
      if (after->kind() == Token::Kind::Then) m_lexer.advance_past_last_peek();

      otherwise = parse_expression();

      after = m_lexer.next_expression_token();
    }

    if (after->kind() != Token::Kind::Fi) {
      throw ErrorWithLocationAndDetails{
          t->source_location(), "Unterminated If condition",
          after->source_location(), "expected 'Fi' here"};
    }

    m_if_condition_depth--;

    lhs = m_lexer.arena().create<IfStatement>(t->source_location(), condition,
                                              then, otherwise);
  } break;

  case Token::Kind::LeftParen: {
    if (m_recursion_depth + m_parentheses_depth > MAX_RECURSION_DEPTH) {
      throw ErrorWithLocation{
          t->source_location(),
          "Bracket nesting level exceeded maximum of " +
              String::from(static_cast<i64>(MAX_RECURSION_DEPTH),
                           heap_allocator())};
    }

    m_parentheses_depth++;
    lhs = parse_expression();
    m_parentheses_depth--;

    Token *rp = m_lexer.next_expression_token();
    ASSERT(rp != nullptr);
    if (rp->kind() != Token::Kind::RightParen) {
      throw ErrorWithLocationAndDetails{
          t->source_location(), "Unterminated parenthesis",
          rp->source_location(), "expected a closing parenthesis here"};
    }
  } break;

  default:
    if (t->flags() & Token::Flag::UnaryOperator) {
      const let op = static_cast<const tokens::Operator *>(t);

      Expression *rhs = parse_expression(op->unary_precedence());

      lhs = op->construct_unary_expression(rhs);
    } else {
      const let raw = t->raw_string();
      throw ErrorWithLocation{t->source_location(),
                              "Expected a value or an expression, found '" +
                                  raw.view() + "'"};
    }
    break;
  }

  loop
  {
    Token *maybe_op = m_lexer.peek_expression_token();
    ASSERT(maybe_op != nullptr);

    switch (maybe_op->kind()) {
    case Token::Kind::EndOfFile:
    case Token::Kind::Semicolon: return lhs;

    case Token::Kind::RightParen: {
      if (m_parentheses_depth == 0) {
        throw ErrorWithLocationAndDetails{
            maybe_op->source_location(), "Unexpected closing parenthesis",
            "There is no matching opening parenthesis to close here"};
      }
      return lhs;
    }

    case Token::Kind::Else:
    case Token::Kind::Fi: {
      if (m_recursion_depth == 0) {
        const let raw = maybe_op->raw_string();
        throw ErrorWithLocation{maybe_op->source_location(),
                                "Unexpected '" + raw.view() +
                                    "' without matching If condition"};
      }
      return lhs;
    }

    case Token::Kind::Then: {
      if (m_if_condition_depth == 0) {
        const let raw = maybe_op->raw_string();
        throw ErrorWithLocation{maybe_op->source_location(),
                                "Unexpected '" + raw.view() +
                                    "' without matching If condition"};
      }
      return lhs;
    }

    default: break;
    }

    if (!(maybe_op->flags() & Token::Flag::BinaryOperator)) {
      const let raw = maybe_op->raw_string();
      throw ErrorWithLocation{maybe_op->source_location(),
                              "Expected a binary operator, found '" +
                                  raw.view() + "'"};
    }

    const let op = static_cast<const tokens::Operator *>(maybe_op);
    if (op->left_precedence() < min_precedence) break;
    m_lexer.advance_past_last_peek();

    Expression *rhs = parse_expression(
        op->left_precedence() + (op->binary_left_associative() ? 1 : -1));
    lhs = op->construct_binary_expression(lhs, rhs);
  }

  return lhs;
}

} /* namespace koshka */
