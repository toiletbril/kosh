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

hot fn CompoundList::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_nodes.count() > 0);

  static const i64 NOTHING_WAS_EXECUTED = -256;

  i64 ret = NOTHING_WAS_EXECUTED;

  /* Only the last node yields the list's status, so a terminal exec rides into
     that node alone. */
  let const was_terminal_exec_allowed = cxt.terminal_exec_allowed();
  cxt.set_terminal_exec_allowed(false);
  defer { cxt.set_terminal_exec_allowed(was_terminal_exec_allowed); };

  for (usize index = 0; index < m_nodes.count(); index++) {
    if (cxt.no_exec()) break;

    const CompoundListCondition *n = m_nodes[index];
    ASSERT(n != nullptr);

    if (n->kind() == CompoundListCondition::Kind::None) {
      if (let const history_source = cxt.history_recording_source_for(this);
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
            m_nodes[history_end_index]->source_location().position;
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
    cxt.set_terminal_exec_allowed(was_terminal_exec_allowed && is_last_node);

    /* set -e keys off the command that actually produced the status, not one
       carried over from a short-circuited sibling. */
    bool did_execute = false;
    const bool is_end_of_and_or_chain =
        index + 1 >= m_nodes.count() ||
        m_nodes[index + 1]->kind() == CompoundListCondition::Kind::None;
    const bool should_ignore_errexit =
        !is_end_of_and_or_chain || n->is_negated();
    /* In bash mood an evaluation error fails the command and the list goes on,
       while a script-fatal error still aborts the run. */
    let const do_run_node = [&]() throws -> i64 {
      if (should_ignore_errexit) cxt.enter_condition();
      defer
      {
        if (should_ignore_errexit) cxt.leave_condition();
      };
      try {
        return n->evaluate(cxt);
      } catch (const InterruptErrorWithLocation &) {
        throw;
      } catch (ErrorWithLocation &error) {
        if (!cxt.is_bash_compatible() || error.is_script_fatal()) {
          throw;
        }
        LOG(Debug,
            "bash mood converted the located error to command status %lld: %s",
            static_cast<long long>(error.command_status()),
            error.message().c_str());
        /* A located error from a function body rebases onto the defining copy
           here, since this catch fires while the call name stack still names
           the function. An error a deeper frame already rendered keeps its
           status without a second render. */
        if (!error.was_rendered()) {
          if (let const windowed = window_function_body_error(cxt, error);
              windowed.has_value())
          {
            show_message(error.to_string(*windowed, &cxt));
          } else {
            const String *source = cxt.current_source();
            show_message(error.to_string(
                source != nullptr ? source->view() : StringView{}, &cxt));
          }
          error.set_rendered();
        }
        SET_AND_RETURN_EXIT_STATUS(cxt, error.command_status());
      } catch (const ErrorBase &error) {
        if (!cxt.is_bash_compatible() || error.is_script_fatal()) {
          throw;
        }
        LOG(Debug, "bash mood converted the error to command status %lld: %s",
            static_cast<long long>(error.command_status()),
            error.message().c_str());
        const String *source = cxt.current_source();
        show_message(error.to_string(
            source != nullptr ? source->view() : StringView{}, &cxt));
        SET_AND_RETURN_EXIT_STATUS(cxt, error.command_status());
      }
    };
    switch (n->kind()) {
    case CompoundListCondition::Kind::None:
      ret = do_run_node();
      did_execute = true;
      break;

    case CompoundListCondition::Kind::Or:
      if (ret != 0) {
        ret = do_run_node();
        did_execute = true;
      }
      break;

    case CompoundListCondition::Kind::And:
      if (ret == 0) {
        ret = do_run_node();
        did_execute = true;
      }
      break;
    }

    /* A break, continue, return, or exit inside a node stops the rest of the
       list and unwinds to the boundary that consumes it. */
    if (cxt.has_pending_control_flow()) break;

    /* POSIX exempts set -e for a command that is an operand of && or || and not
       the last of the and-or list, and for a command the ! reserved word
       negates. */
    const bool was_command_failure_uncaught =
        !cxt.in_condition() && did_execute && !n->is_negated() &&
        is_end_of_and_or_chain && ret != 0 && ret != NOTHING_WAS_EXECUTED;

    if (was_command_failure_uncaught && !cxt.is_posix_mode()) {
      cxt.set_last_exit_status(static_cast<i32>(ret));
      cxt.run_named_trap(StringView{"ERR", 3});
    }

    if (cxt.error_exit() && was_command_failure_uncaught) {
      cxt.set_last_exit_status(static_cast<i32>(ret));
      if (cxt.in_subshell()) {
        cxt.request_exit(ret, source_location());
        break;
      }
      utils::quit(static_cast<i32>(ret), utils::farewell_policy::Goodbye);
    }
  }

  return ret == NOTHING_WAS_EXECUTED ? 0 : ret;
}

CompoundListCondition::CompoundListCondition(SourceLocation location, Kind kind,
                                             const Command *expr)
    : Expression(steal(location)), m_kind(kind), m_cmd(expr)
{}

CompoundListCondition::~CompoundListCondition() = default;

fn CompoundListCondition::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  return m_cmd != nullptr && !m_cmd->is_async() &&
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

hot fn CompoundListCondition::evaluate_impl(EvalContext &cxt) const throws
    -> i64
{
  ASSERT(m_cmd != nullptr);
  cxt.begin_command_evaluation();

  /* A negated or timed command must run to completion here, since the inverse
     or the report applies after the command returns, which an exec would
     skip. */
  if (m_cmd->is_negated() || m_cmd->is_timed()) {
    cxt.set_terminal_exec_allowed(false);
  }

  double user_before = 0.0;
  double system_before = 0.0;
  u64 start_nanos = 0;
  if (m_cmd->is_timed()) {
    os::children_cpu_seconds(user_before, system_before);
    start_nanos = os::monotonic_nanos();
  }

  let status = m_cmd->evaluate(cxt);

  if (m_cmd->is_timed()) {
    let const elapsed_nanos = os::monotonic_nanos() - start_nanos;
    double user_after = 0.0;
    double system_after = 0.0;
    os::children_cpu_seconds(user_after, system_after);
    let const rss_after = os::children_peak_rss_bytes();
    const double real_seconds =
        static_cast<double>(elapsed_nanos) / 1000000000.0;
    let const user_cpu = user_after - user_before;
    let const system_cpu = system_after - system_before;

    let const time_format = cxt.get_variable_value("TIMEFORMAT");
    let const report = utils::format_time_report(
        m_cmd->time_uses_posix_format(), m_cmd->should_time_report_rss(),
        time_format, real_seconds, user_cpu, system_cpu, rss_after);

    if (!report.is_empty()) {
      print_error(report);
      flush();
    }
  }

  /* A pipeline prefixed with ! reports the inverse of its status. */
  if (m_cmd->is_negated()) {
    status = (status == 0) ? 1 : 0;
    cxt.set_last_exit_status(static_cast<i32>(status));
  }

  return status;
}

Pipeline::Pipeline(SourceLocation location) : Command(steal(location)) {}

Pipeline::~Pipeline() = default;

pure fn Pipeline::is_empty() const wontthrow -> bool
{
  return m_commands.is_empty();
}

fn Pipeline::append_command(const Command *node) throws -> void
{
  ASSERT(node != nullptr);

  m_location.length += node->source_location().length;
  m_commands.push(node);
}

cold fn Pipeline::to_string() const throws -> String
{
  let s = String{"Pipeline"};
  if (is_async()) s += ", Async";
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

/* Run a pipeline that has at least one compound stage. Every stage forks, so a
   compound stage evaluates its tree in a child with the pipe already on its
   standard descriptors. */
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

  /* On a make_pipe or fork failure mid-loop the previous read end and the
     current pipe are closed and every spawned child is waited, then the error
     is rethrown. */
  try {
    for (usize stage_index = 0; stage_index < m_commands.count(); stage_index++)
    {
      const Command *stage = m_commands[stage_index];
      ASSERT(stage != nullptr);

      cxt.add_evaluated_expression();

      let const is_first = (stage_index == 0);
      let const is_last = (stage_index + 1 == m_commands.count());

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

      let const stage_location = stage->source_location();
      let const stage_source = cxt.current_source();
      let stage_text = StringView{};
      if (stage_source != nullptr) {
        let stage_end_position =
            stage_location.position + stage_location.length;
        if (stage->source_end_position() > stage_end_position)
          stage_end_position = stage->source_end_position();
        stage_text = stage_source->view().substring_of_length(
            stage_location.position,
            stage_end_position - stage_location.position);
      }

      let const process_group =
          !is_async() ? os::process_group_mode::Inherit
                      : os::background_process_group_mode(process_group_id);
      let const launch = os::launch_compound_stage(
          stage_text, stage_in, stage_out, None, cxt.mood(), stage_location,
          stage_source != nullptr ? stage_source->view() : StringView{},
          process_group, process_group_id);
      let const child = launch.child;

      if (launch.should_evaluate_child) {
        /* This child inherited the read end of its own output pipe. A stage
           that runs its command as a grandchild would otherwise keep the pipe
           open and a producer in this stage would never see its consumer
           leave. */
        if (pipe.has_value()) os::close_fd(pipe->in);

        /* The child evaluates the stage in a subshell, then exits with its
           status. A diagnostic or an exit request inside still yields a child
           status rather than unwinding into the parent's evaluator. */
        i32 stage_status = 0;
        try {
          cxt.enter_subshell();
          stage_status = static_cast<i32>(stage->evaluate(cxt));
          if (cxt.has_pending_control_flow() &&
              cxt.pending_control_flow().kind == control_flow::Kind::Exit)
          {
            stage_status = static_cast<i32>(cxt.pending_control_flow().value);
          }
        } catch (const BrokenPipeExit &) {
          stage_status = KOSH_BROKEN_PIPE_EXIT_STATUS;
        } catch (const ErrorWithLocation &e) {
          const String *source = cxt.current_source();
          koshka::show_message(e.to_string(
              source != nullptr ? source->view() : StringView{}, &cxt));
          stage_status = 1;
        } catch (const Error &e) {
          koshka::show_message(e.to_string());
          stage_status = 1;
        } catch (...) {
          LOG(Debug, "swallowed an unknown error in the pipeline stage child");
          stage_status = 1;
        }
        koshka::flush();
        os::exit_process_immediately(stage_status);
      }

      /* The parent keeps neither pipe end open past the stage that owns it,
         otherwise a reader never sees the writer close. */
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

  if (is_async()) {
    if (last_child != KOSH_INVALID_PROCESS) {
      cxt.set_last_background_pid(os::process_id_of(last_child));
      let did_register_job = false;
      defer
      {
        if (!did_register_job) utils::terminate_and_reap_processes(children);
      };
      let const id = cxt.register_pipeline_job(children, last_child, "pipeline",
                                               process_group_id);
      did_register_job = true;
      if (cxt.shell_is_interactive())
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
  } catch (...) {
    utils::terminate_and_reap_processes(children, waited_child_count);
    throw;
  }
  cxt.set_indexed_array("PIPESTATUS", steal(pipe_status));

  i32 ret = stage_status.is_empty() ? 0 : stage_status.back();
  if (cxt.pipefail()) {
    ret = 0;
    for (usize i = stage_status.count(); i > 0; i--)
      if (stage_status[i - 1] != 0) {
        ret = stage_status[i - 1];
        break;
      }
  }

  LOG(Debug, "the pipeline stages were reaped, %s status is %d",
      cxt.pipefail() ? "the pipefail" : "the last stage's", ret);

  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

hot fn Pipeline::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_commands.count() > 1);

  cxt.set_terminal_exec_allowed(false);

  /* A pipeline of only simple commands keeps the fast path. A compound stage
     takes the fork-per-stage path. A simple stage carrying a prefix assignment
     takes the fork path too, since the fast path builds the stage from its
     argument words alone and the prefix must reach only that stage. */
  if (!m_has_compound_stage.has_value()) {
    bool has_compound_stage = false;
    for (let const stage : m_commands) {
      if (!stage->is_simple_command()) {
        has_compound_stage = true;
        break;
      }
      /* A command-less stage of bare assignments keeps the fast path, so the
         strict diagnostic for x=1 | cat is preserved. */
      const SimpleCommand *simple = static_cast<const SimpleCommand *>(stage);
      if (!simple->local_vars().is_empty() && !simple->args().is_empty()) {
        has_compound_stage = true;
        break;
      }
    }
    m_has_compound_stage = has_compound_stage;
  }

  bool has_compound_stage = *m_has_compound_stage;

  if (!has_compound_stage && cxt.has_functions()) {
    for (let const stage : m_commands) {
      let const *simple = static_cast<const SimpleCommand *>(stage);
      if (simple->args().is_empty()) continue;
      let const *first = simple->args()[0];
      if (first->kind() != Token::Kind::Word) continue;
      const Word &word = static_cast<const tokens::WordToken *>(first)->word();
      if (word.plain_literal_kind() == Word::PlainLiteral::NotPlain ||
          cxt.find_function(word.constant_value()) != nullptr)
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

  /* The arena runs a destructor only for an object it created, and this list
     took plain storage, so a stage still holding open descriptors on an early
     exit is closed by the defer before the release. */
  let const pipeline_mark = cxt.scratch_mark();
  let ecs = ArrayList<ExecContext>{cxt.scratch_allocator()};
  defer
  {
    for (ExecContext &leftover : ecs)
      leftover.close_fds();
    cxt.scratch_release(pipeline_mark);
  };
  ecs.reserve(m_commands.count());

  for (let const stage : m_commands) {
    ASSERT(stage != nullptr);
    ASSERT(stage->is_simple_command());
    const SimpleCommand *e = static_cast<const SimpleCommand *>(stage);

    cxt.add_evaluated_expression();

    /* The location moves onto the stage first so a runtime warning from its
       words carets the stage that read the variable. */
    cxt.set_current_location(e->source_location());

    let stage_arg_locations =
        ArrayList<SourceLocation>{cxt.scratch_allocator()};
    let stage_args =
        cxt.process_args(e->args(), argument_lifetime::Transient,
                         argument_context::Command, &stage_arg_locations);

    if (stage_args.is_empty()) {
      throw ErrorWithLocation{e->source_location(),
                              "A pipeline stage expanded to no command to run"};
    }

    /* A stage whose command does not resolve becomes a no-op context that
       closes its pipe to give the next stage EOF. */
    Maybe<ExecContext> stage_ec;
    try {
      let const *source = cxt.current_source();
      stage_ec = ExecContext::make_from(
          e->source_location(),
          source != nullptr ? source->view() : StringView{}, steal(stage_args),
          cxt.mood(), cxt.koshkit_utilities_are_reachable(),
          cxt.get_program_resolver(), steal(stage_arg_locations));
    } catch (const CommandResolutionErrorWithLocation &resolution_error) {
      report_command_resolution_error(cxt, resolution_error);
      /* The stage still applies its own redirections. A > onto its stdout takes
         the slot ahead of the pipe, so the next stage still sees EOF. */
      let unresolved = ExecContext::make_unresolved(
          e->source_location(),
          static_cast<i32>(resolution_error.command_status()));
      bool was_unresolved_handed_off = false;
      defer
      {
        if (!was_unresolved_handed_off) unresolved.close_fds();
      };
      e->redirect_exec_context(unresolved, cxt);
      was_unresolved_handed_off = true;
      ecs.push(steal(unresolved));
      continue;
    }
    let ec = stage_ec.take();
    /* A later redirection in the same stage may throw after an earlier one
       opened a descriptor, so the descriptors opened so far are closed on that
       throw. The guard is disarmed once the stage is handed off. */
    bool was_stage_redirect_handed_off = false;
    defer
    {
      if (!was_stage_redirect_handed_off) ec.close_fds();
    };
    e->redirect_exec_context(ec, cxt);
    was_stage_redirect_handed_off = true;
    ecs.push(steal(ec));
  }

  /* The status is committed here so $? reads it from the store, since the
     all-simple fast path otherwise returns without recording it. */
  let const ret = utils::execute_contexts_with_pipes(
      steal(ecs), cxt,
      is_async() ? execution_mode::Background : execution_mode::Foreground);
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

} /* namespace expressions */

} /* namespace koshka */
