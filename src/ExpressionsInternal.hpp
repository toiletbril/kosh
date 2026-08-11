#pragma once

#include "Common.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Platform.hpp"
#include "String.hpp"
#include "StringView.hpp"

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

namespace expressions {

pure fn analysis_source_text(const AnalysisContext &actx,
                             SourceLocation location) wontthrow -> StringView;
pure fn analysis_source_span(const AnalysisContext &actx,
                             const Expression &expression) wontthrow
    -> StringView;
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
                                    SourceLocation location) throws -> void;
fn check_arithmetic_test_operators(AnalysisContext &actx, StringView expression,
                                   SourceLocation location) throws -> void;
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
  const ArrayList<prefix_assignment> &local_vars;
  SourceLocation command_source_location;
  StringView command_literal;
  analysis_command_info command_info;
  bool command_is_shadowed;

  pure fn command_id() const wontthrow -> command_name_id
  {
    return command_info.id;
  }

  pure fn is_in_group(u32 group) const wontthrow -> bool
  {
    return command_info.is_in_group(group);
  }

  pure fn command_location() const wontthrow -> SourceLocation
  {
    return args[0]->source_location();
  }
};

/* The borrowed inputs one assignment's value checks read. The segment walk in
   AssignCommand::analyze computes the pattern bit and the raw view once. */
struct assignment_lint_input
{
  StringView name;
  StringView raw_assignment;
  SourceLocation location;
  bool is_append;
  bool has_unquoted_pattern;
  bool has_only_literal_segments;
  bool has_quoted_literal_value;
  bool has_bare_literal_value;
};

fn check_assignment_value_shape(AnalysisContext &actx,
                                const assignment_lint_input &input) throws
    -> void;

fn check_operand_lints_before_scan(AnalysisContext &actx,
                                   const command_lint_input &input) throws
    -> void;
fn check_command_word_shape(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void;
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
    -> void;

alwaysinline fn set_and_return_exit_status(EvalContext &cxt,
                                           i64 status) wontthrow -> i64
{
  cxt.set_last_exit_status(static_cast<i32>(status));
  return status;
}

enum class redirection_outcome
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
                       SourceLocation fallback_location,
                       bool *open_or_stage_failed = nullptr,
                       bool allow_fd_memoization = false) throws
    -> resolved_redirection;

fn allocate_redirection_descriptor(const Redirection &redir,
                                   const resolved_redirection &resolved,
                                   EvalContext &cxt, SourceLocation location,
                                   bool *open_or_stage_failed = nullptr) throws
    -> i32;

enum class loop_disposition
{
  /* No jump, or a continue aimed here, so run the next iteration. */
  RunNext,
  /* A break aimed here, or a jump aimed at an outer loop that is now left
     pending, so this loop stops. */
  StopLoop,
};

fn resolve_loop_control(EvalContext &cxt) throws -> loop_disposition;

} /* namespace expressions */

} /* namespace koshka */
