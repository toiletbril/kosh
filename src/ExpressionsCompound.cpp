/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file evaluates compound lists, and-or conditions, and pipelines. It
 * owns pipeline stage setup, compound-stage children, asynchronous job
 * registration, pipe status collection, negation, errexit handling, and
 * result propagation. The split confines pipeline process machinery outside
 * branch and loop behavior.
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

pure fn internal::full_source_text(const EvalContext &cxt,
                                   const Expression &node) wontthrow
    -> StringView
{
  let const location = node.source_location();
  let span = SourceLocation{location.position, 0, location.source_name_index};
  let end_position = usize{location.position} + usize{location.length};
  if (node.source_end_position() > end_position)
    end_position = node.source_end_position();

  if (let const *simple = node.as_simple_command(); simple != nullptr) {
    span.position = static_cast<u32>(simple->full_source_start_position());
    if (simple->full_source_end_position() > end_position)
      end_position = simple->full_source_end_position();
  }

  return cxt.source_text_in_span(span, end_position);
}

static fn is_line_discard_root(EvalContext &cxt, const ErrorBase &error,
                               const Expression *list) wontthrow -> bool
{
  if (error.is_top_level_line_discarding()) {
    return cxt.execution_store().top_level_line_discard_root() == list;
  }

  return cxt.execution_store().line_discard_root() == list;
}

CompoundList::CompoundList() : Expression({0, 0}) {}

CompoundList::~CompoundList() = default;

fn CompoundList::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  for (let const node : m_nodes)
    if (!node->can_evaluate_in_process_substitution(cxt, active_functions))
      return false;

  return true;
}

pure fn CompoundList::is_empty() const wontthrow -> bool
{
  return m_nodes.is_empty();
}

fn CompoundList::append_node(const CompoundListCondition *node) throws -> void
{
  ASSERT(node != nullptr);

  m_location.length += node->source_location().length;
  m_nodes.push(node);
}

pure fn CompoundList::node_count() const wontthrow -> usize
{
  return m_nodes.count();
}

fn CompoundList::move_nodes_from(usize first_index,
                                 CompoundList &destination) throws -> void
{
  ASSERT(first_index <= m_nodes.count());

  for (usize index = first_index; index < m_nodes.count(); index++) {
    m_location.length -= m_nodes[index]->source_location().length;
    destination.append_node(m_nodes[index]);
  }
  m_nodes.truncate(first_index);
}

fn CompoundList::single_unconditional_command() const wontthrow
    -> const Command *
{
  if (m_nodes.count() != 1) return nullptr;

  let const *node = m_nodes[0];
  if (node == nullptr) return nullptr;

  if (node->kind() != CompoundListCondition::Kind::None || node->is_negated()) {
    return nullptr;
  }

  let const *command = node->command();
  if (command == nullptr) return nullptr;

  if (command->is_async() || command->is_timed()) return nullptr;

  return command;
}

cold fn CompoundList::to_string() const throws -> String
{
  return "CompoundList";
}

cold fn CompoundList::to_ast_string(usize layer) const throws -> String
{
  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[" + to_string() + "]";
  for (let const n : m_nodes) {
    s += '\n';
    s += pad + EXPRESSION_AST_INDENT + n->to_ast_string(layer + 1);
  }

  return s;
}

hot fn CompoundList::evaluate_impl(EvalContext &cxt,
                                   root_evaluation_mode mode) const throws
    -> status_result
{
  ASSERT(m_nodes.count() > 0);

  static const i32 NOTHING_WAS_EXECUTED = -256;

  status_result ret{NOTHING_WAS_EXECUTED, 0};

  let const was_terminal_exec_allowed =
      cxt.execution_store().terminal_exec_allowed();
  cxt.execution_store().terminal_exec_allowed() = false;
  defer
  {
    cxt.execution_store().terminal_exec_allowed() = was_terminal_exec_allowed;
  };

  bool is_discarding_line = false;
  bool did_discard_line = false;
  usize discarded_separator_position = 0;

  for (usize index = 0; index < m_nodes.count(); index++) {
    if (cxt.runtime_state().no_exec()) break;

    if (cxt.control_flow_store().has_pending_loop_jump()) break;

    const CompoundListCondition *n = m_nodes[index];
    ASSERT(n != nullptr);

    let const do_starts_new_line = [&](usize separator_position)
                                       wontthrow -> bool {
      let const source = cxt.execution_store().line_discard_source();
      let const command_position = n->command()->source_location().position;
      return n->kind() == CompoundListCondition::Kind::None &&
             separator_position < command_position &&
             command_position <= source.length &&
             source
                 .substring_of_length(separator_position,
                                      command_position - separator_position)
                 .find_character('\n')
                 .has_value();
    };

    if (cxt.execution_store().line_discard_root() == this &&
        n->kind() == CompoundListCondition::Kind::None &&
        !cxt.runtime_state().is_posix_mode())
    {
      if (index == 0 ||
          do_starts_new_line(m_nodes[index - 1]->source_location().position))
      {
        cxt.job_table_store().forget_waited_jobs();
        cxt.release_finished_coprocess();
      }
    }

    if (is_discarding_line) {
      if (!do_starts_new_line(discarded_separator_position)) {
        discarded_separator_position = n->source_location().position;
        continue;
      }

      is_discarding_line = false;
    }

    if (n->kind() == CompoundListCondition::Kind::None) {
      if (let const history_source =
              cxt.history_recorder().find_source_for(this);
          history_source.has_value())
      {
        usize history_end_index = index;
        while (history_end_index + 1 < m_nodes.count() &&
               m_nodes[history_end_index + 1]->kind() !=
                   CompoundListCondition::Kind::None)
        {
          history_end_index++;
        }

        let const first_command = n->command();
        let const start_position = first_command->source_location().position;
        let const end_position =
            m_nodes[history_end_index]->source_end_position();
        if (start_position < end_position &&
            end_position <= history_source->length)
        {
          if (!cxt.record_history_event(history_source->substring_of_length(
                  start_position, end_position - start_position)))
          {
            throw ErrorWithLocation{
                first_command->source_location(),
                "Unable to record the command because the history file "
                "rejected the entry"};
          }
        }
      }
    }

    let const is_last_node = index + 1 >= m_nodes.count();
    cxt.execution_store().terminal_exec_allowed() =
        was_terminal_exec_allowed && is_last_node;

    bool did_execute = false;
    const bool is_end_of_and_or_chain =
        index + 1 >= m_nodes.count() ||
        m_nodes[index + 1]->kind() == CompoundListCondition::Kind::None;
    const bool should_ignore_errexit =
        !is_end_of_and_or_chain || n->is_negated();
    const bool was_err_trapped = cxt.trap_store().has_err_trap();
    let const is_async_node = n->command()->is_async();
    let const is_contained_async_node =
        is_async_node && cxt.runtime_state().get_mood() != mimic_mood::Default;
    let const do_run_node = [&]() throws -> status_result {
      if (should_ignore_errexit) cxt.execution_store().condition_depth()++;
      defer
      {
        if (should_ignore_errexit) cxt.execution_store().condition_depth()--;
      };
      try {
        let const node_mode = index == 0 ? mode : root_evaluation_mode::Normal;
        return n->evaluate_root_status(cxt, node_mode);
      } catch (const InterruptErrorWithLocation &) {
        throw;
      } catch (ErrorWithLocation &error) {
        if (!is_contained_async_node &&
            (!cxt.runtime_state().is_bash_compatible() ||
             error.is_script_fatal()))
        {
          throw;
        }
        LOG(Debug,
            "bash mood converted the located error to command status %lld: %s",
            static_cast<long long>(error.command_status()),
            error.message().c_str());
        if (!error.was_rendered()) {
          let const trace_location = error.location();
          if (let const windowed = window_function_body_error(cxt, error);
              windowed.has_value())
          {
            show_message(error.to_string(*windowed, &cxt));
          } else {
            show_message(error.to_string(
                cxt.source_store().current_source_view(), &cxt));
          }
          cxt.print_source_backtrace(trace_location, false);
          error.set_rendered();
        }
        if (error.is_line_discarding() && !is_async_node) {
          if (!is_line_discard_root(cxt, error, this)) throw;
          did_discard_line = true;

          return {static_cast<i32>(set_and_return_exit_status(
                      cxt, cxt.execution_store().line_discard_status().value_or(
                               error.command_status()))),
                  0};
        }

        return {static_cast<i32>(set_and_return_exit_status(
                    cxt, is_async_node ? 0 : error.command_status())),
                0};
      } catch (ErrorBase &error) {
        if (!is_contained_async_node &&
            (!cxt.runtime_state().is_bash_compatible() ||
             error.is_script_fatal()))
        {
          throw;
        }
        LOG(Debug, "bash mood converted the error to command status %lld: %s",
            static_cast<long long>(error.command_status()),
            error.message().c_str());
        if (!error.was_rendered()) {
          show_message(
              error.to_string(cxt.source_store().current_source_view(), &cxt));
          error.set_rendered();
        }
        if (error.is_line_discarding() && !is_async_node) {
          if (!is_line_discard_root(cxt, error, this)) throw;
          did_discard_line = true;

          return {static_cast<i32>(set_and_return_exit_status(
                      cxt, cxt.execution_store().line_discard_status().value_or(
                               error.command_status()))),
                  0};
        }

        return {static_cast<i32>(set_and_return_exit_status(
                    cxt, is_async_node ? 0 : error.command_status())),
                0};
      }
    };
    switch (n->kind()) {
    case CompoundListCondition::Kind::None:
      ret = do_run_node();
      did_execute = true;
      break;

    case CompoundListCondition::Kind::Or:
      if (ret.status != 0) {
        ret = do_run_node();
        did_execute = true;
      }
      break;

    case CompoundListCondition::Kind::And:
      if (ret.status == 0) {
        ret = do_run_node();
        did_execute = true;
      }
      break;
    }

    if (did_discard_line) {
      did_discard_line = false;
      is_discarding_line = true;
      discarded_separator_position = n->source_location().position;
      continue;
    }

    const bool has_pending_control_flow =
        cxt.control_flow_store().has_pending();
    const bool was_command_failure_uncaught =
        !has_pending_control_flow &&
        cxt.execution_store().condition_depth() == 0 && did_execute &&
        !n->is_negated() && is_end_of_and_or_chain && ret.status != 0 &&
        ret.status != NOTHING_WAS_EXECUTED &&
        !ret.has(status_flag::ErrResolved);
    const bool is_fatal_exit =
        cxt.runtime_state().error_exit() && was_command_failure_uncaught;

    const bool is_reportable_status =
        did_execute && ret.status != NOTHING_WAS_EXECUTED &&
        !ret.has(status_flag::ExitCodeReported) &&
        (ret.status != 0 || cxt.runtime_state().show_all_exit_codes());

    if (cxt.runtime_state().show_exit_code() && is_reportable_status) {
      let message =
          String{cxt.scratch_allocator(),
                 ret.status != 0 ? "Non-zero exit code (" : "Exit code ("};
      message += String::from(ret.status, cxt.scratch_allocator());
      message += ')';
      if (is_fatal_exit) {
        cxt.show_runtime_error_at(n->command()->source_location(),
                                  message.view());
      } else {
        cxt.show_runtime_warning_at(n->command()->source_location(),
                                    message.view(), {}, true);
      }
      ret.set(status_flag::ExitCodeReported);
    }

    if (has_pending_control_flow) break;

    if (was_command_failure_uncaught && !cxt.runtime_state().is_posix_mode()) {
      cxt.execution_store().set_last_exit_status(ret.status);
      if (was_err_trapped && cxt.should_run_err_trap()) {
        let const failed_location = n->command()->error_report_location();
        cxt.run_named_trap(StringView{"ERR", 3}, &failed_location);
      }

      if (cxt.control_flow_store().has_pending()) {
        ret.set(status_flag::ErrResolved);
        break;
      }
    }

    if (was_command_failure_uncaught && cxt.runtime_state().error_exit()) {
      cxt.execution_store().set_last_exit_status(ret.status);
      if (cxt.in_subshell()) {
        cxt.request_exit(ret.status, source_location());
        break;
      }
      cxt.run_exit_trap(ret.status & 0xFF);
      utils::quit(ret.status, utils::farewell_policy::Goodbye);
    }

    if (did_execute && ret.status != 0) ret.set(status_flag::ErrResolved);
  }

  if (ret.status == NOTHING_WAS_EXECUTED) ret.status = 0;

  return ret;
}

CompoundListCondition::CompoundListCondition(SourceLocation location, Kind kind,
                                             const Command *expr)
    : Expression(steal(location)), m_kind(kind), m_cmd(expr)
{
  ASSERT(m_cmd != nullptr);
}

CompoundListCondition::~CompoundListCondition() = default;

fn CompoundListCondition::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  return !m_cmd->is_async() &&
         m_cmd->can_evaluate_in_process_substitution(cxt, active_functions);
}

pure fn CompoundListCondition::kind() const wontthrow -> Kind { return m_kind; }

pure fn CompoundListCondition::command() const wontthrow -> const Command *
{
  return m_cmd;
}

pure fn CompoundListCondition::is_negated() const wontthrow -> bool
{
  ASSERT(m_cmd != nullptr);
  return m_cmd->is_negated();
}

cold fn CompoundListCondition::to_string() const throws -> String
{
  String k{heap_allocator()};
  switch (kind()) {
  case Kind::None: k = "None"; break;
  case Kind::And: k = "&&"; break;
  case Kind::Or: k = "||"; break;
  default: unreachable("invalid compound-list condition kind %d", ENUM(kind()));
  }
  return "CompoundListCondition, " + k;
}

cold fn CompoundListCondition::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_cmd != nullptr);

  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[" + to_string() + "]\n";
  s += pad + EXPRESSION_AST_INDENT + m_cmd->to_ast_string(layer + 1);

  return s;
}

hot fn CompoundListCondition::evaluate_impl(EvalContext &cxt,
                                            root_evaluation_mode mode) const
    throws -> status_result
{
  ASSERT(m_cmd != nullptr);
  cxt.evaluation_metrics_store().begin_command_evaluation();

  if (m_cmd->is_negated() || m_cmd->is_timed()) {
    cxt.execution_store().terminal_exec_allowed() = false;
  }

  double user_before = 0.0;
  double system_before = 0.0;
  u64 start_nanos = 0;
  if (m_cmd->is_timed()) {
    let const child_times = os::read_child_cpu_times();
    user_before = child_times.user_seconds;
    system_before = child_times.system_seconds;
    start_nanos = os::monotonic_nanos();
  }

  let result = m_cmd->evaluate_root_status(cxt, mode);

  if (m_cmd->is_timed()) {
    let const elapsed_nanos = os::monotonic_nanos() - start_nanos;
    double user_after = 0.0;
    double system_after = 0.0;
    let const child_times = os::read_child_cpu_times();
    user_after = child_times.user_seconds;
    system_after = child_times.system_seconds;
    let const rss_after = os::children_peak_rss_bytes();
    const double real_seconds =
        static_cast<double>(elapsed_nanos) / 1000000000.0;
    let const user_cpu = user_after - user_before;
    let const system_cpu = system_after - system_before;

    let const layout = m_cmd->get_time_format_mode() == time_format_mode::Posix
                           ? utils::time_report_layout::Posix
                       : cxt.runtime_state().is_bash_compatible()
                           ? utils::time_report_layout::Bash
                           : utils::time_report_layout::Rich;

    let const time_format = cxt.get_variable_value("TIMEFORMAT");
    let const report = utils::format_time_report(
        time_format, real_seconds, user_cpu, system_cpu, rss_after, layout,
        m_cmd->get_time_rss_mode() == time_rss_mode::Include
            ? utils::time_report_rss::Include
            : utils::time_report_rss::Omit);

    if (!report.is_empty()) {
      print_error(report);
      flush();
    }
  }

  if (m_cmd->is_negated()) {
    result.status = (result.status == 0) ? 1 : 0;
    cxt.execution_store().set_last_exit_status(result.status);
  }

  return result;
}

Pipeline::Pipeline(SourceLocation location) : Command(steal(location)) {}

Pipeline::~Pipeline() = default;

pure fn Pipeline::is_empty() const wontthrow -> bool
{
  return m_commands.is_empty();
}

static pure fn pipeline_stage_error_status(const EvalContext &cxt,
                                           const ErrorBase &error) wontthrow
    -> i32
{
  if (cxt.runtime_state().is_bash_compatible() && error.is_line_discarding() &&
      !error.is_script_fatal())
  {
    return static_cast<i32>(error.command_status());
  }

  return 1;
}

fn Pipeline::append_command(const Command *node) throws -> void
{
  ASSERT(node != nullptr);

  m_location.length += node->source_location().length;
  m_commands.push(node);
}

fn Pipeline::error_report_location() const wontthrow -> SourceLocation
{
  for (usize index = m_commands.count(); index > 0; index--) {
    let const *simple = m_commands[index - 1]->as_simple_command();
    if (simple != nullptr) return simple->source_location();
  }

  return source_location();
}

cold fn Pipeline::to_string() const throws -> String
{
  let s = String{"Pipeline"};
  append_ast_execution_flags(s);
  return s;
}

cold fn Pipeline::to_ast_string(usize layer) const throws -> String
{
  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[" + to_string() + "]";
  for (let const e : m_commands) {
    s += '\n';
    s += pad + EXPRESSION_AST_INDENT + e->to_ast_string(layer + 1);
  }

  return s;
}

cold fn Pipeline::evaluate_with_compound_stages(EvalContext &cxt) const throws
    -> i64
{
  LOG(Debug, "forking %zu pipeline stages, one child per stage",
      m_commands.count());

  let children = ArrayList<os::process>{cxt.scratch_allocator()};
  os::process last_child = KOSH_INVALID_PROCESS;
  os::descriptor last_stdin = KOSH_INVALID_FD;
  i64 process_group_id = 0;
  let pending_pipe = Maybe<os::Pipe>{};
  let parent_stage_status = Maybe<i32>{};
  bool was_pipeline_abandoned = false;
  let bootstrap = os::subshell_bootstrap{};
  let const child_evaluator = cxt.make_child_evaluator_state(bootstrap);

  try {
    for (usize stage_index = 0; stage_index < m_commands.count(); stage_index++)
    {
      const Command *stage = m_commands[stage_index];
      ASSERT(stage != nullptr);

      cxt.evaluation_metrics_store().add_evaluated_expression(
          cxt.runtime_state().stats_enabled());

      let const is_first = (stage_index == 0);
      let const is_last = (stage_index + 1 == m_commands.count());
      let const should_run_in_parent =
          is_last && !is_async() && cxt.runtime_state().is_bash_compatible() &&
          cxt.runtime_state().is_shopt_enabled(shopt_option_id::Lastpipe) &&
          !cxt.runtime_state().option_is_enabled(shell_option_id::Monitor);

      let const *simple = stage->as_simple_command();
      if (simple != nullptr) {
        let const should_run_stage = publish_simple_command(cxt, *simple);
        if (!should_run_stage) {
          was_pipeline_abandoned = true;
          break;
        }

        cxt.job_table_store().set_stage_boundary_published(true);
      }

      defer { cxt.job_table_store().set_stage_boundary_published(false); };

      let const stage_mode = simple != nullptr
                                 ? root_evaluation_mode::PreparedPipelineStage
                                 : root_evaluation_mode::Normal;

      let stage_in = Maybe<os::descriptor>{};
      let stage_out = Maybe<os::descriptor>{};
      let pipe = Maybe<os::Pipe>{};

      if (!is_last) {
        pipe = os::make_pipe();
        if (!pipe.has_value()) {
          throw ErrorWithLocation{stage->source_location(),
                                  "Could not open a pipe"};
        }
        stage_out = pipe->out;
        pending_pipe = pipe;
      }
      if (!is_first) stage_in = last_stdin;

      if (should_run_in_parent) {
        let saved_stdin = os::saved_descriptor{};
        if (stage_in.has_value()) {
          saved_stdin = os::save_and_replace_descriptor(0, *stage_in);
          os::close_fd(*stage_in);
          last_stdin = KOSH_INVALID_FD;
          if (!saved_stdin.is_dup2_ok) {
            throw ErrorWithLocation{stage->source_location(),
                                    "Could not connect the pipeline input"};
          }
        }
        defer
        {
          if (stage_in.has_value()) os::restore_descriptor(saved_stdin);
        };
        cxt.job_table_store().set_in_pipeline_stage(true);
        defer { cxt.job_table_store().set_in_pipeline_stage(false); };
        parent_stage_status =
            static_cast<i32>(stage->evaluate_root(cxt, stage_mode));
        continue;
      }

      let const stage_location = stage->source_location();
      let const stage_text = os::can_fork_evaluator()
                                 ? StringView{}
                                 : full_source_text(cxt, *stage);

      let const process_group =
          !is_async() ? os::process_group_mode::Inherit
                      : os::background_process_group_mode(process_group_id);
      bootstrap.evaluation_mode = stage_mode;
      cxt.set_child_source_origin(bootstrap, stage_text, stage_location);
      let const launch = os::launch_compound_stage(os::compound_stage_options{
          .source = stage_text,
          .in_fd = stage_in,
          .out_fd = stage_out,
          .location = stage_location,
          .diagnostic_source = cxt.source_store().current_source_view(),
          .process_group_id = process_group_id,
          .evaluator = child_evaluator,
          .process_group = process_group});
      let const child = launch.child;

      if (launch.should_evaluate_child) {
        if (pipe.has_value()) os::close_fd(pipe->in);

        i32 stage_status = 0;
        try {
          cxt.enter_subshell();
          cxt.hide_coprocess_descriptors();
          cxt.job_table_store().inherit_parent_jobs(false);
          cxt.reset_inherited_signal_traps();
          cxt.execution_store().allow_terminal_exec_at_current_depth();
          stage_status =
              static_cast<i32>(stage->evaluate_root(cxt, stage_mode));
          if (cxt.control_flow_store().has_pending() &&
              cxt.control_flow_store().pending().kind ==
                  control_flow::Kind::Exit)
          {
            stage_status =
                static_cast<i32>(cxt.control_flow_store().pending().value);
          }
        } catch (const BrokenPipeExit &) {
          stage_status = KOSH_BROKEN_PIPE_EXIT_STATUS;
        } catch (const ErrorWithLocation &e) {
          if (!e.was_rendered()) {
            koshka::show_message(
                e.to_string(cxt.source_store().current_source_view(), &cxt));
          }
          stage_status = pipeline_stage_error_status(cxt, e);
        } catch (const Error &e) {
          koshka::show_message(e.to_string());
          stage_status = pipeline_stage_error_status(cxt, e);
        } catch (...) {
          LOG(Debug, "swallowed an unknown error in the pipeline stage child");
          stage_status = 1;
        }
        koshka::flush();
        os::exit_process_immediately(stage_status);
      }

      if (stage_out) os::close_fd(*stage_out);
      if (stage_in) os::close_fd(*stage_in);
      if (!is_last) last_stdin = pipe->in;
      pending_pipe = None;

      children.push(child);
      if (is_async() && process_group_id == 0)
        process_group_id = os::process_id_of(child);
      last_child = child;
    }
  } catch (...) {
    if (pending_pipe.has_value()) {
      os::close_fd(pending_pipe->in);
      os::close_fd(pending_pipe->out);
    }
    if (last_stdin != KOSH_INVALID_FD) os::close_fd(last_stdin);
    utils::terminate_and_reap_processes(children);
    throw;
  }

  if (was_pipeline_abandoned) {
    if (last_stdin != KOSH_INVALID_FD) os::close_fd(last_stdin);
    utils::terminate_and_reap_processes(children);

    return cxt.execution_store().last_exit_status();
  }

  if (is_async()) {
    if (last_child != KOSH_INVALID_PROCESS) {
      cxt.job_table_store().set_last_background_pid(
          os::process_id_of(last_child));
      let did_register_job = false;
      defer
      {
        if (!did_register_job) utils::terminate_and_reap_processes(children);
      };
      let const id = cxt.job_table_store().register_pipeline_job(
          children, last_child, "pipeline", process_group_id);
      did_register_job = true;
      if (cxt.execution_store().shell_is_interactive())
        koshka::print_error(
            "[" + String::from(id, heap_allocator()) + "] " +
            String::from(static_cast<u64>(os::process_id_of(last_child)),
                         heap_allocator()) +
            "\n");
    }
    return 0;
  }

  let stage_status = ArrayList<i32>{cxt.scratch_allocator()};
  stage_status.reserve(children.count());
  let pipe_status = ArrayList<String>{heap_allocator()};
  pipe_status.reserve(children.count());
  usize waited_child_count = 0;
  try {
    for (; waited_child_count < children.count(); waited_child_count++) {
      let const status =
          os::wait_and_monitor_process(children[waited_child_count]);
      stage_status.push(status);
      pipe_status.push(String::from(status, heap_allocator()));
    }
    if (parent_stage_status.has_value()) {
      stage_status.push(*parent_stage_status);
      pipe_status.push(String::from(*parent_stage_status, heap_allocator()));
    }
  } catch (...) {
    utils::terminate_and_reap_processes(children, waited_child_count);
    throw;
  }
  cxt.publish_pipe_statuses(steal(pipe_status));

  i32 ret = stage_status.is_empty() ? 0 : stage_status.back();
  if (cxt.runtime_state().pipefail()) {
    ret = 0;
    for (usize i = stage_status.count(); i > 0; i--)
      if (stage_status[i - 1] != 0) {
        ret = stage_status[i - 1];
        break;
      }
  }

  LOG(Debug, "the pipeline stages were reaped, %s status is %d",
      cxt.runtime_state().pipefail() ? "the pipefail" : "the last stage's",
      ret);

  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

hot fn Pipeline::evaluate_impl(EvalContext &cxt, root_evaluation_mode) const
    throws -> status_result
{
  ASSERT(m_commands.count() > 1);

  cxt.execution_store().terminal_exec_allowed() = false;
  cxt.job_table_store().forget_waited_jobs();
  if (!cxt.job_table_store().jobs().is_empty())
    cxt.job_table_store().update_jobs();
  cxt.release_finished_coprocess();

  if (!m_has_compound_stage.has_value()) {
    bool has_compound_stage = false;
    bool has_assignment_only_stage = false;
    for (let const stage : m_commands) {
      if (!stage->is_simple_command()) {
        has_compound_stage = true;
        break;
      }

      const SimpleCommand *simple = static_cast<const SimpleCommand *>(stage);
      if (simple->local_vars().is_empty()) continue;

      if (simple->args().is_empty()) {
        has_assignment_only_stage = true;
        continue;
      }

      has_compound_stage = true;
      break;
    }
    m_has_compound_stage = has_compound_stage;
    m_has_assignment_only_stage = has_assignment_only_stage;
  }

  bool has_compound_stage =
      *m_has_compound_stage ||
      (m_has_assignment_only_stage &&
       cxt.runtime_state().get_mood() != mimic_mood::Default);

  if (!has_compound_stage && cxt.function_store().has_functions()) {
    for (let const stage : m_commands) {
      let const *simple = static_cast<const SimpleCommand *>(stage);
      if (simple->args().is_empty()) continue;
      let const *first = simple->args()[0];
      if (first->kind() != Token::Kind::Word) continue;
      const Word &word = static_cast<const tokens::WordToken *>(first)->word();
      if (word.plain_literal_kind() == Word::PlainLiteral::NotPlain ||
          cxt.function_store().find_function(word.constant_value()).has_value())
      {
        has_compound_stage = true;
        break;
      }
    }
  }

  LOG(Debug, "the pipeline has %zu stages, taking the %s path",
      m_commands.count(),
      has_compound_stage ? "fork-per-stage" : "all-simple fast");

  if (has_compound_stage) return evaluate_with_compound_stages(cxt);

  let const substitution_mark = cxt.mark_process_substitutions();
  defer
  {
    if (is_async()) {
      cxt.hold_process_substitutions(substitution_mark);
    } else {
      cxt.cleanup_process_substitutions(substitution_mark);
    }
  };

  let const pipeline_mark = cxt.expansion_store().scratch_arena().mark();
  let ecs = ArrayList<ExecContext>{cxt.scratch_allocator()};
  defer
  {
    for (ExecContext &leftover : ecs)
      leftover.close_fds();
    cxt.expansion_store().scratch_arena().release(pipeline_mark);
  };
  ecs.reserve(m_commands.count());

  for (let const stage : m_commands) {
    ASSERT(stage != nullptr);
    ASSERT(stage->is_simple_command());
    const SimpleCommand *e = static_cast<const SimpleCommand *>(stage);

    cxt.evaluation_metrics_store().add_evaluated_expression(
        cxt.runtime_state().stats_enabled());

    cxt.source_store().set_current_location(e->source_location());
    let const should_run_stage = publish_simple_command(cxt, *e);
    if (!should_run_stage) return cxt.execution_store().last_exit_status();

    let const stage_write_mark = cxt.begin_confined_variable_writes();
    defer { cxt.rollback_confined_variable_writes(stage_write_mark); };

    let const do_push_unresolved_stage = [&](i32 status, StringView rendered)
                                             throws -> bool {
      let unresolved = ExecContext::make_from_unresolved(e->source_location(),
                                                         status, rendered);
      bool was_unresolved_handed_off = false;
      defer
      {
        if (!was_unresolved_handed_off) unresolved.close_fds();
      };
      try {
        e->redirect_exec_context(unresolved, cxt);
      } catch (const TrapAbandonedRedirection &) {
        return false;
      }
      was_unresolved_handed_off = true;
      ecs.push(steal(unresolved));
      return true;
    };

    let stage_arg_locations =
        ArrayList<SourceLocation>{cxt.scratch_allocator()};
    let stage_args = ArrayList<String>{cxt.scratch_allocator()};
    try {
      stage_args = cxt.process_args(e->args(), &stage_arg_locations,
                                    argument_lifetime::Transient,
                                    argument_context::Command);
    } catch (ErrorWithLocation &expansion_error) {
      let const is_stage_failure =
          expansion_error.is_line_discarding() ||
          (expansion_error.is_script_fatal() &&
           cxt.runtime_state().get_mood() != mimic_mood::Default);
      if (!is_stage_failure) throw;

      let const rendered = expansion_error.to_string(
          cxt.source_store().current_source_view(), &cxt);
      if (!do_push_unresolved_stage(
              static_cast<i32>(expansion_error.command_status()),
              rendered.view()))
      {
        return cxt.execution_store().last_exit_status();
      }
      continue;
    }
    expand_command_aliases(cxt, stage_args, stage_arg_locations);

    if (stage_args.is_empty()) {
      if (cxt.runtime_state().get_mood() == mimic_mood::Default) {
        throw ErrorWithLocation{
            e->source_location(),
            "A pipeline stage expanded to no command to run"};
      }

      let empty_stage =
          ExecContext::make_from_unresolved(e->source_location(), 0, {});
      bool was_empty_stage_handed_off = false;
      defer
      {
        if (!was_empty_stage_handed_off) empty_stage.close_fds();
      };
      try {
        e->redirect_exec_context(empty_stage, cxt);
      } catch (const TrapAbandonedRedirection &) {
        return cxt.execution_store().last_exit_status();
      }
      was_empty_stage_handed_off = true;
      ecs.push(steal(empty_stage));
      continue;
    }
    cxt.write_xtrace(stage_args);

    Maybe<ExecContext> stage_ec;
    try {
      stage_ec = ExecContext::make_from(
          e->source_location(), cxt.source_store().current_source_view(),
          steal(stage_args),
          cxt.runtime_state().koshkit_utilities_are_reachable(),
          cxt.runtime_state().is_shopt_enabled(shopt_option_id::Checkhash),
          cxt.program_resolver(), steal(stage_arg_locations),
          cxt.runtime_state().get_mood(),
          cxt.execution_store().shell_is_interactive() &&
              cxt.is_shopt_enabled("autocd"));
    } catch (CommandResolutionErrorWithLocation &resolution_error) {
      let const windowed = window_function_body_error(cxt, resolution_error);
      let const rendered = resolution_error.to_string(
          windowed.has_value() ? *windowed
                               : cxt.source_store().current_source_view(),
          &cxt);
      if (!do_push_unresolved_stage(
              static_cast<i32>(resolution_error.command_status()),
              rendered.view()))
      {
        return cxt.execution_store().last_exit_status();
      }
      continue;
    }
    let ec = stage_ec.take();
    bool was_stage_redirect_handed_off = false;
    defer
    {
      if (!was_stage_redirect_handed_off) ec.close_fds();
    };
    try {
      e->redirect_exec_context(ec, cxt);
    } catch (const TrapAbandonedRedirection &) {
      return cxt.execution_store().last_exit_status();
    } catch (const ErrorWithLocation &redirection_error) {
      let const rendered = redirection_error.to_string(
          cxt.source_store().current_source_view(), &cxt);
      ec.set_unresolved(static_cast<i32>(redirection_error.command_status()),
                        rendered.view());
    }

    was_stage_redirect_handed_off = true;
    ecs.push(steal(ec));
  }

  let const ret = utils::execute_contexts_with_pipes(
      steal(ecs), cxt,
      is_async() ? execution_mode::Background : execution_mode::Foreground);
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

} /* namespace expressions */

} /* namespace koshka */
