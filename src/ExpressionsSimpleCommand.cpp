/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements assignment-only commands, simple command storage, and
 * redirection resolution. It prepares descriptors and process substitutions
 * for execution. ExpressionsSimpleCommandEval.cpp owns alias expansion and
 * final dispatch.
 */

#include "base/Arena.hpp"
#include "Builtin.hpp"
#include "CLI.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "base/Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

using namespace internal;

static pure fn is_process_dynamic_name(StringView text, usize position,
                                       bool is_bash_dynamic) wontthrow -> bool
{
  if (!is_bash_dynamic) return false;
  if (position > 0 && lexer::is_variable_name(text[position - 1])) return false;

  usize end_position = position;
  while (end_position < text.length &&
         lexer::is_variable_name(text[end_position]))
    end_position++;

  return is_process_dynamic_variable_name(
      text.substring_of_length(position, end_position - position));
}

static fn
word_is_safe_for_in_process_substitution(const Word &word,
                                         bool is_bash_dynamic) wontthrow -> bool
{
  for (let const &segment : word.segments) {
    if (segment.kind == WordSegment::Kind::ProcessSubstitution ||
        segment.kind == WordSegment::Kind::FunctionSubstitution)
    {
      return false;
    }

    if (segment.kind == WordSegment::Kind::VariableReference) {
      if (!segment.text.is_empty() && segment.text[0] == '!') {
        return false;
      }
      if (is_process_dynamic_name(segment.text.view(), 0, is_bash_dynamic))
        return false;
    }

    if (segment.kind == WordSegment::Kind::ArithmeticExpansion)
      for (usize position = 0; position < segment.text.count(); position++)
        if (is_process_dynamic_name(segment.text.view(), position,
                                    is_bash_dynamic))
          return false;
  }

  return true;
}

AssignCommand::AssignCommand(SourceLocation location, const Assignment *a)
    : Command(steal(location)), m_assignment(a)
{}

AssignCommand::~AssignCommand() = default;

pure fn AssignCommand::assignment() const wontthrow -> const Assignment *
{
  return m_assignment;
}

fn AssignCommand::is_assignment() const wontthrow -> bool { return true; }

fn AssignCommand::as_assign_command() const wontthrow -> const AssignCommand *
{
  return this;
}

fn AssignCommand::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
  ASSERT(m_assignment != nullptr);

  if (actx.is_posix_sh_shebang &&
      m_assignment->get_update_mode() == assignment_update_mode::Append) {
    actx.report_diagnostic(diagnostic_id::sc3024, source_location(),
                           {m_assignment->key().view()});
  }

  let const shape = scan_assignment_value(actx, m_assignment->value_word(),
                                          source_location());

  let const raw_assignment = m_assignment->raw_string();

  check_assignment_value_shape(
      actx, assignment_lint_input{m_assignment->key().view(),
                                  raw_assignment.view(), source_location(),
                                  m_assignment->get_update_mode(), false,
                                  shape});
  let const first_colon = raw_assignment.view().find_character(':');
  if (m_assignment->key().view() == "PATH" &&
      (raw_assignment.view().starts_with(StringView{"PATH=~/"}) ||
       raw_assignment.view().starts_with(StringView{"PATH+=~/"}) ||
       (first_colon.has_value() && raw_assignment.view()
                                       .substring(*first_colon + 1)
                                       .starts_with(StringView{"~/"}))))
  {
    actx.report_diagnostic(diagnostic_id::sc2147, source_location());
  }

  let const prompt_has_control_escape =
      view_contains(raw_assignment.view(), StringView{"\\e"}) ||
      view_contains(raw_assignment.view(), StringView{"\\033"}) ||
      view_contains(raw_assignment.view(), StringView{"\\x1b"});
  let const prompt_has_display_guards =
      view_contains(raw_assignment.view(), StringView{"\\["}) &&
      view_contains(raw_assignment.view(), StringView{"\\]"});
  if (m_assignment->key().view() == "PS1" && prompt_has_control_escape &&
      !prompt_has_display_guards)
  {
    actx.report_diagnostic(diagnostic_id::sc2025, source_location());
  }

  /* The fold reads the constant table, so it runs before the table records this
     assignment. */
  optimizer::optimize_node(this, actx);

  let const &name = m_assignment->key();

  if (actx.is_direct_pipeline_stage) {
    actx.report_diagnostic(diagnostic_id::sc2030_assignment, source_location());
    actx.pipeline_lost_names.add(name.view());
  }

  /* A PATH assignment leaves the runtime search path unknown to the prepass, so
     a later command's not-found check stays quiet. */
  if (name.view() == "PATH") actx.mark_path_unknown(true);
  if (is_source_location_variable(name.view()))
    actx.mark_working_directory_unknown();

  /* An element assignment a[i]=v changes what $a reads without recording a
     scalar literal, so the base name before the bracket is forgotten. */
  if (let const bracket = name.view().find_character('['); bracket.has_value())
  {
    let const base = name.view().substring_of_length(0, *bracket);
    if (actx.is_direct_pipeline_stage) actx.pipeline_lost_names.add(base);
    if (name.length() > *bracket + 1 && name[name.length() - 1] == ']') {
      let const subscript = name.view().substring_of_length(
          *bracket + 1, name.length() - *bracket - 2);
      if (actx.external_input_names.contains(subscript))
        actx.report_diagnostic(diagnostic_id::external_array_subscript,
                               source_location());
    }
    let const name_location = source_location().subspan(0, base.length);
    actx.note_variable_occurrence(
        base, name_location, variable_occurrence_kind::Assignment,
        !is_unconditional || actx.has_seen_runtime_definer);
    actx.note_variable_assignment(base, source_location(),
                                  is_unconditional &&
                                      !actx.has_seen_runtime_definer);
    actx.note_variable_assignment_record(base, nullptr, source_location(),
                                         !is_unconditional ||
                                             actx.has_seen_runtime_definer,
                                         m_assignment->get_update_mode());
    actx.add_array_valued_name(base);
    LOG(All,
        "forgetting the constant for the array base '%.*s' after an element "
        "assignment",
        static_cast<int>(*bracket), name.view().data);
    actx.constant_variables.erase(base);
    return;
  }

  let const name_location = source_location().subspan(0, name.count());
  actx.note_variable_occurrence(
      name.view(), name_location, variable_occurrence_kind::Assignment,
      !is_unconditional || actx.has_seen_runtime_definer,
      m_assignment->get_update_mode());
  actx.note_variable_assignment(name.view(), source_location(),
                                is_unconditional &&
                                    !actx.has_seen_runtime_definer);
  /* The record is taken before the constant table gives up on this name. A
     conditional or appending assignment stays answerable. */
  actx.note_variable_assignment_record(
      name.view(), &m_assignment->value_word(), source_location(),
      !is_unconditional || actx.has_seen_runtime_definer,
      m_assignment->get_update_mode());

  if (actx.function_scope_depth > 0 &&
      m_assignment->get_update_mode() != assignment_update_mode::Append &&
      actx.function_local_names.find(name.view()) == nullptr &&
      actx.global_assigned_names.find(name.view()) == nullptr &&
      !actx.inherited_global_assigned_names.contains(name.view()) &&
      !(actx.eval_context != nullptr &&
        actx.eval_context->get_variable_value(name.view()).has_value()))
  {
    actx.report_diagnostic(diagnostic_id::no_local, source_location(),
                           {name.view()});
  }

  if (actx.function_scope_depth == 0 && is_unconditional &&
      !actx.has_seen_runtime_definer)
  {
    actx.add_global_assigned_name(name.view(), source_location());
  }

  /* A conditional or nested assignment may not run, a runtime definer may have
     changed the name out of view, and NAME+=VALUE depends on the untracked
     prior value, so each forgets the name. */
  if (!is_unconditional || actx.has_seen_runtime_definer ||
      m_assignment->get_update_mode() == assignment_update_mode::Append)
  {
    LOG(All,
        "forgetting the constant for '%s', the assignment is conditional, "
        "appends, or follows a runtime definer",
        name.c_str());
    actx.constant_variables.erase(name.view());
    return;
  }

  let const literal = optimizer::literal_word_value(m_assignment->value_word());
  if (literal.has_value()) {
    LOG(All, "recording the constant '%s' = '%s'", name.c_str(),
        literal->c_str());
    actx.constant_variables.set(name.view(), literal->view());
    actx.optimizer_eliminated_count++;
    if (actx.should_report_optimizer_diagnostics)
      actx.trace_optimizer_line(String{"recorded constant: "} + name + " = " +
                                *literal);
  } else {
    LOG(All,
        "forgetting the constant for '%s', its value is only known at run "
        "time",
        name.c_str());
    actx.constant_variables.erase(name.view());
  }
}

hot fn AssignCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_assignment != nullptr);

  LOG(All, "assigning the variable '%s'", m_assignment->key().c_str());

  cxt.set_current_location(source_location());

  let const should_run_assignment =
      publish_command_and_run_debug_trap(cxt, [&] {
        return source_command_text(
            cxt, source_location(), source_end_position(), [&] {
              return String{heap_allocator(),
                            m_assignment->raw_string().view()};
            });
      });
  if (!should_run_assignment) return cxt.last_exit_status();

  /* A command substitution in the value leaves the status of the last one, so
     the reset to 0 waits until after the expansion and a $? in the value reads
     the prior command's status. */
  let const value_ran_substitution =
      m_assignment->value_word().runs_substitution();

  try {
    let value = cxt.expand_word_for_assignment(m_assignment->value_word());

    if (cxt.should_echo_expanded()) {
      let trace = String{cxt.scratch_allocator(), m_assignment->key().view()};
      trace += m_assignment->get_update_mode() == assignment_update_mode::Append
                   ? "+="
                   : "=";
      append_shell_quoted_arg(trace, value.view());
      cxt.write_xtrace(trace.view());
    }

    let const key_view = m_assignment->key().view();
    if (let const bracket = key_view.find_character('[');
        bracket.has_value() && key_view[key_view.length - 1] == ']')
    {
      let const array_name = key_view.substring_of_length(0, *bracket);
      let const subscript = key_view.substring_of_length(
          *bracket + 1, key_view.length - *bracket - 2);
      cxt.assign_array_element(array_name, subscript, value.view(),
                               m_assignment->get_update_mode());
      if (!value_ran_substitution) cxt.set_last_exit_status(0);
      cxt.publish_single_pipe_status(cxt.last_exit_status());
      return cxt.last_exit_status();
    }

    /* NAME+=VALUE prepends the current value of NAME, empty when unset. An
       integer name adds rather than concatenates. */
    if (m_assignment->get_update_mode() == assignment_update_mode::Append) {
      let appended =
          String{cxt.get_variable_value(m_assignment->key()).value_or("")};
      if (cxt.is_integer_variable(m_assignment->key()))
        cxt.append_integer_expression(appended, value.view());
      else
        appended += value;
      value = steal(appended);
    }

    cxt.set_shell_variable(m_assignment->key(), value);
    if (cxt.export_all()) {
      let const &key = m_assignment->key();
      cxt.record_environment_change(key);
      os::set_environment_variable(key, value);
      cxt.mark_exported(key);
    }
    if (!value_ran_substitution) cxt.set_last_exit_status(0);
    cxt.publish_single_pipe_status(cxt.last_exit_status());
    return cxt.last_exit_status();
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (const Error &e) {
    relocate_error(e, source_location());
  }
}

cold fn AssignCommand::to_string() const throws -> String
{
  let result = "Assign " + m_assignment->to_ast_string();
  append_ast_execution_flags(result);
  return result;
}

fn AssignCommand::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  unused(active_functions);
  return !is_async() && !is_timed() &&
         word_is_safe_for_in_process_substitution(
             m_assignment->value_word(), cxt.bash_dynamic_variables_enabled());
}

SimpleCommand::SimpleCommand(SourceLocation location,
                             ArrayList<const Token *> &&args)
    : Command(steal(location)), m_args(steal(args))
{
  /* The location spans from the first word to the end of the last, so a caret
     covers the whole command and not only the command word. */
  if (!m_args.is_empty()) {
    let const first = m_args[0]->source_location();
    let const last = m_args.back()->source_location();
    m_location.position = first.position;
    m_location.length =
        static_cast<u32>(last.position + last.length - first.position);
    set_source_end_position(m_location.position + m_location.length);
  }
}

SimpleCommand::~SimpleCommand() = default;

fn SimpleCommand::set_full_source_end_position(usize position) wontthrow -> void
{
  m_full_source_end_position = static_cast<u32>(position);
}

pure fn SimpleCommand::full_source_start_position() const wontthrow -> usize
{
  let start_position = usize{m_location.position};
  for (let const &var : m_local_vars) {
    let const assignment_position = var.get_location().position;
    if (assignment_position < start_position)
      start_position = assignment_position;
  }

  return start_position;
}

pure fn SimpleCommand::assignments_source_end_position() const wontthrow
    -> usize
{
  let end_position = source_end_position();
  for (let const &var : m_local_vars) {
    let const location = var.get_location();
    let const var_end_position = usize{location.position} + location.length;
    if (var_end_position > end_position) end_position = var_end_position;
  }

  for (let const &assignment : m_array_args) {
    let const assignment_end_position = usize{assignment.end_position};
    if (assignment_end_position > end_position)
      end_position = assignment_end_position;
  }

  return end_position;
}

pure fn SimpleCommand::full_source_end_position() const wontthrow -> usize
{
  let const words_end_position = source_end_position();

  return m_full_source_end_position > words_end_position
             ? m_full_source_end_position
             : words_end_position;
}

fn SimpleCommand::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  if (is_async() || is_timed() || !m_redirections.is_empty() ||
      !m_array_args.is_empty() || cxt.has_aliases() || m_args.is_empty())
  {
    return false;
  }

  for (let const argument : m_args) {
    if (argument->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(argument)->word();
    if (!word_is_safe_for_in_process_substitution(
            word, cxt.bash_dynamic_variables_enabled()))
      return false;
  }

  let const command_name = static_command_name(m_args[0]);
  if (!command_name.has_value()) return false;

  if (let const function_body = cxt.find_function(*command_name);
      function_body != nullptr)
  {
    if (!active_functions.add(*command_name)) return true;
    defer { active_functions.remove(*command_name); };
    return function_body->can_evaluate_in_process_substitution(
        cxt, active_functions);
  }

  constexpr PackedStringKey SAFE_BUILTIN_KEYS[] = {
      SSK(":"),     SSK("echo"),   SSK("false"),  SSK("let"),
      SSK("local"), SSK("printf"), SSK("return"), SSK("true"),
  };
  constexpr StaticStringSet SAFE_BUILTINS{SAFE_BUILTIN_KEYS};
  return SAFE_BUILTINS.contains(*command_name);
}

fn SimpleCommand::set_redirections(ArrayList<Redirection> &&redirections) throws
    -> void
{
  m_redirections.fill(steal(redirections));
}

fn SimpleCommand::append_redirection(const Redirection &redirection,
                                     Allocator allocator) throws -> void
{
  ArrayList<Redirection> redirections{allocator};
  redirections.reserve(m_redirections.count() + 1);
  for (let const &existing : m_redirections) {
    redirections.push(existing);
  }

  redirections.push(redirection);
  m_redirections.fill(steal(redirections));
}

fn SimpleCommand::set_array_args(
    ArrayList<array_builtin_assignment> &&array_args) throws -> void
{
  m_array_args.fill(steal(array_args));
}

namespace {

using expressions::Redirection;

/* Keep one binding for each nonstandard target. The last redirection of that
   descriptor wins, and the file it replaces closes here unless the loop
   redirection cache owns it. */
fn bind_nonstandard_fd(ArrayList<nonstandard_descriptor> &nonstandard,
                       nonstandard_descriptor binding) throws -> void
{
  for (let &existing : nonstandard) {
    if (existing.target_fd != binding.target_fd) continue;

    if (existing.file_fd != KOSH_INVALID_FD && !existing.is_file_borrowed) {
      os::close_fd(existing.file_fd);
    }
    existing = binding;

    return;
  }

  nonstandard.push(binding);
}

/* Route an opened descriptor into the slot its target names, fd 0 to input, 1
   to output, 2 to error. Any other target keeps its own number and joins the
   nonstandard list. The last redirection of a descriptor wins. A descriptor in
   the slot closes first unless the loop redirection cache owns it. */
fn assign_redirected_fd(ExecContext &ec,
                        ArrayList<nonstandard_descriptor> &nonstandard, i32 fd,
                        os::descriptor file_fd,
                        bool is_file_borrowed = false) throws -> void
{
  if (fd == 0) {
    if (ec.in_fd && !ec.is_in_fd_borrowed) os::close_fd(*ec.in_fd);
    ec.in_fd = file_fd;
    ec.is_in_fd_borrowed = is_file_borrowed;

    return;
  }

  if (fd == 1) {
    if (ec.out_fd && !ec.is_out_fd_borrowed) os::close_fd(*ec.out_fd);
    ec.out_fd = file_fd;
    ec.is_out_fd_borrowed = is_file_borrowed;

    return;
  }

  if (fd == 2) {
    if (ec.err_fd && !ec.is_err_fd_borrowed) os::close_fd(*ec.err_fd);
    ec.err_fd = file_fd;
    ec.is_err_fd_borrowed = is_file_borrowed;

    return;
  }

  bind_nonstandard_fd(
      nonstandard, nonstandard_descriptor{file_fd, fd, -1, is_file_borrowed});
}

/* A resolved duplication target, the descriptor or close marker in fd, or the
   csh both-streams filename when >&word expanded to a name, read as >word
   2>&1. */
struct resolved_duplication
{
  i32 fd{-1};
  Maybe<String> both_streams_file{};
};

fn resolve_duplication(const Redirection &redir, EvalContext &cxt) throws
    -> resolved_duplication
{
  if (redir.target == nullptr)
    return resolved_duplication{redir.dup_fd, koshka::None};

  ArrayList<const Token *> target_tokens{cxt.scratch_allocator()};
  target_tokens.push(redir.target);
  ArrayList<String> fields = cxt.process_args(target_tokens);
  if (fields.count() != 1) {
    throw ErrorWithLocation{redir.target->source_location(),
                            "Duplication target is not a single descriptor"};
  }

  String &field = fields[0];
  if (field == "-")
    return resolved_duplication{Redirection::DUP_FD_CLOSE, koshka::None};

  let const parsed_descriptor = field.view().to<i64>();
  if (parsed_descriptor.is_error() || parsed_descriptor.value() < 0) {
    if (redir.is_dup_filename_allowed)
      return resolved_duplication{-1, steal(field)};
    throw ErrorWithLocation{redir.target->source_location(),
                            "'" + field + "' is not a valid descriptor"};
  }
  return resolved_duplication{static_cast<i32>(parsed_descriptor.value()),
                              koshka::None};
}

} /* namespace */

static fn redirection_open_mode(Redirection::Kind kind,
                                bool no_clobber) wontthrow -> os::file_open_mode
{
  switch (kind) {
  case Redirection::Kind::TruncateOutput:
    return no_clobber ? os::file_open_mode::TruncateNoClobber
                      : os::file_open_mode::Truncate;
  case Redirection::Kind::TruncateOutputOverride:
    return os::file_open_mode::Truncate;
  case Redirection::Kind::AppendOutput: return os::file_open_mode::Append;
  case Redirection::Kind::ReadWrite: return os::file_open_mode::ReadWrite;
  case Redirection::Kind::ReadInput: return os::file_open_mode::Read;
  default: unreachable("only a file-opening redirection has an open mode");
  }
}

static fn redirection_open_error(StringView path) throws -> String
{
  let const system_error = os::last_system_error_message();
  if (os::path_is_directory(path)) return String{"Is a directory"};

  return system_error;
}

/* Resolve one redirection to an unplaced outcome, the shared open-and-stage
   work the three redirection sites repeat. The returned descriptor is the
   caller's to place and to close. A failure throws a located error, and
   open_or_stage_failed is set true only for the open, stage, and
   ambiguous-target failures the simple-command path recovers from, so a
   duplication-resolve or word-expansion error stays fatal. */
[[noreturn]] fn
reject_restricted_output_redirection(const Redirection &redir,
                                     const SourceLocation &fallback_location,
                                     bool *open_or_stage_failed) throws -> void
{
  if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
  throw ErrorWithLocation{
      redir.target != nullptr ? redir.target->source_location()
                              : fallback_location,
      "Output redirection is forbidden in a restricted shell"};
}

fn internal::resolve_redirection(const Redirection &redir, EvalContext &cxt,
                                 const SourceLocation &fallback_location,
                                 bool *open_or_stage_failed,
                                 bool allow_fd_memoization) throws
    -> resolved_redirection
{
  if (cxt.restricted_enforcement_active() &&
      (redir.kind == Redirection::Kind::TruncateOutput ||
       redir.kind == Redirection::Kind::TruncateOutputOverride ||
       redir.kind == Redirection::Kind::AppendOutput ||
       redir.kind == Redirection::Kind::ReadWrite))
  {
    reject_restricted_output_redirection(redir, fallback_location,
                                         open_or_stage_failed);
  }

  if (redir.kind == Redirection::Kind::Heredoc ||
      redir.kind == Redirection::Kind::HereString)
  {
    let expanded_body = String{cxt.scratch_allocator()};
    let body = StringView{};
    if (redir.kind == Redirection::Kind::Heredoc) {
      ASSERT(redir.heredoc != nullptr);
      body = redir.heredoc->text.view();
      if (redir.should_expand_heredoc) {
        let source_location = SourceLocation{};
        const SourceLocation *source_location_pointer = nullptr;
        if (redir.heredoc->has_contiguous_source) {
          source_location =
              SourceLocation{redir.heredoc->source_position, body.length,
                             fallback_location.source_name_index};
          source_location_pointer = &source_location;
        }
        expanded_body = cxt.expand_heredoc_body(body, source_location_pointer);
        body = expanded_body.view();
      }
    } else {
      ASSERT(redir.target != nullptr);
      expanded_body = cxt.expand_word_for_assignment(
          static_cast<const tokens::WordToken *>(redir.target)->word());
      expanded_body += "\n";
      body = expanded_body.view();
    }

    let opened = os::write_to_temp_file(body);
    if (!opened) {
      if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
      throw ErrorWithLocation{redir.target != nullptr
                                  ? redir.target->source_location()
                                  : fallback_location,
                              "Could not stage the heredoc body: " +
                                  os::last_system_error_message()};
    }
    return resolved_redirection{redirection_outcome::Heredoc, redir.fd,
                                opened.take(), -1};
  }

  if (redir.kind == Redirection::Kind::DuplicateOutput ||
      redir.kind == Redirection::Kind::DuplicateInput)
  {
    let resolved_dup = resolve_duplication(redir, cxt);
    if (resolved_dup.both_streams_file.has_value()) {
      if (cxt.restricted_enforcement_active()) {
        reject_restricted_output_redirection(redir, fallback_location,
                                             open_or_stage_failed);
      }
      let opened = os::open_file_descriptor(
          *resolved_dup.both_streams_file,
          redirection_open_mode(Redirection::Kind::TruncateOutput,
                                cxt.no_clobber()));
      if (!opened) {
        if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
        throw ErrorWithLocation{
            redir.target->source_location(),
            "Could not open '" + *resolved_dup.both_streams_file + "': " +
                redirection_open_error(*resolved_dup.both_streams_file)};
      }
      return resolved_redirection{redirection_outcome::BothStreams, 1,
                                  opened.take(), -1};
    }
    return resolved_redirection{
        redirection_outcome::Duplicate, redir.fd, {}, resolved_dup.fd};
  }

  ASSERT(redir.target != nullptr);

  ArrayList<const Token *> target_tokens{cxt.scratch_allocator()};
  target_tokens.push(redir.target);
  const ArrayList<String> target =
      cxt.process_args(target_tokens, nullptr, argument_lifetime::Transient);
  if (target.count() != 1) {
    if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
    throw ErrorWithLocation{redir.target->source_location(),
                            "Redirection target is not a single file"};
  }

  let mode = redirection_open_mode(redir.kind, cxt.no_clobber());

  const String &target_path = target[0];

  const bool should_memoize_append =
      allow_fd_memoization && mode == os::file_open_mode::Append &&
      cxt.loop_depth() > 0 && redir.fd_allocation_name_token == nullptr;
  if (should_memoize_append) {
    let cached = cxt.find_loop_redirect_fd(redir.fd, target_path, mode);
    if (cached.has_value())
      return resolved_redirection{redirection_outcome::OpenedFile, redir.fd,
                                  cached.value(), -1, /*is_cached=*/true};
  }

  let opened = os::open_file_descriptor(target_path, mode);
  if (!opened) {
    if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;

    /* An interrupt ends the shell, and the open never reached the file. The
       flag stays set so the command boundary above still sees the interrupt. */
    if (os::INTERRUPT_REQUESTED)
      throw InterruptErrorWithLocation{redir.target->source_location()};

    throw ErrorWithLocation{redir.target->source_location(),
                            "Could not open '" + target_path +
                                "': " + redirection_open_error(target_path)};
  }

  let const file_fd = opened.take();
  if (should_memoize_append &&
      cxt.retain_loop_redirect_fd(redir.fd, target_path, mode, file_fd))
  {
    return resolved_redirection{redirection_outcome::OpenedFile, redir.fd,
                                file_fd, -1, /*is_cached=*/true};
  }

  return resolved_redirection{redirection_outcome::OpenedFile, redir.fd,
                              file_fd, -1, /*is_cached=*/false};
}

static fn read_fd_allocation_value(EvalContext &cxt,
                                   const fd_allocation_target &target) throws
    -> Maybe<String>
{
  if (!target.has_subscript) return cxt.get_variable_value(target.name);

  let value =
      cxt.read_array_element_arithmetic_text(target.name, target.subscript);
  if (value.is_empty()) return None;

  return value;
}

fn internal::allocate_redirection_descriptor(
    const Redirection &redir, const resolved_redirection &resolved,
    EvalContext &cxt, const SourceLocation &location,
    bool *open_or_stage_failed, const Maybe<String> *known_current_value) throws
    -> i32
{
  if (redir.fd_allocation_name_token == nullptr) return redir.fd;

  let const &allocation_word =
      static_cast<const tokens::WordToken *>(redir.fd_allocation_name_token)
          ->word();
  let const target = allocation_word.get_fd_allocation_target();
  ASSERT(target.has_value());
  let const allocation_name = allocation_word.fd_allocation_name();
  ASSERT(allocation_name.has_value());

  if (resolved.kind == redirection_outcome::Duplicate &&
      resolved.dup_from_fd == Redirection::DUP_FD_CLOSE)
  {
    let const read_value = known_current_value != nullptr
                               ? Maybe<String>{}
                               : read_fd_allocation_value(cxt, *target);
    let const *current_value =
        known_current_value != nullptr ? known_current_value : &read_value;

    if (current_value->has_value()) {
      let const parsed = (*current_value)->view().to<i64>();
      if (!parsed.is_error() && parsed.value() >= 0) {
        return static_cast<i32>(parsed.value());
      }
    }

    if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
    throw ErrorWithLocation{location, "'" + String{*allocation_name} +
                                          "' does not name an open descriptor"};
  }

  let const allocated_fd = os::allocate_free_shell_fd(10);
  if (allocated_fd < 0) {
    if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
    throw ErrorWithLocation{location, "Could not allocate a file descriptor"};
  }

  let const allocated_text = String::from(allocated_fd, heap_allocator());
  if (target->has_subscript) {
    cxt.assign_array_element(target->name, target->subscript,
                             allocated_text.view(),
                             assignment_update_mode::Replace);
  } else {
    cxt.set_shell_variable(target->name, allocated_text.view());
  }

  return allocated_fd;
}

static pure fn is_reprint_blank(char byte) wontthrow -> bool
{
  return byte == ' ' || byte == '\t';
}

static pure fn should_reprint_source(StringView source,
                                     bool are_bash_additions_enabled) wontthrow
    -> bool
{
  for (usize position = 0; position < source.length; position++) {
    let const byte = source[position];

    if (byte == '\t') return true;

    if (position == 0) continue;

    let const previous_byte = source[position - 1];

    if (byte == ' ' && previous_byte == ' ') {
      return true;
    }

    if (byte == '(' &&
        (previous_byte == '$' || previous_byte == '<' || previous_byte == '>'))
    {
      return true;
    }

    if (byte == '\'' && previous_byte == '$' && are_bash_additions_enabled) {
      return true;
    }

    if (byte == '\n' && previous_byte == '\\') {
      return true;
    }
  }

  return false;
}

static pure fn scan_ansi_c_quote_end(StringView source,
                                     usize body_start) wontthrow -> Maybe<usize>
{
  for (usize position = body_start; position < source.length; position++) {
    if (source[position] == '\'') return position + 1;

    if (source[position] == '\\') position++;
  }

  return None;
}

static fn append_reprinted_source(String &out, StringView source,
                                  bool should_collapse_blanks,
                                  bool should_space_operators,
                                  bool should_join_lines,
                                  bool are_bash_additions_enabled) throws
    -> void;

/* The byte just past the double quote that closes the one opened before
   `position`, or the end of the source when the quote is unterminated. */
static fn find_double_quote_end(StringView source, usize position) throws
    -> usize
{
  while (position < source.length) {
    let const byte = source[position];

    if (byte == '\\' && position + 1 < source.length) {
      position += 2;
      continue;
    }

    if (byte == '"') return position;

    if (byte == '$' && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      let const end =
          lexer::scan_balanced_shell_region(source, position + 2, ')');
      if (!end.has_value()) return source.length;

      position = *end;
      continue;
    }

    position++;
  }

  return source.length;
}

/* The body a command substitution or a subshell reprints, with the padding bash
   drops removed from both ends. */
static pure fn trimmed_reprint_body(StringView body) wontthrow -> StringView
{
  while (!body.is_empty() && (is_reprint_blank(body[0]) || body[0] == '\n'))
    body = body.substring(1);

  while (!body.is_empty() && (is_reprint_blank(body[body.length - 1]) ||
                              body[body.length - 1] == '\n'))
    body = body.substring_of_length(0, body.length - 1);

  return body;
}

/* The redirection a reprint spells. A duplication always prints its descriptor
   and binds its target with no blank between them. Every other form prints a
   descriptor only when it differs from the default the operator carries, and it
   separates its target with one blank. */
struct reprinted_redirection
{
  usize length;
  StringView spelling;
  i32 default_fd;
  i32 fd;
  bool is_duplication;
};

/* The redirection whose operator starts at `position`, or nothing when the
   bytes there spell none. A leading descriptor counts only where a token
   starts, since the digits of `a2>b` belong to the word before the operator. */
static pure fn match_reprinted_redirection(StringView source, usize position,
                                           bool is_token_start) wontthrow
    -> Maybe<reprinted_redirection>
{
  usize scan = position;
  i32 fd = -1;

  if (lexer::is_number(source[scan])) {
    if (!is_token_start) return {};

    i64 parsed_fd = 0;
    while (scan < source.length && lexer::is_number(source[scan])) {
      parsed_fd = (parsed_fd * 10) + (source[scan] - '0');

      if (parsed_fd > 0xffff) return {};

      scan++;
    }

    if (scan >= source.length) return {};

    if (source[scan] != '<' && source[scan] != '>') {
      return {};
    }

    fd = static_cast<i32>(parsed_fd);
  }

  let const byte = source[scan];
  let const next_byte = scan + 1 < source.length ? source[scan + 1] : '\0';
  let const third_byte = scan + 2 < source.length ? source[scan + 2] : '\0';

  reprinted_redirection redirection{};
  redirection.default_fd = -1;
  redirection.fd = fd;
  redirection.is_duplication = false;

  if (byte == '>') {
    redirection.default_fd = 1;

    if (next_byte == '&') {
      redirection.spelling = StringView{">&"};
      redirection.is_duplication = true;
    } else if (next_byte == '>') {
      redirection.spelling = StringView{">>"};
    } else if (next_byte == '|') {
      redirection.spelling = StringView{">|"};
    } else {
      redirection.spelling = StringView{">"};
    }
  } else if (byte == '<') {
    redirection.default_fd = 0;

    if (next_byte == '&') {
      redirection.spelling = StringView{"<&"};
      redirection.is_duplication = true;
    } else if (next_byte == '<' && third_byte == '<') {
      redirection.spelling = StringView{"<<<"};
    } else if (next_byte == '<' && third_byte == '-') {
      redirection.spelling = StringView{"<<-"};
    } else if (next_byte == '<') {
      redirection.spelling = StringView{"<<"};
    } else if (next_byte == '>') {
      redirection.spelling = StringView{"<>"};
    } else {
      redirection.spelling = StringView{"<"};
    }
  } else if (byte == '&' && next_byte == '>') {
    redirection.spelling =
        third_byte == '>' ? StringView{"&>>"} : StringView{"&>"};
  } else {
    return {};
  }

  redirection.length = (scan - position) + redirection.spelling.length;

  return redirection;
}

/* Whether any byte past `position` still spells a command. The answer decides
   whether a terminator that bash drops at the end is written. */
static pure fn has_reprint_remainder(StringView source,
                                     usize position) wontthrow -> bool
{
  while (position < source.length) {
    if (!is_reprint_blank(source[position]) && source[position] != '\n') {
      return true;
    }

    position++;
  }

  return false;
}

/* The subshell region the way bash reprints it, with one blank inside each
   parenthesis and the commands of a body written across several lines separated
   by a semicolon and a blank. The region spans the opening parenthesis through
   the closing one. */
static fn append_reprinted_subshell(String &out, StringView region,
                                    bool are_bash_additions_enabled) throws
    -> void
{
  let const body =
      trimmed_reprint_body(region.substring_of_length(1, region.length - 2));
  if (body.is_empty()) {
    out.append(region);

    return;
  }

  out.append("( ");
  append_reprinted_source(out, body, true, true, true,
                          are_bash_additions_enabled);
  out.append(" )");
}

static fn append_reprinted_source(String &out, StringView source,
                                  bool should_collapse_blanks,
                                  bool should_space_operators,
                                  bool should_join_lines,
                                  bool are_bash_additions_enabled) throws
    -> void
{
  let const start_length = out.length();
  usize position = 0;

  while (position < source.length) {
    let const byte = source[position];

    if (byte == '\\' && position + 1 < source.length) {
      if (source[position + 1] == '\n') {
        position += 2;
        continue;
      }

      out.push(byte);
      out.push(source[position + 1]);
      position += 2;
      continue;
    }

    if (should_collapse_blanks && is_reprint_blank(byte)) {
      while (position < source.length && is_reprint_blank(source[position]))
        position++;

      if (out.length() != 0 && out.back() != ' ') {
        out.push(' ');
      }

      continue;
    }

    if (byte == '\'' || byte == '`') {
      let const start = position++;
      while (position < source.length && source[position] != byte)
        position++;

      if (position < source.length) position++;
      out.append(source.substring_of_length(start, position - start));
      continue;
    }

    if (byte == '"') {
      let const end = find_double_quote_end(source, position + 1);
      out.push('"');
      append_reprinted_source(
          out, source.substring_of_length(position + 1, end - position - 1),
          false, false, false, are_bash_additions_enabled);

      if (end < source.length) {
        out.push('"');
        position = end + 1;
      } else {
        position = end;
      }

      continue;
    }

    if (byte == '$' && position + 1 < source.length) {
      let const next_byte = source[position + 1];

      if (next_byte == '\'' && should_collapse_blanks &&
          are_bash_additions_enabled)
      {
        let const end = scan_ansi_c_quote_end(source, position + 2);

        if (end.has_value()) {
          let decoded = String{heap_allocator()};
          utils::decode_ansi_c_escapes(
              decoded,
              source.substring_of_length(position + 2, *end - position - 3));
          append_shell_quoted_arg(out, decoded.view(), true);
          position = *end;
          continue;
        }
      }

      if (next_byte == '{' || next_byte == '(') {
        let const is_arithmetic = next_byte == '(' &&
                                  position + 2 < source.length &&
                                  source[position + 2] == '(';
        let const closing_byte = next_byte == '{' ? '}' : ')';
        let const end = lexer::scan_balanced_shell_region(source, position + 2,
                                                          closing_byte);
        if (!end.has_value()) {
          out.append(source.substring(position));

          return;
        }

        if (next_byte == '{' || is_arithmetic) {
          out.append(source.substring_of_length(position, *end - position));
          position = *end;
          continue;
        }

        let const body = trimmed_reprint_body(
            source.substring_of_length(position + 2, *end - position - 3));

        /* A body that opens a subshell would read as arithmetic when it follows
           the dollar sign directly. Bash writes a blank between them. */
        out.append(!body.is_empty() && body[0] == '(' ? "$( " : "$(");
        append_reprinted_source(out, body, true, true, false,
                                are_bash_additions_enabled);
        out.push(')');
        position = *end;
        continue;
      }
    }

    if (should_collapse_blanks && (byte == '<' || byte == '>') &&
        position + 1 < source.length && source[position + 1] == '(')
    {
      let const end =
          lexer::scan_balanced_shell_region(source, position + 2, ')');

      if (end.has_value()) {
        if (should_space_operators && out.length() > start_length &&
            out.back() != ' ')
        {
          out.push(' ');
        }

        out.push(byte);
        out.push('(');
        append_reprinted_source(out,
                                trimmed_reprint_body(source.substring_of_length(
                                    position + 2, *end - position - 3)),
                                true, true, true, are_bash_additions_enabled);
        out.push(')');
        position = *end;
        continue;
      }
    }

    if (should_space_operators) {
      let const is_token_start = out.length() == start_length ||
                                 out.back() == ' ' || out.back() == '\n';
      let const next_byte =
          position + 1 < source.length ? source[position + 1] : '\0';

      let do_trim_trailing_blank = [&] {
        while (out.length() > start_length && out.back() == ' ')
          out.pop_back();
      };

      let do_skip_following_blanks = [&] {
        while (position < source.length &&
               (is_reprint_blank(source[position]) || source[position] == '\n'))
        {
          position++;
        }
      };

      let do_append_operator = [&](StringView text, usize operator_length) {
        do_trim_trailing_blank();

        if (out.length() == start_length) text = text.substring(1);

        out.append(text);
        position += operator_length;
        do_skip_following_blanks();
      };

      if (byte == '#' && is_token_start) {
        while (position < source.length && source[position] != '\n')
          position++;

        do_trim_trailing_blank();
        continue;
      }

      /* A nested subshell carries the same layout as the one that holds it. A
         doubled parenthesis opens an arithmetic command. Bash keeps that
         command the way it is written. */
      if (byte == '(' && is_token_start && next_byte != '(') {
        let const end =
            lexer::scan_balanced_shell_region(source, position + 1, ')');

        if (end.has_value()) {
          append_reprinted_subshell(
              out, source.substring_of_length(position, *end - position),
              are_bash_additions_enabled);
          position = *end;
          continue;
        }
      }

      let const matched =
          match_reprinted_redirection(source, position, is_token_start);
      if (matched.has_value()) {
        let const redirection = *matched;
        i32 printed_fd = -1;

        if (out.length() > start_length && out.back() != ' ') {
          out.push(' ');
        }

        if (redirection.is_duplication) {
          printed_fd =
              redirection.fd != -1 ? redirection.fd : redirection.default_fd;
        } else if (redirection.fd != -1 &&
                   redirection.fd != redirection.default_fd)
        {
          printed_fd = redirection.fd;
        }

        if (printed_fd != -1) {
          out += String::from(static_cast<i64>(printed_fd), heap_allocator());
        }

        out.append(redirection.spelling);
        position += redirection.length;

        if (!redirection.is_duplication) {
          out.push(' ');
          do_skip_following_blanks();
        }

        continue;
      }

      if (byte == ';' && (next_byte == ';' || next_byte == '&')) {
        do_trim_trailing_blank();

        while (position < source.length &&
               (source[position] == ';' || source[position] == '&'))
        {
          out.push(source[position]);
          position++;
        }

        do_skip_following_blanks();

        if (position < source.length) out.push(' ');

        continue;
      }

      if (byte == ';' || (should_join_lines && byte == '\n')) {
        position++;
        do_trim_trailing_blank();

        if (!has_reprint_remainder(source, position)) {
          position = source.length;
          continue;
        }

        out.append("; ");
        do_skip_following_blanks();
        continue;
      }

      if (byte == '&' && next_byte == '&') {
        do_append_operator(StringView{" && "}, 2);
        continue;
      }

      if (byte == '|' && next_byte == '|') {
        do_append_operator(StringView{" || "}, 2);
        continue;
      }

      /* Bash keeps no merging pipe of its own and reprints the merge as the
         duplication it stands for. */
      if (byte == '|' && next_byte == '&') {
        do_append_operator(StringView{" 2>&1 | "}, 2);
        continue;
      }

      if (byte == '|') {
        do_append_operator(StringView{" | "}, 1);
        continue;
      }

      if (byte == '&') {
        position++;
        do_trim_trailing_blank();

        if (out.length() > start_length) out.push(' ');

        out.push('&');
        do_skip_following_blanks();

        if (position < source.length) out.push(' ');

        continue;
      }
    }

    out.push(byte);
    position++;
  }
}

fn internal::reprinted_command_text(StringView source,
                                    bool are_bash_additions_enabled) throws
    -> String
{
  let text = String{heap_allocator()};
  if (!should_reprint_source(source, are_bash_additions_enabled)) {
    text.append(source);

    return text;
  }

  text.reserve(source.length);
  append_reprinted_source(text, source, true, false, false,
                          are_bash_additions_enabled);

  return text;
}

fn internal::subshell_command_text(EvalContext &cxt,
                                   const SourceLocation &location,
                                   usize end_position) throws -> String
{
  let text = String{heap_allocator()};
  let const source = cxt.source_text_in_span(location, end_position);
  if (source.length == 0 || source[0] != '(') {
    return text;
  }

  let const body_end = lexer::scan_balanced_shell_region(source, 1, ')');
  if (!body_end.has_value()) return text;

  let const are_bash_additions_enabled = cxt.bash_additions_enabled();

  text.reserve(source.length + 2);
  append_reprinted_subshell(text, source.substring_of_length(0, *body_end),
                            are_bash_additions_enabled);

  if (*body_end < source.length) {
    append_reprinted_source(text, source.substring(*body_end), true, true,
                            false, are_bash_additions_enabled);
  }

  return text;
}

fn internal::append_word_source_text(EvalContext &cxt, String &out,
                                     const Token &word) throws -> void
{
  let const text = cxt.source_text_in_span(word.source_location(), 0);
  if (text.length != 0) {
    out += reprinted_command_text(text, cxt.bash_additions_enabled());

    return;
  }

  out += word.raw_string();
}

/* The descriptor the operator carries. A named allocation prints its name in
   braces, a descriptor equal to the form's own default prints nothing, and
   every other descriptor prints its number. */
static fn append_redirection_descriptor(String &out, const Redirection &redir,
                                        i32 default_fd) throws -> void
{
  if (redir.fd_allocation_name_token != nullptr) {
    let const name =
        static_cast<const tokens::WordToken *>(redir.fd_allocation_name_token)
            ->word()
            .fd_allocation_name();
    if (name.has_value()) {
      out.push('{');
      out.append(*name);
      out.push('}');
      return;
    }
  }

  if (redir.fd == default_fd) return;

  out += String::from(static_cast<i64>(redir.fd), heap_allocator());
}

/* The descriptor a duplication carries. Bash prints it even when it is the
   form's own default. */
static fn append_duplication_descriptor(String &out,
                                        const Redirection &redir) throws -> void
{
  append_redirection_descriptor(out, redir, -1);
}

/* The heredoc terminator. The terminator is the delimiter word without its
   quoting and without the dash that requested the tab stripping. */
static fn heredoc_terminator(const Redirection &redir) throws -> String
{
  ASSERT(redir.heredoc_delimiter != nullptr);
  let terminator =
      static_cast<const tokens::WordToken *>(redir.heredoc_delimiter)
          ->word()
          .to_literal_string();
  if (redir.should_strip_heredoc_tabs && !terminator.is_empty()) {
    return String{heap_allocator(), terminator.view().substring(1)};
  }

  return terminator;
}

static fn append_one_redirection(EvalContext &cxt, String &out,
                                 const Redirection &redir) throws -> void
{
  let const do_append_target = [&] throws {
    if (redir.target != nullptr)
      append_word_source_text(cxt, out, *redir.target);
  };

  switch (redir.kind) {
  case Redirection::Kind::TruncateOutput:
    if (redir.is_both_streams_spelling) {
      out += "&> ";
      do_append_target();
      return;
    }

    append_redirection_descriptor(out, redir, 1);
    out += "> ";
    do_append_target();
    return;

  case Redirection::Kind::TruncateOutputOverride:
    append_redirection_descriptor(out, redir, 1);
    out += ">| ";
    do_append_target();
    return;

  case Redirection::Kind::AppendOutput:
    if (redir.is_both_streams_spelling) {
      out += "&>> ";
      do_append_target();
      return;
    }

    append_redirection_descriptor(out, redir, 1);
    out += ">> ";
    do_append_target();
    return;

  case Redirection::Kind::ReadInput:
    append_redirection_descriptor(out, redir, 0);
    out += "< ";
    do_append_target();
    return;

  case Redirection::Kind::ReadWrite:
    append_redirection_descriptor(out, redir, 0);
    out += "<> ";
    do_append_target();
    return;

  case Redirection::Kind::HereString:
    append_redirection_descriptor(out, redir, 0);
    out += "<<< ";
    do_append_target();
    return;

  case Redirection::Kind::Heredoc:
    append_redirection_descriptor(out, redir, 0);
    out += "<<";
    if (redir.heredoc_delimiter != nullptr)
      append_word_source_text(cxt, out, *redir.heredoc_delimiter);
    return;

  case Redirection::Kind::DuplicateOutput:
  case Redirection::Kind::DuplicateInput: {
    /* A close prints as an output duplication in either direction, the way bash
       spells 0>&- for <&-. */
    if (redir.dup_fd == Redirection::DUP_FD_CLOSE) {
      append_duplication_descriptor(out, redir);
      out += ">&-";
      return;
    }

    let const is_output = redir.kind == Redirection::Kind::DuplicateOutput;
    if (redir.dup_fd >= 0) {
      append_duplication_descriptor(out, redir);
      out += is_output ? ">&" : "<&";
      out += String::from(static_cast<i64>(redir.dup_fd), heap_allocator());
      return;
    }

    append_redirection_descriptor(out, redir, is_output ? 1 : 0);
    out += is_output ? ">&" : "<&";
    do_append_target();
    return;
  }
  }
}

/* Whether the duplication is the one the parser adds behind &>file. Bash spells
   it as part of the operator and never prints it on its own. */
static pure fn
is_synthesized_error_duplication(const Redirection &redir) wontthrow -> bool
{
  return redir.kind == Redirection::Kind::DuplicateOutput && redir.fd == 2 &&
         redir.dup_fd == 1 && redir.target == nullptr &&
         redir.fd_allocation_name_token == nullptr;
}

fn internal::append_redirections_text(
    EvalContext &cxt, String &out,
    const SparseList<Redirection> &redirections) throws -> void
{
  bool should_drop_error_duplication = false;
  bool has_heredoc = false;
  for (let const &redir : redirections) {
    if (should_drop_error_duplication) {
      should_drop_error_duplication = false;
      if (is_synthesized_error_duplication(redir)) continue;
    }

    if (!out.is_empty()) out.push(' ');

    append_one_redirection(cxt, out, redir);
    should_drop_error_duplication = redir.is_both_streams_spelling;
    has_heredoc |= redir.kind == Redirection::Kind::Heredoc;
  }

  if (!has_heredoc) return;

  /* Each here-document follows the whole command, in the order the operators
     appear. */
  for (let const &redir : redirections) {
    if (redir.kind != Redirection::Kind::Heredoc) continue;

    out.push('\n');
    if (redir.heredoc != nullptr) out.append(redir.heredoc->text.view());
    out += heredoc_terminator(redir);
    out.push('\n');
  }
}

fn internal::publish_simple_command(EvalContext &cxt,
                                    const SimpleCommand &command,
                                    root_evaluation_mode mode) throws -> bool
{
  return publish_command_and_run_debug_trap(
      cxt,
      [&] throws {
        let location = command.source_location();
        let const start_position =
            static_cast<u32>(command.full_source_start_position());
        if (start_position < location.position) {
          location.length += location.position - start_position;
          location.position = start_position;
        }

        let text = source_command_text(
            cxt, location, command.assignments_source_end_position(),
            [&] { return utils::merge_tokens_to_string(command.args()); });
        append_redirections_text(cxt, text, command.redirections());
        return text;
      },
      mode);
}

struct stage_fd_allocation_restore
{
  fd_allocation_target target;
  String previous_value;
  bool was_set;
};

fn SimpleCommand::redirect_exec_context(ExecContext &ec,
                                        EvalContext &cxt) const throws -> void
{
  LOG(Debug, "applying %zu redirections to the pipeline stage",
      m_redirections.count());

  /* A binding opened here is owned by the list until the context adopts it. A
     later redirection that throws still releases what the earlier ones opened.
     A binding the loop redirection cache owns stays open. */
  ArrayList<nonstandard_descriptor> nonstandard{heap_allocator()};
  bool was_nonstandard_handed_off = false;
  defer
  {
    if (was_nonstandard_handed_off) return;

    for (let const &binding : nonstandard) {
      if (binding.file_fd != KOSH_INVALID_FD && !binding.is_file_borrowed) {
        os::close_fd(binding.file_fd);
      }
    }
  };

  ArrayList<stage_fd_allocation_restore> allocation_restores{heap_allocator()};
  defer
  {
    for (usize i = allocation_restores.count(); i > 0; i--) {
      let const &restore = allocation_restores[i - 1];
      try {
        if (restore.was_set) {
          if (restore.target.has_subscript) {
            cxt.assign_array_element(restore.target.name,
                                     restore.target.subscript,
                                     restore.previous_value.view(),
                                     assignment_update_mode::Replace);
          } else {
            cxt.set_shell_variable(restore.target.name,
                                   restore.previous_value.view());
          }
        } else if (restore.target.has_subscript) {
          cxt.unset_array_element(restore.target.name,
                                  restore.target.subscript);
        } else {
          cxt.unset_shell_variable(restore.target.name);
        }
      } catch (...) {}
    }
  };

  for (let const &redir : m_redirections) {
    let const r = resolve_redirection(redir, cxt, source_location(),
                                      /*open_or_stage_failed=*/nullptr,
                                      /*should_allow_fd_memoization=*/true);

    let previous = Maybe<String>{};
    let const *known_previous = static_cast<const Maybe<String> *>(nullptr);

    if (redir.fd_allocation_name_token != nullptr) {
      let const &allocation_word =
          static_cast<const tokens::WordToken *>(redir.fd_allocation_name_token)
              ->word();
      let const allocation_target = allocation_word.get_fd_allocation_target();
      ASSERT(allocation_target.has_value());

      previous = read_fd_allocation_value(cxt, *allocation_target);
      known_previous = &previous;
      let record = stage_fd_allocation_restore{*allocation_target,
                                               previous.has_value()
                                                   ? String{previous->view()}
                                                   : String{heap_allocator()},
                                               previous.has_value()};
      allocation_restores.push(steal(record));
    }

    let const target_fd = allocate_redirection_descriptor(
        redir, r, cxt, source_location(), nullptr, known_previous);

    switch (r.kind) {
    case redirection_outcome::Heredoc:
      assign_redirected_fd(ec, nonstandard, target_fd, r.opened_fd,
                           r.is_cached);
      break;
    case redirection_outcome::BothStreams:
      assign_redirected_fd(ec, nonstandard, 1, r.opened_fd, r.is_cached);
      ec.should_duplicate_error_to_output = true;
      ec.was_output_to_error_last = false;
      ec.did_output_file_follow_error_dup = false;
      break;
    case redirection_outcome::OpenedFile:
      /* A file already in the slot is what the pending dup read. It moves into
         the other slot and stays open while this file takes its place. An empty
         slot means the dup read the stream the stage inherits. The ordering
         mark carries that stream to the routing. */
      if (target_fd == 1 && ec.should_duplicate_error_to_output) {
        if (ec.out_fd) {
          if (ec.err_fd && !ec.is_err_fd_borrowed) os::close_fd(*ec.err_fd);

          ec.err_fd = ec.out_fd;
          ec.is_err_fd_borrowed = ec.is_out_fd_borrowed;
          ec.out_fd = {};
          ec.is_out_fd_borrowed = false;
          ec.should_duplicate_error_to_output = false;
        } else {
          ec.did_output_file_follow_error_dup = true;
        }
      }
      if (target_fd == 2 && ec.should_duplicate_output_to_error) {
        if (ec.err_fd) {
          if (ec.out_fd && !ec.is_out_fd_borrowed) os::close_fd(*ec.out_fd);

          ec.out_fd = ec.err_fd;
          ec.is_out_fd_borrowed = ec.is_err_fd_borrowed;
          ec.err_fd = {};
          ec.is_err_fd_borrowed = false;
          ec.should_duplicate_output_to_error = false;
        } else {
          ec.did_error_file_follow_output_dup = true;
        }
      }

      assign_redirected_fd(ec, nonstandard, target_fd, r.opened_fd,
                           r.is_cached);
      break;
    case redirection_outcome::Duplicate:
      if (r.dup_from_fd == target_fd) break;

      if (target_fd == 2 && r.dup_from_fd == 1) {
        ec.should_duplicate_error_to_output = true;
        ec.was_output_to_error_last = false;
        ec.did_output_file_follow_error_dup = false;
      } else if (target_fd == 1 && r.dup_from_fd == 2) {
        ec.should_duplicate_output_to_error = true;
        ec.was_output_to_error_last = true;
        ec.did_error_file_follow_output_dup = false;
      } else if (target_fd > 2) {
        /* A close leaves no source, and a duplication names the descriptor the
           stage carries once the three standard slots are placed. */
        let const dup_from_fd =
            r.dup_from_fd == Redirection::DUP_FD_CLOSE ? -1 : r.dup_from_fd;
        bind_nonstandard_fd(
            nonstandard,
            nonstandard_descriptor{KOSH_INVALID_FD, target_fd, dup_from_fd});
      } else if (r.dup_from_fd == Redirection::DUP_FD_CLOSE) {
        /* One of the three standard descriptors closes after the routing places
           it. The close joins the list that runs last. */
        bind_nonstandard_fd(nonstandard, nonstandard_descriptor{KOSH_INVALID_FD,
                                                                target_fd, -1});
      } else {
        /* The source is a descriptor the shell holds and the stage never
           carries. The slot receives an independent copy of the same open file.
           The context owns that copy and releases it with the rest. */
        let const copied = os::duplicate_shell_fd(r.dup_from_fd);
        if (copied == KOSH_INVALID_FD) {
          let const location = redir.target != nullptr
                                   ? redir.target->source_location()
                                   : source_location();
          throw ErrorWithLocation{
              location, String::from(r.dup_from_fd, heap_allocator()) +
                            ": Bad file descriptor"};
        }

        assign_redirected_fd(ec, nonstandard, target_fd, copied);
      }
      break;
    }
  }

  ec.nonstandard_fds.fill(steal(nonstandard));
  was_nonstandard_handed_off = true;
}

fn SimpleCommand::is_simple_command() const wontthrow -> bool { return true; }

pure fn SimpleCommand::args() const wontthrow
    -> const ArrayList<const Token *> &
{
  return m_args;
}

pure fn SimpleCommand::redirections() const wontthrow
    -> const SparseList<Redirection> &
{
  return m_redirections;
}

fn SimpleCommand::as_simple_command() const wontthrow -> const SimpleCommand *
{
  return this;
}

cold fn SimpleCommand::to_string() const throws -> String
{
  String s = "SimpleCommand";

  if (!m_args.is_empty()) {
    s += " \"" + m_args[0]->raw_string() + "\"";
    for (usize i = 1; i < m_args.count(); i++) {
      s += " \"";
      s += m_args[i]->raw_string();
      s += "\"";
    }
  }
  append_ast_execution_flags(s);

  return s;
}

} /* namespace expressions */

} /* namespace koshka */
