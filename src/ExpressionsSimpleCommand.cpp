#include "Arena.hpp"
#include "Builtin.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

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

  if (actx.is_posix_sh_shebang && m_assignment->is_append()) {
    actx.report_diagnostic(diagnostic_id::sc3024, source_location(),
                           {m_assignment->key().view()});
  }

  let const shape = scan_assignment_value(actx, m_assignment->value_word(),
                                          source_location());

  let const raw_assignment = m_assignment->raw_string();

  check_assignment_value_shape(
      actx, assignment_lint_input{m_assignment->key().view(),
                                  raw_assignment.view(), source_location(),
                                  m_assignment->is_append(), false, shape});
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
                                         m_assignment->is_append());
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
      m_assignment->is_append());
  actx.note_variable_assignment(name.view(), source_location(),
                                is_unconditional &&
                                    !actx.has_seen_runtime_definer);
  /* The record is taken before the constant table gives up on this name. A
     conditional or appending assignment stays answerable. */
  actx.note_variable_assignment_record(
      name.view(), &m_assignment->value_word(), source_location(),
      !is_unconditional || actx.has_seen_runtime_definer,
      m_assignment->is_append());

  if (actx.function_scope_depth > 0 && !m_assignment->is_append() &&
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
      m_assignment->is_append())
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

  /* A command substitution in the value leaves the status of the last one, so
     the reset to 0 waits until after the expansion and a $? in the value reads
     the prior command's status. */
  let const value_ran_substitution =
      m_assignment->value_word().runs_substitution();

  try {
    let value = cxt.expand_word_for_assignment(m_assignment->value_word());

    let const key_view = m_assignment->key().view();
    if (let const bracket = key_view.find_character('[');
        bracket.has_value() && key_view[key_view.length - 1] == ']')
    {
      let const array_name = key_view.substring_of_length(0, *bracket);
      let const subscript = key_view.substring_of_length(
          *bracket + 1, key_view.length - *bracket - 2);
      cxt.assign_array_element(array_name, subscript, value.view(),
                               m_assignment->is_append());
      if (!value_ran_substitution) cxt.set_last_exit_status(0);
      cxt.publish_single_pipe_status(cxt.last_exit_status());
      return cxt.last_exit_status();
    }

    /* NAME+=VALUE prepends the current value of NAME, empty when unset. An
       integer name adds rather than concatenates. */
    if (m_assignment->is_append()) {
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
  return "Assign " + m_assignment->to_ast_string();
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
  }
}

SimpleCommand::~SimpleCommand() = default;

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

fn SimpleCommand::set_array_args(
    ArrayList<array_builtin_assignment> &&array_args) throws -> void
{
  m_array_args.fill(steal(array_args));
}

namespace {

using expressions::Redirection;

/* Route an opened descriptor into one of the three standard slots a stage
   carries, fd 0 to input, 2 to error, any other to output. The last
   redirection of a descriptor wins, so a descriptor in the slot closes first.
 */
fn assign_standard_fd(Maybe<os::descriptor> &in_fd,
                      Maybe<os::descriptor> &out_fd,
                      Maybe<os::descriptor> &err_fd, i32 fd,
                      os::descriptor file_fd) throws -> void
{
  if (fd == 0) {
    if (in_fd) os::close_fd(*in_fd);
    in_fd = file_fd;
  } else if (fd == 2) {
    if (err_fd) os::close_fd(*err_fd);
    err_fd = file_fd;
  } else {
    if (out_fd) os::close_fd(*out_fd);
    out_fd = file_fd;
  }
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

fn resolve_redirection(const Redirection &redir, EvalContext &cxt,
                       const SourceLocation &fallback_location,
                       bool *open_or_stage_failed,
                       bool allow_fd_memoization) throws -> resolved_redirection
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
        throw ErrorWithLocation{redir.target->source_location(),
                                "Could not open '" +
                                    *resolved_dup.both_streams_file +
                                    "': " + os::last_system_error_message()};
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
      cxt.process_args(target_tokens, argument_lifetime::Transient);
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
    throw ErrorWithLocation{redir.target->source_location(),
                            "Could not open '" + target_path +
                                "': " + os::last_system_error_message()};
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

fn allocate_redirection_descriptor(const Redirection &redir,
                                   const resolved_redirection &resolved,
                                   EvalContext &cxt,
                                   const SourceLocation &location,
                                   bool *open_or_stage_failed) throws -> i32
{
  if (redir.fd_allocation_name_token == nullptr) return redir.fd;

  let const allocation_name =
      static_cast<const tokens::WordToken *>(redir.fd_allocation_name_token)
          ->word()
          .fd_allocation_name();
  ASSERT(allocation_name.has_value());

  if (resolved.kind == redirection_outcome::Duplicate &&
      resolved.dup_from_fd == Redirection::DUP_FD_CLOSE)
  {
    let const current_value = cxt.get_variable_value(*allocation_name);
    if (current_value.has_value()) {
      let const parsed = current_value->view().to<i64>();
      if (!parsed.is_error() && parsed.value() >= 0)
        return static_cast<i32>(parsed.value());
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

  cxt.set_shell_variable(*allocation_name,
                         String::from(allocated_fd, heap_allocator()));
  return allocated_fd;
}

fn SimpleCommand::redirect_exec_context(ExecContext &ec,
                                        EvalContext &cxt) const throws -> void
{
  LOG(Debug, "applying %zu redirections to the pipeline stage",
      m_redirections.count());
  for (let const &redir : m_redirections) {
    let const r = resolve_redirection(redir, cxt, source_location());
    switch (r.kind) {
    case redirection_outcome::Heredoc:
      if (ec.in_fd) os::close_fd(*ec.in_fd);
      ec.in_fd = r.opened_fd;
      break;
    case redirection_outcome::BothStreams:
      assign_standard_fd(ec.in_fd, ec.out_fd, ec.err_fd, 1, r.opened_fd);
      ec.should_duplicate_error_to_output = true;
      ec.was_output_to_error_last = false;
      break;
    case redirection_outcome::OpenedFile:
      assign_standard_fd(ec.in_fd, ec.out_fd, ec.err_fd, r.target_fd,
                         r.opened_fd);
      break;
    case redirection_outcome::Duplicate:
      if (r.dup_from_fd == r.target_fd) break;

      if (r.target_fd == 2 && r.dup_from_fd == 1) {
        ec.should_duplicate_error_to_output = true;
        ec.was_output_to_error_last = false;
      } else if (r.target_fd == 1 && r.dup_from_fd == 2) {
        ec.should_duplicate_output_to_error = true;
        ec.was_output_to_error_last = true;
      }
      break;
    }
  }
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
  if (is_async()) s += ", Async";

  return s;
}

} /* namespace expressions */

} /* namespace koshka */
