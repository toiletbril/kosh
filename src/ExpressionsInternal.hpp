#pragma once

#include "Common.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Platform.hpp"
#include "String.hpp"
#include "StringView.hpp"
#include "Utils.hpp"

#define SET_AND_RETURN_EXIT_STATUS(cxt, status)                                \
  return ::koshka::expressions::set_and_return_exit_status(                    \
      (cxt), static_cast<i64>(status))

namespace koshka {

fn indent_for_layer(usize layer) throws -> String;
fn report_command_resolution_error(
    EvalContext &cxt, const CommandResolutionErrorWithLocation &e) throws
    -> void;

/* The returned view is the windowed source, or None when no window applies and
   the caller renders against the current source. */
fn window_function_body_error(EvalContext &cxt,
                              ErrorWithLocation &error) wontthrow
    -> Maybe<StringView>;

fn static_command_name(const Token *token) throws -> Maybe<StringView>;

/* The token's raw text without a copy when the token owns its bytes. A token
   that composes its text on demand writes into storage, which the caller keeps
   alive for as long as the returned view is read. */
fn borrowed_token_text(const Token *token, String &storage) throws
    -> StringView;
fn wrapped_command_index(command_name_id wrapper_id,
                         const ArrayList<const Token *> &args) throws
    -> Maybe<usize>;
fn apply_followed_source_effects(AnalysisContext &actx,
                                 const followed_source_effects &effects,
                                 bool should_merge_parent_state,
                                 bool should_merge_parent_uncertainty) throws
    -> void;
fn analyze_followed_source(AnalysisContext &actx,
                           const ArrayList<const Token *> &args,
                           usize command_index, bool should_merge_parent_state,
                           bool should_merge_parent_uncertainty) throws -> bool;
fn command_resolves(
    StringView name, const SourceLocation &location,
    const AnalysisContext &actx,
    Maybe<utils::unavailable_path_source_component> &unavailable) throws
    -> bool;
pure fn word_has_malformed_glob_bracket(const Word &word) wontthrow -> bool;

namespace expressions {

pure fn analysis_source_text(const AnalysisContext &actx,
                             const SourceLocation &location) wontthrow
    -> StringView;
pure fn analysis_source_span(const AnalysisContext &actx,
                             const Expression &expression) wontthrow
    -> StringView;

/* The segment span widened over the leading sigil and the braces around it. */
pure fn expansion_location_with_sigil(const AnalysisContext &actx,
                                      SourceLocation location) wontthrow
    -> SourceLocation;

/* One span reaching from the start of the first location to the end of the
   last. An empty location contributes nothing. */
pure fn location_spanning(SourceLocation first, SourceLocation last) wontthrow
    -> SourceLocation;
pure fn view_contains(StringView view, StringView needle) wontthrow -> bool;
pure fn arithmetic_reads_external_input(const AnalysisContext &actx,
                                        StringView expression) wontthrow
    -> bool;
cold fn word_is_bare_glob(const Word &word) wontthrow -> bool;

/* What one test operand expands to, gathered in the segment walk the operand
   loop already performs. */
struct test_operand_shape
{
  bool has_array_spread{false};
  bool has_brace_expansion{false};
  bool has_unquoted_glob{false};
  bool has_unquoted_expansion{false};
  bool has_positional_reference{false};
};

cold fn classify_test_operand(const Word &word) wontthrow -> test_operand_shape;
fn operand_target_name(StringView text) wontthrow -> StringView;
cold fn args_have_stdin_operand(const ArrayList<const Token *> &args) throws
    -> bool;
fn check_posix_word_portability(AnalysisContext &actx,
                                const WordSegment &segment,
                                SourceLocation fallback_location) throws
    -> void;
fn check_posix_arithmetic_operators(AnalysisContext &actx,
                                    StringView expression,
                                    const SourceLocation &location) throws
    -> void;
/* expression_base_position is the source position of the first byte of
   expression, and None when the caller holds a copy it cannot place. A target
   name is recorded only when the computed span reads back as that name. */
fn check_arithmetic_expression_lints(
    AnalysisContext &actx, StringView expression,
    const SourceLocation &location,
    Maybe<usize> expression_base_position = None,
    bool is_conditional = false) throws -> void;
fn check_numeric_comparison_operand(AnalysisContext &actx,
                                    StringView operator_view,
                                    const Token *operand_token,
                                    bool should_prefer_string_comparison) throws
    -> void;
pure fn is_test_unary_operator_word(StringView op) wontthrow -> bool;

/* The borrowed inputs one simple command's name-keyed checks read. The walk in
   SimpleCommand::analyze computes each field once and the check bodies in
   Diagnostics.cpp take them as parameters, so a check adds no traversal. */
struct command_lint_input
{
  const ArrayList<const Token *> &args;
  const ArrayList<Redirection> &redirections;
  const SparseList<prefix_assignment> &local_vars;
  SourceLocation command_source_location;
  StringView command_literal;
  analysis_command_info command_info;
  bool is_command_shadowed;
  bool is_conditional;

  pure fn command_id() const wontthrow -> command_name_id
  {
    return command_info.id;
  }

  pure fn is_in_group(u32 group) const wontthrow -> bool
  {
    return command_info.is_in_group(group);
  }

  /* A redirected compound command carries redirections without a command word.
     The node location stands in for the missing first argument. */
  pure fn command_location() const wontthrow -> SourceLocation
  {
    return args.is_empty() ? command_source_location
                           : args[0]->source_location();
  }
};

/* What one assignment value expands to, gathered in one walk of its
   segments. */
struct assignment_value_shape
{
  bool has_unquoted_pattern{false};
  bool has_only_literal_segments{true};
  bool has_quoted_literal_value{false};
  bool has_bare_literal_value{true};
};

/* The walk shared by a standalone assignment and a command prefix assignment.
   It gathers the shape and reports the findings one segment decides. */
fn scan_assignment_value(AnalysisContext &actx, const Word &value_word,
                         const SourceLocation &location) throws
    -> assignment_value_shape;

/* The borrowed inputs one assignment's value checks read. The segment walk in
   scan_assignment_value computes the shape and the raw view once. */
struct assignment_lint_input
{
  StringView name;
  StringView raw_assignment;
  SourceLocation location;
  bool is_append;
  bool is_command_prefix;
  assignment_value_shape shape;
};

fn check_assignment_value_shape(AnalysisContext &actx,
                                const assignment_lint_input &input) throws
    -> void;

/* The borrowed inputs one case clause's checks read. CaseClause::analyze fills
   it once and the pattern loop passes it to each check body. */
struct case_lint_input
{
  const Word *case_word;
  StringView case_word_source;
  SourceLocation case_location;
  StringView getopts_optstring;
  SourceLocation getopts_location;
  bool is_getopts_case;
};

/* What the pattern loop learned about the arms it has passed. A letter is one
   bit, with 'a' to 'z' at 0 to 25 and 'A' to 'Z' at 26 to 51. */
struct case_arm_tally
{
  u64 handled_option_letters{0};
  bool has_default_arm{false};
  bool has_question_arm{false};
};

fn check_case_word_shape(AnalysisContext &actx,
                         const case_lint_input &input) throws -> void;
fn check_case_pattern_shape(AnalysisContext &actx, const case_lint_input &input,
                            const Word &pattern_word,
                            StringView pattern_literal,
                            StringView pattern_source,
                            const SourceLocation &pattern_location,
                            case_arm_tally &tally) throws -> void;
fn check_case_option_coverage(AnalysisContext &actx,
                              const case_lint_input &input,
                              const case_arm_tally &tally) throws -> void;

/* The homoglyph and carriage return findings the syntax tree cannot carry,
   since a token holds the bytes without their surrounding quoting. */
fn check_source_bytes(AnalysisContext &actx, StringView source) throws -> void;

/* The shebang findings and the POSIX gate, read from one walk of the first
   line. */
fn check_shebang(AnalysisContext &actx, StringView source,
                 bool is_named_script_file) throws -> void;

/* The placement and spelling findings for the directive comments the lexer
   recorded. A directive comment is rare, so this walk touches almost no
   script. */
fn check_shellcheck_directives(
    AnalysisContext &actx, StringView source,
    const ArrayList<shellcheck_directive_span> &directives) throws -> void;

/* The terminator findings the lexer recorded for a here-document that ran to
   the end of the source. The list is empty for a script whose here-documents
   all closed. */
fn check_heredoc_terminators(
    AnalysisContext &actx, StringView source,
    const ArrayList<heredoc_terminator_miss> &misses) throws -> void;

fn check_operand_lints_before_scan(AnalysisContext &actx,
                                   const command_lint_input &input) throws
    -> void;
fn check_command_word_shape(AnalysisContext &actx,
                            const command_lint_input &input) throws -> bool;
fn check_operand_lints_after_scan(AnalysisContext &actx,
                                  const command_lint_input &input) throws
    -> void;
fn check_command_name_lints(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void;
fn check_command_value_lints(AnalysisContext &actx,
                             const command_lint_input &input) throws -> void;
fn check_redirection_lints(AnalysisContext &actx,
                           const command_lint_input &input) throws -> void;
fn check_test_operand_lints(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void;
fn check_prefix_assignment_reads(AnalysisContext &actx,
                                 const command_lint_input &input) throws
    -> bool;

alwaysinline fn set_and_return_exit_status(EvalContext &cxt,
                                           i64 status) wontthrow -> i64
{
  cxt.set_last_exit_status(static_cast<i32>(status));
  return status;
}

enum class redirection_outcome : u8
{
  Heredoc,     /* opened_fd holds a staged temp body for target_fd */
  OpenedFile,  /* opened_fd holds a freshly opened file for target_fd */
  BothStreams, /* opened_fd opens like >file, fd 1 and fd 2 both follow it */
  Duplicate,   /* dup_from_fd names the source, or DUP_FD_CLOSE for the close */
};

/* opened_fd is owned by the caller, which places it and closes it. */
struct resolved_redirection
{
  redirection_outcome kind{};
  i32 target_fd{-1};
  os::descriptor opened_fd{};
  i32 dup_from_fd{-1};
  bool is_cached{false};
};

fn resolve_redirection(const Redirection &redir, EvalContext &cxt,
                       const SourceLocation &fallback_location,
                       bool *open_or_stage_failed = nullptr,
                       bool should_allow_fd_memoization = false) throws
    -> resolved_redirection;

fn allocate_redirection_descriptor(const Redirection &redir,
                                   const resolved_redirection &resolved,
                                   EvalContext &cxt,
                                   const SourceLocation &location,
                                   bool *open_or_stage_failed = nullptr) throws
    -> i32;

enum class loop_disposition : u8
{
  /* No jump, or a continue aimed here, so run the next iteration. */
  RunNext,
  /* A break aimed here, or a jump aimed at an outer loop that is now left
     pending, so this loop stops. */
  StopLoop,
};

fn resolve_loop_control(EvalContext &cxt) throws -> loop_disposition;

/* Whether the shell or the environment gives the name a value on its own, so a
   script that reads it without assigning it is correct. */
pure fn is_shell_maintained_variable(StringView name) wontthrow -> bool;

/* The assignments holding a bare command name that no command word ever
   expanded. A run of the name may follow the assignment, so the decision waits
   for the end of the walk. */
fn check_command_name_assignments(AnalysisContext &actx) throws -> void;

/* The names left in reads_before_assignment once the walk is done. A name still
   listed there was read at the top level and no later assignment claimed it. */
fn check_unassigned_variable_reads(AnalysisContext &actx) throws -> void;

/* The function definitions and calls the walk gathered. A call and a definition
   only agree once both are known, so the comparison waits for the end. */
fn check_function_argument_dataflow(AnalysisContext &actx) throws -> void;

} /* namespace expressions */

} /* namespace koshka */
