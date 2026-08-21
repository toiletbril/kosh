#include "Builtin.hpp"
#include "Cli.hpp"
#include "Containers.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace utils {

fn set_foreground_program_title(const ArrayList<String> &arguments,
                                EvalContext &cxt) throws -> void
{
  if (arguments.is_empty()) return;

  if (!cxt.shell_is_interactive() || !cxt.startup_finished() ||
      cxt.is_completion_function_running() || cxt.is_prompt_command_running())
  {
    return;
  }

  let &command_title = cxt.get_foreground_program_title_buffer();
  command_title.clear();
  for (usize index = 0; index < arguments.count(); index++) {
    if (index > 0) command_title.push(' ');
    append_shell_quoted_arg(command_title, arguments[index]);
  }
  toiletline::set_title(command_title.view());
}

fn execute_context(ExecContext &&ec, EvalContext &cxt,
                   execution_mode mode) throws -> i32
{
  let const is_async = mode == execution_mode::Background;
  if (ec.is_builtin()) {
    LOG(Debug, "dispatching the builtin '%s'", ec.program().c_str());
    return execute_builtin(steal(ec), cxt);
  }

  /* The terminal external command may replace the shell in place when it is the
     last command, not in a subshell, and no EXIT trap is pending. */
  let const can_replace_shell =
      cxt.terminal_exec_allowed() && !cxt.in_subshell() && !cxt.has_exit_trap();

  /* Mimicry runs the script in-process, a background command keeps its fork.
   */
  if (cxt.mimicry() && !is_async) {
    if (Maybe<mimic_mood> mode = ec.program_path().detect_mimic_shell();
        mode.has_value())
    {
      LOG(Debug, "execute_context mimicking the shell for '%s'",
          ec.program().c_str());
      if (cxt.shell_is_interactive() && os::shell_has_controlling_terminal()) {
        let command = String{heap_allocator()};
        for (usize index = 0; index < ec.args().count(); index++) {
          if (index > 0) command.push(' ');
          append_shell_quoted_arg(command, ec.args()[index]);
        }

        /* The child blocks on this pipe until the parent hands off the
           terminal, so it never touches the terminal before the handoff. */
        let const sync_pipe = os::make_pipe();

        koshka::flush();
        let const forked_child = os::try_fork_job_process();
        if (forked_child.has_value()) {
          const os::process child = *forked_child;
          if (os::process_id_of(child) == 0) {
            if (sync_pipe.has_value()) {
              /* The child drops its write end so the read unblocks on EOF if
                 the parent dies before the handoff. */
              os::close_fd(sync_pipe->out);
              char handoff_byte = 0;
              (void) os::read_fd(sync_pipe->in, &handoff_byte, 1);
              os::close_fd(sync_pipe->in);
            }
            i32 status = 1;
            try {
              status =
                  cxt.run_mimicked_script(ec, *mode, script_isolation::Shared);
            } catch (const ErrorBase &error) {
              const String *source = cxt.current_source();
              show_message(error.to_string(
                  source != nullptr ? source->view() : StringView{}, &cxt));
              status = static_cast<i32>(error.command_status());
            } catch (...) {}
            os::exit_process_immediately(status);
          }

          if (sync_pipe.has_value()) {
            os::close_fd(sync_pipe->in);
          }
          os::give_controlling_terminal_to(child);
          if (sync_pipe.has_value()) {
            (void) os::write_fd(sync_pipe->out, "x", 1);
            os::close_fd(sync_pipe->out);
          }

          let was_stopped = false;
          const i32 status = os::wait_and_monitor_process(child, &was_stopped);
          os::reclaim_controlling_terminal();

          if (was_stopped) {
            const i32 id = cxt.register_stopped_job(child, command, status,
                                                    os::process_id_of(child));
            cxt.notify_stopped_job(id, command.view());
          }
          return status;
        }

        if (sync_pipe.has_value()) {
          os::close_fd(sync_pipe->in);
          os::close_fd(sync_pipe->out);
        }
      }
      return cxt.run_mimicked_script(ec, *mode,
                                     can_replace_shell
                                         ? script_isolation::Shared
                                         : script_isolation::Isolated);
    }
  }

  /* The terminal external command replaces the shell in place, the way dash
     execs the last command under EV_EXIT. The EXIT trap is rechecked at run
     time here, since one set earlier in this chunk must still run. */
  if (!is_async && can_replace_shell) {
    LOG(Debug,
        "execute_context replacing the shell with the terminal command '%s'",
        ec.program().c_str());
    flush();
    unused(cxt.materialize_kosh_identity());
    try {
      os::replace_process(steal(ec));
    } catch (const ErrorWithLocation &error) {
      /* Resolved but unexecutable exits 126, missing exits 127. */
      const String *source = cxt.current_source();
      show_message(error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      quit(126, farewell_policy::Silent);
    } catch (const Error &error) {
      const String *source = cxt.current_source();
      let located = ErrorWithLocation{ec.source_location(), error.message()};
      located.set_command_status(error.command_status());
      show_message(located.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      quit(127, farewell_policy::Silent);
    }
    LOG(Debug, "running the file as a shell script in place");
    ec.in_fd.reset();
    ec.out_fd.reset();
    ec.err_fd.reset();
    const mimic_mood mode = cxt.mood();
    quit(cxt.run_program_fallback(ec, mode,
                                  can_replace_shell
                                      ? script_isolation::Shared
                                      : script_isolation::Isolated),
         farewell_policy::Silent);
  }

  LOG(Debug, "spawning the external command '%s'%s", ec.program().c_str(),
      is_async ? " in the background" : "");

  /* An interactive foreground command runs in its own process group and holds
     the terminal, so it dies on its own Ctrl-C. */
  let const is_foreground_job = !is_async && cxt.shell_is_interactive() &&
                                os::shell_has_controlling_terminal();

  let command = String{heap_allocator()};
  if (is_async || is_foreground_job) {
    for (usize i = 0; i < ec.args().count(); i++) {
      if (i > 0) command += ' ';
      append_shell_quoted_arg(command, ec.args()[i]);
    }
    if (is_async) command += " &";
  }

  let const source = cxt.current_source();
  unused(cxt.materialize_kosh_identity());
  os::process p =
      os::execute_program(ec,
                          is_async ? os::script_fallback_policy::Reject
                                   : os::script_fallback_policy::Allow,
                          is_async ? os::process_group_mode::NewBackground
                          : is_foreground_job ? os::process_group_mode::New
                                              : os::process_group_mode::Inherit,
                          source != nullptr ? source->view() : StringView{});
  if (p == KOSH_INVALID_PROCESS) {
    LOG(Debug, "running the file as a shell script in this process");
    const mimic_mood mode = cxt.mood();
    return cxt.run_program_fallback(ec, mode,
                                    can_replace_shell
                                        ? script_isolation::Shared
                                        : script_isolation::Isolated);
  }
  if (is_async) {
    cxt.set_last_background_pid(os::process_id_of(p));
    let const process_group_id = os::process_id_of(p);
    const i32 id = cxt.register_job(p, command, process_group_id);
    if (cxt.shell_is_interactive())
      koshka::print_error("[" + String::from(id, heap_allocator()) + "] " +
                          String::from(static_cast<u64>(os::process_id_of(p)),
                                       heap_allocator()) +
                          "\n");
    return 0;
  }

  LOG(Debug, "waiting for the foreground child to finish");
  let should_reclaim_after_wait = is_foreground_job;
  if (should_reclaim_after_wait) os::give_controlling_terminal_to(p);
  defer
  {
    if (should_reclaim_after_wait) os::reclaim_controlling_terminal();
  };
  let was_stopped = false;
  const i32 foreground_status = os::wait_and_monitor_process(
      p, is_foreground_job ? &was_stopped : nullptr);
  if (should_reclaim_after_wait) {
    os::reclaim_controlling_terminal();
    should_reclaim_after_wait = false;
  }
  if (was_stopped) {
    const i32 id = cxt.register_stopped_job(p, command, foreground_status,
                                            os::process_id_of(p));
    cxt.notify_stopped_job(id, command.view());
  }
  return foreground_status;
}

pure static fn builtin_can_launch_fresh(Builtin::Kind kind) wontthrow -> bool
{
  switch (kind) {
  case Builtin::Kind::Echo:
  case Builtin::Kind::Pwd:
  case Builtin::Kind::True:
  case Builtin::Kind::False:
  case Builtin::Kind::Test:
  case Builtin::Kind::Printf:
  case Builtin::Kind::Koshkit: return true;
  default: return false;
  }
}

fn terminate_and_reap_processes(const ArrayList<os::process> &processes,
                                usize first_process_position) wontthrow -> void
{
  for (usize position = first_process_position; position < processes.count();
       position++)
    unused(os::signal_process(processes[position], 9));

  for (usize position = first_process_position; position < processes.count();
       position++)
  {
    try {
      os::wait_and_monitor_process(processes[position]);
    } catch (...) {}
  }
}

fn execute_contexts_with_pipes(ArrayList<ExecContext> &&ecs, EvalContext &cxt,
                               execution_mode mode) throws -> i32
{
  let const is_async = mode == execution_mode::Background;
  ASSERT(ecs.count() > 1);

  if (!is_async && cxt.shell_is_interactive() && cxt.startup_finished() &&
      !cxt.is_completion_function_running() && !cxt.is_prompt_command_running())
  {
    let command = String{cxt.scratch_allocator()};
    for (usize stage = 0; stage < ecs.count(); stage++) {
      if (stage > 0) command += " | ";
      for (usize argument = 0; argument < ecs[stage].args().count(); argument++)
      {
        if (argument > 0) command.push(' ');
        append_shell_quoted_arg(command, ecs[stage].args()[argument]);
      }
    }
    toiletline::set_title(command.view());
  }

  LOG(Debug, "running a pipeline of %zu stages%s", ecs.count(),
      is_async ? " in the background" : "");

  i32 ret = 0;

  /* Every external stage is collected so all of them are reaped, not only the
     last. Otherwise a first stage like yes is left a zombie when the last stage
     exits. */
  let children = ArrayList<os::process>{heap_allocator()};
  os::process last_child = KOSH_INVALID_PROCESS;
  os::descriptor last_stdin = KOSH_INVALID_FD;
  i64 process_group_id = 0;
  bool should_reap_children_on_unwind = true;
  defer
  {
    if (should_reap_children_on_unwind) {
      for (ExecContext &pending_context : ecs)
        pending_context.close_fds();
      if (last_stdin != KOSH_INVALID_FD) os::close_fd(last_stdin);
      terminate_and_reap_processes(children);
    }
  };

  /* Each stage's status is recorded against its position, so pipefail can
     report the rightmost stage that failed and the plain case can read the last
     stage. A builtin stage yields its status at once and an external one's
     status arrives from the wait below, tracked by the parallel child-to-stage
     list. */
  let const stage_count = ecs.count();
  let stage_status = ArrayList<i32>{heap_allocator()};
  stage_status.reserve(stage_count);
  for (usize i = 0; i < stage_count; i++)
    stage_status.push(0);
  let child_stage = ArrayList<usize>{heap_allocator()};

  bool is_first = true;
  usize stage_index = 0;

  for (ExecContext &ec : ecs) {
    Maybe<os::Pipe> pipe;

    let const is_last = (&ec == &ecs.back());

    if (!is_last) {
      pipe = os::make_pipe();
      if (!pipe) {
        throw ErrorWithLocation{ec.source_location(), "Could not open a pipe"};
      }
      /* An explicit > takes the stage's stdout, so the pipe end closes unused.
       */
      if (!ec.out_fd)
        ec.out_fd = pipe->out;
      else
        os::close_fd(pipe->out);
    }

    if (!is_first) {
      if (!ec.in_fd)
        ec.in_fd = last_stdin;
      else
        os::close_fd(last_stdin);
    }
    if (!is_last) {
      last_stdin = pipe->in;
    }

    if (ec.is_unresolved()) {
      stage_status[stage_index] = ec.get_unresolved_status();
      ec.close_fds();
    } else if (!ec.is_builtin()) {
      let const source = cxt.current_source();
      unused(cxt.materialize_kosh_identity());
      let const process_group =
          !is_async ? os::process_group_mode::Inherit
                    : os::background_process_group_mode(process_group_id);
      let const child = os::execute_program(
          ec, os::script_fallback_policy::Reject, process_group,
          source != nullptr ? source->view() : StringView{},
          os::terminal_handoff::Keep, process_group_id);
      if (is_async && process_group_id == 0) {
        process_group_id = os::process_id_of(child);
      }
      children.push(child);
      child_stage.push(stage_index);
      last_child = child;
    } else if (!is_last || is_async) {
      let const source = cxt.current_source();
      let const process_group =
          !is_async ? os::process_group_mode::Inherit
                    : os::background_process_group_mode(process_group_id);
      let forked_child = os::try_fork_compound_stage(
          ec.in_fd, ec.out_fd, ec.err_fd, ec.source_location(),
          source != nullptr ? source->view() : StringView{}, process_group,
          process_group_id);
      let preflight_status = Maybe<i32>{};
      let preflight_location = SourceLocation{};
      let preflight_message = String{cxt.scratch_allocator()};
      if (!forked_child.has_value() &&
          builtin_can_launch_fresh(ec.builtin_kind()))
      {
        const usize utility_index = ec.program() == "koshkit" ? 1 : 0;
        bool should_launch_fresh_stage =
            ec.builtin_kind() != Builtin::Kind::Koshkit;
        bool should_restore_environment = false;
        if (ec.builtin_kind() == Builtin::Kind::Koshkit &&
            utility_index < ec.args().count())
        {
          let const utility_kind =
              koshkit::find_util(ec.args()[utility_index].view());
          if (utility_kind.has_value()) {
            should_launch_fresh_stage = true;
            if (!is_async && *utility_kind == koshkit::Utility::Kind::Timeout) {
              preflight_status = koshkit::preflight_timeout_stage(
                  ec, cxt, utility_index, preflight_location,
                  preflight_message);
            }
            should_restore_environment =
                *utility_kind == koshkit::Utility::Kind::Env;
          }
        }

        if (should_launch_fresh_stage && !preflight_status.has_value()) {
          let stage_source = String{cxt.scratch_allocator()};
          if (should_restore_environment) {
            static const StringView RESTORED_ENVIRONMENT_NAMES[] = {
                "PWD",
                "KOSH",
                "KOSH_VERSION",
                "KOSH_COMMIT",
                "KOSH_BUILD_MODE",
                "KOSH_OS",
                "BASH_VERSION",
                "BASH",
                "SHLVL",
                "PATH",
                "NO_COLOR",
                "KOSH_INTERNAL_SUPPRESS_ROOT_TRACE"};
            for (let const name : RESTORED_ENVIRONMENT_NAMES) {
              let const value = os::get_environment_variable(name);
              if (value.has_value()) {
                stage_source.append("export ");
                stage_source.append(name);
                stage_source.push('=');
                append_shell_quoted_arg(stage_source, value->view(), true);
              } else {
                stage_source.append("unset ");
                stage_source.append(name);
              }
              stage_source.append("; ");
            }
          }
          if (ec.builtin_kind() == Builtin::Kind::Koshkit &&
              ec.program() != "koshkit")
          {
            stage_source.append("koshkit ");
          }
          for (usize argument_index = 0; argument_index < ec.args().count();
               argument_index++)
          {
            if (argument_index > 0) stage_source.push(' ');
            append_shell_quoted_arg(stage_source, ec.args()[argument_index],
                                    true);
          }

          let stage_out = ec.out_fd;
          let stage_err = ec.err_fd;
          ec.apply_dup_routing(
              [&]() { stage_err = stage_out.value_or(KOSH_STDOUT); },
              [&]() { stage_out = stage_err.value_or(KOSH_STDERR); });
          try {
            let const launch = os::launch_compound_stage(
                stage_source.view(), ec.in_fd, stage_out, stage_err, cxt.mood(),
                ec.source_location(),
                source != nullptr ? source->view() : StringView{},
                process_group, process_group_id);
            forked_child = launch.child;
          } catch (...) {
            ec.close_fds();
            os::close_fd(last_stdin);
            last_stdin = KOSH_INVALID_FD;
            throw;
          }
        }
      }

      if (preflight_status.has_value()) {
        let const error =
            ErrorWithLocation{preflight_location, preflight_message.view()};
        let diagnostic = String{cxt.scratch_allocator()};
        diagnostic += error.to_string(
            source != nullptr ? source->view() : StringView{}, &cxt);
        diagnostic.push('\n');
        let diagnostic_out = ec.out_fd;
        let diagnostic_err = ec.err_fd;
        ec.apply_dup_routing(
            [&]() { diagnostic_err = diagnostic_out.value_or(KOSH_STDOUT); },
            [&]() { diagnostic_out = diagnostic_err.value_or(KOSH_STDERR); });
        os::signal_internal_diagnostic();
        if (!os::write_all(diagnostic_err.value_or(KOSH_STDERR),
                           diagnostic.data(), diagnostic.count()))
        {
          let const saved_errno = errno;
          if (saved_errno == EPIPE) throw BrokenPipeExit{};
          throw Error{"Unable to write to stderr: " +
                      os::last_system_error_message()};
        }

        stage_status[stage_index] = *preflight_status;
        ec.close_fds();
      } else if (!forked_child.has_value()) {
        cxt.set_in_pipeline_stage(true);
        defer { cxt.set_in_pipeline_stage(false); };
        ret = execute_builtin(steal(ec), cxt);
        stage_status[stage_index] = ret;
      } else {
        const os::process child = *forked_child;
        if (os::process_id_of(child) == 0) {
          ec.in_fd = koshka::None;
          ec.out_fd = koshka::None;
          ec.err_fd = koshka::None;
          if (last_stdin != KOSH_INVALID_FD) os::close_fd(last_stdin);
          cxt.set_in_pipeline_stage(true);
          cxt.enter_subshell();
          i32 child_status = 0;
          try {
            child_status = execute_builtin(steal(ec), cxt);
          } catch (const BrokenPipeExit &) {
            child_status = KOSH_BROKEN_PIPE_EXIT_STATUS;
          } catch (const ErrorWithLocation &e) {
            const String *source = cxt.current_source();
            koshka::show_message(e.to_string(
                source != nullptr ? source->view() : StringView{}, &cxt));
            child_status = static_cast<i32>(e.command_status());
          } catch (const Error &e) {
            koshka::show_message(e.to_string());
            child_status = static_cast<i32>(e.command_status());
          } catch (...) {
            child_status = 1;
          }
          koshka::flush();
          os::exit_process_immediately(child_status);
        }

        if (is_async && process_group_id == 0) {
          process_group_id = os::process_id_of(child);
        }
        ec.close_fds();
        children.push(child);
        child_stage.push(stage_index);
        last_child = child;
      }
    } else {
      /* The last builtin stage runs in this process so a cd affects the shell.
         The flag makes exec spawn a child rather than replace the shell. */
      cxt.set_in_pipeline_stage(true);
      defer { cxt.set_in_pipeline_stage(false); };
      ret = execute_builtin(steal(ec), cxt);
      stage_status[stage_index] = ret;
    }

    is_first = false;
    stage_index++;
  }

  if (is_async) {
    if (last_child != KOSH_INVALID_PROCESS) {
      cxt.set_last_background_pid(os::process_id_of(last_child));
      const i32 id = cxt.register_pipeline_job(children, last_child, "pipeline",
                                               process_group_id);
      should_reap_children_on_unwind = false;
      if (cxt.shell_is_interactive())
        koshka::print_error(
            "[" + String::from(id, heap_allocator()) + "] " +
            String::from(static_cast<u64>(os::process_id_of(last_child)),
                         heap_allocator()) +
            "\n");
    }
    return ret;
  }

  usize waited_child_count = 0;
  try {
    for (; waited_child_count < children.count(); waited_child_count++)
      stage_status[child_stage[waited_child_count]] =
          os::wait_and_monitor_process(children[waited_child_count]);
  } catch (...) {
    terminate_and_reap_processes(children, waited_child_count);
    should_reap_children_on_unwind = false;
    throw;
  }
  should_reap_children_on_unwind = false;

  let pipe_status = ArrayList<String>{heap_allocator()};
  pipe_status.reserve(stage_count);
  for (usize i = 0; i < stage_count; i++)
    pipe_status.push(String::from(stage_status[i], heap_allocator()));
  cxt.set_indexed_array("PIPESTATUS", steal(pipe_status));

  /* pipefail reports the rightmost failing stage, otherwise the last stage. */
  if (cxt.pipefail()) {
    for (usize i = stage_count; i > 0; i--)
      if (stage_status[i - 1] != 0) return stage_status[i - 1];
    return 0;
  }

  return stage_status[stage_count - 1];
}

/* The one context quit reads the interactive state and the memory-report flag
   from, so quit gates the goodbye on a real interactive prompt and a script,
   a -c, or a subshell exits silently the way dash does. A null pointer, the
   state before the context exists, reads as a non-interactive shell with the
   report off. */
static const EvalContext *QUIT_CONTEXT = nullptr;

fn set_quit_context(const EvalContext *context) wontthrow -> void
{
  QUIT_CONTEXT = context;
}

/* The granular memory report, the live bump bytes and the reserved capacity of
   each arena, then the malloc heap in use. The arena capacity counts the blocks
   the bump allocator holds, while the heap figure counts the String buffers and
   other long-lived allocations the arenas do not own. */
cold fn print_memory_report() wontthrow -> void
{
  if (AST_ARENA != nullptr)
    std::fprintf(stderr,
                 "AST arena: used %zu, reserved %zu, blocks %zu, destructors "
                 "%zu of %zu\n",
                 AST_ARENA->bytes_used(), AST_ARENA->bytes_capacity(),
                 AST_ARENA->block_count(), AST_ARENA->destructor_count(),
                 AST_ARENA->destructor_capacity());
  if (QUIT_CONTEXT != nullptr) {
    let const stats = QUIT_CONTEXT->function_storage_stats();
    std::fprintf(stderr,
                 "Function arenas: used %zu, reserved %zu, blocks %zu, "
                 "destructors %zu of %zu\n",
                 stats.bytes_used, stats.bytes_capacity, stats.block_count,
                 stats.destructor_count, stats.destructor_capacity);
  }
  os::malloc_heap_stats heap_stats{};
  if (os::read_malloc_heap_stats(heap_stats))
    std::fprintf(stderr,
                 "Malloc heap: in use %zu, total arena %zu, mmapped %zu\n",
                 heap_stats.bytes_in_use, heap_stats.arena_bytes,
                 heap_stats.mapped_bytes);
}

[[noreturn]] fn quit(i32 code, farewell_policy farewell) throws -> void
{
  let const should_goodbye = farewell == farewell_policy::Goodbye;
  LOG(Info, "quitting with code %d", code);

  if (QUIT_CONTEXT != nullptr && QUIT_CONTEXT->memory_stats_enabled()) {
    print_memory_report();
  }

  const u8 actual_code = static_cast<u8>(code);

  if (!os::is_child_process()) {
    if (toiletline::is_active()) {
      try {
        toiletline::exit();
      } catch (const Error &e) {
        show_message(e.to_string());
      }
    }

    if (should_goodbye && QUIT_CONTEXT != nullptr &&
        QUIT_CONTEXT->shell_is_interactive())
    {
      if (let const farewell =
              QUIT_CONTEXT->get_variable_value("KOSH_FAREWELL");
          farewell.has_value())
      {
        if (!farewell->is_empty()) {
          let message = String{heap_allocator(), farewell->view()};
          if (code != 0) {
            message += " (Code ";
            message += String::from(actual_code, heap_allocator());
            message += ')';
          }
          show_message(message);
        }
      } else {
        let message = String{heap_allocator(), "Goodbye :c"};
        if (code != 0) {
          message += " (Code ";
          message += String::from(actual_code, heap_allocator());
          message += ')';
        }
        show_message(message);
      }
    }
  }

  std::exit(actual_code);
}

} /* namespace utils */

} /* namespace koshka */
