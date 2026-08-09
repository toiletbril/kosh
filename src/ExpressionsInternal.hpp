#pragma once

#include "Common.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Platform.hpp"
#include "String.hpp"
#include "StringView.hpp"

#define SET_AND_RETURN_EXIT_STATUS(cxt, status)                                \
  return ::shit::expressions::set_and_return_exit_status(                      \
      (cxt), static_cast<i64>(status))

namespace shit {

fn indent_for_layer(usize layer) throws -> String;
fn report_command_resolution_error(
    EvalContext &cxt, const CommandResolutionErrorWithLocation &e) throws
    -> void;

/* The returned view is the windowed source, or None when no window applies and
   the caller renders against the current source. */
fn window_function_body_error(EvalContext &cxt,
                              ErrorWithLocation &error) wontthrow
    -> Maybe<StringView>;

fn static_command_name(const Token *token) throws -> Maybe<String>;

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

} /* namespace shit */
