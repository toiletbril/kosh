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
    : Expression(location), m_kind(kind), m_cmd(expr)
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
  u64 rss_before = 0;
  if (m_cmd->is_timed()) {
    os::children_cpu_seconds(user_before, system_before);
    rss_before = os::children_peak_rss_bytes();
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
    let const peak_rss_bytes = rss_after > rss_before ? rss_after : 0;

    /* The -p form prints the posix report and ignores TIMEFORMAT. Otherwise a
       set TIMEFORMAT drives the format, an empty value prints nothing, and an
       unset value keeps the pretty default. */
    String report{cxt.scratch_allocator()};
    if (m_cmd->time_uses_posix_format()) {
      report =
          utils::format_time_report_posix(real_seconds, user_cpu, system_cpu);
    } else if (let const time_format = cxt.get_variable_value("TIMEFORMAT");
               time_format.has_value())
    {
      if (!time_format->is_empty())
        report = utils::format_time_report_custom(
            time_format->view(), real_seconds, user_cpu, system_cpu);
    } else {
      report = utils::format_time_report_pretty(real_seconds, user_cpu,
                                                system_cpu, peak_rss_bytes);
    }

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

Pipeline::Pipeline(SourceLocation location) : Command(location) {}

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

  /* The rewind runs no destructor, so a stage still holding open descriptors on
     an early exit is closed by the defer before the release. */
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
          cxt.mood(), cxt.koshkit(), cxt.get_program_resolver(),
          steal(stage_arg_locations));
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

CompoundCommand::CompoundCommand(SourceLocation location) : Command(location) {}

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
    : CompoundCommand(location), m_branches(steal(branches)),
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
    let const was_silenced = actx.should_silence_unresolved_commands;
    if (is_dead_branch) actx.should_silence_unresolved_commands = true;
    body->analyze(actx, false);
    actx.should_silence_unresolved_commands = was_silenced;

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
    : CompoundCommand(location), m_condition(condition), m_body(body),
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

fn resolve_loop_control(EvalContext &cxt) throws -> loop_disposition
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
  m_condition->analyze(actx, is_unconditional);
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

  let const was_silenced = actx.should_silence_unresolved_commands;
  let const was_inside_read_loop = actx.is_inside_read_loop;
  if (has_input_reading_loop_condition) actx.is_inside_read_loop = true;
  if (is_folded_to_skip()) actx.should_silence_unresolved_commands = true;
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;
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

SelectLoop::SelectLoop(SourceLocation location, StringView variable_name,
                       ArrayList<const Token *> &&words, bool has_in_clause,
                       const Expression *body)
    : CompoundCommand(location), m_variable_name(variable_name),
      m_has_in_clause(has_in_clause), m_body(body)
{
  m_words = steal(words);
}

SelectLoop::~SelectLoop() = default;

cold fn SelectLoop::to_string() const throws -> String
{
  return "SelectLoop \"" + StringView{m_variable_name} + "\"";
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

  LOG(Debug, "the select loop offers %zu choices for '%s'", values.count(),
      m_variable_name.c_str());

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
    : CompoundCommand(location), m_variable_name(variable_name),
      m_variable_location(variable_location), m_has_in_clause(has_in_clause),
      m_body(body)
{
  m_words = steal(words);
}

ForLoop::~ForLoop() = default;

cold fn ForLoop::to_string() const throws -> String
{
  let result = String{"ForLoop \""};
  result += StringView{m_variable_name};
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

  LOG(Debug, "the for loop binds '%s' over %zu values", m_variable_name.c_str(),
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

  unused(is_unconditional);

  let const outer_loop_location =
      actx.active_loop_variables.find(m_variable_name.view());
  if (outer_loop_location != nullptr) {
    actx.report_diagnostic(diagnostic_id::sc2165, m_variable_location,
                           {m_variable_name.view()}, *outer_loop_location);
    actx.report_diagnostic(diagnostic_id::sc2167, *outer_loop_location,
                           {m_variable_name.view()}, m_variable_location);
  }

  let const had_outer_loop_variable = outer_loop_location != nullptr;
  let saved_outer_loop_location = SourceLocation{};
  if (had_outer_loop_variable) saved_outer_loop_location = *outer_loop_location;
  actx.active_loop_variables.set(m_variable_name.view(), m_variable_location);
  defer
  {
    if (had_outer_loop_variable)
      actx.active_loop_variables.set(m_variable_name.view(),
                                     saved_outer_loop_location);
    else
      actx.active_loop_variables.erase(m_variable_name.view());
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

      case WordSegment::Kind::VariableReference:
        actx.note_positional_reference(segment.text.view());
        word_is_literal = false;
        if (!segment.is_in_double_quotes) has_unquoted_expansion = true;
        break;

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

  /* The rule reads the word list while unchanged, so optimize runs before the
     constant table is cleared for the body. */
  optimizer::optimize_node(this, actx);

  /* Clearing the constant table before the body keeps a pre-loop constant from
     being inlined into a counter the body increments. */
  actx.constant_variables.clear();
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;
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
    : CompoundCommand(location), m_word(word)
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
  for (let const &item : m_items) {
    ASSERT(item.body != nullptr);
    item.body->analyze(actx, false);
  }

  ASSERT(m_word != nullptr);

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
  let earlier_patterns = ArrayList<String>{heap_allocator()};
  let earlier_pattern_locations = ArrayList<SourceLocation>{heap_allocator()};
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
      for (usize earlier_index = 0; earlier_index < earlier_patterns.count();
           earlier_index++)
      {
        let const &earlier = earlier_patterns[earlier_index];
        if (raw_pattern.view() == earlier.view()) {
          actx.report_diagnostic(diagnostic_id::sc2221,
                                 pattern->source_location(), {},
                                 earlier_pattern_locations[earlier_index]);
          is_duplicate = true;
          break;
        }
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
      earlier_patterns.push(String{raw_pattern.view()});
      earlier_pattern_locations.push(pattern->source_location());
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
    : CompoundCommand(location), m_body(body)
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

} // namespace expressions

} // namespace koshka
