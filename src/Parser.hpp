#pragma once

#include "Containers.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"

namespace koshka {

using namespace expressions;

class Parser
{
public:
  Parser(Lexer &&lexer);
  ~Parser();

  fn construct_ast() throws -> Expression *;

  fn construct_next_top_level_ast() throws -> Expression *;
  pure fn is_at_end() const wontthrow -> bool;

  fn construct_ast(ArrayList<String> &errors, EvalContext *context) throws
      -> Expression *;

  pure fn debug_words() const wontthrow -> const ArrayList<Word> &;
  fn take_shellcheck_suppressions() throws -> ArrayList<shellcheck_suppression>;

private:
  static constexpr usize MAX_RECURSION_DEPTH = 64;

  /* The compound-command nesting limit guards the native stack against a
     pathologically nested source such as thousands of open parentheses. It is
     looser than the arithmetic limit because a legitimate script nests far
     fewer compound commands than an arithmetic expression nests operators. */
  static constexpr usize MAX_COMMAND_DEPTH = 512;

  Lexer m_lexer;

  usize m_command_depth{0};
  usize m_recursion_depth{0};
  usize m_if_condition_depth{0};
  usize m_parentheses_depth{0};
  bool m_should_stop_after_top_level_unit{false};
  bool m_has_parsed_source_command{false};
  ArrayList<shellcheck_suppression> m_shellcheck_suppressions{heap_allocator()};

  mustuse fn parse_simple_command() throws -> Command *;

  fn recover_to_next_statement() throws -> void;

  fn skip_newlines_after_pipe() throws -> void;
  fn skip_semicolons_and_newlines() throws -> void;

  /* Build one file or descriptor-duplication redirection for descriptor fd. The
     operator is already consumed and op_location is its position. Shared by the
     simple command parser and the trailing redirect parser. fd_was_explicit
     records whether the source spelled the descriptor, since a bare >&word
     with a literal non-numeric word is the csh both-streams spelling while
     2>&word keeps the descriptor reading. */
  fn build_file_or_dup_redirection(
      i32 fd, Token::Kind op_kind, SourceLocation op_location,
      Maybe<SourceLocation> &first_location,
      ArrayList<expressions::Redirection> &out, bool fd_was_explicit,
      const Token *fd_allocation_name_token = nullptr) throws -> void;

  fn build_both_streams_redirection(
      bool is_append, SourceLocation op_location,
      Maybe<SourceLocation> &first_location,
      ArrayList<expressions::Redirection> &out) throws -> void;

  mustuse fn wrap_with_stderr_to_stdout(Command *command) throws -> Command *;

  fn build_here_string_redirection(
      SourceLocation op_location, Maybe<SourceLocation> &first_location,
      ArrayList<expressions::Redirection> &out) throws -> void;

  /* Build one heredoc redirection on descriptor fd. The << operator is already
     consumed and op_location is its position. A digit prefix such as the 3 in
     3<<EOF supplies a non-zero fd. */
  fn build_heredoc_redirection(i32 fd, SourceLocation op_location,
                               Maybe<SourceLocation> &first_location,
                               ArrayList<expressions::Redirection> &out) throws
      -> void;

  /* The digit word is already peeked and word_location is its position. Consume
     it, and when the next token is a redirect operator touching the digit run,
     parse the descriptor and append the redirection to out, then return true.
     Return false with the digit consumed when no adjacent operator follows, so
     the caller decides what the bare number means. Shared by the simple command
     parser and the trailing redirect parser. */
  mustuse fn try_parse_descriptor_prefixed_redirection(
      const tokens::WordToken *word_token, SourceLocation word_location,
      Maybe<SourceLocation> &first_location,
      ArrayList<expressions::Redirection> &out) throws -> bool;

  mustuse fn try_parse_trailing_redirection(
      ArrayList<expressions::Redirection> &out) throws -> bool;

  mustuse fn attach_trailing_redirections(Command *compound) throws
      -> Command *;

  mustuse fn parse_command_list(
      std::initializer_list<Token::Kind> terminators) throws -> Expression *;

  /* A do-group body cannot be empty, the way dash and bash both reject a loop
     with nothing between 'do' and 'done'. The caret points at the terminator
     the empty list stopped on. */
  fn reject_empty_loop_body(const Expression *body) throws -> void;

  mustuse fn parse_if() throws -> Command *;
  mustuse fn parse_while_or_until(bool is_until) throws -> Command *;
  mustuse fn parse_for() throws -> Command *;
  mustuse fn parse_select() throws -> Command *;
  mustuse fn parse_optional_in_clause_words(
      ArrayList<const Token *> &words) throws -> bool;
  mustuse fn parse_case() throws -> Command *;
  mustuse fn parse_brace_group() throws -> Command *;
  mustuse fn parse_paren_command() throws -> Command *;
  mustuse fn parse_subshell(Token *open) throws -> Command *;
  mustuse fn capture_double_paren_body(Token *open) throws -> StringView;
  mustuse fn parse_arithmetic_command(Token *open) throws -> Command *;
  mustuse fn parse_c_style_for(SourceLocation location, Token *open) throws
      -> Command *;
  mustuse fn parse_conditional_command() throws -> Command *;
  mustuse fn parse_function_definition(const Token *name_token) throws
      -> Command *;

  mustuse fn parse_keyword_function_definition() throws -> Command *;

  mustuse fn finish_function_body(SourceLocation location,
                                  StringView name) throws -> Command *;

  /* Consume a bash array assignment group NAME=(...) or NAME+=(...) and return
     its element tokens. Bash mode expands them into the array, POSIX mode
     discards the list and evaluates the assignment as a no-op. */
  fn consume_bash_array_assignment() throws -> ArrayList<const Token *>;

  mustuse fn parse_expression(u8 min_precedence = 0) throws -> Expression *;
};

} /* namespace koshka */
