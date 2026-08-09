#include "Arena.hpp"
#include "Builtin.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Shitbox.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace shit {

namespace expressions {

static pure fn is_process_dynamic_name(StringView text,
                                       usize position) wontthrow -> bool
{
  static const StringView PROCESS_DYNAMIC_NAMES[] = {
      "BASHPID",
      "PPID",
      "RANDOM",
      "SRANDOM",
  };
  for (let const name : PROCESS_DYNAMIC_NAMES) {
    if (position + name.length > text.length ||
        text.substring_of_length(position, name.length) != name)
    {
      continue;
    }

    const usize end_position = position + name.length;
    const bool has_name_before =
        position > 0 && lexer::is_variable_name(text[position - 1]);
    const bool has_name_after = end_position < text.length &&
                                lexer::is_variable_name(text[end_position]);
    if (!has_name_before && !has_name_after) return true;
  }

  return false;
}

static fn word_is_safe_for_in_process_substitution(const Word &word) wontthrow
    -> bool
{
  for (let const &segment : word.segments) {
    if (segment.kind == WordSegment::Kind::ProcessSubstitution ||
        segment.kind == WordSegment::Kind::FunctionSubstitution)
    {
      return false;
    }

    if (segment.kind == WordSegment::Kind::VariableReference) {
      if (!segment.text.is_empty() && segment.text[0] == '!') return false;
      if (is_process_dynamic_name(segment.text.view(), 0)) return false;
    }

    if (segment.kind == WordSegment::Kind::ArithmeticExpansion)
      for (usize position = 0; position < segment.text.count(); position++)
        if (is_process_dynamic_name(segment.text.view(), position))
          return false;
  }

  return true;
}

AssignCommand::AssignCommand(SourceLocation location, const Assignment *a)
    : Command(location), m_assignment(a)
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

cold fn AssignCommand::analyze(AnalysisContext &actx,
                               bool is_unconditional) const throws -> void
{
  ASSERT(m_assignment != nullptr);

  for (let const &segment : m_assignment->value_word().segments) {
    if (segment.kind == WordSegment::Kind::VariableReference &&
        segment.text.view() == "@")
    {
      actx.fail_shellcheck(
          2124, source_location(),
          "A scalar assignment from $@ loses argument boundaries",
          "Assign one value or use an array", analyze_severity::Annoying);
      break;
    }
    if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
        segment.text.view().find_character('$').has_value())
    {
      actx.warn_shellcheck(2004, source_location(),
                           "Arithmetic variables do not need a dollar sign",
                           "Use the variable name directly inside arithmetic");
    }
  }

  let const raw_assignment = m_assignment->raw_string();
  let const first_colon = raw_assignment.view().find_character(':');
  if (m_assignment->key().view() == "PATH" &&
      (raw_assignment.view().starts_with(StringView{"PATH=~/"}) ||
       raw_assignment.view().starts_with(StringView{"PATH+=~/"}) ||
       (first_colon.has_value() && raw_assignment.view()
                                       .substring(*first_colon + 1)
                                       .starts_with(StringView{"~/"}))))
  {
    actx.fail_shellcheck(
        2147, source_location(), "A tilde inside PATH remains literal",
        "Expand HOME before assigning PATH", analyze_severity::Annoying);
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
    actx.warn_shellcheck(2025, source_location(),
                         "PS1 control escapes need balanced display guards",
                         "Wrap nonprinting prompt escapes in \\[ and \\]");
  }

  if (!m_assignment->is_append()) {
    const WordSegment *arithmetic_segment = nullptr;
    usize arithmetic_segment_count = 0;
    let has_other_segment = false;
    for (let const &segment : m_assignment->value_word().segments) {
      if (segment.kind == WordSegment::Kind::ArithmeticExpansion) {
        arithmetic_segment = &segment;
        arithmetic_segment_count++;
      } else if (!segment.text.is_empty()) {
        has_other_segment = true;
      }
    }

    if (arithmetic_segment_count == 1 && !has_other_segment) {
      let expression = arithmetic_segment->text.view();
      while (!expression.is_empty() &&
             (expression[0] == ' ' || expression[0] == '\t'))
        expression = expression.substring(1);
      while (!expression.is_empty() &&
             (expression[expression.length - 1] == ' ' ||
              expression[expression.length - 1] == '\t'))
        expression = expression.substring_of_length(0, expression.length - 1);

      if (!expression.is_empty())
        actx.warn(source_location(),
                  StringView{"The assignment of '"} + m_assignment->key() +
                      "' wraps an arithmetic expansion",
                  StringView{"Use `let "} + m_assignment->key() + "='" +
                      expression + "'`");
    }
  }

  /* The fold reads the constant table, so it runs before the table records this
     assignment. */
  optimizer::optimize_node(this, actx);

  let const &name = m_assignment->key();

  if (actx.is_direct_pipeline_stage) {
    actx.warn_shellcheck(
        2030, source_location(),
        "This pipeline assignment is lost when the stage exits",
        "Move the assignment outside the pipeline");
    actx.pipeline_lost_names.add(name.view());
  }

  /* A PATH assignment leaves the runtime search path unknown to the prepass, so
     a later command's not-found check stays quiet. */
  if (name.view() == "PATH") actx.should_silence_unresolved_commands = true;

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
        actx.warn(source_location(),
                  "An array subscript from external input is evaluated as "
                  "arithmetic code",
                  "Validate the subscript as decimal digits before using it");
    }
    actx.note_variable_assignment(base);
    LOG(All,
        "forgetting the constant for the array base '%.*s' after an element "
        "assignment",
        static_cast<int>(*bracket), name.view().data);
    actx.constant_variables.erase(base);
    return;
  }

  actx.note_variable_assignment(name.view());

  /* The shellcheck SC2030-style warning for a scalar assignment inside a
     function body with no prior local, which leaks the value to the global
     scope. */
  if (actx.function_scope_depth > 0 && !m_assignment->is_append() &&
      !actx.function_local_names.contains(name.view()) &&
      !actx.global_assigned_names.contains(name.view()) &&
      !(actx.eval_context != nullptr &&
        actx.eval_context->get_variable_value(name.view()).has_value()))
  {
    actx.warn(source_location(),
              StringView{"This assignment to '"} + name +
                  "' in a function has no local, so the value leaks to the "
                  "global scope",
              "Declare it with local to keep it inside the function");
  }

  if (actx.function_scope_depth == 0 && is_unconditional &&
      !actx.has_seen_runtime_definer)
  {
    actx.global_assigned_names.add(name.view());
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
    actx.optimizer_recorded_constants++;
    if (actx.should_trace_optimizer)
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

    const StringView key_view = m_assignment->key().view();
    if (let const bracket = key_view.find_character('[');
        bracket.has_value() && key_view[key_view.length - 1] == ']')
    {
      const StringView array_name = key_view.substring_of_length(0, *bracket);
      const StringView subscript = key_view.substring_of_length(
          *bracket + 1, key_view.length - *bracket - 2);
      cxt.assign_array_element(array_name, subscript, value.view(),
                               m_assignment->is_append());
      if (!value_ran_substitution) cxt.set_last_exit_status(0);
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
  unused(cxt);
  unused(active_functions);
  return !is_async() && !is_timed() &&
         word_is_safe_for_in_process_substitution(m_assignment->value_word());
}

SimpleCommand::SimpleCommand(SourceLocation location,
                             ArrayList<const Token *> &&args)
    : Command(location), m_args(steal(args))
{

  /* The location spans from the first word to the end of the last, so a caret
     covers the whole command and not only the command word. */
  if (!m_args.is_empty()) {
    const SourceLocation first = m_args[0]->source_location();
    const SourceLocation last = m_args.back()->source_location();
    m_location.position = first.position;
    m_location.length = last.position + last.length - first.position;
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
    if (!word_is_safe_for_in_process_substitution(word)) return false;
  }

  let const command_name = static_command_name(m_args[0]);
  if (!command_name.has_value()) return false;

  if (let const function_body = cxt.find_function(command_name->view());
      function_body != nullptr)
  {
    if (active_functions.contains(command_name->view())) return true;

    active_functions.add(command_name->view());
    defer { active_functions.remove(command_name->view()); };
    return function_body->can_evaluate_in_process_substitution(
        cxt, active_functions);
  }

  constexpr PackedStringKey SAFE_BUILTIN_KEYS[] = {
      SSK(":"),     SSK("echo"),   SSK("false"),  SSK("let"),
      SSK("local"), SSK("printf"), SSK("return"), SSK("true"),
  };
  constexpr StaticStringSet SAFE_BUILTINS{SAFE_BUILTIN_KEYS};
  return SAFE_BUILTINS.contains(command_name->view());
}

fn SimpleCommand::set_redirections(ArrayList<Redirection> &&redirections) throws
    -> void
{
  m_redirections = steal(redirections);
}

fn SimpleCommand::set_array_args(
    ArrayList<array_builtin_assignment> &&array_args) throws -> void
{
  m_array_args = steal(array_args);
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
    return resolved_duplication{redir.dup_fd, shit::None};

  ArrayList<const Token *> target_tokens{cxt.scratch_allocator()};
  target_tokens.push(redir.target);
  ArrayList<String> fields = cxt.process_args(target_tokens);
  if (fields.count() != 1) {
    throw ErrorWithLocation{redir.target->source_location(),
                            "Duplication target is not a single descriptor"};
  }

  String &field = fields[0];
  if (field == "-")
    return resolved_duplication{Redirection::DUP_FD_CLOSE, shit::None};

  let const parsed_descriptor = field.view().to<i64>();
  if (parsed_descriptor.is_error() || parsed_descriptor.value() < 0) {
    if (redir.can_dup_be_filename)
      return resolved_duplication{-1, steal(field)};
    throw ErrorWithLocation{redir.target->source_location(),
                            "'" + field + "' is not a valid descriptor"};
  }
  return resolved_duplication{static_cast<i32>(parsed_descriptor.value()),
                              shit::None};
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
  default: return os::file_open_mode::Read;
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
                                     SourceLocation fallback_location,
                                     bool *open_or_stage_failed) throws -> void
{
  if (open_or_stage_failed != nullptr) *open_or_stage_failed = true;
  throw ErrorWithLocation{
      redir.target != nullptr ? redir.target->source_location()
                              : fallback_location,
      "Output redirection is forbidden in a restricted shell"};
}

fn resolve_redirection(const Redirection &redir, EvalContext &cxt,
                       SourceLocation fallback_location,
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
                             fallback_location.filename};
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
                                   EvalContext &cxt, SourceLocation location,
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

  const i32 allocated_fd = os::allocate_free_shell_fd(10);
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
      ec.dup_err_to_out = true;
      ec.dup_out_to_err_came_last = false;
      break;
    case redirection_outcome::OpenedFile:
      assign_standard_fd(ec.in_fd, ec.out_fd, ec.err_fd, r.target_fd,
                         r.opened_fd);
      break;
    case redirection_outcome::Duplicate:
      /* An arbitrary descriptor or the close form is not one of the stage's
         three slots and is left to the compound path. */
      if (r.dup_from_fd == r.target_fd) {
      } else if (r.target_fd == 2 && r.dup_from_fd == 1) {
        ec.dup_err_to_out = true;
        ec.dup_out_to_err_came_last = false;
      } else if (r.target_fd == 1 && r.dup_from_fd == 2) {
        ec.dup_out_to_err = true;
        ec.dup_out_to_err_came_last = true;
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
    -> const ArrayList<Redirection> &
{
  return m_redirections;
}

fn SimpleCommand::as_simple_command() const wontthrow -> const SimpleCommand *
{
  return this;
}

namespace {

/* Replace a command word that names an alias with the alias body. The body is
   split on whitespace, and a name already expanded is not expanded again so a
   self-referential alias terminates. A quoted space inside the body is not
   preserved, since the full tokenizer is not re-run. */
fn expand_command_aliases(EvalContext &cxt, ArrayList<String> &args,
                          ArrayList<SourceLocation> &arg_locations) throws
    -> void
{
  if (!cxt.has_aliases()) return;

  HashSet already_expanded{heap_allocator()};

  while (!args.is_empty()) {
    let const &word = args[0];

    if (already_expanded.contains(word.view())) break;

    let const body = cxt.get_alias(word);
    if (!body.has_value()) break;
    already_expanded.add(word.view());
    LOG(Debug, "expanding the alias '%s'", word.c_str());

    let rebuilt = ArrayList<String>{heap_allocator()};
    let rebuilt_locations = ArrayList<SourceLocation>{heap_allocator()};
    let current = String{cxt.scratch_allocator()};
    let const &body_value = *body;
    /* An alias body word has no span in this command, so it inherits the
       command word's location. */
    let const body_location =
        !arg_locations.is_empty() ? arg_locations[0] : SourceLocation{};
    for (usize i = 0; i < body_value.count(); i++) {
      const char c = body_value[i];
      if (c == ' ' || c == '\t') {
        if (!current.is_empty()) {
          rebuilt.push(String{
              heap_allocator(), StringView{current.data(), current.count()}
          });
          rebuilt_locations.push(body_location);
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.is_empty()) {
      rebuilt.push(String{
          heap_allocator(), StringView{current.data(), current.count()}
      });
      rebuilt_locations.push(body_location);
    }

    for (usize i = 1; i < args.count(); i++) {
      rebuilt.push(steal(args[i]));
      if (i < arg_locations.count())
        rebuilt_locations.push(arg_locations[i]);
      else
        rebuilt_locations.push(body_location);
    }

    args = steal(rebuilt);
    arg_locations = steal(rebuilt_locations);
  }
}

/* Whether the command word is itself a glob pattern. The lone [ that opens a
   test command carries no closing ] in the same word and is left alone. */
static fn command_word_is_glob(const Word &word) wontthrow -> bool
{
  bool has_open_bracket = false;
  for (const WordSegment &segment : word.segments) {
    if (segment.kind != WordSegment::Kind::UnquotedText) continue;
    for (usize i = 0; i < segment.text.count(); i++) {
      let const c = segment.text[i];
      if (c == '*' || c == '?') return true;
      if (c == '[') has_open_bracket = true;
      if (c == ']' && has_open_bracket) return true;
    }
  }
  return false;
}

} /* namespace */

hot fn SimpleCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  /* A command may have no words when it is only a redirection or only
     assignments, so those still run below. */
  ASSERT(m_args.count() > 0 || !m_redirections.is_empty() ||
         m_local_vars.count() > 0 || !m_array_args.is_empty());

  cxt.set_current_location(source_location());

  if (cxt.bash_dynamic_variables_enabled())
    cxt.set_current_command(utils::merge_tokens_to_string(m_args));

  if (cxt.has_debug_trap() && !cxt.is_posix_mode())
    cxt.run_named_trap(StringView{"DEBUG", 5});

  /* The check reads the typed command word before its expansion, so a pattern
     that happens to match a single file is still caught. */
  if (!m_args.is_empty() && m_args[0]->kind() == Token::Kind::Word) {
    const Word &command_word =
        static_cast<const tokens::WordToken *>(m_args[0])->word();
    if (!m_command_word_is_glob.has_value())
      m_command_word_is_glob = command_word_is_glob(command_word);

    if (*m_command_word_is_glob) {
      let const location = m_args[0]->source_location();
      let const message =
          StringView{"A glob pattern in command position is rarely intended as "
                     "a command name"};
      let const note =
          StringView{"Quote it to run a literal name, or list the matches with "
                     "compgen -G"};
      if (cxt.mood() == mimic_mood::Default)
        throw ErrorWithLocationAndDetails{location, message, note};
      cxt.show_runtime_warning_at(location, message, note);
    }
  }

  if (cxt.should_echo()) {
    shit::print(utils::merge_tokens_to_string(m_args) + "\n");
    shit::flush();
  }

  let const args_mark = cxt.scratch_mark();
  defer { cxt.scratch_release(args_mark); };
  /* The mark is taken before the expansion so this command reaps only the
     process substitution it opens, leaving an enclosing command's for that
     command to reap. */
  let const substitution_mark = cxt.mark_process_substitutions();
  let program_arg_locations =
      ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let program_args =
      cxt.process_args(m_args, argument_lifetime::Transient,
                       argument_context::Command, &program_arg_locations);
  defer { cxt.cleanup_process_substitutions(substitution_mark); };
  expand_command_aliases(cxt, program_args, program_arg_locations);

  if (!program_args.is_empty())
    cxt.guard_restricted_path(program_args[0].view(),
                              program_arg_locations.is_empty()
                                  ? source_location()
                                  : program_arg_locations[0],
                              restricted_path_use::Command);

  LOG(Info, "dispatching the command '%s' with %zu words",
      program_args.is_empty() ? "" : program_args[0].c_str(),
      program_args.count());

  /* A bare exec, exec with no further argument, applies its redirections to the
     shell's own descriptors for good. A function named exec shadows it. */
  const Expression *command_word_function =
      (!program_args.is_empty() && cxt.has_functions())
          ? cxt.find_function(program_args[0])
          : nullptr;

  const bool is_bare_exec = program_args.count() == 1 &&
                            program_args[0] == "exec" &&
                            command_word_function == nullptr;

  if (is_bare_exec) {
    for (let const &redir : m_redirections) {
      if (redir.fd_allocation_name_token == nullptr)
        cxt.snapshot_subshell_descriptor(redir.fd);
      if (redir.can_dup_be_filename) cxt.snapshot_subshell_descriptor(2);
    }
  }

  /* A POSIX special builtin not shadowed by a function exits the shell on a
     redirection error and keeps a prefix assignment, so it is computed once and
     read on both paths. */
  const bool command_is_special_builtin =
      !program_args.is_empty() && command_word_function == nullptr &&
      is_special_builtin_name(program_args[0].view());

  /* A heredoc on the standard input passes its staged descriptor through this
     slot, and the guard closes it on any path that does not hand it off. */
  Maybe<os::descriptor> redirect_in_fd;
  bool was_redirect_in_fd_handed_off = false;
  /* The standard fds are routed in source order so a later 2>&1 copies the
     descriptor its source points at now rather than the one a deferred slot
     would place last. */
  ArrayList<os::saved_descriptor> dup_saved_descriptors{
      cxt.scratch_allocator()};
  defer
  {
    for (usize i = dup_saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(dup_saved_descriptors[i - 1]);
  };
  defer
  {
    if (!was_redirect_in_fd_handed_off && redirect_in_fd) {
      os::close_fd(*redirect_in_fd);
    }
  };

  /* Set true just before a redirection resource failure throws, so the catch
     tells it apart from a fatal expansion error in a target word. */
  bool did_redirection_open_fail = false;
  try {
    for (let const &original_redir : m_redirections) {
      let redir = original_redir;
      let const r = resolve_redirection(redir, cxt, source_location(),
                                        &did_redirection_open_fail,
                                        /*allow_fd_memoization=*/!is_bare_exec);

      redir.fd = allocate_redirection_descriptor(original_redir, r, cxt,
                                                 source_location(),
                                                 &did_redirection_open_fail);

      switch (r.kind) {
      case redirection_outcome::Heredoc: {
        const os::descriptor body_fd = r.opened_fd;
        /* Inside an in-process subshell the move is backed up first, so it
           stays contained the way a fork would contain it. */
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          shit::flush();
          os::replace_descriptor(redir.fd, body_fd);
          if (!os::descriptor_is_shell_fd(body_fd, redir.fd))
            os::close_fd(body_fd);
          break;
        }

        /* A numbered heredoc such as 3<<EOF targets descriptor N, staged onto
           the real shell fd N around the command and restored afterward. */
        if (redir.fd == 0) {
          if (redirect_in_fd) os::close_fd(*redirect_in_fd);
          redirect_in_fd = body_fd;
          break;
        }

        /* The temp file already lands on fd N when mkstemp handed back that
           number, so the collision is handled directly and the restore closes
           fd N, which was free before mkstemp claimed it. */
        const bool body_is_target_fd =
            os::descriptor_is_shell_fd(body_fd, redir.fd);
        if (body_is_target_fd) {
          dup_saved_descriptors.push(
              os::saved_descriptor{.shell_fd = redir.fd, .was_open = false});
          break;
        }
        dup_saved_descriptors.push(
            os::save_and_replace_descriptor(redir.fd, body_fd));
        os::close_fd(body_fd);
        break;
      }

      case redirection_outcome::BothStreams: {
        /* The filename lands on the standard output and the standard error
           follows it, the pair bash builds for csh >&file. */
        const os::descriptor file_fd = r.opened_fd;
        shit::flush();
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(1);
          cxt.snapshot_subshell_descriptor(2);
          const bool did_replace_out = os::replace_descriptor(1, file_fd);
          const bool did_replace_err = os::replace_descriptor(2, file_fd);
          os::close_fd(file_fd);
          if (!did_replace_out || !did_replace_err) {
            did_redirection_open_fail = true;
            throw ErrorWithLocation{redir.target->source_location(),
                                    "Bad file descriptor"};
          }
          break;
        }
        const os::saved_descriptor saved_out =
            os::save_and_replace_descriptor(1, file_fd);
        dup_saved_descriptors.push(saved_out);
        const os::saved_descriptor saved_err =
            os::save_and_replace_descriptor(2, file_fd);
        dup_saved_descriptors.push(saved_err);
        os::close_fd(file_fd);
        if (!saved_out.is_dup2_ok || !saved_err.is_dup2_ok) {
          did_redirection_open_fail = true;
          throw ErrorWithLocation{redir.target->source_location(),
                                  "Bad file descriptor"};
        }
        break;
      }

      case redirection_outcome::Duplicate: {
        const i32 from_fd = r.dup_from_fd;

        /* Inside an in-process subshell the move is backed up and contained at
           the subshell's end. The flush keeps buffered output on the original
           descriptor before it moves. */
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          shit::flush();

          if (from_fd == Redirection::DUP_FD_CLOSE) {
            os::close_shell_fd(redir.fd);
            break;
          }

          if (!os::replace_descriptor(redir.fd,
                                      os::descriptor_for_shell_fd(from_fd)))
          {
            const SourceLocation location =
                redir.target != nullptr ? redir.target->source_location()
                                        : source_location();
            did_redirection_open_fail = true;
            throw ErrorWithLocation{location,
                                    String::from(from_fd, heap_allocator()) +
                                        ": Bad file descriptor"};
          }
          break;
        }

        if (from_fd == redir.fd) {
          break;
        }

        /* A cross-route such as 2>&1 points the real shell descriptor at the
           target in source order so a later file redirect on the source does
           not change what the copy already captured. */
        shit::flush();

        if (from_fd == Redirection::DUP_FD_CLOSE) {
          dup_saved_descriptors.push(os::save_and_replace_descriptor(
              redir.fd, os::descriptor_for_shell_fd(redir.fd)));
          os::close_fd(os::descriptor_for_shell_fd(redir.fd));
          break;
        }

        const os::saved_descriptor saved = os::save_and_replace_descriptor(
            redir.fd, os::descriptor_for_shell_fd(from_fd));
        dup_saved_descriptors.push(saved);
        if (!saved.is_dup2_ok) {
          const SourceLocation location = redir.target != nullptr
                                              ? redir.target->source_location()
                                              : source_location();
          did_redirection_open_fail = true;
          throw ErrorWithLocation{location,
                                  String::from(from_fd, heap_allocator()) +
                                      ": Bad file descriptor"};
        }
        break;
      }

      case redirection_outcome::OpenedFile: {
        const os::descriptor file_fd = r.opened_fd;
        /* The dup2 onto fd N replaces whatever fd N held, so a second exec onto
           the same number closes the earlier file rather than leaking it. The
           flush keeps buffered output on the original descriptor. */
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          shit::flush();
          const bool was_replaced = os::replace_descriptor(redir.fd, file_fd);
          if (!os::descriptor_is_shell_fd(file_fd, redir.fd))
            os::close_fd(file_fd);
          if (!was_replaced) {
            did_redirection_open_fail = true;
            throw ErrorWithLocation{redir.target->source_location(),
                                    String::from(redir.fd, heap_allocator()) +
                                        ": Bad file descriptor"};
          }
          break;
        }

        /* Staged onto the real shell fd N in source order so a later 2>&1
           copies the descriptor fd N points at now. A redirect onto fd 1 or 2
           mutates the shell's own stdout or stderr, so it is flushed first. */
        if (redir.fd == 1 || redir.fd == 2) {
          shit::flush();
        }
        const bool file_is_target_fd =
            os::descriptor_is_shell_fd(file_fd, redir.fd);
        if (file_is_target_fd) {
          /* open returned fd N itself, so the collision is recorded for restore
             without a close. */
          dup_saved_descriptors.push(
              os::saved_descriptor{.shell_fd = redir.fd, .was_open = false});
        } else {
          dup_saved_descriptors.push(
              os::save_and_replace_descriptor(redir.fd, file_fd));
          if (!r.is_cached) os::close_fd(file_fd);
        }
        break;
      }
      }
    }
  } catch (const ErrorWithLocation &redirection_error) {
    /* Only an open or dup failure is caught here. An expansion error in a
       target word stays fatal. */
    if (!did_redirection_open_fail) throw;
    /* A special builtin's redirection error exits a non-interactive shell, so
       it is not recovered. The defers above put the partial redirections
       back. */
    if (command_is_special_builtin) throw;
    const String *source = cxt.current_source();
    show_message(redirection_error.to_string(
        source != nullptr ? source->view() : StringView{}, &cxt));
    /* bash reports a redirection failure with status 1 and dash with 2. */
    let const redirection_status = cxt.is_bash_compatible() ? 1 : 2;
    cxt.set_last_exit_status(redirection_status);
    cxt.publish_single_pipe_status(redirection_status);
    return redirection_status;
  }

  /* The append form reads the current value from the shell store first so a
     non-exported shell variable still contributes. An integer name evaluates
     the join to its decimal here. */
  let do_apply_append = [&](StringView name, String &value_ref) throws {
    let appended = String{cxt.scratch_allocator()};
    if (let const existing = cxt.get_variable_value(name))
      appended.append(existing->view());
    if (cxt.is_integer_variable(name)) {
      cxt.append_integer_expression(appended, value_ref.view());
      char decimal[24];
      value_ref = String{
          cxt.scratch_allocator(),
          utils::int_to_text_into(cxt.evaluate_arithmetic(appended.view()),
                                  decimal, sizeof(decimal))};
    } else {
      appended += value_ref;
      value_ref = steal(appended);
    }
  };

  /* An expansion may drop every word. A command-less line still carries its
     assignments, which persist in the current shell. */
  if (program_args.is_empty()) {
    for (let const &var : m_local_vars) {
      const StringView name = var.name.view();
      let value = cxt.expand_word_for_assignment(var.value);
      if (var.is_append) do_apply_append(name, value);
      cxt.set_shell_variable(name, value);
      if (cxt.export_all()) {
        cxt.record_environment_change(name);
        os::set_environment_variable(name, value.view());
        cxt.mark_exported(name);
      }
    }
    /* Bare array assignments apply after the scalars in source order. */
    for (let const &assignment : m_array_args) {
      if (cxt.is_readonly(assignment.name))
        throw Error{"Unable to assign '" + assignment.name +
                    "' because it is read only"};
      ArrayList<String> values =
          cxt.process_args(assignment.elements, argument_lifetime::Persistent,
                           argument_context::ArrayLiteral);
      cxt.assign_indexed_array_elements(assignment.name, steal(values),
                                        assignment.is_append);
    }
    /* A value that ran a command substitution leaves the status of the last
       one. A line with no substitution resets to 0. */
    let do_token_ran_substitution = [&](const Token *token) {
      if (token == nullptr || token->kind() != Token::Kind::Word) return false;
      return static_cast<const tokens::WordToken *>(token)
          ->word()
          .runs_substitution();
    };
    let ran_substitution = false;
    for (let const token : m_args)
      ran_substitution = ran_substitution || do_token_ran_substitution(token);
    for (let const &var : m_local_vars)
      ran_substitution = ran_substitution || var.value.runs_substitution();
    for (let const &assignment : m_array_args)
      for (let const token : assignment.elements)
        ran_substitution = ran_substitution || do_token_ran_substitution(token);
    /* When a substitution ran, its own last command already set PIPESTATUS. */
    if (!ran_substitution) {
      cxt.set_last_exit_status(0);
      cxt.publish_single_pipe_status(0);
    }
    return cxt.last_exit_status();
  }

  /* A prefix assignment before a special builtin persists after the command as
     a regular shell variable. A per-command assignment otherwise applies to the
     environment for this command, restored on every exit path. */
  struct saved_env_var
  {
    String name;
    Maybe<String> previous_value;
  };
  ArrayList<saved_env_var> saved_env{cxt.scratch_allocator()};
  saved_env.reserve(m_local_vars.count());
  /* A prefix IFS=... drives the shell's own word splitting for this command
     through the live separator cache. The effective separators are saved before
     the first such prefix and restored on exit. */
  bool ifs_was_assigned = false;
  String saved_ifs_separators{cxt.scratch_allocator()};
  Maybe<ProgramResolver> saved_program_resolver{};
  /* The assignments apply left to right, each committed before the next is
     expanded, so a later value reads an earlier same-line one. */
  for (let const &var : m_local_vars) {
    const StringView name = var.name.view();
    if (cxt.is_readonly(name))
      throw Error{"Unable to assign '" + name + "' because it is read only"};
    const bool is_read_field_separator =
        name == "IFS" && command_word_function == nullptr &&
        !program_args.is_empty() && program_args[0] == "read";
    Maybe<String> previous;
    if (!is_read_field_separator) previous = os::get_environment_variable(name);
    let expanded_value = String{cxt.scratch_allocator()};
    try {
      expanded_value = cxt.expand_word_for_assignment(var.value);
    } catch (const ErrorWithLocation &) {
      throw;
    } catch (const Error &e) {
      relocate_error(e, source_location());
    }
    if (var.is_append) do_apply_append(name, expanded_value);

    /* A special builtin keeps the assignment outside the bash mood, so it
       commits to the store. The bash mood drops it after the command, so it
       falls to the temporary path instead. */
    if (command_is_special_builtin && !cxt.is_bash_compatible()) {
      cxt.set_shell_variable(name, expanded_value);
      if (cxt.export_all()) {
        cxt.record_environment_change(name);
        os::set_environment_variable(name, expanded_value.view());
        cxt.mark_exported(name);
      }
      continue;
    }

    if (!is_read_field_separator) {
      saved_env.push(saved_env_var{String{name}, steal(previous)});
      os::set_environment_variable(name, expanded_value.view());
      cxt.mark_exported(name);
    }
    /* The resolver reads its own MAYBE_PATH, so a prefix PATH=... must update
       it for the environment write to change the search order. */
    if (name == "PATH") {
      if (!saved_program_resolver.has_value())
        saved_program_resolver =
            Maybe<ProgramResolver>{cxt.get_program_resolver()};
      cxt.get_program_resolver().assign_path(String{expanded_value.view()});
    }
    /* The value before the first IFS prefix is saved once, so a name repeated
       on the line still reverts to where it began. */
    if (name == "IFS") {
      if (!ifs_was_assigned) {
        ifs_was_assigned = true;
        saved_ifs_separators =
            cxt.get_variable_value("IFS").value_or(String{" \t\n"});
      }
      cxt.set_field_separators(expanded_value.view());
    }
  }
  defer
  {
    /* The restore runs newest first, so a name spelled more than once restores
       to the value it held before the first of its assignments. */
    for (usize i = saved_env.count(); i > 0; i--) {
      const saved_env_var &restore = saved_env[i - 1];
      if (restore.previous_value)
        os::set_environment_variable(restore.name.view(),
                                     restore.previous_value->view());
      else
        os::unset_environment_variable(restore.name.view());
      cxt.sync_exported_after_restore(restore.name.view(),
                                      restore.previous_value.has_value());
    }
    if (saved_program_resolver.has_value())
      cxt.get_program_resolver() = steal(*saved_program_resolver);
    if (ifs_was_assigned) cxt.set_field_separators(saved_ifs_separators.view());
  };

  ASSERT(!program_args.is_empty());
  let const &program_name = program_args[0];

  /* The command name is copied for the array-argument application below, since
     the argument vector moves into the exec context before that point. */
  let array_command_name = String{cxt.scratch_allocator()};
  if (!m_array_args.is_empty())
    array_command_name = String{program_args[0].view()};

  if (const Expression *function_body = command_word_function;
      function_body != nullptr)
  {
    /* An input redirection on the call lands on the real fd 0 for the body's
       duration, so the in-process body and every child it spawns read the
       staged bytes. */
    if (redirect_in_fd) {
      dup_saved_descriptors.push(
          os::save_and_replace_descriptor(0, *redirect_in_fd));
      os::close_fd(*redirect_in_fd);
      redirect_in_fd = shit::None;
      was_redirect_in_fd_handed_off = true;
    }

    let saved_params = cxt.take_positional_params();
    let call_params = ArrayList<String>{heap_allocator()};
    call_params.reserve(program_args.count() - 1);
    for (usize i = 1; i < program_args.count(); i++)
      call_params.push_managed(program_args[i]);
    cxt.set_positional_params(steal(call_params));
    defer { cxt.set_positional_params(steal(saved_params)); };

    /* Bound the call nesting so a function that recurses without a base case
       errors with a caret here rather than exhausting the native stack. */
    cxt.enter_function_call(source_location());
    defer { cxt.leave_function_call(); };

    /* A loop in the caller is not the body's to break, so the body starts with
       a fresh loop count. */
    let const saved_loop_depth = cxt.loop_depth();
    cxt.set_loop_depth(0);
    defer { cxt.set_loop_depth(saved_loop_depth); };

    /* Registered first so it runs last, after the scope pop restores the
       locals. */
    let const call_mark = cxt.scratch_mark();
    defer { cxt.scratch_release(call_mark); };

    cxt.enter_function_scope();
    cxt.push_function_call_name(program_name.view());
    defer
    {
      cxt.pop_function_call_name();
      cxt.leave_function_scope();
    };

    /* A command at the tail of the body must not exec the shell in place, since
       the call's cleanup has to run after the body. */
    let const saved_terminal_exec = cxt.terminal_exec_allowed();
    cxt.set_terminal_exec_allowed(false);
    defer { cxt.set_terminal_exec_allowed(saved_terminal_exec); };

    /* The body runs in the mood and diagnostics state the function was defined
       in, so a function defined in bash mood runs bash even after a later set
       --mood. The swap only happens when the defining state differs from the
       live state. */
    let const *const definition_info =
        cxt.function_definition_info_of(program_name.view());
    let const needs_state_swap =
        definition_info != nullptr &&
        (definition_info->defining_runtime.mood != cxt.mood() ||
         definition_info->defining_runtime.warning_level !=
             cxt.warning_level() ||
         definition_info->defining_runtime.are_diagnostics_disabled !=
             cxt.diagnostics_disabled());
    Maybe<function_runtime_state> saved_runtime_state = None;
    if (needs_state_swap) {
      saved_runtime_state =
          cxt.enter_definition_state(definition_info->defining_runtime);
    }
    defer
    {
      if (saved_runtime_state.has_value())
        cxt.leave_definition_state(*saved_runtime_state);
    };

    /* A located error thrown from the body is rendered here while the stack
       still names the function, since the top-level handler cannot reach the
       definition file once this frame unwinds. window_function_body_error
       rebases the position onto the definition copy. The error is marked
       rendered so the top-level handler keeps the status without printing it
       twice. */
    i64 function_ret = 0;
    try {
      function_ret = function_body->evaluate(cxt);
      if (!cxt.is_posix_mode()) cxt.run_named_trap(StringView{"RETURN", 6});
    } catch (ErrorWithLocationAndDetails &error) {
      if (!error.was_rendered())
        if (let const windowed = window_function_body_error(cxt, error);
            windowed.has_value())
        {
          show_message(error.to_string(*windowed, &cxt));
          show_message(error.details_to_string(*windowed, &cxt));
          error.set_rendered();
        }
      throw;
    } catch (ErrorWithLocation &error) {
      if (!error.was_rendered())
        if (let const windowed = window_function_body_error(cxt, error);
            windowed.has_value())
        {
          show_message(error.to_string(*windowed, &cxt));
          error.set_rendered();
        }
      throw;
    }

    /* A return supplies the status. A break or continue is scoped to a loop
       inside this function and is consumed here. An exit stays pending for the
       shell. */
    if (cxt.has_pending_control_flow()) {
      const control_flow::Kind kind = cxt.pending_control_flow().kind;
      if (kind == control_flow::Kind::Return) {
        function_ret = cxt.pending_control_flow().value;
        cxt.clear_control_flow();
      } else if (kind == control_flow::Kind::Break ||
                 kind == control_flow::Kind::Continue)
      {
        cxt.clear_control_flow();
      }
    }

    SET_AND_RETURN_EXIT_STATUS(cxt, function_ret);
  }

  /* $_ reads the last argument of the previous command, captured here before
     the argument vector moves into the exec context. */
  let const last_argument = program_args.is_empty()
                                ? String{cxt.scratch_allocator()}
                                : program_args.back();

  Maybe<ExecContext> resolved_ec;
  try {
    let const *source = cxt.current_source();
    resolved_ec = ExecContext::make_from(
        source_location(), source != nullptr ? source->view() : StringView{},
        steal(program_args), cxt.mood(), cxt.shitbox(),
        cxt.get_program_resolver(), steal(program_arg_locations));
  } catch (const CommandResolutionErrorWithLocation &e) {
    report_command_resolution_error(cxt, e);
    let const status = e.command_status();
    cxt.set_last_exit_status(static_cast<i32>(status));
    cxt.publish_single_pipe_status(static_cast<i32>(status));
    return status;
  }
  let ec = resolved_ec.take();

  /* The exec context now owns and closes the staged input descriptor. The
     stdout and stderr redirects already took effect on the real shell fds. */
  if (redirect_in_fd) ec.in_fd = redirect_in_fd;
  was_redirect_in_fd_handed_off = true;

  const i64 ret = utils::execute_context(
      steal(ec), cxt,
      is_async() ? execution_mode::Background : execution_mode::Foreground);
  cxt.set_last_argument(last_argument.view());

  /* An assignment builtin with NAME=(...) array arguments applies them after it
     runs, in the scope the builtin selects. The builtin ran first, so a local
     outside a function has already errored and the elements never reach here.
   */
  if (!m_array_args.is_empty()) {
    let const is_local = array_command_name == "local";
    let const is_declare =
        array_command_name == "declare" || array_command_name == "typeset";
    let const is_function_local = is_declare && cxt.in_function_scope();
    let const is_export = array_command_name == "export";
    /* The -r flag sits in the builtin's arguments, so it is read off them. */
    let is_readonly_request = array_command_name == "readonly";
    /* The -A flag routes to the string-keyed store rather than the indexed
       one. */
    let is_associative_request = false;
    if (is_declare || is_local) {
      for (let const arg : m_args) {
        let const text = arg->raw_string();
        if (text.length() >= 2 && text.view()[0] == '-') {
          if (!is_readonly_request &&
              text.view().find_character('r').has_value())
          {
            is_readonly_request = true;
          }
          if (text.view().find_character('A').has_value())
            is_associative_request = true;
        }
      }
    }
    for (let const &assignment : m_array_args) {
      if (is_local || is_function_local) {
        cxt.declare_local(assignment.name);
      }
      ArrayList<String> values =
          cxt.process_args(assignment.elements, argument_lifetime::Persistent,
                           argument_context::ArrayLiteral);
      if (is_associative_request) {
        /* A bare element with no bracketed key becomes a key with an empty
           value. */
        cxt.declare_associative_array(assignment.name);
        for (let const &element : values) {
          const StringView text = element.view();
          if (!text.is_empty() && text[0] == '[') {
            if (let const close = text.find_character(']');
                close.has_value() && *close + 1 < text.length &&
                text[*close + 1] == '=')
            {
              cxt.set_associative_element(
                  assignment.name, text.substring_of_length(1, *close - 1),
                  text.substring(*close + 2));
              continue;
            }
          }
          cxt.set_associative_element(assignment.name, text, StringView{});
        }
      } else {
        cxt.assign_indexed_array_elements(assignment.name, steal(values),
                                          assignment.is_append);
      }
      if (is_export) cxt.mark_exported(assignment.name);
      if (is_readonly_request) cxt.mark_readonly(assignment.name);
    }
  }

  cxt.set_last_exit_status(static_cast<i32>(ret));
  cxt.publish_single_pipe_status(static_cast<i32>(ret));
  return ret;
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

} /* namespace shit */
