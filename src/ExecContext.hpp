/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines per-command execution context. It carries arguments,
 * source locations, descriptors, and command metadata into builtins and
 * koshkit utilities. This short-lived command state is separate from the
 * long-lived EvalContext state. The descriptor routing template requires the
 * complete definition at its call sites.
 */

#pragma once

#include "base/Arena.hpp"
#include "base/Bitset.hpp"
#include "Builtin.hpp"
#include "base/Common.hpp"
#include "base/Containers.hpp"
#include "Errors.hpp"
#include "EvalTypes.hpp"
#include "base/Maybe.hpp"
#include "MimicMood.hpp"
#include "base/Path.hpp"
#include "Platform.hpp"
#include "ProgramResolver.hpp"

namespace koshka {

/* A stage redirection that is applied after the three standard slots are
   placed. Its target is a descriptor above 2, or one of the three when the
   redirection closes it. An opened file rides in file_fd and is owned by the
   context unless is_file_borrowed marks it as the property of the loop
   redirection cache. A duplication leaves file_fd invalid and names its source
   in dup_from_fd, and a close leaves both unset. */
struct nonstandard_descriptor
{
  os::descriptor file_fd{KOSH_INVALID_FD};
  i32 target_fd{-1};
  i32 dup_from_fd{-1};
  bool is_file_borrowed{false};
};

class ExecContext
{
public:
  static fn make_from(const SourceLocation &location, StringView source,
                      ArrayList<String> &&args, mimic_mood mood,
                      bool are_koshkit_utilities_reachable,
                      bool should_check_hash, ProgramResolver &program_resolver,
                      ArrayList<SourceLocation> &&arg_locations) throws
      -> ExecContext;

  /* Build directly from an already resolved builtin kind or program path,
     skipping the PATH search. A simple command memoizes its resolution. */
  static fn from_resolved(SourceLocation location, ResolvedCommand kind,
                          ArrayList<String> &&args,
                          ArrayList<SourceLocation> &&arg_locations) throws
      -> ExecContext;

  /* The rendered diagnostic rides the context because a pipeline stage only
     learns its pipe end after every stage is built, and the message has to
     reach that end. */
  static fn make_unresolved(const SourceLocation &location,
                            i32 resolution_status, StringView diagnostic) throws
      -> ExecContext;

  /* Turn an already built context into an unresolved one, keeping the
     descriptors its redirections placed. A stage whose redirection fails part
     way through reports through the destinations the earlier redirections
     named. */
  fn set_unresolved(i32 resolution_status, StringView diagnostic) throws
      -> void;

  Maybe<os::descriptor> in_fd{};
  Maybe<os::descriptor> out_fd{};
  Maybe<os::descriptor> err_fd{};

  /* A slot descriptor that the loop redirection cache owns is borrowed. The
     cache closes it when its loop ends, and the context leaves it open. */
  bool is_in_fd_borrowed{false};
  bool is_out_fd_borrowed{false};
  bool is_err_fd_borrowed{false};

  /* Almost every command redirects nothing outside the three standard slots.
     The list stays at one null pointer until a stage fills it. */
  SparseList<nonstandard_descriptor> nonstandard_fds{};

  /* 2>&1 routes the standard error to wherever the standard output goes, and
     1>&2 the reverse. Each dup reads the current target of its source
     descriptor. was_output_to_error_last records which one the source wrote
     last when both are present. */
  bool should_duplicate_error_to_output{false};
  bool should_duplicate_output_to_error{false};
  bool was_output_to_error_last{false};

  /* A redirection onto the source descriptor written after its dup moves only
     that descriptor. The dup keeps the stream the command inherits. The
     pipeline places a pipe end without setting either flag. That leaves the dup
     on the pipe the way bash routes it. */
  bool did_output_file_follow_error_dup{false};
  bool did_error_file_follow_output_dup{false};

  /* exec -c hands the program an empty environment. The flag rides the context
     to the spawn site, where the envp becomes a single null instead of environ.
   */
  bool should_use_empty_environment{false};
  bool should_use_fallback_argv0{false};

  /* Set when a koshkit utility runs from a symlink. Its help names the kosh
     binary behind it. */
  bool is_multicall{false};

  bool has_stripped_array_operands{false};

  pure fn is_builtin() const wontthrow -> bool;
  pure fn is_unresolved() const wontthrow -> bool;
  pure fn get_unresolved_status() const wontthrow -> i32;
  pure fn get_unresolved_diagnostic() const wontthrow -> StringView;

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

  /* Place the standard output and standard error files and apply the 2>&1 and
     1>&2 cross-routing in the order the source wrote them. A dup written before
     the redirection of its source descriptor copies the inherited stream, and
     one written after copies the file. When both dups are present the one that
     came last in the source runs last. The four callables carry the platform's
     own way to place a descriptor and to point one descriptor at the other, a
     posix_spawn file action, a dup2, or a Windows handle assignment. */
  template <typename PlaceOut, typename PlaceErr, typename ApplyErrToOut,
            typename ApplyOutToErr>
  fn apply_output_routing(PlaceOut place_out, PlaceErr place_err,
                          ApplyErrToOut apply_err_to_out,
                          ApplyOutToErr apply_out_to_err) const -> void
  {
    let const do_apply_dups = [&](bool has_err_to_out, bool has_out_to_err) {
      if (has_err_to_out && has_out_to_err) {
        if (was_output_to_error_last) {
          apply_err_to_out();
          apply_out_to_err();
        } else {
          apply_out_to_err();
          apply_err_to_out();
        }

        return;
      }

      if (has_err_to_out) apply_err_to_out();
      if (has_out_to_err) apply_out_to_err();
    };

    do_apply_dups(
        should_duplicate_error_to_output && did_output_file_follow_error_dup,
        should_duplicate_output_to_error && did_error_file_follow_output_dup);

    place_out();
    place_err();

    do_apply_dups(
        should_duplicate_error_to_output && !did_output_file_follow_error_dup,
        should_duplicate_output_to_error && !did_error_file_follow_output_dup);
  }

  /* Place every redirection whose target is not one of the three standard
     descriptors. It runs after the standard routing. A duplication reads the
     descriptor that routing already placed. The three callables carry the
     platform's own way to move a file onto a descriptor, to point one
     descriptor at another, and to close one. */
  template <typename PlaceFile, typename PlaceDup, typename CloseTarget>
  fn apply_nonstandard_routing(PlaceFile place_file, PlaceDup place_dup,
                               CloseTarget close_target) const -> void
  {
    for (let const &binding : nonstandard_fds) {
      if (binding.file_fd != KOSH_INVALID_FD) {
        place_file(binding.file_fd, binding.target_fd);
        continue;
      }

      if (binding.dup_from_fd >= 0) {
        place_dup(binding.dup_from_fd, binding.target_fd);
        continue;
      }

      close_target(binding.target_fd);
    }
  }

private:
  ExecContext(SourceLocation location, ResolvedCommand &&kind,
              ArrayList<String> &&args,
              ArrayList<SourceLocation> &&arg_locations);

  ResolvedCommand m_kind;

  String m_unresolved_diagnostic{heap_allocator()};
  SourceLocation m_location;
  ArrayList<String> m_args{heap_allocator()};
  ArrayList<SourceLocation> m_arg_locations{heap_allocator()};
};

} /* namespace koshka */
