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
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

CompoundCommand::CompoundCommand(SourceLocation location)
    : Command(steal(location))
{}

fn CompoundCommand::evaluate_async(EvalContext &cxt) const throws -> i64
{
  let const source = cxt.current_source();
  let command_text = StringView{};
  if (source != nullptr) {
    let command_end_position =
        source_location().position + source_location().length;
    if (source_end_position() > command_end_position)
      command_end_position = source_end_position();
    command_text = source->view().substring_of_length(
        source_location().position,
        command_end_position - source_location().position);
  }

  let const launch = os::launch_compound_stage(
      command_text, None, None, None, cxt.mood(), source_location(),
      source != nullptr ? source->view() : StringView{},
      os::process_group_mode::NewBackground);
  let const child = launch.child;

  if (launch.should_evaluate_child) {
    i32 status = 1;
    try {
      cxt.enter_subshell();
      status = static_cast<i32>(evaluate_impl(cxt));
      if (cxt.has_pending_control_flow() &&
          cxt.pending_control_flow().kind == control_flow::Kind::Exit)
      {
        status = static_cast<i32>(cxt.pending_control_flow().value);
      }
    } catch (const BrokenPipeExit &) {
      status = KOSH_BROKEN_PIPE_EXIT_STATUS;
    } catch (const ErrorWithLocation &e) {
      koshka::show_message(
          e.to_string(source != nullptr ? source->view() : StringView{}, &cxt));
      status = static_cast<i32>(e.command_status());
    } catch (const Error &e) {
      koshka::show_message(e.to_string());
      status = static_cast<i32>(e.command_status());
    } catch (...) {
      LOG(Debug, "the compound command child swallowed an unknown error");
    }
    koshka::flush();
    os::exit_process_immediately(status);
  }

  let const process_id = os::process_id_of(child);
  cxt.set_last_background_pid(process_id);
  let command = String{command_text};
  command += " &";
  let const id = cxt.register_job(child, command.view(), process_id);
  if (cxt.shell_is_interactive()) {
    koshka::print_error(
        "[" + String::from(id, heap_allocator()) + "] " +
        String::from(static_cast<u64>(process_id), heap_allocator()) + "\n");
  }

  cxt.publish_single_pipe_status(0);
  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

fn CompoundCommand::is_compound_command() const wontthrow -> bool
{
  return true;
}

fn CompoundCommand::redirect_to(usize d, String &f, bool duplicate) throws
    -> void
{
  unused(d);
  unused(f);
  unused(duplicate);
  throw ErrorWithLocation{source_location(),
                          "Redirection on a compound command is not supported"};
}

fn CompoundCommand::set_fully_eliminated() const wontthrow -> void
{
  m_is_fully_eliminated = true;
}

pure fn CompoundCommand::is_fully_eliminated() const wontthrow -> bool
{
  return m_is_fully_eliminated;
}

IfClause::IfClause(SourceLocation location, ArrayList<if_branch> &&branches,
                   const Expression *otherwise)
    : CompoundCommand(steal(location)), m_branches(steal(branches)),
      m_otherwise(otherwise)
{}

IfClause::~IfClause() = default;

fn IfClause::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  for (let const &branch : m_branches) {
    if (!branch.condition->can_evaluate_in_process_substitution(
            cxt, active_functions) ||
        !branch.body->can_evaluate_in_process_substitution(cxt,
                                                           active_functions))
    {
      return false;
    }
  }

  return m_otherwise == nullptr ||
         m_otherwise->can_evaluate_in_process_substitution(cxt,
                                                           active_functions);
}

cold fn IfClause::to_string() const throws -> String { return "IfClause"; }

cold fn IfClause::to_ast_string(usize layer) const throws -> String
{
  let const pad = indent_for_layer(layer);
  let const child_pad = pad + EXPRESSION_AST_INDENT;
  let s = pad + "[" + to_string() + "]";
  for (let const &[ condition, body ] : m_branches) {
    ASSERT(condition != nullptr);
    ASSERT(body != nullptr);

    s += "\n" + child_pad + condition->to_ast_string(layer + 1);
    s += "\n" + child_pad + body->to_ast_string(layer + 1);
  }

  if (m_otherwise != nullptr)
    s += "\n" + child_pad + m_otherwise->to_ast_string(layer + 1);

  return s;
}

hot fn IfClause::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  cxt.set_terminal_exec_allowed(false);
  let const can_skip_condition_commands =
      !cxt.has_debug_trap() && !cxt.should_echo_expanded();

  if (m_is_fully_eliminated && can_skip_condition_commands) {
    LOG(Debug, "running the fully eliminated if as a no-op");
    cxt.publish_single_pipe_status(1);
    SET_AND_RETURN_EXIT_STATUS(cxt, 0);
  }

  /* An index past the last branch means every condition failed, so the else
     body runs or the if yields 0. */
  if (m_folded_branch.has_value() && can_skip_condition_commands) {
    LOG(Debug,
        "running the folded if branch %zu of %zu without testing conditions",
        *m_folded_branch, m_branches.count());
    if (*m_folded_branch < m_branches.count())
      return m_branches[*m_folded_branch].body->evaluate(cxt);
    if (m_otherwise != nullptr) return m_otherwise->evaluate(cxt);
    cxt.publish_single_pipe_status(1);
    SET_AND_RETURN_EXIT_STATUS(cxt, 0);
  }

  for (let const &[ condition, body ] : m_branches) {
    ASSERT(condition != nullptr);
    ASSERT(body != nullptr);

    i64 condition_status;
    {
      cxt.enter_condition();
      defer { cxt.leave_condition(); };
      condition_status = condition->evaluate(cxt);
    }

    if (cxt.has_pending_control_flow()) return condition_status;
    if (condition_status == 0) return body->evaluate(cxt);
  }

  if (m_otherwise != nullptr) return m_otherwise->evaluate(cxt);

  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

fn IfClause::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  /* The fold reads the constant table while it still holds the values recorded
     before this if, so it runs before any child analyze mutates the table. */
  optimizer::optimize_node(this, actx);

  /* The first condition runs whenever the if runs. The elif conditions and all
     bodies are conditional. */
  let saved_tested_command_names = actx.tested_command_names.clone();
  let condition_failure_names = saved_tested_command_names.clone();
  let merged_occurrence_assignments = VariableOccurrenceStateMap{};
  let merged_inherited_occurrence_assignments = VariableOccurrenceStateMap{};
  let has_merged_occurrence_exit = false;
  let is_first_branch = true;
  for (usize i = 0; i < m_branches.count(); i++) {
    let const & [ condition, body ] = m_branches[i];
    ASSERT(condition != nullptr);
    ASSERT(body != nullptr);

    actx.tested_command_names = condition_failure_names.clone();
    let const was_retaining_tested_command_names =
        actx.should_retain_tested_command_names;
    actx.should_retain_tested_command_names = true;
    let const was_analyzing_condition = actx.is_analyzing_condition;
    actx.is_analyzing_condition = true;
    condition->analyze(actx, is_unconditional && is_first_branch);
    actx.is_analyzing_condition = was_analyzing_condition;
    actx.should_retain_tested_command_names =
        was_retaining_tested_command_names;
    let const is_dead_branch =
        has_folded_branch() && folded_branch_index() != i;
    let condition_failure_occurrence_assignments =
        actx.variable_occurrence_assignments.snapshot();
    let condition_failure_inherited_occurrence_assignments =
        actx.inherited_variable_occurrence_assignments.snapshot();
    let const was_silenced = actx.should_silence_unresolved_commands;
    if (is_dead_branch) actx.should_silence_unresolved_commands = true;
    body->analyze(actx, false);
    actx.should_silence_unresolved_commands = was_silenced;

    if (!is_dead_branch) {
      if (!has_merged_occurrence_exit) {
        merged_occurrence_assignments =
            steal(actx.variable_occurrence_assignments);
        merged_inherited_occurrence_assignments =
            steal(actx.inherited_variable_occurrence_assignments);
        has_merged_occurrence_exit = true;
      } else {
        merge_variable_occurrence_states(merged_occurrence_assignments,
                                         actx.variable_occurrence_assignments);
        merge_variable_occurrence_states(
            merged_inherited_occurrence_assignments,
            actx.inherited_variable_occurrence_assignments);
      }
    }

    actx.variable_occurrence_assignments =
        steal(condition_failure_occurrence_assignments);
    actx.inherited_variable_occurrence_assignments =
        steal(condition_failure_inherited_occurrence_assignments);

    actx.tested_command_names = condition_failure_names.clone();
    condition->append_presence_tested_command_names(
        actx, actx.tested_command_names, false);
    condition_failure_names = steal(actx.tested_command_names);
    is_first_branch = false;
  }

  let const else_is_dead =
      has_folded_branch() && folded_branch_index() != m_branches.count();
  let const was_else_silenced = actx.should_silence_unresolved_commands;
  if (else_is_dead) actx.should_silence_unresolved_commands = true;
  actx.tested_command_names = steal(condition_failure_names);
  if (m_otherwise != nullptr) m_otherwise->analyze(actx, false);
  actx.should_silence_unresolved_commands = was_else_silenced;
  actx.tested_command_names = steal(saved_tested_command_names);

  if (!else_is_dead) {
    if (!has_merged_occurrence_exit) {
      merged_occurrence_assignments =
          steal(actx.variable_occurrence_assignments);
      merged_inherited_occurrence_assignments =
          steal(actx.inherited_variable_occurrence_assignments);
    } else {
      merge_variable_occurrence_states(merged_occurrence_assignments,
                                       actx.variable_occurrence_assignments);
      merge_variable_occurrence_states(
          merged_inherited_occurrence_assignments,
          actx.inherited_variable_occurrence_assignments);
    }
  }

  actx.variable_occurrence_assignments = steal(merged_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(merged_inherited_occurrence_assignments);

  /* A branch ran conditionally and may have reassigned a name, so a value
     recorded before this if is no longer proven after it. */
  actx.constant_variables.clear();
}

pure fn IfClause::branches() const wontthrow -> const ArrayList<if_branch> &
{
  return m_branches;
}

pure fn IfClause::otherwise() const wontthrow -> const Expression *
{
  return m_otherwise;
}

fn IfClause::set_folded_branch(usize index) const wontthrow -> void
{
  m_folded_branch = index;
}

pure fn IfClause::has_folded_branch() const wontthrow -> bool
{
  return m_folded_branch.has_value();
}

pure fn IfClause::folded_branch_index() const wontthrow -> usize
{
  return *m_folded_branch;
}

fn IfClause::as_if_clause() const wontthrow -> const IfClause * { return this; }

WhileLoop::WhileLoop(SourceLocation location, const Expression *condition,
                     const Expression *body, bool is_until)
    : CompoundCommand(steal(location)), m_condition(condition), m_body(body),
      m_is_until(is_until)
{}

WhileLoop::~WhileLoop() = default;

cold fn WhileLoop::to_string() const throws -> String
{
  return m_is_until ? "UntilLoop" : "WhileLoop";
}

cold fn WhileLoop::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  let const child_pad = pad + EXPRESSION_AST_INDENT;
  let s = pad + "[" + to_string() + "]";
  s += "\n" + child_pad + m_condition->to_ast_string(layer + 1);
  s += "\n" + child_pad + m_body->to_ast_string(layer + 1);
  return s;
}

hot fn resolve_loop_control(EvalContext &cxt) throws -> loop_disposition
{
  if (!cxt.has_pending_control_flow()) return loop_disposition::RunNext;

  let &control = cxt.pending_control_flow();
  if (control.kind != control_flow::Kind::Break &&
      control.kind != control_flow::Kind::Continue)
  {
    /* A return or an exit is not this loop's to consume. */
    return loop_disposition::StopLoop;
  }

  /* A jump aimed at an outer loop decrements and stays pending. */
  if (control.value > 1) {
    control.value -= 1;
    LOG(All, "the loop jump targets an outer loop, %lld levels stay pending",
        static_cast<long long>(control.value));
    return loop_disposition::StopLoop;
  }

  /* The jump targets this loop and is consumed here. */
  let const is_break = control.kind == control_flow::Kind::Break;
  cxt.clear_control_flow();
  LOG(All, "consuming the %s aimed at this loop",
      is_break ? "break" : "continue");
  return is_break ? loop_disposition::StopLoop : loop_disposition::RunNext;
}

hot fn WhileLoop::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_body != nullptr);

  cxt.set_terminal_exec_allowed(false);

  LOG(Debug, "entering the %s loop%s", m_is_until ? "until" : "while",
      m_folded_to_skip ? ", folded to skip the body" : "");

  let const can_skip_condition_commands =
      !cxt.has_debug_trap() && !cxt.should_echo_expanded();
  if ((m_folded_to_skip || m_is_fully_eliminated) &&
      can_skip_condition_commands)
  {
    cxt.publish_single_pipe_status(m_is_until ? 0 : 1);
    SET_AND_RETURN_EXIT_STATUS(cxt, 0);
  }

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const redirect_fd_mark = cxt.mark_loop_redirect_fds();
  defer { cxt.cleanup_loop_redirect_fds(redirect_fd_mark); };

  i64 ret = 0;
  loop
  {
    i64 condition_status;
    {
      cxt.enter_condition();
      defer { cxt.leave_condition(); };
      condition_status = m_condition->evaluate(cxt);
    }
    if (cxt.no_exec()) break;
    if (cxt.has_pending_control_flow()) {
      if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
      continue;
    }

    let const should_run_body =
        m_is_until ? (condition_status != 0) : (condition_status == 0);
    if (!should_run_body) break;

    ret = m_body->evaluate(cxt);
    if (cxt.no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
  }
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

fn WhileLoop::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_body != nullptr);

  /* The table is cleared before optimize so a pre-loop constant is never
     inlined into the condition, which would freeze a loop whose counter was
     folded to its initial value. */
  actx.constant_variables.clear();

  optimizer::optimize_node(this, actx);

  let const was_inside_loop_condition = actx.is_inside_loop_condition;
  let const prior_loop_condition_reads_input =
      actx.has_input_reading_loop_condition;
  let saved_tested_command_names = actx.tested_command_names.clone();
  let const was_retaining_tested_command_names =
      actx.should_retain_tested_command_names;
  actx.is_inside_loop_condition = true;
  actx.has_input_reading_loop_condition = false;
  actx.should_retain_tested_command_names = true;
  let const was_analyzing_condition = actx.is_analyzing_condition;
  let const saved_getopts = actx.active_getopts;
  actx.active_getopts = {};
  actx.is_analyzing_condition = true;
  /* The loop is already entered when its condition list runs, so a break or a
     continue there leaves this loop. */
  actx.loop_body_depth++;
  m_condition->analyze(actx, is_unconditional);
  actx.loop_body_depth--;
  actx.is_analyzing_condition = was_analyzing_condition;
  actx.should_retain_tested_command_names = was_retaining_tested_command_names;
  let const has_input_reading_loop_condition =
      actx.has_input_reading_loop_condition;
  actx.is_inside_loop_condition = was_inside_loop_condition;
  actx.has_input_reading_loop_condition = prior_loop_condition_reads_input;

  if (m_is_until) {
    actx.tested_command_names = saved_tested_command_names.clone();
    m_condition->append_presence_tested_command_names(
        actx, actx.tested_command_names, false);
  }

  let condition_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let condition_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();
  let const was_silenced = actx.should_silence_unresolved_commands;
  let const was_inside_read_loop = actx.is_inside_read_loop;
  if (has_input_reading_loop_condition) actx.is_inside_read_loop = true;
  if (is_folded_to_skip()) actx.should_silence_unresolved_commands = true;
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;

  merge_variable_occurrence_states(condition_occurrence_assignments,
                                   actx.variable_occurrence_assignments);
  merge_variable_occurrence_states(
      condition_inherited_occurrence_assignments,
      actx.inherited_variable_occurrence_assignments);
  actx.variable_occurrence_assignments =
      steal(condition_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(condition_inherited_occurrence_assignments);
  actx.is_inside_read_loop = was_inside_read_loop;
  actx.should_silence_unresolved_commands = was_silenced;
  actx.tested_command_names = steal(saved_tested_command_names);
  actx.active_getopts = saved_getopts;
}

pure fn WhileLoop::condition() const wontthrow -> const Expression *
{
  return m_condition;
}

pure fn WhileLoop::is_until() const wontthrow -> bool { return m_is_until; }

fn WhileLoop::set_folded_to_skip() const wontthrow -> void
{
  m_folded_to_skip = true;
}

pure fn WhileLoop::is_folded_to_skip() const wontthrow -> bool
{
  return m_folded_to_skip;
}

fn WhileLoop::as_while_loop() const wontthrow -> const WhileLoop *
{
  return this;
}

SelectLoop::SelectLoop(SourceLocation location,
                       SourceLocation variable_location,
                       StringView variable_name,
                       ArrayList<const Token *> &&words, bool has_in_clause,
                       const Expression *body)
    : CompoundCommand(steal(location)), m_variable_name(variable_name),
      m_body(body), m_variable_location(steal(variable_location)),
      m_has_in_clause(has_in_clause)
{
  m_words = steal(words);
}

SelectLoop::~SelectLoop() = default;

cold fn SelectLoop::to_string() const throws -> String
{
  return "SelectLoop \"" + m_variable_name + "\"";
}

cold fn SelectLoop::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);
  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

fn SelectLoop::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  cxt.set_terminal_exec_allowed(false);
  cxt.set_current_location(source_location());

  let const values =
      m_has_in_clause ? cxt.process_args(m_words) : cxt.positional_params();
  if (values.is_empty()) return 0;

  LOG(Debug, "the select loop offers %zu choices for '%.*s'", values.count(),
      static_cast<int>(m_variable_name.length), m_variable_name.data);

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const redirect_fd_mark = cxt.mark_loop_redirect_fds();
  defer { cxt.cleanup_loop_redirect_fds(redirect_fd_mark); };

  i64 ret = 0;
  bool should_reprint_menu = true;
  loop
  {
    /* The numbered menu and the prompt go to standard error. The menu reprints
       only after an empty line. */
    if (should_reprint_menu) {
      let menu = String{cxt.scratch_allocator()};
      for (usize i = 0; i < values.count(); i++) {
        menu += String::from(static_cast<i64>(i + 1), heap_allocator());
        menu += ") ";
        menu.append(values[i].view());
        menu += '\n';
      }
      koshka::print_error(menu.view());
      should_reprint_menu = false;
    }
    koshka::print_error(cxt.get_variable_value("PS3").value_or(String{"#? "}));

    bool was_newline_terminated = false;
    let const input =
        utils::read_line_from_fd(KOSH_STDIN, was_newline_terminated);
    /* End of input ends the loop, and bash echoes a newline to standard output
       the way a terminal end-of-file does. */
    if (!input) {
      koshka::print("\n");
      ret = 1;
      break;
    }

    let const &reply = *input;
    LOG(All, "the select prompt read the reply '%s'", reply.c_str());
    cxt.set_shell_variable("REPLY", reply.view());
    if (reply.is_empty()) {
      should_reprint_menu = true;
      continue;
    }

    /* A valid menu number binds the name to that word, any other input binds it
       to the empty string. */
    let const choice = reply.view().to<i64>();
    if (!choice.is_error() && choice.value() >= 1 &&
        static_cast<usize>(choice.value()) <= values.count())
    {
      cxt.set_shell_variable(
          m_variable_name,
          values[static_cast<usize>(choice.value()) - 1].view());
    } else {
      cxt.set_shell_variable(m_variable_name, "");
    }

    ret = m_body->evaluate(cxt);
    if (cxt.no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
  }
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

ForLoop::ForLoop(SourceLocation location, SourceLocation variable_location,
                 StringView variable_name, ArrayList<const Token *> &&words,
                 bool has_in_clause, const Expression *body)
    : CompoundCommand(steal(location)), m_variable_name(variable_name),
      m_body(body), m_variable_location(steal(variable_location)),
      m_has_in_clause(has_in_clause)
{
  m_words = steal(words);
}

ForLoop::~ForLoop() = default;

cold fn ForLoop::to_string() const throws -> String
{
  let result = String{"ForLoop \""};
  result += m_variable_name;
  result += "\"";
  return result;
}

cold fn ForLoop::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  let s = pad + "[" + to_string() + "]";
  s += "\n" + pad + EXPRESSION_AST_INDENT + m_body->to_ast_string(layer + 1);
  return s;
}

hot fn ForLoop::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  cxt.set_terminal_exec_allowed(false);

  if (m_is_fully_eliminated) {
    cxt.publish_single_pipe_status(0);
    SET_AND_RETURN_EXIT_STATUS(cxt, 0);
  }

  cxt.set_current_location(source_location());
  let const values =
      m_has_in_clause ? cxt.process_args(m_words) : cxt.positional_params();

  /* The default mood scopes the loop variable so the name does not leak, while
     the bash and posix moods leave it set. */
  let const scope_variable = !(cxt.is_bash_compatible() || cxt.is_posix_mode());
  Maybe<String> saved_value =
      scope_variable ? cxt.get_variable_value(m_variable_name) : None;
  defer
  {
    if (scope_variable) {
      if (saved_value.has_value())
        cxt.set_shell_variable(m_variable_name, saved_value->view());
      else
        cxt.unset_shell_variable(m_variable_name);
    }
  };

  LOG(Debug, "the for loop binds '%.*s' over %zu values",
      static_cast<int>(m_variable_name.length), m_variable_name.data,
      values.count());

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const redirect_fd_mark = cxt.mark_loop_redirect_fds();
  defer { cxt.cleanup_loop_redirect_fds(redirect_fd_mark); };

  i64 ret = 0;
  for (let const &value : values) {
    cxt.set_shell_variable(m_variable_name, value);
    ret = m_body->evaluate(cxt);
    if (cxt.no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
  }
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

fn ForLoop::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  ASSERT(m_body != nullptr);

  let loop_entry_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let loop_entry_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();

  let const outer_loop_location =
      actx.active_loop_variables.find(m_variable_name);
  if (outer_loop_location != nullptr) {
    actx.report_diagnostic(diagnostic_id::sc2165, m_variable_location,
                           {m_variable_name}, *outer_loop_location);
    actx.report_diagnostic(diagnostic_id::sc2167, *outer_loop_location,
                           {m_variable_name}, m_variable_location);
  }

  let const had_outer_loop_variable = outer_loop_location != nullptr;
  let saved_outer_loop_location = SourceLocation{};
  if (had_outer_loop_variable) saved_outer_loop_location = *outer_loop_location;
  actx.active_loop_variables.set(m_variable_name, m_variable_location);
  defer
  {
    if (had_outer_loop_variable)
      actx.active_loop_variables.set(m_variable_name,
                                     saved_outer_loop_location);
    else
      actx.active_loop_variables.erase(m_variable_name);
  };

  /* One walk of the word list decides every word-shaped finding, so a further
     check reads the flags this loop already holds. */
  let const word_list_holds_one_word = m_has_in_clause && m_words.count() == 1;

  for (let const t : m_words) {
    if (t->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(t)->word();
    let const source_text = analysis_source_text(actx, t->source_location());

    let word_is_literal = true;
    let has_glob_character = false;
    let has_unquoted_glob = false;
    let has_unquoted_brace = false;
    let has_unquoted_expansion = false;

    for (let const &segment : word.segments) {
      switch (segment.kind) {
      case WordSegment::Kind::LiteralText:
      case WordSegment::Kind::DoubleQuotedText:
        if (segment.has_glob_metacharacter()) has_glob_character = true;
        break;

      case WordSegment::Kind::UnquotedText: {
        if (segment.has_glob_metacharacter()) {
          has_glob_character = true;
          has_unquoted_glob = true;
        }
        if (segment.text.view().find_character('{').has_value())
          has_unquoted_brace = true;
        break;
      }

      /* A for over $(cat file) is shellcheck SC2013, over $(ls) is SC2045, and
         over $(find ...) is SC2044. */
      case WordSegment::Kind::CommandSubstitution: {
        word_is_literal = false;
        if (segment.is_in_double_quotes) break;
        has_unquoted_expansion = true;

        let const body = segment.text.view();
        usize start = 0;
        while (start < body.length &&
               (body[start] == ' ' || body[start] == '\t'))
          start++;
        let const trimmed = body.substring(start);
        if (trimmed.starts_with(StringView{"ls "}) || trimmed == "ls")
          actx.report_diagnostic(diagnostic_id::sc2045, t->source_location());
        else if (trimmed.starts_with(StringView{"cat "}))
          actx.report_diagnostic(diagnostic_id::sc2013, t->source_location());
        else if (trimmed.starts_with(StringView{"find "}) || trimmed == "find")
          actx.report_diagnostic(diagnostic_id::sc2044, t->source_location());
        break;
      }

      case WordSegment::Kind::VariableReference: {
        note_variable_reference(actx, segment, t->source_location());
        word_is_literal = false;
        if (!segment.is_in_double_quotes) has_unquoted_expansion = true;
        break;
      }

      case WordSegment::Kind::ArithmeticExpansion:
        word_is_literal = false;
        if (!segment.is_in_double_quotes) has_unquoted_expansion = true;
        break;

      default: word_is_literal = false; break;
      }
    }

    if (word_is_literal && has_glob_character && source_text.length >= 2 &&
        (source_text[0] == '\'' || source_text[0] == '"'))
    {
      actx.report_diagnostic(diagnostic_id::sc2066, t->source_location());
    }

    if (has_unquoted_expansion && has_unquoted_glob) {
      actx.report_diagnostic(diagnostic_id::sc2231, t->source_location(),
                             {source_text});
    }

    if (word_list_holds_one_word && word_is_literal && !has_glob_character &&
        !has_unquoted_brace && !source_text.is_empty())
    {
      actx.report_diagnostic(diagnostic_id::sc2043, t->source_location(),
                             {source_text});
    }
  }

  let const is_conditional = !is_unconditional ||
                             actx.has_seen_runtime_definer ||
                             (m_has_in_clause && m_words.is_empty());
  actx.note_variable_occurrence(m_variable_name, m_variable_location,
                                variable_occurrence_kind::Assignment,
                                is_conditional);
  actx.note_variable_binding_record(m_variable_name, m_variable_location,
                                    assignment_binder::ForLoop, is_conditional);

  /* The rule reads the word list while unchanged, so optimize runs before the
     constant table is cleared for the body. */
  optimizer::optimize_node(this, actx);

  /* Clearing the constant table before the body keeps a pre-loop constant from
     being inlined into a counter the body increments. */
  actx.constant_variables.clear();
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;

  merge_variable_occurrence_states(loop_entry_occurrence_assignments,
                                   actx.variable_occurrence_assignments);
  merge_variable_occurrence_states(
      loop_entry_inherited_occurrence_assignments,
      actx.inherited_variable_occurrence_assignments);
  actx.variable_occurrence_assignments =
      steal(loop_entry_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(loop_entry_inherited_occurrence_assignments);
}

fn ForLoop::as_for_loop() const wontthrow -> const ForLoop * { return this; }

pure fn ForLoop::has_in_clause() const wontthrow -> bool
{
  return m_has_in_clause;
}

pure fn ForLoop::words() const wontthrow -> const ArrayList<const Token *> &
{
  return m_words;
}

CaseClause::CaseClause(SourceLocation location, const Token *word,
                       ArrayList<case_item> &&items)
    : CompoundCommand(steal(location)), m_word(word)
{
  m_items = steal(items);
}

CaseClause::~CaseClause() = default;

cold fn CaseClause::to_string() const throws -> String { return "CaseClause"; }

cold fn CaseClause::to_ast_string(usize layer) const throws -> String
{
  let const pad = indent_for_layer(layer);
  let const child_pad = pad + EXPRESSION_AST_INDENT;
  let s = pad + "[" + to_string() + "]";
  for (let const &item : m_items) {
    ASSERT(item.body != nullptr);
    s += "\n" + child_pad + item.body->to_ast_string(layer + 1);
  }
  return s;
}

fn CaseClause::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_word != nullptr);

  cxt.set_terminal_exec_allowed(false);
  cxt.set_current_location(source_location());

  /* A case word and its patterns expand with variables and tilde but no field
     splitting and no globbing, so a pattern keeps its metacharacters. */
  let const do_expand_no_glob = [&cxt](const Token *t) -> String {
    ASSERT(t != nullptr);
    if (t->kind() == Token::Kind::Word) {
      try {
        return cxt.expand_word_for_assignment(
            static_cast<const tokens::WordToken *>(t)->word());
      } catch (const ErrorWithLocation &) {
        throw;
      } catch (const Error &e) {
        relocate_error(e, t->source_location());
      }
    }
    return t->raw_string();
  };

  let const subject = do_expand_no_glob(m_word);

  LOG(Debug, "the case subject expanded to '%s'", subject.c_str());

  let const do_arm_matches = [&](const case_item &item) throws -> bool {
    for (let const pattern_token : item.patterns) {
      /* A quoted or escaped metacharacter in the pattern is a literal, so the
         expansion carries a parallel mask the matcher reads. A constant literal
         pattern matches on an exact compare and skips the mask build. */
      if (pattern_token->kind() == Token::Kind::Word) {
        const Word &pattern_word =
            static_cast<const tokens::WordToken *>(pattern_token)->word();
        if (pattern_word.plain_literal_kind() != Word::PlainLiteral::NotPlain) {
          if (subject.view() == pattern_word.constant_value()) return true;
          continue;
        }
      }

      let pattern_active = Bitset{cxt.scratch_allocator()};
      let pattern = String{cxt.scratch_allocator()};
      if (pattern_token->kind() == Token::Kind::Word) {
        try {
          pattern = cxt.expand_case_pattern_masked(
              static_cast<const tokens::WordToken *>(pattern_token)->word(),
              pattern_active);
        } catch (const ErrorWithLocation &) {
          throw;
        } catch (const Error &e) {
          relocate_error(e, pattern_token->source_location());
        }
      } else {
        pattern = pattern_token->raw_string();
        for (usize k = 0; k < pattern.count(); k++)
          pattern_active.push(true);
      }
      if (utils::glob_matches(pattern, subject, pattern_active, 0,
                              cxt.extglob_enabled()))
        return true;
    }
    return false;
  };

  /* A ;& fall-through runs the next arm body without matching it, and a ;;&
     resumes matching at the arms past the one that just ran. */
  i64 result = 0;
  bool did_run_a_body = false;
  usize i = 0;
  while (i < m_items.count()) {
    if (!do_arm_matches(m_items[i])) {
      i++;
      continue;
    }

    LOG(All, "case arm %zu matched, running its body", i);

    bool should_resume_matching = false;
    loop
    {
      ASSERT(m_items[i].body != nullptr);
      result = m_items[i].body->evaluate(cxt);
      cxt.set_last_exit_status(static_cast<i32>(result));
      did_run_a_body = true;
      if (cxt.has_pending_control_flow()) return result;

      let const terminator = m_items[i].terminator;
      if (terminator == case_terminator::FallThrough && i + 1 < m_items.count())
      {
        i++;
        continue;
      }
      if (terminator == case_terminator::ContinueMatch) {
        i++;
        should_resume_matching = true;
      }
      break;
    }
    if (should_resume_matching) continue;
    return result;
  }

  if (!did_run_a_body) {
    LOG(Debug, "no case arm matched the subject");
    cxt.set_last_exit_status(0);
  }
  return result;
}

fn CaseClause::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  unused(is_unconditional);
  let const common_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let const common_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();
  let has_unquoted_default_pattern = false;
  for (let const &item : m_items) {
    for (let const pattern : item.patterns) {
      if (pattern->kind() != Token::Kind::Word) continue;

      let const &pattern_word =
          static_cast<const tokens::WordToken *>(pattern)->word();
      if (pattern_word.segments.count() != 1) continue;

      let const &segment = pattern_word.segments[0];
      if (segment.kind == WordSegment::Kind::UnquotedText &&
          segment.text == "*")
      {
        has_unquoted_default_pattern = true;
      }
    }
  }
  let merged_occurrence_assignments =
      VariableOccurrenceStateMap{common_occurrence_assignments};
  let merged_inherited_occurrence_assignments =
      VariableOccurrenceStateMap{common_inherited_occurrence_assignments};
  let continued_occurrence_assignments =
      VariableOccurrenceStateMap{common_occurrence_assignments};
  let continued_inherited_occurrence_assignments =
      VariableOccurrenceStateMap{common_inherited_occurrence_assignments};
  let has_merged_occurrence_exit = !has_unquoted_default_pattern;
  let has_continued_occurrence_path = false;

  ASSERT(m_word != nullptr);
  if (m_word->kind() == Token::Kind::Word) {
    let const &case_word =
        static_cast<const tokens::WordToken *>(m_word)->word();
    for (let const &segment : case_word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;
      note_variable_reference(actx, segment, m_word->source_location());
    }
  }

  for (usize i = 0; i < m_items.count(); i++) {
    let const &item = m_items[i];
    ASSERT(item.body != nullptr);

    actx.variable_occurrence_assignments = common_occurrence_assignments;
    actx.inherited_variable_occurrence_assignments =
        common_inherited_occurrence_assignments;
    if (has_continued_occurrence_path) {
      merge_variable_occurrence_states(actx.variable_occurrence_assignments,
                                       continued_occurrence_assignments);
      merge_variable_occurrence_states(
          actx.inherited_variable_occurrence_assignments,
          continued_inherited_occurrence_assignments);
    }

    for (let const pattern : item.patterns) {
      if (pattern->kind() != Token::Kind::Word) continue;
      let const &pattern_word =
          static_cast<const tokens::WordToken *>(pattern)->word();
      for (let const &segment : pattern_word.segments) {
        if (segment.kind != WordSegment::Kind::VariableReference) continue;
        note_variable_reference(actx, segment, pattern->source_location());
      }
    }

    item.body->analyze(actx, false);

    let const has_later_item = i + 1 < m_items.count();
    if (item.terminator != case_terminator::FallThrough || !has_later_item) {
      if (!has_merged_occurrence_exit) {
        merged_occurrence_assignments =
            actx.variable_occurrence_assignments.snapshot();
        merged_inherited_occurrence_assignments =
            actx.inherited_variable_occurrence_assignments.snapshot();
        has_merged_occurrence_exit = true;
      } else {
        merge_variable_occurrence_states(merged_occurrence_assignments,
                                         actx.variable_occurrence_assignments);
        merge_variable_occurrence_states(
            merged_inherited_occurrence_assignments,
            actx.inherited_variable_occurrence_assignments);
      }
    }

    has_continued_occurrence_path =
        has_later_item && item.terminator != case_terminator::Break;
    if (has_continued_occurrence_path) {
      continued_occurrence_assignments =
          actx.variable_occurrence_assignments.snapshot();
      continued_inherited_occurrence_assignments =
          actx.inherited_variable_occurrence_assignments.snapshot();
    }
  }

  actx.variable_occurrence_assignments = steal(merged_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(merged_inherited_occurrence_assignments);

  let case_input = case_lint_input{};
  case_input.case_location = m_word->source_location();
  case_input.case_word_source =
      analysis_source_text(actx, case_input.case_location);

  if (m_word->kind() == Token::Kind::Word) {
    case_input.case_word =
        &static_cast<const tokens::WordToken *>(m_word)->word();
    check_case_word_shape(actx, case_input);

    /* The case reads the getopts result when its word names the variable that
       call fills, which is what makes the arms an option catalog. */
    let const &case_word = *case_input.case_word;
    if (!actx.active_getopts.variable_name.is_empty() &&
        case_word.segments.count() == 1 &&
        case_word.segments[0].kind == WordSegment::Kind::VariableReference &&
        case_word.segments[0].text.view() == actx.active_getopts.variable_name)
    {
      case_input.is_getopts_case = true;
      case_input.getopts_optstring = actx.active_getopts.optstring;
      case_input.getopts_location = actx.active_getopts.location;
    }
  }

  /* A case with no catch-all *) arm is shellcheck SC2249. The catch-all is an
     unquoted * glob, a single UnquotedText segment whose text is *. A quoted
     '*' matches only a literal asterisk. */
  let tally = case_arm_tally{};
  let earlier_patterns = StringMap<SourceLocation>{heap_allocator()};
  let earlier_shadow_prefixes = ArrayList<String>{heap_allocator()};
  let earlier_shadow_locations = ArrayList<SourceLocation>{heap_allocator()};
  for (let const &item : m_items) {
    for (let const pattern : item.patterns) {
      if (pattern->kind() != Token::Kind::Word) continue;
      let const &pattern_word =
          static_cast<const tokens::WordToken *>(pattern)->word();
      let const literal = pattern_word.to_literal_string();
      let const raw_pattern = pattern->raw_string();
      let is_duplicate = false;
      if (let const *earlier_location =
              earlier_patterns.find(raw_pattern.view());
          earlier_location != nullptr)
      {
        actx.report_diagnostic(diagnostic_id::sc2221,
                               pattern->source_location(), {},
                               *earlier_location);
        is_duplicate = true;
      }
      if (!is_duplicate) {
        for (usize prefix_index = 0;
             prefix_index < earlier_shadow_prefixes.count(); prefix_index++)
        {
          let const &prefix = earlier_shadow_prefixes[prefix_index];
          if (!literal.view().starts_with(prefix.view())) continue;
          actx.report_diagnostic(diagnostic_id::sc2222,
                                 pattern->source_location(), {},
                                 earlier_shadow_locations[prefix_index]);
          break;
        }
      }
      if (!is_duplicate)
        earlier_patterns.set(raw_pattern.view(), pattern->source_location());
      if (pattern_word.segments.count() == 1 &&
          pattern_word.segments[0].kind == WordSegment::Kind::UnquotedText &&
          !literal.is_empty() && literal[literal.count() - 1] == '*')
      {
        earlier_shadow_prefixes.push(
            String{literal.view().substring_of_length(0, literal.count() - 1)});
        earlier_shadow_locations.push(pattern->source_location());
      }

      check_case_pattern_shape(
          actx, case_input, pattern_word, literal.view(),
          analysis_source_text(actx, pattern->source_location()),
          pattern->source_location(), tally);
    }
  }

  if (!tally.has_default_arm)
    actx.report_diagnostic(diagnostic_id::sc2249, case_input.case_location);

  check_case_option_coverage(actx, case_input, tally);

  /* An arm body runs conditionally and may reassign a name, so a value recorded
     before the case is no longer proven after it. */
  actx.constant_variables.clear();
}

BraceGroup::BraceGroup(SourceLocation location, const Expression *body)
    : CompoundCommand(steal(location)), m_body(body)
{}

BraceGroup::~BraceGroup() = default;

fn BraceGroup::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  return m_body != nullptr &&
         m_body->can_evaluate_in_process_substitution(cxt, active_functions);
}

cold fn BraceGroup::to_string() const throws -> String { return "BraceGroup"; }

cold fn BraceGroup::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

fn BraceGroup::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  cxt.set_terminal_exec_allowed(false);

  if (m_is_fully_eliminated) {
    cxt.publish_single_pipe_status(0);
    SET_AND_RETURN_EXIT_STATUS(cxt, 0);
  }

  return m_body->evaluate(cxt);
}

fn BraceGroup::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  m_body->analyze(actx, is_unconditional);
}

} /* namespace expressions */

} /* namespace koshka */
