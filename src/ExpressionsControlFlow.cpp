/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements asynchronous compound commands, if clauses, loops,
 * case clauses, brace groups, and coprocesses. It applies break, continue,
 * return, redirection, folding, and branch dataflow semantics across
 * control-flow nodes. The split keeps branch and loop behavior separate from
 * pipeline process machinery.
 */

#include "Builtin.hpp"
#include "CLI.hpp"
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
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace expressions {

using namespace internal;

static fn append_word_loop_header(EvalContext &cxt, String &header,
                                  StringView keyword, StringView variable_name,
                                  bool has_in_clause,
                                  const ArrayList<const Token *> &words) throws
    -> void
{
  if (!cxt.runtime_state().should_echo_expanded() &&
      !cxt.runtime_state().bash_dynamic_variables_enabled() &&
      !cxt.trap_store().has_debug_trap())
  {
    return;
  }

  header.append(keyword);
  header.push(' ');
  header.append(variable_name);

  if (!has_in_clause) {
    header += " in \"$@\"";
    return;
  }

  header += " in";
  for (usize index = 0; index < words.count(); index++) {
    ASSERT(words[index] != nullptr);
    header.push(' ');
    append_word_source_text(cxt, header, *words[index]);
  }
}

CompoundCommand::CompoundCommand(SourceLocation location)
    : Command(steal(location))
{}

static fn does_async_child_use_command_string_status(
    const EvalContext &cxt, const Command &command) wontthrow -> bool
{
  let const filename = command.source_location().get_filename();

  return (command.is_simple_command() || command.is_assignment()) &&
         cxt.execution_store().subshell_depth() == 0 &&
         cxt.runtime_state().is_bash_compatible() &&
         !cxt.runtime_state().error_exit() && filename.has_value() &&
         filename->data == COMMAND_STRING_SOURCE_NAME.data;
}

static fn async_error_status(const EvalContext &cxt, const ErrorBase &error,
                             bool should_use_command_string_status) wontthrow
    -> i32
{
  if (should_use_command_string_status && error.is_script_fatal() &&
      error.command_status() == 1)
  {
    return BASH_COMMAND_STRING_FATAL_STATUS;
  }

  if (error.is_script_fatal() && cxt.runtime_state().is_posix_mode()) return 2;

  return static_cast<i32>(error.command_status());
}

fn Command::evaluate_async_body(EvalContext &cxt) const throws -> i64
{
  return evaluate_impl(cxt);
}

fn Command::evaluate_async(EvalContext &cxt) const throws -> i64
{
  let const do_evaluate_body = [](void *context, EvalContext &body_cxt) -> i64 {
    return static_cast<const Command *>(context)->evaluate_async_body(body_cxt);
  };

  return evaluate_async_with(cxt, do_evaluate_body,
                             const_cast<Command *>(this));
}

fn Command::evaluate_async_with(EvalContext &cxt, async_body body,
                                void *context,
                                StringView expanded_child_source) const throws
    -> i64
{
  if (!cxt.job_table_store().jobs().is_empty())
    cxt.job_table_store().update_jobs();
  cxt.release_finished_coprocess();

  let const source_view = cxt.source_store().current_source_view();
  let const command_text =
      cxt.source_text_in_span(source_location(), source_end_position());

  let const child_source =
      expanded_child_source.is_empty() ? command_text : expanded_child_source;
  let bootstrap = os::subshell_bootstrap{};
  let const should_use_command_string_status =
      does_async_child_use_command_string_status(cxt, *this);
  let const evaluator = cxt.make_child_evaluator_state(bootstrap);
  bootstrap.should_use_command_string_status = should_use_command_string_status;
  cxt.set_child_source_origin(bootstrap, child_source, source_location());
  let const launch = os::launch_compound_stage(os::compound_stage_options{
      .source = child_source,
      .location = source_location(),
      .diagnostic_source = source_view,
      .evaluator = evaluator,
      .process_group = os::process_group_mode::NewBackground});
  let const child = launch.child;

  if (launch.should_evaluate_child) {
    i32 status = 1;
    let const do_run_exit_trap_after_error = [&](i32 error_status) {
      if ((!is_simple_command() && !is_assignment()) ||
          !cxt.runtime_state().is_bash_compatible())
      {
        return;
      }
      try {
        cxt.run_exit_trap(error_status & 0xFF);
      } catch (...) {
        LOG(Debug, "the async child failed to run the EXIT trap");
      }
    };
    try {
      cxt.enter_subshell();
      cxt.hide_coprocess_descriptors();
      cxt.job_table_store().inherit_parent_jobs(false);
      cxt.execution_store().allow_terminal_exec_at_current_depth();
      status = static_cast<i32>(body(context, cxt));
      if (cxt.control_flow_store().has_pending() &&
          cxt.control_flow_store().pending().kind == control_flow::Kind::Exit)
      {
        status = static_cast<i32>(cxt.control_flow_store().pending().value);
      }
    } catch (const BrokenPipeExit &) {
      status = KOSH_BROKEN_PIPE_EXIT_STATUS;
    } catch (const ErrorWithLocation &e) {
      if (!e.was_rendered()) {
        koshka::show_message(e.to_string(source_view, &cxt));
      }
      status = async_error_status(cxt, e, should_use_command_string_status);
      do_run_exit_trap_after_error(status);
    } catch (const Error &e) {
      if (!e.was_rendered()) koshka::show_message(e.to_string());
      status = async_error_status(cxt, e, should_use_command_string_status);
      do_run_exit_trap_after_error(status);
    } catch (...) {
      LOG(Debug, "the compound command child swallowed an unknown error");
    }
    koshka::flush();
    os::exit_process_immediately(status);
  }

  let const process_id = os::process_id_of(child);
  cxt.job_table_store().set_last_background_pid(process_id);
  let const id =
      cxt.job_table_store().register_job(child, command_text, process_id);
  if (cxt.execution_store().shell_is_interactive()) {
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

fn CompoundCommand::set_fully_eliminated() const wontthrow -> void
{
  set_execution_flag(ExecutionFlag::FullyEliminated);
}

pure fn CompoundCommand::is_fully_eliminated() const wontthrow -> bool
{
  return has_execution_flag(ExecutionFlag::FullyEliminated);
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

cold fn IfClause::to_string() const throws -> String
{
  let result = String{"IfClause"};
  append_ast_execution_flags(result);
  return result;
}

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
  return evaluate_status_impl(cxt).status;
}

hot fn IfClause::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  cxt.execution_store().terminal_exec_allowed() = false;
  let const should_skip_condition_commands = !folded_commands_are_observed(cxt);

  if (is_fully_eliminated() && should_skip_condition_commands) {
    LOG(Debug, "running the fully eliminated if as a no-op");
    cxt.publish_single_pipe_status(1);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  if (m_folded_branch.has_value() && should_skip_condition_commands) {
    LOG(Debug,
        "running the folded if branch %zu of %zu without testing conditions",
        *m_folded_branch, m_branches.count());
    if (*m_folded_branch < m_branches.count())
      return m_branches[*m_folded_branch].body->evaluate_status(cxt);
    if (m_otherwise != nullptr) return m_otherwise->evaluate_status(cxt);
    cxt.publish_single_pipe_status(1);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  for (let const &[ condition, body ] : m_branches) {
    ASSERT(condition != nullptr);
    ASSERT(body != nullptr);

    i64 condition_status;
    {
      cxt.execution_store().condition_depth()++;
      defer { cxt.execution_store().condition_depth()--; };
      condition_status = condition->evaluate(cxt);
    }

    if (cxt.control_flow_store().has_pending())
      return {static_cast<i32>(condition_status), 0};
    if (condition_status == 0) return body->evaluate_status(cxt);
  }

  if (m_otherwise != nullptr) return m_otherwise->evaluate_status(cxt);

  return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
}

fn IfClause::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  optimizer::optimize_node(this, actx);

  let saved_tested_command_names = actx.tested_command_names.clone();
  let condition_failure_names = saved_tested_command_names.clone();
  let merged_occurrences = variable_occurrence_pair{};
  let has_merged_occurrence_exit = false;
  let did_skip_exiting_branch = false;
  let is_first_branch = true;
  for (usize i = 0; i < m_branches.count(); i++) {
    let const & [ condition, body ] = m_branches[i];
    ASSERT(condition != nullptr);
    ASSERT(body != nullptr);

    actx.tested_command_names = condition_failure_names.clone();
    let const was_retaining_tested_command_names =
        actx.walk.should_retain_tested_command_names;
    actx.walk.should_retain_tested_command_names = true;
    let const was_analyzing_condition = actx.walk.is_analyzing_condition;
    actx.walk.is_analyzing_condition = true;
    if (!is_first_branch) actx.conditional_branch_depth++;
    condition->analyze(actx, is_unconditional && is_first_branch);
    if (!is_first_branch) actx.conditional_branch_depth--;
    actx.walk.is_analyzing_condition = was_analyzing_condition;
    actx.walk.should_retain_tested_command_names =
        was_retaining_tested_command_names;
    let const is_dead_branch =
        has_folded_branch() && folded_branch_index() != i;
    let condition_failure_occurrences = actx.occurrences.snapshot();
    let const was_silenced = actx.effects.should_silence_unresolved_commands;
    if (is_dead_branch) actx.effects.should_silence_unresolved_commands = true;
    actx.conditional_branch_depth++;
    body->analyze(actx, false);
    actx.conditional_branch_depth--;
    actx.effects.should_silence_unresolved_commands = was_silenced;

    let const is_exiting_branch = !is_dead_branch && body->always_exits(actx);
    if (is_exiting_branch) did_skip_exiting_branch = true;

    if (!is_dead_branch && !is_exiting_branch) {
      if (!has_merged_occurrence_exit) {
        merged_occurrences = steal(actx.occurrences);
        has_merged_occurrence_exit = true;
      } else {
        merged_occurrences.merge(actx.occurrences);
      }
    }

    actx.occurrences = steal(condition_failure_occurrences);

    actx.tested_command_names = condition_failure_names.clone();
    condition->append_presence_tested_command_names(
        actx, actx.tested_command_names, false);
    condition_failure_names = steal(actx.tested_command_names);
    is_first_branch = false;
  }

  let const else_is_dead =
      has_folded_branch() && folded_branch_index() != m_branches.count();
  let const was_else_silenced = actx.effects.should_silence_unresolved_commands;
  if (else_is_dead) actx.effects.should_silence_unresolved_commands = true;
  actx.tested_command_names = steal(condition_failure_names);
  actx.conditional_branch_depth++;
  if (m_otherwise != nullptr) m_otherwise->analyze(actx, false);
  actx.conditional_branch_depth--;
  actx.effects.should_silence_unresolved_commands = was_else_silenced;
  actx.tested_command_names = steal(saved_tested_command_names);

  let const is_exiting_else =
      m_otherwise != nullptr && m_otherwise->always_exits(actx);
  if (is_exiting_else) did_skip_exiting_branch = true;

  if (!else_is_dead && !is_exiting_else) {
    if (!has_merged_occurrence_exit) {
      merged_occurrences = steal(actx.occurrences);
      has_merged_occurrence_exit = true;
    } else {
      merged_occurrences.merge(actx.occurrences);
    }
  }

  if (has_merged_occurrence_exit || !did_skip_exiting_branch)
    actx.occurrences = steal(merged_occurrences);

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
                     const Expression *body, loop_kind kind)
    : CompoundCommand(steal(location)), m_condition(condition), m_body(body)
{
  set_execution_flag(ExecutionFlag::UntilLoop, kind == loop_kind::Until);
}

WhileLoop::~WhileLoop() = default;

cold fn WhileLoop::to_string() const throws -> String
{
  let result = String{is_until() ? "UntilLoop" : "WhileLoop"};
  append_ast_execution_flags(result);
  return result;
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

hot fn internal::resolve_loop_control(EvalContext &cxt) throws
    -> loop_disposition
{
  if (!cxt.runtime_state().is_posix_mode())
    cxt.job_table_store().forget_waited_jobs();
  cxt.release_finished_coprocess();

  if (!cxt.control_flow_store().has_pending()) return loop_disposition::RunNext;

  let &control = cxt.control_flow_store().pending();
  if (control.kind != control_flow::Kind::Break &&
      control.kind != control_flow::Kind::Continue)
  {
    return loop_disposition::StopLoop;
  }

  if (control.value > 1) {
    control.value -= 1;
    LOG(All, "the loop jump targets an outer loop, %lld levels stay pending",
        static_cast<long long>(control.value));
    return loop_disposition::StopLoop;
  }

  let const is_break = control.kind == control_flow::Kind::Break;
  cxt.control_flow_store().clear();
  LOG(All, "consuming the %s aimed at this loop",
      is_break ? "break" : "continue");
  return is_break ? loop_disposition::StopLoop : loop_disposition::RunNext;
}

hot fn WhileLoop::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  return evaluate_status_impl(cxt).status;
}

hot fn WhileLoop::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_body != nullptr);

  cxt.execution_store().terminal_exec_allowed() = false;

  let const is_until_loop = is_until();
  let const is_folded_to_skip = this->is_folded_to_skip();
  LOG(Debug, "entering the %s loop%s", is_until_loop ? "until" : "while",
      is_folded_to_skip ? ", folded to skip the body" : "");

  let const should_skip_condition_commands = !folded_commands_are_observed(cxt);
  if ((is_folded_to_skip || is_fully_eliminated()) &&
      should_skip_condition_commands)
  {
    cxt.publish_single_pipe_status(is_until_loop ? 0 : 1);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const redirect_fd_mark = cxt.mark_loop_redirect_fds();
  defer { cxt.cleanup_loop_redirect_fds(redirect_fd_mark); };

  status_result result{};
  loop
  {
    i64 condition_status;
    {
      cxt.execution_store().condition_depth()++;
      defer { cxt.execution_store().condition_depth()--; };
      condition_status = m_condition->evaluate(cxt);
    }
    if (cxt.runtime_state().no_exec()) break;
    if (cxt.control_flow_store().has_pending()) {
      if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
      continue;
    }

    let const should_run_body =
        is_until_loop ? (condition_status != 0) : (condition_status == 0);
    if (!should_run_body) break;

    result = m_body->evaluate_status(cxt);
    if (cxt.runtime_state().no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
  }

  if (cxt.control_flow_store().has_pending()) {
    result.status = cxt.execution_store().last_exit_status();
    return result;
  }

  cxt.execution_store().set_last_exit_status(result.status);
  return result;
}

fn WhileLoop::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_body != nullptr);

  actx.constant_variables.clear();

  optimizer::optimize_node(this, actx);

  let const saved_walk = actx.walk;
  let saved_tested_command_names = actx.tested_command_names.clone();
  actx.walk.is_inside_loop_condition = true;
  actx.walk.has_input_reading_loop_condition = false;
  actx.walk.should_retain_tested_command_names = true;
  let const saved_getopts = actx.active_getopts;
  actx.active_getopts = {};
  actx.walk.is_analyzing_condition = true;
  actx.loop_body_depth++;
  m_condition->analyze(actx, is_unconditional);
  actx.loop_body_depth--;
  let const has_input_reading_loop_condition =
      actx.walk.has_input_reading_loop_condition;
  actx.walk = saved_walk;

  if (is_until()) {
    actx.tested_command_names = saved_tested_command_names.clone();
    m_condition->append_presence_tested_command_names(
        actx, actx.tested_command_names, false);
  }

  let condition_occurrences = actx.occurrences.snapshot();
  let const was_silenced = actx.effects.should_silence_unresolved_commands;
  if (has_input_reading_loop_condition) actx.walk.is_inside_read_loop = true;
  if (is_folded_to_skip())
    actx.effects.should_silence_unresolved_commands = true;
  actx.loop_body_depth++;
  actx.conditional_branch_depth++;
  m_body->analyze(actx, false);
  actx.conditional_branch_depth--;
  actx.loop_body_depth--;

  condition_occurrences.merge(actx.occurrences);
  actx.occurrences = steal(condition_occurrences);
  actx.walk.is_inside_read_loop = saved_walk.is_inside_read_loop;
  actx.effects.should_silence_unresolved_commands = was_silenced;
  actx.tested_command_names = steal(saved_tested_command_names);
  actx.active_getopts = saved_getopts;
}

pure fn WhileLoop::condition() const wontthrow -> const Expression *
{
  return m_condition;
}

pure fn WhileLoop::is_until() const wontthrow -> bool
{
  return has_execution_flag(ExecutionFlag::UntilLoop);
}

fn WhileLoop::set_folded_to_skip() const wontthrow -> void
{
  set_execution_flag(ExecutionFlag::FoldedLoopToSkip);
}

pure fn WhileLoop::is_folded_to_skip() const wontthrow -> bool
{
  return has_execution_flag(ExecutionFlag::FoldedLoopToSkip);
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
  let result = "SelectLoop \"" + m_variable_name + "\"";
  append_ast_execution_flags(result);
  return result;
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
  return evaluate_status_impl(cxt).status;
}

fn SelectLoop::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_body != nullptr);

  cxt.execution_store().terminal_exec_allowed() = false;
  cxt.source_store().set_current_location(source_location());

  let const values = m_has_in_clause ? cxt.process_args(m_words)
                                     : cxt.variable_store().positional_params();

  let select_trace = String{cxt.scratch_allocator()};
  append_word_loop_header(cxt, select_trace, "select", m_variable_name,
                          m_has_in_clause, m_words);
  let const should_run_select = publish_command_and_run_debug_trap(
      cxt, [&] { return String{heap_allocator(), select_trace.view()}; });
  if (!should_run_select) return {cxt.execution_store().last_exit_status()};

  cxt.write_xtrace(select_trace.view());

  if (values.is_empty()) return {};

  LOG(Debug, "the select loop offers %zu choices for '%.*s'", values.count(),
      static_cast<int>(m_variable_name.length), m_variable_name.data);

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const redirect_fd_mark = cxt.mark_loop_redirect_fds();
  defer { cxt.cleanup_loop_redirect_fds(redirect_fd_mark); };

  status_result result{};
  bool should_reprint_menu = true;
  loop
  {
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

    let const input = utils::read_line_from_fd(KOSH_STDIN);
    if (!input.line.has_value()) {
      koshka::print("\n");
      result.status = 1;
      break;
    }

    let const &reply = *input.line;
    LOG(All, "the select prompt read the reply '%s'", reply.c_str());
    cxt.set_shell_variable("REPLY", reply.view());
    if (reply.is_empty()) {
      should_reprint_menu = true;
      continue;
    }

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

    result = m_body->evaluate_status(cxt);
    if (cxt.runtime_state().no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
  }

  if (cxt.control_flow_store().has_pending()) {
    result.status = cxt.execution_store().last_exit_status();
    return result;
  }

  cxt.execution_store().set_last_exit_status(result.status);
  return result;
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
  append_ast_execution_flags(result);
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
  return evaluate_status_impl(cxt).status;
}

hot fn ForLoop::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_body != nullptr);

  cxt.execution_store().terminal_exec_allowed() = false;

  if (is_fully_eliminated()) {
    cxt.publish_single_pipe_status(0);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  cxt.source_store().set_current_location(source_location());
  let const substitution_mark = cxt.mark_process_substitutions();
  defer { cxt.cleanup_process_substitutions(substitution_mark); };
  let const values = m_has_in_clause ? cxt.process_args(m_words)
                                     : cxt.variable_store().positional_params();

  let const scope_variable = !(cxt.runtime_state().is_bash_compatible() ||
                               cxt.runtime_state().is_posix_mode());
  Maybe<String> saved_value =
      scope_variable ? cxt.get_variable_value(m_variable_name) : None;
  defer
  {
    if (scope_variable && !cxt.is_readonly(m_variable_name)) {
      try {
        if (saved_value.has_value())
          cxt.set_shell_variable(m_variable_name, saved_value->view());
        else
          cxt.unset_shell_variable(m_variable_name);
      } catch (...) {
        LOG(Debug, "restoring the for loop variable failed and was swallowed");
      }
    }
  };

  LOG(Debug, "the for loop binds '%.*s' over %zu values",
      static_cast<int>(m_variable_name.length), m_variable_name.data,
      values.count());

  let loop_trace = String{cxt.scratch_allocator()};
  append_word_loop_header(cxt, loop_trace, "for", m_variable_name,
                          m_has_in_clause, m_words);

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const redirect_fd_mark = cxt.mark_loop_redirect_fds();
  defer { cxt.cleanup_loop_redirect_fds(redirect_fd_mark); };

  status_result result{};
  for (let const &value : values) {
    cxt.source_store().set_current_location(source_location());

    let const should_run_iteration = publish_command_and_run_debug_trap(
        cxt, [&] { return String{heap_allocator(), loop_trace.view()}; });
    if (!should_run_iteration) {
      if (cxt.control_flow_store().has_pending()) break;

      result.status = cxt.trap_store().last_trap_action_status();
      continue;
    }

    cxt.write_xtrace(loop_trace.view());
    try {
      if (cxt.variable_store().attributes().is_nameref(m_variable_name)) rarely
        {
          cxt.bind_nameref(m_variable_name, value);
        }
      else
        cxt.set_shell_variable(m_variable_name, value);
    } catch (ErrorBase &error) {
      cxt.mark_expansion_error(error,
                               expansion_error_reach::CommandOrPosixScript);
      relocate_if_unlocated(error, source_location());
    }
    result = m_body->evaluate_status(cxt);
    if (cxt.runtime_state().no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
  }

  if (cxt.control_flow_store().has_pending()) {
    result.status = cxt.execution_store().last_exit_status();
    return result;
  }

  cxt.execution_store().set_last_exit_status(result.status);
  return result;
}

fn ForLoop::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  ASSERT(m_body != nullptr);

  analyze_token_list_substitutions(actx, m_words, is_unconditional);

  let loop_entry_occurrences = actx.occurrences.snapshot();

  let const outer_loop_location =
      actx.active_loop_variables.find(m_variable_name);
  if (outer_loop_location.has_value()) {
    actx.report_diagnostic(diagnostic_id::sc2165, m_variable_location,
                           {m_variable_name}, *outer_loop_location.value());
    actx.report_diagnostic(diagnostic_id::sc2167, *outer_loop_location.value(),
                           {m_variable_name}, m_variable_location);
  }

  let const had_outer_loop_variable = outer_loop_location.has_value();
  let saved_outer_loop_location = SourceLocation{};
  if (had_outer_loop_variable)
    saved_outer_loop_location = *outer_loop_location.value();
  actx.active_loop_variables.set(m_variable_name, m_variable_location);
  defer
  {
    if (had_outer_loop_variable)
      actx.active_loop_variables.set(m_variable_name,
                                     saved_outer_loop_location);
    else
      actx.active_loop_variables.erase(m_variable_name);
  };

  let const word_list_holds_one_word = m_has_in_clause && m_words.count() == 1;

  for (let const t : m_words) {
    if (t->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(t)->word();
    let const source_text = analysis_source_text(actx, t->source_location());

    check_posix_word_portability(actx, word, t->source_location());

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
                             actx.effects.has_seen_runtime_definer ||
                             (m_has_in_clause && m_words.is_empty());
  actx.note_variable_occurrence(m_variable_name, m_variable_location,
                                variable_occurrence_kind::Assignment,
                                is_conditional);
  actx.note_variable_binding_record(m_variable_name, m_variable_location,
                                    assignment_binder::ForLoop, is_conditional);

  optimizer::optimize_node(this, actx);

  actx.constant_variables.clear();
  actx.loop_body_depth++;
  actx.conditional_branch_depth++;
  m_body->analyze(actx, false);
  actx.conditional_branch_depth--;
  actx.loop_body_depth--;

  loop_entry_occurrences.merge(actx.occurrences);
  actx.occurrences = steal(loop_entry_occurrences);
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

cold fn CaseClause::to_string() const throws -> String
{
  let result = String{"CaseClause"};
  append_ast_execution_flags(result);
  return result;
}

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
  return evaluate_status_impl(cxt).status;
}

fn CaseClause::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_word != nullptr);

  cxt.execution_store().terminal_exec_allowed() = false;
  cxt.source_store().set_current_location(source_location());

  let const should_run_case = publish_command_and_run_debug_trap(cxt, [&] {
    let header_text = String{heap_allocator(), "case "};
    append_word_source_text(cxt, header_text, *m_word);
    header_text += " in ";
    return header_text;
  });
  if (!should_run_case) return {cxt.execution_store().last_exit_status()};

  let const substitution_mark = cxt.mark_process_substitutions();
  defer { cxt.cleanup_process_substitutions(substitution_mark); };

  let const do_expand_no_glob = [&cxt](const Token *t) -> String {
    ASSERT(t != nullptr);
    if (t->kind() == Token::Kind::Word) {
      try {
        return cxt.expand_word_for_assignment(
            static_cast<const tokens::WordToken *>(t)->word());
      } catch (const Error &e) {
        relocate_if_unlocated(e, t->source_location());
      }
    }
    return t->raw_string();
  };

  let const subject = do_expand_no_glob(m_word);

  LOG(Debug, "the case subject expanded to '%s'", subject.c_str());

  let const extglob = cxt.get_extglob_mode();
  let const charset = cxt.get_glob_charset_for(subject.view());
  let const is_case_insensitive = cxt.is_shopt_enabled("nocasematch");
  let const folded_subject =
      is_case_insensitive ? utils::lowercase_for_glob(subject.view(), charset,
                                                      cxt.scratch_allocator())
                          : String{cxt.scratch_allocator()};

  let const do_arm_matches = [&](const case_item &item) throws -> bool {
    for (let const pattern_token : item.patterns) {
      if (pattern_token->kind() == Token::Kind::Word) {
        const Word &pattern_word =
            static_cast<const tokens::WordToken *>(pattern_token)->word();
        if (!is_case_insensitive &&
            pattern_word.plain_literal_kind() != Word::PlainLiteral::NotPlain)
        {
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
        } catch (const Error &e) {
          relocate_if_unlocated(e, pattern_token->source_location());
        }
      } else {
        pattern = pattern_token->raw_string();
        for (usize k = 0; k < pattern.count(); k++)
          pattern_active.push(true);
      }
      if (is_case_insensitive) {
        let const folded_pattern = utils::lowercase_for_glob(
            pattern.view(), cxt.get_glob_charset_for(pattern.view()),
            cxt.scratch_allocator());
        if (utils::glob_matches(folded_pattern.view(), folded_subject.view(),
                                pattern_active, 0, extglob, charset))
        {
          return true;
        }
        continue;
      }

      if (utils::glob_matches(pattern, subject, pattern_active, 0, extglob,
                              charset))
        return true;
    }
    return false;
  };

  status_result result{};
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
      result = m_items[i].body->evaluate_status(cxt);
      cxt.execution_store().set_last_exit_status(result.status);
      did_run_a_body = true;
      if (cxt.control_flow_store().has_pending()) return result;

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
    cxt.execution_store().set_last_exit_status(0);
  }
  return result;
}

fn CaseClause::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  analyze_token_substitutions(actx, m_word, is_unconditional);
  for (let const &item : m_items) {
    analyze_token_list_substitutions(actx, item.patterns, is_unconditional);
  }

  let const common_occurrences = actx.occurrences.snapshot();
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
  let merged_occurrences = common_occurrences;
  let continued_occurrences = common_occurrences;
  let has_merged_occurrence_exit = !has_unquoted_default_pattern;
  let has_continued_occurrence_path = false;

  ASSERT(m_word != nullptr);
  if (m_word->kind() == Token::Kind::Word) {
    let const &case_word =
        static_cast<const tokens::WordToken *>(m_word)->word();
    check_posix_word_portability(actx, case_word, m_word->source_location());

    for (let const &segment : case_word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;
      note_variable_reference(actx, segment, m_word->source_location());
    }
  }

  for (usize i = 0; i < m_items.count(); i++) {
    let const &item = m_items[i];
    ASSERT(item.body != nullptr);

    actx.occurrences = common_occurrences;
    if (has_continued_occurrence_path)
      actx.occurrences.merge(continued_occurrences);

    for (let const pattern : item.patterns) {
      if (pattern->kind() != Token::Kind::Word) continue;
      let const &pattern_word =
          static_cast<const tokens::WordToken *>(pattern)->word();
      check_posix_word_portability(actx, pattern_word,
                                   pattern->source_location());

      for (let const &segment : pattern_word.segments) {
        if (segment.kind != WordSegment::Kind::VariableReference) continue;
        note_variable_reference(actx, segment, pattern->source_location());
      }
    }

    actx.conditional_branch_depth++;
    item.body->analyze(actx, false);
    actx.conditional_branch_depth--;

    let const has_later_item = i + 1 < m_items.count();
    let const is_exiting_item = item.body->always_exits(actx);
    if (!is_exiting_item &&
        (item.terminator != case_terminator::FallThrough || !has_later_item))
    {
      if (!has_merged_occurrence_exit) {
        merged_occurrences = actx.occurrences.snapshot();
        has_merged_occurrence_exit = true;
      } else {
        merged_occurrences.merge(actx.occurrences);
      }
    }

    has_continued_occurrence_path = !is_exiting_item && has_later_item &&
                                    item.terminator != case_terminator::Break;
    if (has_continued_occurrence_path)
      continued_occurrences = actx.occurrences.snapshot();
  }

  actx.occurrences = steal(merged_occurrences);

  let case_input = case_lint_input{};
  case_input.case_location = m_word->source_location();
  case_input.case_word_source =
      analysis_source_text(actx, case_input.case_location);

  if (m_word->kind() == Token::Kind::Word) {
    case_input.case_word =
        &static_cast<const tokens::WordToken *>(m_word)->word();
    check_case_word_shape(actx, case_input);

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
      if (let const earlier_location =
              earlier_patterns.find(raw_pattern.view());
          earlier_location.has_value())
      {
        actx.report_diagnostic(diagnostic_id::sc2221,
                               pattern->source_location(), {},
                               *earlier_location.value());
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

  actx.constant_variables.clear();
}

BraceGroup::BraceGroup(SourceLocation location, const Expression *body)
    : CompoundCommand(steal(location)), m_body(body)
{}

BraceGroup::~BraceGroup() = default;

fn BraceGroup::always_exits(const AnalysisContext &actx) const wontthrow -> bool
{
  return m_body->always_exits(actx);
}

fn BraceGroup::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  return m_body->can_evaluate_in_process_substitution(cxt, active_functions);
}

cold fn BraceGroup::to_string() const throws -> String
{
  let result = String{"BraceGroup"};
  append_ast_execution_flags(result);
  return result;
}

cold fn BraceGroup::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

fn BraceGroup::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  return evaluate_status_impl(cxt).status;
}

fn BraceGroup::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_body != nullptr);

  cxt.execution_store().terminal_exec_allowed() = false;

  if (is_fully_eliminated()) {
    cxt.publish_single_pipe_status(0);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  return m_body->evaluate_status(cxt);
}

fn BraceGroup::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  m_body->analyze(actx, is_unconditional);
}

CoprocCommand::CoprocCommand(SourceLocation location, StringView name,
                             const Expression *body)
    : CompoundCommand(steal(location)), m_name(name), m_body(body)
{}

CoprocCommand::~CoprocCommand() = default;

cold fn CoprocCommand::to_string() const throws -> String
{
  let result = String{"Coproc "};
  result += m_name;
  append_ast_execution_flags(result);
  return result;
}

cold fn CoprocCommand::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

fn CoprocCommand::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  m_body->analyze(actx, is_unconditional);
}

static constexpr i32 COPROCESS_FD_FLOOR = 10;

fn CoprocCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  cxt.release_finished_coprocess();

  let const source_view = cxt.source_store().current_source_view();
  let const command_text =
      cxt.source_text_in_span(source_location(), source_end_position());

  let const body_location = m_body->source_location();
  let const body_text =
      os::can_fork_evaluator() ? StringView{} : full_source_text(cxt, *m_body);

  let toward_child = os::make_pipe();
  if (!toward_child.has_value()) {
    throw ErrorWithLocation{source_location(),
                            "Could not open a coprocess pipe"};
  }

  let away_from_child = os::make_pipe();
  if (!away_from_child.has_value()) {
    os::close_fd(toward_child->in);
    os::close_fd(toward_child->out);
    throw ErrorWithLocation{source_location(),
                            "Could not open a coprocess pipe"};
  }

  LOG(Debug, "launching the coprocess '%.*s'", static_cast<int>(m_name.length),
      m_name.data);

  let bootstrap = os::subshell_bootstrap{};
  let const evaluator = cxt.make_child_evaluator_state(bootstrap);
  cxt.set_child_source_origin(bootstrap, body_text, body_location);

  let const launch = os::launch_compound_stage(os::compound_stage_options{
      .source = body_text,
      .in_fd = toward_child->in,
      .out_fd = away_from_child->out,
      .location = source_location(),
      .diagnostic_source = source_view,
      .evaluator = evaluator,
      .process_group = os::process_group_mode::NewBackground});
  let const child = launch.child;

  if (launch.should_evaluate_child) {
    os::close_fd(toward_child->out);
    os::close_fd(away_from_child->in);

    i32 status = 1;
    try {
      cxt.enter_subshell();
      cxt.job_table_store().inherit_parent_jobs(false);
      status = static_cast<i32>(m_body->evaluate(cxt));
      if (cxt.control_flow_store().has_pending() &&
          cxt.control_flow_store().pending().kind == control_flow::Kind::Exit)
      {
        status = static_cast<i32>(cxt.control_flow_store().pending().value);
      }
    } catch (const BrokenPipeExit &) {
      status = KOSH_BROKEN_PIPE_EXIT_STATUS;
    } catch (const ErrorWithLocation &e) {
      if (!e.was_rendered()) {
        koshka::show_message(e.to_string(source_view, &cxt));
      }
      status = static_cast<i32>(e.command_status());
    } catch (const Error &e) {
      koshka::show_message(e.to_string());
      status = static_cast<i32>(e.command_status());
    } catch (...) {
      LOG(Debug, "the coprocess child swallowed an unknown error");
    }
    koshka::flush();
    os::exit_process_immediately(status);
  }

  os::close_fd(toward_child->in);
  os::close_fd(away_from_child->out);

  let const read_fd = os::move_descriptor_to_free_shell_fd(away_from_child->in,
                                                           COPROCESS_FD_FLOOR);
  let const write_fd = os::move_descriptor_to_free_shell_fd(toward_child->out,
                                                            COPROCESS_FD_FLOOR);
  if (read_fd < 0 || write_fd < 0) {
    if (read_fd < 0)
      os::close_fd(away_from_child->in);
    else
      os::close_shell_fd(read_fd);

    if (write_fd < 0)
      os::close_fd(toward_child->out);
    else
      os::close_shell_fd(write_fd);

    throw ErrorWithLocation{source_location(),
                            "Could not place the coprocess descriptors"};
  }

  let const process_id = os::process_id_of(child);
  cxt.set_coprocess_descriptors(read_fd, write_fd, process_id, m_name);

  let descriptors = ArrayList<String>{heap_allocator()};
  descriptors.push(String::from(read_fd, heap_allocator()));
  descriptors.push(String::from(write_fd, heap_allocator()));
  cxt.set_indexed_array(m_name, steal(descriptors));

  let pid_name = String{m_name};
  pid_name += "_PID";
  cxt.set_shell_variable(
      pid_name.view(),
      String::from(static_cast<u64>(process_id), heap_allocator()));

  cxt.job_table_store().set_last_background_pid(process_id);

  let const id =
      cxt.job_table_store().register_job(child, command_text, process_id);
  if (cxt.execution_store().shell_is_interactive()) {
    koshka::print_error(
        "[" + String::from(id, heap_allocator()) + "] " +
        String::from(static_cast<u64>(process_id), heap_allocator()) + "\n");
  }

  cxt.publish_single_pipe_status(0);
  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

} /* namespace expressions */

} /* namespace koshka */
