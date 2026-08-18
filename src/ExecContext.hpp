#pragma once

#include "Arena.hpp"
#include "Bitset.hpp"
#include "Builtin.hpp"
#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"
#include "EvalTypes.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ProgramResolver.hpp"
#include "ResolvedCommand.hpp"
#include "RuntimeState.hpp"

namespace koshka {

class ExecContext
{
public:
  static fn make_from(const SourceLocation &location, StringView source,
                      ArrayList<String> &&args, mimic_mood mood,
                      bool are_koshkit_utilities_reachable,
                      ProgramResolver &program_resolver,
                      ArrayList<SourceLocation> &&arg_locations) throws
      -> ExecContext;

  /* Build directly from an already resolved builtin kind or program path,
     skipping the PATH search. A simple command memoizes its resolution. */
  static fn from_resolved(SourceLocation location, ResolvedCommand kind,
                          ArrayList<String> &&args,
                          ArrayList<SourceLocation> &&arg_locations) throws
      -> ExecContext;

  static fn make_unresolved(const SourceLocation &location,
                            i32 resolution_status) throws -> ExecContext;

  Maybe<os::descriptor> in_fd{};
  Maybe<os::descriptor> out_fd{};
  Maybe<os::descriptor> err_fd{};

  /* 2>&1 routes the standard error to wherever the standard output goes, and
     1>&2 the reverse. Applied after the file descriptors are placed. When both
     are present the source order decides the result, since each dup reads the
     current target of its source descriptor, so was_output_to_error_last
     records which one the source wrote last. */
  bool should_duplicate_error_to_output{false};
  bool should_duplicate_output_to_error{false};
  bool was_output_to_error_last{false};

  /* exec -c hands the program an empty environment. The flag rides the context
     to the spawn site, where the envp becomes a single null instead of environ.
   */
  bool should_use_empty_environment{false};
  bool should_use_fallback_argv0{false};

  /* Set when a koshkit utility runs from a symlink. Its help names the kosh
     binary behind it. */
  bool is_multicall{false};

  pure fn is_builtin() const wontthrow -> bool;
  pure fn is_unresolved() const wontthrow -> bool;
  pure fn get_unresolved_status() const wontthrow -> i32;

  pure fn args() const wontthrow -> const ArrayList<String> &;
  pure fn program() const wontthrow -> const String &;
  pure fn source_location() const wontthrow -> const SourceLocation &;
  pure fn arg_locations() const wontthrow -> const ArrayList<SourceLocation> &;
  /* The source span of the field at index, clamped to the whole-command span
     when the index is out of range or the list is empty, so a builtin that
     forgot to thread spans degrades to the whole-command caret. */
  pure fn arg_location_at(usize index) const wontthrow -> SourceLocation;

  fn close_fds() throws -> void;
  fn print_to_stdout(StringView s) const throws -> void;
  fn print_to_stderr(StringView s) const throws -> void;

  fn execute(execution_mode mode) throws -> i32;

  pure fn program_path() const wontthrow -> const Path &;
  fn set_program_path(Path path) throws -> void;
  pure fn builtin_kind() const wontthrow -> const Builtin::Kind &;

  /* Apply the 2>&1 and 1>&2 cross-routing in the order the source wrote them.
     Each duplication reads the current target of its source descriptor, so when
     both are present the one that came last in the source must run last. The
     two callables carry the platform's own way to point one descriptor at the
     other, a posix_spawn file action, a dup2, or a Windows handle assignment.
   */
  template <typename ApplyErrToOut, typename ApplyOutToErr>
  fn apply_dup_routing(ApplyErrToOut apply_err_to_out,
                       ApplyOutToErr apply_out_to_err) const -> void
  {
    if (should_duplicate_error_to_output && should_duplicate_output_to_error) {
      if (was_output_to_error_last) {
        apply_err_to_out();
        apply_out_to_err();
      } else {
        apply_out_to_err();
        apply_err_to_out();
      }
    } else if (should_duplicate_error_to_output) {
      apply_err_to_out();
    } else if (should_duplicate_output_to_error) {
      apply_out_to_err();
    }
  }

private:
  ExecContext(SourceLocation location, ResolvedCommand &&kind,
              ArrayList<String> &&args,
              ArrayList<SourceLocation> &&arg_locations);

  ResolvedCommand m_kind;

  SourceLocation m_location;
  ArrayList<String> m_args{heap_allocator()};
  ArrayList<SourceLocation> m_arg_locations{heap_allocator()};
};

} /* namespace koshka */
