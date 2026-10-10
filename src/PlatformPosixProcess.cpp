/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This routed POSIX source fragment implements fork and exec, output capture,
 * pipelines, process substitution, process groups, terminal handoff, waiting
 * and signals, child accounting, measured execution, priorities, platform
 * identity, and the native entry point. The split confines process lifecycle
 * and job-control dependencies to one backend fragment.
 */

#include "CLI.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Utils.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace os {

static fn fork_job_process() throws -> process;

static process_launch_counts PROCESS_LAUNCH_COUNTS{};

static bool IS_TERMINAL_OWNER = true;

static inline fn note_fork_launch() wontthrow -> void
{
  PROCESS_LAUNCH_COUNTS.fork_count++;
}

static inline fn note_exec_launch() wontthrow -> void
{
  PROCESS_LAUNCH_COUNTS.exec_count++;
}

static inline fn note_spawn_launch() wontthrow -> void
{
  note_fork_launch();
  note_exec_launch();
}

template <typename Call>
alwaysinline static fn retry_interrupted(Call do_call) wontthrow
    -> decltype(do_call())
{
  loop
  {
    let const result = do_call();
    if (result != -1 || errno != EINTR) return result;
  }
}

static fn reap_child(pid_t child) wontthrow -> void
{
  unused(retry_interrupted([child] { return waitpid(child, nullptr, 0); }));
}

static fn kill_and_reap(pid_t child) wontthrow -> void
{
  kill(child, SIGKILL);
  reap_child(child);
}

fn get_process_launch_counts() wontthrow -> process_launch_counts
{
  return PROCESS_LAUNCH_COUNTS;
}

fn is_child_process() wontthrow -> bool { return getpid() != PARENT_SHELL_PID; }

fn is_running_setuid() wontthrow -> bool
{
  return geteuid() != getuid() || getegid() != getgid();
}

fn drop_elevated_identity() wontthrow -> bool
{
  let const real_group_id = getgid();
  let const real_user_id = getuid();
  if (setregid(real_group_id, real_group_id) != 0) return false;
  return setreuid(real_user_id, real_user_id) == 0;
}

fn process_id_of(process p) wontthrow -> i64 { return static_cast<i64>(p); }
fn process_group_of(process p) throws -> process { return -p; }
fn close_process_reference(process p) wontthrow -> void { unused(p); }
fn process_has_id(process p, i64 id) wontthrow -> bool
{
  return p == static_cast<process>(id);
}

cold fn spawn_failure_child(SourceLocation location, const Path &program_path,
                            int spawn_error, StringView source,
                            i64 process_group_id,
                            process_group_mode process_group) throws -> process
{
  LOG(Debug, "forking a child to report the spawn failure for '%s'",
      program_path.c_str());

  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    if (process_group != process_group_mode::Inherit) {
      let const target_group = process_group == process_group_mode::Join
                                   ? static_cast<pid_t>(process_group_id)
                                   : 0;
      (void) setpgid(0, target_group);
    }
    errno = spawn_error;
    let error = ErrorWithLocation{steal(location),
                                  "Unable to execute `" + program_path.text() +
                                      "`: " + last_system_error_message()};
    koshka::show_message(error.to_string(source));
    koshka::flush();
    _exit(spawn_error == ENOENT ? 127 : 126);
  }

  if (process_group != process_group_mode::Inherit) {
    let const target_group = process_group == process_group_mode::Join
                                 ? static_cast<pid_t>(process_group_id)
                                 : child_pid;
    (void) setpgid(child_pid, target_group);
  }

  return child_pid;
}

hot fn execute_program(ExecContext &ec,
                       const program_execution_options &options) throws
    -> process
{
  let const allow_script_fallback =
      options.fallback == script_fallback_policy::Allow;
  let const new_process_group =
      options.process_group != process_group_mode::Inherit;
  let const should_hand_off_controlling_terminal_before_start =
      options.handoff == terminal_handoff::BeforeStart;
  ASSERT(ec.args().count() > 0, "a program needs at least argv[0]");

  LOG(Debug, "spawning '%s' with %zu arguments", ec.program_path().c_str(),
      ec.args().count());

  bool was_fds_handed_to_fallback = false;
  defer
  {
    if (!was_fds_handed_to_fallback) ec.close_fds();
  };

  if (should_hand_off_controlling_terminal_before_start) {
    let const start_pipe = os::make_pipe();
    let const outcome_pipe = os::make_pipe();
    if (!start_pipe.has_value() || !outcome_pipe.has_value()) {
      if (start_pipe.has_value()) {
        os::close_fd(start_pipe->in);
        os::close_fd(start_pipe->out);
      }
      if (outcome_pipe.has_value()) {
        os::close_fd(outcome_pipe->in);
        os::close_fd(outcome_pipe->out);
      }
      throw ErrorWithLocation{ec.source_location(),
                              "Could not open the program start gate"};
    }

    process child;
    try {
      koshka::flush();
      child = fork_job_process();
    } catch (...) {
      os::close_fd(start_pipe->in);
      os::close_fd(start_pipe->out);
      os::close_fd(outcome_pipe->in);
      os::close_fd(outcome_pipe->out);
      throw;
    }
    if (child == 0) {
      os::close_fd(start_pipe->out);
      os::close_fd(outcome_pipe->in);
      char start_byte = 0;
      let const start_read = os::read_fd(start_pipe->in, &start_byte, 1);
      os::close_fd(start_pipe->in);
      if (!start_read.has_value() || *start_read == 0) {
        os::exit_process_immediately(1);
      }

      try {
        os::replace_process(steal(ec));
        unused(os::write_fd(outcome_pipe->out, "f", 1));
        os::close_fd(outcome_pipe->out);
        os::exit_process_immediately(0);
      } catch (const ErrorBase &error) {
        show_message(error.to_string(options.source));
        flush();
        os::exit_process_immediately(static_cast<i32>(error.command_status()));
      } catch (...) {
        os::exit_process_immediately(1);
      }
    }

    os::close_fd(start_pipe->in);
    os::close_fd(outcome_pipe->out);
    os::give_controlling_terminal_to(child);
    let const start_write = os::write_fd(start_pipe->out, "x", 1);
    os::close_fd(start_pipe->out);
    if (!start_write.has_value()) {
      unused(os::signal_process(child, 9));
      os::reap_process_quietly(child);
      os::close_fd(outcome_pipe->in);
      throw ErrorWithLocation{ec.source_location(),
                              "Could not release the program start gate"};
    }

    char outcome = 0;
    let const outcome_read = os::read_fd(outcome_pipe->in, &outcome, 1);
    os::close_fd(outcome_pipe->in);
    if (!outcome_read.has_value()) {
      unused(os::signal_process(child, 9));
      os::reap_process_quietly(child);
      throw ErrorWithLocation{ec.source_location(),
                              "Could not read the program start outcome"};
    }
    if (*outcome_read == 1 && outcome == 'f' && allow_script_fallback) {
      os::reap_process_quietly(child);
      was_fds_handed_to_fallback = true;
      return KOSH_INVALID_PROCESS;
    }

    ec.close_fds();
    return child;
  }

  let const child_args = make_os_args(ec.args());

  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  defer { posix_spawn_file_actions_destroy(&file_actions); };

  let const do_keep_across_exec = [](descriptor fd) {
    let const flags = fcntl(fd, F_GETFD);
    if (flags != -1 && (flags & FD_CLOEXEC) != 0)
      fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
  };
  if (ec.in_fd && *ec.in_fd == STDIN_FILENO) do_keep_across_exec(STDIN_FILENO);
  if (ec.in_fd && *ec.in_fd != STDIN_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.in_fd, STDIN_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.in_fd);
  }
  ec.apply_output_routing(
      [&]() {
        if (ec.out_fd && *ec.out_fd == STDOUT_FILENO)
          do_keep_across_exec(STDOUT_FILENO);
        if (ec.out_fd && *ec.out_fd != STDOUT_FILENO) {
          posix_spawn_file_actions_adddup2(&file_actions, *ec.out_fd,
                                           STDOUT_FILENO);
          posix_spawn_file_actions_addclose(&file_actions, *ec.out_fd);
        }
      },
      [&]() {
        if (ec.err_fd && *ec.err_fd == STDERR_FILENO)
          do_keep_across_exec(STDERR_FILENO);
        if (ec.err_fd && *ec.err_fd != STDERR_FILENO) {
          posix_spawn_file_actions_adddup2(&file_actions, *ec.err_fd,
                                           STDERR_FILENO);
          posix_spawn_file_actions_addclose(&file_actions, *ec.err_fd);
        }
      },
      [&]() {
        posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO,
                                         STDERR_FILENO);
      },
      [&]() {
        posix_spawn_file_actions_adddup2(&file_actions, STDERR_FILENO,
                                         STDOUT_FILENO);
      });
  ec.apply_nonstandard_routing(
      [&](os::descriptor file_fd, i32 target_fd) {
        posix_spawn_file_actions_adddup2(&file_actions, file_fd, target_fd);
        if (file_fd != target_fd)
          posix_spawn_file_actions_addclose(&file_actions, file_fd);
      },
      [&](i32 dup_from_fd, i32 target_fd) {
        posix_spawn_file_actions_adddup2(&file_actions, dup_from_fd, target_fd);
      },
      [&](i32 target_fd) {
        let const do_is_closed_by_slot = [&](const Maybe<os::descriptor> &slot,
                                             i32 standard_fd) {
          return slot.has_value() && *slot == target_fd && *slot != standard_fd;
        };

        if (do_is_closed_by_slot(ec.in_fd, STDIN_FILENO)) return;
        if (do_is_closed_by_slot(ec.out_fd, STDOUT_FILENO)) return;
        if (do_is_closed_by_slot(ec.err_fd, STDERR_FILENO)) return;

        if (fcntl(target_fd, F_GETFD) != -1)
          posix_spawn_file_actions_addclose(&file_actions, target_fd);
      });

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  defer { posix_spawnattr_destroy(&attr); };

  sigset_t empty_mask;
  sigemptyset(&empty_mask);
  posix_spawnattr_setsigmask(&attr, &empty_mask);

  sigset_t default_signals;
  sigemptyset(&default_signals);
  sigaddset(&default_signals, SIGINT);
  sigaddset(&default_signals, SIGCHLD);
  if (!IS_PIPE_SIGNAL_IGNORED_BY_TRAP && !WAS_PIPE_SIGNAL_IGNORED_AT_ENTRY)
    sigaddset(&default_signals, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &default_signals);

  short spawn_flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  if (new_process_group) {
    ASSERT(options.process_group != process_group_mode::Join ||
           options.process_group_id > 0);
    posix_spawnattr_setpgroup(&attr,
                              options.process_group == process_group_mode::Join
                                  ? static_cast<pid_t>(options.process_group_id)
                                  : 0);
    spawn_flags |= POSIX_SPAWN_SETPGROUP;
  }
  posix_spawnattr_setflags(&attr, spawn_flags);

  pid_t child_pid = 0;
  char *const empty_environment[] = {nullptr};
  note_spawn_launch();
  const int spawn_error =
      posix_spawn(&child_pid, ec.program_path().c_str(), &file_actions, &attr,
                  const_cast<char *const *>(child_args.begin()),
                  ec.should_use_empty_environment
                      ? const_cast<char *const *>(empty_environment)
                      : environ);

  if (spawn_error == ENOEXEC && allow_script_fallback) {
    was_fds_handed_to_fallback = true;
    return KOSH_INVALID_PROCESS;
  }

  ec.close_fds();

  if (spawn_error != 0)
    return spawn_failure_child(ec.source_location(), ec.program_path(),
                               spawn_error, options.source,
                               options.process_group_id, options.process_group);

  return child_pid;
}

fn shell_has_controlling_terminal() wontthrow -> bool
{
  return IS_TERMINAL_OWNER && isatty(STDIN_FILENO) == 1;
}

static fn is_terminal_foreground_group() wontthrow -> bool
{
  return tcgetpgrp(STDIN_FILENO) == getpgrp();
}

fn ProgramCapture::start(const ArrayList<String> &argv,
                         u64 timeout_nanos) wontthrow -> Maybe<ProgramCapture>
{
  if (argv.is_empty()) return None;

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return None;
  const int read_end = pipe_fds[0];
  const int write_end = pipe_fds[1];
  fcntl(read_end, F_SETFD, FD_CLOEXEC);

  const int devnull_fd = open("/dev/null", O_RDONLY);
  if (devnull_fd < 0) {
    close(read_end);
    close(write_end);
    return None;
  }

  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  posix_spawn_file_actions_adddup2(&file_actions, devnull_fd, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&file_actions, write_end, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&file_actions, write_end, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&file_actions, read_end);
  posix_spawn_file_actions_addclose(&file_actions, write_end);
  posix_spawn_file_actions_addclose(&file_actions, devnull_fd);

  let const raw_args = make_os_args(argv);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t default_signals;
  sigemptyset(&default_signals);
  sigaddset(&default_signals, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &default_signals);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);

  pid_t child_pid = 0;
  note_spawn_launch();
  const int spawn_result =
      posix_spawn(&child_pid, raw_args[0], &file_actions, &attr,
                  const_cast<char *const *>(raw_args.begin()), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&file_actions);
  close(write_end);
  close(devnull_fd);
  if (spawn_result != 0) {
    close(read_end);
    return None;
  }

  let capture = ProgramCapture{};
  capture.m_child = child_pid;
  capture.m_output = read_end;
  capture.m_deadline_nanos = monotonic_nanos() + timeout_nanos;
  return capture;
}

fn ProgramCapture::step() wontthrow -> State
{
  if (m_child == KOSH_INVALID_PROCESS) return State::Failed;

  for (u32 step_read_count = 0;
       m_output != KOSH_INVALID_FD && step_read_count < 16; step_read_count++)
  {
    struct pollfd watch;
    watch.fd = m_output;
    watch.events = POLLIN;
    watch.revents = 0;
    const int ready = poll(&watch, 1, 0);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) {
      abandon();
      return State::Failed;
    }
    if (ready == 0) break;

    char buffer[4096];
    const ssize_t read_count = read(m_output, buffer, sizeof(buffer));
    if (read_count < 0 && errno == EINTR) continue;
    if (read_count < 0) {
      abandon();
      return State::Failed;
    }
    if (read_count == 0) {
      close(m_output);
      m_output = KOSH_INVALID_FD;
      break;
    }
    m_captured.append(StringView{buffer, static_cast<usize>(read_count)});
  }

  if (m_output == KOSH_INVALID_FD) {
    int wait_status = 0;
    let const waited_pid = retry_interrupted(
        [&] { return waitpid(m_child, &wait_status, WNOHANG); });
    if (waited_pid == m_child) {
      m_child = KOSH_INVALID_PROCESS;
      return State::Finished;
    }
    if (waited_pid < 0) {
      m_child = KOSH_INVALID_PROCESS;
      return State::Failed;
    }
  }

  if (monotonic_nanos() >= m_deadline_nanos) {
    abandon();
    return State::Failed;
  }

  return State::Running;
}

fn ProgramCapture::wait(u64 wait_nanos) const wontthrow -> void
{
  let const now_nanos = monotonic_nanos();
  let limit_nanos =
      m_deadline_nanos > now_nanos ? m_deadline_nanos - now_nanos : u64{0};
  if (wait_nanos < limit_nanos) limit_nanos = wait_nanos;
  int wait_millis = static_cast<int>(limit_nanos / 1'000'000);
  if (wait_millis <= 0) wait_millis = 1;

  if (m_output == KOSH_INVALID_FD) {
    poll(nullptr, 0, 1);
    return;
  }

  struct pollfd watch;
  watch.fd = m_output;
  watch.events = POLLIN;
  watch.revents = 0;
  poll(&watch, 1, wait_millis);
}

fn ProgramCapture::abandon() wontthrow -> void
{
  if (m_output != KOSH_INVALID_FD) {
    close(m_output);
    m_output = KOSH_INVALID_FD;
  }
  if (m_child == KOSH_INVALID_PROCESS) return;

  kill_and_reap(m_child);
  m_child = KOSH_INVALID_PROCESS;
}

fn give_controlling_terminal_to(process p) wontthrow -> void
{
  if (!shell_has_controlling_terminal() || !is_terminal_foreground_group()) {
    return;
  }

  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, p);
  signal(SIGTTOU, previous);
}

fn give_controlling_terminal_to_process_group(i64 process_group_id) wontthrow
    -> void
{
  if (!shell_has_controlling_terminal() || process_group_id <= 0 ||
      !is_terminal_foreground_group())
  {
    return;
  }

  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, static_cast<pid_t>(process_group_id));
  signal(SIGTTOU, previous);
}

fn reclaim_controlling_terminal() wontthrow -> void
{
  if (!shell_has_controlling_terminal()) return;
  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, getpgrp());
  signal(SIGTTOU, previous);
}

static fn fork_compound_stage(
    Maybe<descriptor> in_fd, Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
    SourceLocation location = {}, StringView source = {},
    i64 process_group_id = 0,
    process_group_mode process_group = process_group_mode::Inherit) throws
    -> process
{
  LOG(Debug, "forking a compound pipeline stage");

  note_fork_launch();
  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    try {
      if (process_group != process_group_mode::Inherit) {
        ASSERT(process_group != process_group_mode::Join ||
               process_group_id > 0);
        let const target_group = process_group == process_group_mode::Join
                                     ? static_cast<pid_t>(process_group_id)
                                     : 0;
        check_syscall(setpgid(0, target_group));
      }

      if (process_group == process_group_mode::NewBackground ||
          process_group == process_group_mode::Join)
      {
        IS_TERMINAL_OWNER = false;
      }

      let const do_lift_above_standard = [](Maybe<descriptor> &fd,
                                            descriptor target) {
        if (!fd || *fd > STDERR_FILENO || *fd == target) return;

        let const lifted = fcntl(*fd, F_DUPFD, STDERR_FILENO + 1);
        check_syscall(lifted);
        check_syscall(close(*fd));
        fd = lifted;
      };
      do_lift_above_standard(in_fd, STDIN_FILENO);
      do_lift_above_standard(out_fd, STDOUT_FILENO);
      do_lift_above_standard(err_fd, STDERR_FILENO);

      let const do_install = [](Maybe<descriptor> fd, descriptor target) {
        if (!fd || *fd == target) return;

        check_syscall(dup2(*fd, target));
        check_syscall(close(*fd));
      };
      do_install(in_fd, STDIN_FILENO);
      do_install(out_fd, STDOUT_FILENO);
      do_install(err_fd, STDERR_FILENO);

      reset_signal_handlers();

#if defined KOSH_HAS_ADDRESS_SANITIZER
      __lsan_disable();
#endif
    } catch (const koshka::Error &e) {
      koshka::show_message(
          ErrorWithLocation{steal(location), e.message()}.to_string(source));
      koshka::flush();
      exit_process_immediately(1);
    } catch (...) {
      LOG(Debug,
          "swallowed an unknown error while preparing the forked stage child");
      exit_process_immediately(1);
    }
  }

  if (process_group != process_group_mode::Inherit) {
    let const target_group = process_group == process_group_mode::Join
                                 ? static_cast<pid_t>(process_group_id)
                                 : child_pid;
    (void) setpgid(child_pid, target_group);
  }

  return child_pid;
}

static fn fork_job_process() throws -> process
{
  LOG(Debug, "forking a mimicked job into its own process group");

  note_fork_launch();
  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    try {
      reset_signal_handlers();
      (void) setpgid(0, 0);

#if defined KOSH_HAS_ADDRESS_SANITIZER
      __lsan_disable();
#endif
    } catch (...) {
      exit_process_immediately(1);
    }
    return 0;
  }

  (void) setpgid(child_pid, child_pid);
  return child_pid;
}

fn try_fork_compound_stage(const fork_compound_stage_options &options) throws
    -> Maybe<process>
{
  return fork_compound_stage(options.in_fd, options.out_fd, options.err_fd,
                             options.location, options.diagnostic_source,
                             options.process_group_id, options.process_group);
}

fn try_fork_job_process() throws -> Maybe<process>
{
  return fork_job_process();
}

fn can_fork_evaluator() wontthrow -> bool { return true; }

static constexpr i32 PROCESS_SUBSTITUTION_FD_FLOOR = 63;

fn launch_process_substitution(const process_substitution_options &options)
    throws -> process_substitution_launch
{
  unused(options.source);
  unused(options.should_trace_sources);
  unused(options.evaluator);

  let const command_writes_pipe =
      options.direction == process_substitution_direction::CommandWrites;

  let const pipe = make_pipe();
  if (!pipe.has_value())
    throw Error{"Could not open a pipe for the process substitution: " +
                last_system_error_message()};

  bool was_pipe_handed_off = false;
  defer
  {
    if (!was_pipe_handed_off) {
      close_fd(pipe->in);
      close_fd(pipe->out);
    }
  };

  const process child = command_writes_pipe
                            ? fork_compound_stage(None, pipe->out, None)
                            : fork_compound_stage(pipe->in, None, None);
  was_pipe_handed_off = true;

  if (child == 0) {
    return process_substitution_launch{
        .child_close_fd = command_writes_pipe ? Maybe<descriptor>{pipe->in}
                                              : Maybe<descriptor>{pipe->out},
        .child = child,
        .should_evaluate_child = true,
    };
  }

  descriptor retained_fd = command_writes_pipe ? pipe->in : pipe->out;
  close_fd(command_writes_pipe ? pipe->out : pipe->in);
  if (let const moved_fd = move_descriptor_to_free_shell_fd(
          retained_fd, PROCESS_SUBSTITUTION_FD_FLOOR);
      moved_fd != -1)
  {
    retained_fd = moved_fd;
  }
  make_fd_inheritable(retained_fd);

  let path = String{"/dev/fd/"};
  path += String::from(static_cast<i64>(retained_fd), heap_allocator());
  return process_substitution_launch{
      .path = steal(path),
      .retained_fd = retained_fd,
      .child = child,
  };
}

fn finish_process_substitution(opaque *cleanup) wontthrow -> void
{
  unused(cleanup);
}

fn release_finished_process_substitution(opaque *cleanup) wontthrow -> bool
{
  unused(cleanup);
  return true;
}

fn launch_compound_stage(const compound_stage_options &options) throws
    -> compound_stage_launch
{
  const process child = fork_compound_stage(
      steal(options.in_fd), steal(options.out_fd), steal(options.err_fd),
      steal(options.location), options.diagnostic_source,
      options.process_group_id, options.process_group);
  return compound_stage_launch{
      .child = child,
      .should_evaluate_child = child == 0,
  };
}

fn take_subshell_bootstrap() wontthrow -> subshell_bootstrap
{
  return subshell_bootstrap{};
}

wontreturn fn exit_process_immediately(i32 status) wontthrow -> void
{
  _exit(status);
}

fn replace_process(ExecContext &&ec) throws -> void
{
  ASSERT(ec.args().count() > 0, "a program needs at least argv[0]");

  LOG(Debug, "replacing the shell process with '%s'",
      ec.program_path().c_str());

  let const child_args = make_os_args(ec.args());

  let const do_install_for_exec = [](descriptor fd, descriptor target) {
    if (fd != target) {
      check_syscall(dup2(fd, target));
      check_syscall(close(fd));
      return;
    }

    let const flags = fcntl(fd, F_GETFD);
    if (flags != -1 && (flags & FD_CLOEXEC) != 0)
      fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
  };
  if (ec.in_fd) do_install_for_exec(*ec.in_fd, STDIN_FILENO);
  ec.apply_output_routing(
      [&]() {
        if (ec.out_fd) do_install_for_exec(*ec.out_fd, STDOUT_FILENO);
      },
      [&]() {
        if (ec.err_fd) do_install_for_exec(*ec.err_fd, STDERR_FILENO);
      },
      [&]() { check_syscall(dup2(STDOUT_FILENO, STDERR_FILENO)); },
      [&]() { check_syscall(dup2(STDERR_FILENO, STDOUT_FILENO)); });
  ec.apply_nonstandard_routing(
      [&](os::descriptor file_fd, i32 target_fd) {
        check_syscall(dup2(file_fd, target_fd));
        if (file_fd != target_fd) check_syscall(close(file_fd));
      },
      [&](i32 dup_from_fd, i32 target_fd) {
        check_syscall(dup2(dup_from_fd, target_fd));
      },
      [&](i32 target_fd) { close(target_fd); });

  sigset_t saved_signal_mask;
  struct sigaction saved_sigchild_action = {};
  struct sigaction saved_sigint_action = {};
  struct sigaction saved_sigpipe_action = {};
  let const saved_interrupt_requested = INTERRUPT_REQUESTED;
  check_syscall(sigprocmask(SIG_SETMASK, nullptr, &saved_signal_mask));
  check_syscall(sigaction(SIGCHLD, nullptr, &saved_sigchild_action));
  check_syscall(sigaction(SIGINT, nullptr, &saved_sigint_action));
  check_syscall(sigaction(SIGPIPE, nullptr, &saved_sigpipe_action));
  defer
  {
    sigaction(SIGCHLD, &saved_sigchild_action, nullptr);
    sigaction(SIGINT, &saved_sigint_action, nullptr);
    sigaction(SIGPIPE, &saved_sigpipe_action, nullptr);
    INTERRUPT_REQUESTED = saved_interrupt_requested;
    sigprocmask(SIG_SETMASK, &saved_signal_mask, nullptr);
  };

  reset_signal_handlers();

  char *const empty_environment[] = {nullptr};
  note_exec_launch();
  execve(ec.program_path().c_str(),
         const_cast<char *const *>(child_args.begin()),
         ec.should_use_empty_environment
             ? const_cast<char *const *>(empty_environment)
             : environ);

  let const exec_error = errno;
  if (exec_error == ENOEXEC) return;
  errno = exec_error;
  let const reason = last_system_error_message();
  let error = koshka::ErrorWithLocation{
      ec.source_location(),
      "Unable to execute `" + ec.program_path().text() + "`: " + reason};
  error.set_command_status(exec_error == ENOENT ? 127 : 126);
  throw error;
}

fn redirect_self(const ExecContext &ec) throws -> void
{
  if (ec.in_fd) check_syscall(dup2(*ec.in_fd, STDIN_FILENO));
  if (ec.out_fd) check_syscall(dup2(*ec.out_fd, STDOUT_FILENO));
  if (ec.err_fd) check_syscall(dup2(*ec.err_fd, STDERR_FILENO));
}

fn make_pipe() wontthrow -> Maybe<Pipe>
{
  LOG(Debug, "opening a close-on-exec pipe");

  descriptor p[2] = {KOSH_INVALID_FD, KOSH_INVALID_FD};

#if defined __linux__ || defined __FreeBSD__ || defined __NetBSD__ ||          \
    defined __OpenBSD__ || defined __DragonFly__
  if (pipe2(p, O_CLOEXEC) != 0) {
    return koshka::None;
  }
#else
  if (pipe(p) != 0) {
    return koshka::None;
  }

  for (descriptor end : p) {
    const int flags = fcntl(end, F_GETFD);
    if (flags != -1) fcntl(end, F_SETFD, flags | FD_CLOEXEC);
  }
#endif

  for (descriptor &end : p) {
    if (end > STDERR_FILENO) continue;

    let const lifted = fcntl(end, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (lifted == -1) {
      close(p[0]);
      close(p[1]);
      return koshka::None;
    }
    close(end);
    end = lifted;
  }

  return Pipe{p[0], p[1]};
}

struct pooled_worker
{
  pthread_t thread_id{};
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  void (*entry)(opaque *){nullptr};
  opaque *context{nullptr};
  bool has_job{false};
  bool is_done{false};
  pooled_worker *next_idle{nullptr};
};

static pthread_mutex_t IDLE_WORKERS_MUTEX = PTHREAD_MUTEX_INITIALIZER;
static pooled_worker *IDLE_WORKERS = nullptr;

static fn forget_idle_workers_in_child() wontthrow -> void
{
  IDLE_WORKERS = nullptr;
  pthread_mutex_init(&IDLE_WORKERS_MUTEX, nullptr);
}

static fn pooled_worker_main(opaque *raw_worker) wontthrow -> opaque *
{
  let const worker = static_cast<pooled_worker *>(raw_worker);

  sigset_t blocked_signals;
  sigfillset(&blocked_signals);
  sigdelset(&blocked_signals, SIGSEGV);
  sigdelset(&blocked_signals, SIGBUS);
  sigdelset(&blocked_signals, SIGFPE);
  sigdelset(&blocked_signals, SIGILL);
  pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr);

  pthread_mutex_lock(&worker->mutex);
  loop
  {
    while (!worker->has_job)
      pthread_cond_wait(&worker->changed, &worker->mutex);

    let const entry = worker->entry;
    let const context = worker->context;
    pthread_mutex_unlock(&worker->mutex);
    entry(context);
    pthread_mutex_lock(&worker->mutex);

    worker->has_job = false;
    worker->is_done = true;
    pthread_cond_broadcast(&worker->changed);
  }

  return nullptr;
}

static fn take_idle_worker() wontthrow -> pooled_worker *
{
  pthread_mutex_lock(&IDLE_WORKERS_MUTEX);
  let const worker = IDLE_WORKERS;
  if (worker != nullptr) IDLE_WORKERS = worker->next_idle;
  pthread_mutex_unlock(&IDLE_WORKERS_MUTEX);

  return worker;
}

static fn create_worker() wontthrow -> pooled_worker *
{
  static bool did_register_fork_handler = false;
  if (!did_register_fork_handler) {
    pthread_atfork(nullptr, nullptr, forget_idle_workers_in_child);
    did_register_fork_handler = true;
  }

  let const storage =
      os::allocate_aligned(sizeof(pooled_worker), alignof(pooled_worker));
  if (storage == nullptr) return nullptr;
  let const worker = new (storage) pooled_worker{};
  if (pthread_create(&worker->thread_id, nullptr, pooled_worker_main, worker) !=
      0)
  {
    os::free_aligned(worker);
    return nullptr;
  }

  pthread_detach(worker->thread_id);

  return worker;
}

fn start_thread(void (*entry)(opaque *), opaque *context) wontthrow
    -> Maybe<thread>
{
  let worker = take_idle_worker();
  if (worker == nullptr) worker = create_worker();
  if (worker == nullptr) return koshka::None;

  pthread_mutex_lock(&worker->mutex);
  worker->entry = entry;
  worker->context = context;
  worker->is_done = false;
  worker->has_job = true;
  pthread_cond_broadcast(&worker->changed);
  pthread_mutex_unlock(&worker->mutex);

  return thread{worker};
}

fn join_thread(thread t) wontthrow -> void
{
  let const worker = static_cast<pooled_worker *>(t.handle);

  pthread_mutex_lock(&worker->mutex);
  while (!worker->is_done)
    pthread_cond_wait(&worker->changed, &worker->mutex);
  worker->is_done = false;
  pthread_mutex_unlock(&worker->mutex);

  pthread_mutex_lock(&IDLE_WORKERS_MUTEX);
  worker->next_idle = IDLE_WORKERS;
  IDLE_WORKERS = worker;
  pthread_mutex_unlock(&IDLE_WORKERS_MUTEX);
}

fn wait_and_monitor_process(process pid, bool *was_stopped) throws -> i32
{
  ASSERT(pid >= 0);

  LOG(Debug, "waiting on process %lld", static_cast<long long>(pid));

  i32 status{};
  const int wait_flags = was_stopped != nullptr ? WUNTRACED : 0;
  let const changed_pid =
      retry_interrupted([&] { return waitpid(pid, &status, wait_flags); });
  check_syscall(changed_pid);

  if (!WIFCONTINUED(status)) note_child_reaped();

  if (was_stopped != nullptr && WIFSTOPPED(status)) {
    *was_stopped = true;
    return 128 + WSTOPSIG(status);
  }

  if (WIFSIGNALED(status)) {
    const i32 sig = WTERMSIG(status);
    const char *sig_str = strsignal(sig);
    const String sig_desc =
        (sig_str != nullptr) ? String{sig_str} : String{"Unknown"};

    if (sig == SIGPIPE) {
    } else if (sig != SIGINT) {
      koshka::print_error(
          "[Process " + String::from(changed_pid, heap_allocator()) + ": " +
          sig_desc + ", signal " + String::from(sig, heap_allocator()) + "]\n");
    } else {
      koshka::print("\n");
    }

    return 128 + sig;
  } else if (!WIFEXITED(status)) {
    throw koshka::Error{"The process did not exit, was not signalled, and did "
                        "not stop: " +
                        last_system_error_message()};
  } else {
    return WEXITSTATUS(status);
  }
}

fn wait_for_child_state_change() wontthrow -> void
{
  sigset_t blocked_signals;
  sigemptyset(&blocked_signals);
  sigaddset(&blocked_signals, SIGCHLD);

  sigset_t previous_mask;
  if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_mask) != 0) return;

  while (CHILD_STATE_CHANGED == 0 && peek_pending_signal_besides_child() == 0) {
    let wait_mask = previous_mask;
    sigdelset(&wait_mask, SIGCHLD);
    sigsuspend(&wait_mask);
  }

  CHILD_STATE_CHANGED = 0;
  (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
}

fn reap_process_quietly(process pid) throws -> i32
{
  ASSERT(pid >= 0);

  LOG(Debug, "quietly reaping process %lld", static_cast<long long>(pid));

  i32 status{};
  loop
  {
    const pid_t w = retry_interrupted([&] { return waitpid(pid, &status, 0); });
    if (w == -1 && errno == ECHILD) {
      return 0;
    }
    if (check_syscall(w) == pid) break;
  }

  note_child_reaped();

  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

fn poll_process(process p, i32 &status_out,
                process_termination *termination_out) wontthrow -> process_state
{
  i32 status = 0;
  let const result = retry_interrupted(
      [&] { return waitpid(p, &status, WNOHANG | WUNTRACED | WCONTINUED); });

  if (result == 0) return process_state::Unchanged;
  if (result == -1) {
    status_out = 0;
    return process_state::Exited;
  }

  if (WIFSTOPPED(status)) {
    note_child_reaped();
    status_out = 128 + WSTOPSIG(status);
    return process_state::Stopped;
  }
  if (WIFCONTINUED(status)) return process_state::Running;

  note_child_reaped();

  if (WIFSIGNALED(status)) {
    status_out = 128 + WTERMSIG(status);
    if (termination_out != nullptr) {
      termination_out->signal_number = WTERMSIG(status);
      termination_out->did_dump_core = WCOREDUMP(status);
    }

    return process_state::Exited;
  }
  status_out = WEXITSTATUS(status);
  return process_state::Exited;
}

fn signal_process(process p, i32 signal_number) wontthrow -> bool
{
  return kill(p, signal_number) == 0;
}

#if defined __linux__
static fn read_process_stat_after_command(i64 process_id, char (&buffer)[512],
                                          StringView &text,
                                          usize &position) wontthrow -> bool
{
  char stat_path[64];
  if (!format_proc_pid_path(stat_path, process_id, "/stat")) return false;

  let const stat_length = read_small_file(stat_path, buffer, sizeof(buffer));
  text = StringView{buffer, stat_length};
  let const command_end = text.find_last_character(')');
  if (!command_end.has_value()) return false;

  position = *command_end + 1;

  return true;
}

static fn is_zombie_process(pid_t process_id) wontthrow -> bool
{
  char stat_buffer[512];
  StringView stat_text;
  usize position = 0;
  if (!read_process_stat_after_command(process_id, stat_buffer, stat_text,
                                       position))
  {
    return false;
  }

  let const state = stat_text.next_ascii_whitespace_word(position);

  return !state.is_empty() && state[0] == 'Z';
}
#endif

fn process_is_running(process p) wontthrow -> bool
{
  if (kill(p, 0) != 0 && errno != EPERM) return false;

#if defined __linux__
  if (p > 0 && is_zombie_process(p)) return false;
#endif

  return true;
}

#if defined __linux__
static pid_t LAST_RUNNING_GROUP_ID = 0;
static i64 LAST_RUNNING_MEMBER_ID = 0;

static fn is_running_group_member(i64 process_id, pid_t group_id) wontthrow
    -> bool
{
  char stat_buffer[512];
  StringView stat_text;
  usize position = 0;
  if (!read_process_stat_after_command(process_id, stat_buffer, stat_text,
                                       position))
  {
    return false;
  }

  let const state = stat_text.next_ascii_whitespace_word(position);
  unused(stat_text.next_ascii_whitespace_word(position));
  let const member_group_id =
      stat_text.next_ascii_whitespace_word(position).to<i64>();
  if (state.is_empty() || member_group_id.is_error() ||
      member_group_id.value() != group_id)
  {
    return false;
  }
  if (state[0] != 'Z') return true;

  for (usize field_number = 6; field_number < 20; field_number++)
    unused(stat_text.next_ascii_whitespace_word(position));

  let const thread_count =
      stat_text.next_ascii_whitespace_word(position).to<i64>();
  return !thread_count.is_error() && thread_count.value() > 1;
}

static fn process_group_has_running_member(pid_t group_id) wontthrow -> bool
{
  if (LAST_RUNNING_GROUP_ID == group_id &&
      is_running_group_member(LAST_RUNNING_MEMBER_ID, group_id))
  {
    return true;
  }

  DIR *proc_directory = ::opendir("/proc");
  if (proc_directory == nullptr) return true;
  defer { ::closedir(proc_directory); };

  for (struct dirent *entry = ::readdir(proc_directory); entry != nullptr;
       entry = ::readdir(proc_directory))
  {
    let const name = StringView{entry->d_name};
    if (name.is_empty() || !name.is_all_decimal_digits()) continue;

    let const process_id = name.to<i64>();
    if (process_id.is_error()) continue;

    if (is_running_group_member(process_id.value(), group_id)) {
      LOG(Debug, "process group %d keeps running member %.*s",
          static_cast<int>(group_id), static_cast<int>(name.length), name.data);
      LAST_RUNNING_GROUP_ID = group_id;
      LAST_RUNNING_MEMBER_ID = process_id.value();
      return true;
    }
  }

  LOG(Debug, "process group %d holds only exited members",
      static_cast<int>(group_id));
  return false;
}
#endif

fn process_group_has_members(process group) wontthrow -> bool
{
  if (kill(group, 0) != 0) return errno == EPERM;

#if defined __linux__
  return process_group_has_running_member(static_cast<pid_t>(-group));
#else
  return true;
#endif
}

fn is_process_signal_supported(i32 signal_number) wontthrow -> bool
{
  return signal_number >= 0 && signal_number < NSIG;
}

fn process_from_pid(i64 pid) wontthrow -> process
{
  if (pid > INT32_MAX || pid < INT32_MIN) return static_cast<process>(INT32_MAX);

  return static_cast<process>(pid);
}

static const utils::signal_pair SIGNAL_PAIRS[] = {
    {SIGHUP,    "HUP"   },
    {SIGINT,    "INT"   },
    {SIGQUIT,   "QUIT"  },
    {SIGILL,    "ILL"   },
    {SIGTRAP,   "TRAP"  },
    {SIGABRT,   "ABRT"  },
#ifdef SIGEMT
    {SIGEMT,    "EMT"   },
#endif
    {SIGFPE,    "FPE"   },
    {SIGKILL,   "KILL"  },
    {SIGBUS,    "BUS"   },
    {SIGSEGV,   "SEGV"  },
    {SIGSYS,    "SYS"   },
    {SIGPIPE,   "PIPE"  },
    {SIGALRM,   "ALRM"  },
    {SIGTERM,   "TERM"  },
    {SIGURG,    "URG"   },
    {SIGSTOP,   "STOP"  },
    {SIGTSTP,   "TSTP"  },
    {SIGCONT,   "CONT"  },
    {SIGCHLD,   "CHLD"  },
    {SIGTTIN,   "TTIN"  },
    {SIGTTOU,   "TTOU"  },
#ifdef SIGIO
    {SIGIO,     "IO"    },
#endif
    {SIGXCPU,   "XCPU"  },
    {SIGXFSZ,   "XFSZ"  },
    {SIGVTALRM, "VTALRM"},
    {SIGPROF,   "PROF"  },
    {SIGWINCH,  "WINCH" },
#ifdef SIGINFO
    {SIGINFO,   "INFO"  },
#endif
    {SIGUSR1,   "USR1"  },
    {SIGUSR2,   "USR2"  },
#ifdef SIGSTKFLT
    {SIGSTKFLT, "STKFLT"},
#endif
#ifdef SIGPWR
    {SIGPWR,    "PWR"   },
#endif
};

static fn build_signal_pairs(ArrayList<String> &names) throws
    -> ArrayList<utils::signal_pair>
{
  let pairs = ArrayList<utils::signal_pair>{heap_allocator()};

#if defined SIGRTMIN && defined SIGRTMAX
  let const lowest = static_cast<i32>(SIGRTMIN);
  let const highest = static_cast<i32>(SIGRTMAX);
  let const generated_count = static_cast<usize>(highest - lowest + 1);
#else
  let const generated_count = usize{0};
#endif

  pairs.reserve(countof(SIGNAL_PAIRS) + generated_count);
  names.reserve(generated_count);

  for (usize i = 0; i < countof(SIGNAL_PAIRS); i++)
    pairs.push(SIGNAL_PAIRS[i]);

#if defined SIGRTMIN && defined SIGRTMAX
  for (i32 number = lowest; number <= highest; number++) {
    let const from_lowest = number - lowest;
    let const from_highest = highest - number;
    let const is_near_lowest = from_lowest <= from_highest;
    let const offset = is_near_lowest ? from_lowest : from_highest;

    let name = String{heap_allocator(), is_near_lowest ? StringView{"RTMIN"}
                                                       : StringView{"RTMAX"}};
    if (offset > 0) {
      name.push(is_near_lowest ? '+' : '-');
      name.append(String::from(offset, heap_allocator()).view());
    }

    names.push(steal(name));
    pairs.push(utils::signal_pair{number, names.back().view()});
  }
#endif

  return pairs;
}

static fn signal_pairs() throws -> const ArrayList<utils::signal_pair> &
{
  static ArrayList<String> names = ArrayList<String>{heap_allocator()};
  static ArrayList<utils::signal_pair> pairs = build_signal_pairs(names);

  return pairs;
}

fn signal_number_from_name(StringView name) throws -> Maybe<i32>
{
  let const &pairs = signal_pairs();

  return utils::find_signal_number(pairs.begin(), pairs.count(), name);
}

fn signal_name_from_number(i32 number) throws -> Maybe<String>
{
  let const &pairs = signal_pairs();

  return utils::find_signal_name(pairs.begin(), pairs.count(), number);
}

fn signal_description_from_number(i32 number) throws -> String
{
  let const description = strsignal(number);
  if (description == nullptr) {
    return "Unknown signal " + String::from(number, heap_allocator());
  }

  return String{description};
}

fn signal_names() throws -> const ArrayList<StringView> &
{
  let const &pairs = signal_pairs();
  static ArrayList<StringView> names =
      utils::collect_signal_names(pairs.begin(), pairs.count());

  return names;
}

hot fn make_os_args(const ArrayList<String> &args) throws -> os_args
{
  ASSERT(args.count() > 0, "argv must carry at least the program name");

  os_args result{heap_allocator()};
  result.reserve(args.count() + 1);

  for (let const &arg : args)
    result.push(arg.c_str());

  result.push(nullptr);

  return result;
}

fn monotonic_nanos() wontthrow -> u64
{
  struct timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<u64>(now.tv_sec) * 1000000000ULL +
         static_cast<u64>(now.tv_nsec);
}

fn get_parent_process_id() wontthrow -> i64
{
  return static_cast<i64>(getppid());
}

fn get_real_user_id() wontthrow -> i64 { return static_cast<i64>(getuid()); }

fn get_effective_user_id() wontthrow -> i64
{
  return static_cast<i64>(geteuid());
}

fn get_real_group_id() wontthrow -> i64 { return static_cast<i64>(getgid()); }

fn get_effective_group_id() wontthrow -> i64
{
  return static_cast<i64>(getegid());
}

fn get_supplementary_group_ids(Allocator allocator) throws -> ArrayList<u32>
{
  let groups = ArrayList<u32>{allocator};
  let const group_count = getgroups(0, nullptr);
  if (group_count < 0) return groups;

  let native_groups = ArrayList<gid_t>{allocator};
  native_groups.reserve(static_cast<usize>(group_count));
  for (int index = 0; index < group_count; index++)
    native_groups.push(0);
  let const read_count = getgroups(group_count, native_groups.begin());
  if (read_count < 0) return groups;
  for (int index = 0; index < read_count; index++)
    groups.push(static_cast<u32>(native_groups[static_cast<usize>(index)]));

  let const effective_group = static_cast<u32>(getegid());
  if (!groups.find(effective_group).has_value()) groups.push(effective_group);
  return groups;
}

fn machine_type() throws -> String
{
  static const String cached = []() -> String {
    struct utsname info{};
    if (uname(&info) != 0) return String{"unknown"};
    let const machine = StringView{info.machine, std::strlen(info.machine)};
#if defined __APPLE__
    if (machine == "arm64") return String{"aarch64"};
#endif
    return String{machine};
  }();
  return cached;
}

fn executable_system_name() throws -> String
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_COSMO
  return String{"any"};
#elif defined __APPLE__
  return String{"Darwin"};
#elif defined __linux__
  return String{"Linux"};
#else
  struct utsname info{};
  if (uname(&info) != 0) return String{"unknown"};
  return String{
      StringView{info.sysname, std::strlen(info.sysname)}
  };
#endif
}

fn system_release_name() throws -> String
{
  struct utsname info{};
  if (uname(&info) != 0) return String{"unknown"};
  return String{info.release};
}

fn system_version_name() throws -> String
{
  struct utsname info{};
  if (uname(&info) != 0) return String{"unknown"};
  return String{info.version};
}

fn machine_target_name() throws -> String
{
#if defined __APPLE__
  return machine_type() + "-apple-darwin" + system_release_name();
#else
  return machine_type() + "-unknown-" + ostype_name();
#endif
}

fn ostype_name() wontthrow -> StringView
{
#if defined __APPLE__
  return "darwin";
#elif defined __GLIBC__
  return "linux-gnu";
#else
  return "linux-musl";
#endif
}

fn executable_machine_name() throws -> String
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_COSMO
  return String{"any"};
#elif defined __aarch64__ || defined __arm64__
  return String{"arm64"};
#elif defined __x86_64__ || defined __amd64__
  return String{"x86_64"};
#else
  return machine_type();
#endif
}

fn realtime_microseconds() wontthrow -> u64
{
  struct timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<u64>(now.tv_sec) * 1000000ULL +
         static_cast<u64>(now.tv_nsec) / 1000ULL;
}

fn format_local_time(StringView format, i64 epoch) throws -> String
{
  const time_t when = epoch < 0 ? time(nullptr) : static_cast<time_t>(epoch);
  struct tm broken_down{};
  if (localtime_r(&when, &broken_down) == nullptr)
    return String{heap_allocator()};
  let const format_string = String{format};
  char buffer[512];
  let const written =
      strftime(buffer, sizeof(buffer), format_string.c_str(), &broken_down);
  return String{
      StringView{buffer, written}
  };
}

fn read_child_cpu_times() wontthrow -> child_cpu_times
{
  child_cpu_times result{};
  struct rusage usage{};
  if (getrusage(RUSAGE_CHILDREN, &usage) != 0) return result;
  result.user_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                        static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0;
  result.system_seconds =
      static_cast<double>(usage.ru_stime.tv_sec) +
      static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
  return result;
}

fn children_peak_rss_bytes() wontthrow -> u64
{
  struct rusage usage{};
  if (getrusage(RUSAGE_CHILDREN, &usage) != 0) return 0;

  return platform_peak_rss_bytes(usage.ru_maxrss);
}

namespace {

struct measured_child
{
  pid_t pid;
  int start_descriptor;
};

enum class barrier_transfer_direction : u8
{
  Read,
  Write,
};

fn transfer_barrier_byte(int descriptor,
                         barrier_transfer_direction direction) wontthrow -> bool
{
  char byte = 1;
  let const transfer_count = retry_interrupted([&] {
    return direction == barrier_transfer_direction::Write
               ? write(descriptor, &byte, 1)
               : read(descriptor, &byte, 1);
  });

  return transfer_count == 1;
}

fn spawn_measured_child(const ArrayList<String> &argv, measured_output output,
                        measured_child &child_out) wontthrow -> bool
{
  let const raw_argv = make_os_args(argv);

  int ready_descriptors[2];
  if (pipe(ready_descriptors) != 0) return false;

  int start_descriptors[2];
  if (pipe(start_descriptors) != 0) {
    close(ready_descriptors[0]);
    close(ready_descriptors[1]);
    return false;
  }

  note_spawn_launch();
  const pid_t child_pid = fork();
  if (child_pid == -1) {
    close(ready_descriptors[0]);
    close(ready_descriptors[1]);
    close(start_descriptors[0]);
    close(start_descriptors[1]);
    return false;
  }

  if (child_pid == 0) {
    close(ready_descriptors[0]);
    close(start_descriptors[1]);
    signal(SIGPIPE, SIG_DFL);
    if (output == measured_output::Suppress) {
      const int null_fd = open("/dev/null", O_WRONLY);
      if (null_fd != -1) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd != STDOUT_FILENO && null_fd != STDERR_FILENO) {
          close(null_fd);
        }
      }
    }

    let const is_ready = transfer_barrier_byte(
        ready_descriptors[1], barrier_transfer_direction::Write);
    close(ready_descriptors[1]);
    let const should_start = transfer_barrier_byte(
        start_descriptors[0], barrier_transfer_direction::Read);
    close(start_descriptors[0]);
    if (!is_ready || !should_start) _exit(127);

    execvp(raw_argv[0], const_cast<char *const *>(raw_argv.begin()));
    _exit(127);
  }

  close(ready_descriptors[1]);
  close(start_descriptors[0]);
  let const is_ready = transfer_barrier_byte(ready_descriptors[0],
                                             barrier_transfer_direction::Read);
  close(ready_descriptors[0]);
  if (!is_ready) {
    close(start_descriptors[1]);
    reap_child(child_pid);
    return false;
  }

  child_out = {child_pid, start_descriptors[1]};
  return true;
}

fn timeval_nanos(const timeval &value) wontthrow -> u64
{
  return static_cast<u64>(value.tv_sec) * 1000000000ULL +
         static_cast<u64>(value.tv_usec) * 1000ULL;
}

fn fill_usage_from_rusage(const struct rusage &usage,
                          process_resource_usage &resources) wontthrow -> void
{
  resources.user_nanos = timeval_nanos(usage.ru_utime);
  resources.system_nanos = timeval_nanos(usage.ru_stime);
  resources.peak_rss_bytes = platform_peak_rss_bytes(usage.ru_maxrss);
  resources.voluntary_context_switch_count = static_cast<u64>(usage.ru_nvcsw);
  resources.involuntary_context_switch_count =
      static_cast<u64>(usage.ru_nivcsw);
  resources.minor_fault_count = static_cast<u64>(usage.ru_minflt);
  resources.major_fault_count = static_cast<u64>(usage.ru_majflt);
  resources.block_input_count = static_cast<u64>(usage.ru_inblock);
  resources.block_output_count = static_cast<u64>(usage.ru_oublock);
}

#if defined __linux__
fn open_proc_io_descriptor(pid_t pid) wontthrow -> int
{
  char path[64];
  unused(format_proc_pid_path(path, pid, "/io"));
  return ::open(path, O_RDONLY | O_CLOEXEC);
}

fn fill_usage_from_proc_io(int io_descriptor,
                           process_resource_usage &resources) wontthrow -> void
{
  struct io_field
  {
    StringView name;
    Maybe<u64> process_resource_usage::*field;
  };
  static constexpr io_field FIELDS[] = {
      {"rchar:", &process_resource_usage::read_byte_count   },
      {"wchar:", &process_resource_usage::written_byte_count},
      {"syscr:", &process_resource_usage::read_call_count   },
      {"syscw:", &process_resource_usage::write_call_count  },
  };

  if (io_descriptor < 0) return;

  char buffer[2048];
  let const length = retry_interrupted(
      [&] { return ::pread(io_descriptor, buffer, sizeof(buffer), 0); });

  if (length <= 0) return;

  let const text = StringView{buffer, static_cast<usize>(length)};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    for (let const &known : FIELDS) {
      if (!line.starts_with(known.name)) continue;
      if (let const parsed = leading_digits(line, known.name.length).to<u64>();
          !parsed.is_error())
        resources.*known.field = parsed.value();
      break;
    }
  }
}
#endif

fn wait_for_measured_child(pid_t child_pid, i64 &status_out, u64 &peak_rss_out,
                           process_resource_usage *resources,
                           int io_descriptor) wontthrow -> bool
{
#if defined __linux__
  if (resources != nullptr) {
    siginfo_t info{};
    let const wait_result = retry_interrupted([&] {
      return waitid(P_PID, static_cast<id_t>(child_pid), &info,
                    WEXITED | WNOWAIT);
    });
    if (wait_result == 0) fill_usage_from_proc_io(io_descriptor, *resources);
  }
#else
  unused(io_descriptor);
#endif

  int status = 0;
  struct rusage usage{};
  let const waited =
      retry_interrupted([&] { return wait4(child_pid, &status, 0, &usage); });

  if (waited != child_pid) {
    if (waited == -1 && errno != ECHILD) kill_and_reap(child_pid);
    return false;
  }

  if (WIFEXITED(status))
    status_out = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    status_out = 128 + WTERMSIG(status);
  else
    status_out = -1;

  peak_rss_out = platform_peak_rss_bytes(usage.ru_maxrss);
  if (resources != nullptr) fill_usage_from_rusage(usage, *resources);

  return true;
}

} /* namespace */

fn read_own_resource_usage() wontthrow -> process_resource_usage
{
  process_resource_usage resources{};
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    fill_usage_from_rusage(usage, resources);
#if defined __linux__
  let const io_descriptor = open_proc_io_descriptor(getpid());
  fill_usage_from_proc_io(io_descriptor, resources);
  if (io_descriptor >= 0) ::close(io_descriptor);
#endif

  return resources;
}

fn run_measured(const ArrayList<String> &argv, const Maybe<descriptor> &,
                measured_output output, bool should_collect_resources) throws
    -> Maybe<measured_result>
{
  if (argv.is_empty()) return None;

  measured_result result{};

  measured_child child{};
  if (!spawn_measured_child(argv, output, child)) return None;

  int io_descriptor = -1;
#if defined __linux__
  if (should_collect_resources)
    io_descriptor = open_proc_io_descriptor(child.pid);
#endif
  defer
  {
    if (io_descriptor >= 0) ::close(io_descriptor);
  };

  PlatformPerfSession perf_session;
  bool has_perf = perf_session.prepare(child.pid);
  if (has_perf) has_perf = perf_session.start();
  if (!has_perf) perf_session.cancel();

  const u64 start_nanos = monotonic_nanos();
  let const did_release = transfer_barrier_byte(
      child.start_descriptor, barrier_transfer_direction::Write);
  close(child.start_descriptor);
  if (!did_release) {
    kill_and_reap(child.pid);
    return None;
  }

  if (!wait_for_measured_child(
          child.pid, result.exit_status, result.peak_rss_bytes,
          should_collect_resources ? &result.resources : nullptr,
          io_descriptor))
    return None;
  result.wall_nanos = monotonic_nanos() - start_nanos;

  if (has_perf) {
    result.has_perf = perf_session.finish(result.perf);
    result.is_perf_system_wide =
        result.has_perf && perf_session.is_system_wide();
  }
  return result;
}

static pure fn native_priority_target(priority_target target) wontthrow -> int
{
  switch (target) {
  case priority_target::Process: return PRIO_PROCESS;
  case priority_target::ProcessGroup: return PRIO_PGRP;
  case priority_target::User: return PRIO_USER;
  }
  unreachable();
}

fn get_priority(i64 id, priority_target target) wontthrow -> Maybe<i32>
{
  errno = 0;
  let const priority =
      getpriority(native_priority_target(target), static_cast<id_t>(id));
  if (priority == -1 && errno != 0) return None;
  return priority;
}

fn set_priority(i64 id, i32 priority, priority_target target) wontthrow -> bool
{
  return setpriority(native_priority_target(target), static_cast<id_t>(id),
                     priority) == 0;
}

template <typename PrepareChild>
static fn run_through_error_pipe(const auto &raw_argv,
                                 PrepareChild do_prepare_child) wontthrow
    -> Maybe<i32>
{
  int exec_error_pipe[2];
  if (pipe(exec_error_pipe) != 0) return None;
  if (fcntl(exec_error_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
    let const saved_errno = errno;
    close(exec_error_pipe[0]);
    close(exec_error_pipe[1]);
    errno = saved_errno;
    return None;
  }

  note_spawn_launch();
  let const child = fork();
  if (child == -1) {
    let const saved_errno = errno;
    close(exec_error_pipe[0]);
    close(exec_error_pipe[1]);
    errno = saved_errno;
    return None;
  }
  if (child == 0) {
    close(exec_error_pipe[0]);
    do_prepare_child(exec_error_pipe[1]);
    execvp(raw_argv[0], const_cast<char *const *>(raw_argv.begin()));
    let const child_errno = errno;
    unused(
        os::write_all(exec_error_pipe[1], &child_errno, sizeof(child_errno)));
    _exit(child_errno == ENOENT ? 127 : 126);
  }

  close(exec_error_pipe[1]);
  int child_errno = 0;
  let const error_length = retry_interrupted([&] {
    return read(exec_error_pipe[0], &child_errno, sizeof(child_errno));
  });
  let const read_errno = errno;
  close(exec_error_pipe[0]);
  int status = 0;
  let const waited =
      retry_interrupted([&] { return waitpid(child, &status, 0); });
  if (waited != child) return None;
  if (error_length == -1) {
    errno = read_errno;
    return None;
  }
  if (error_length == static_cast<ssize_t>(sizeof(child_errno))) {
    errno = child_errno;
    return None;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return None;
}

fn run_nice(const ArrayList<String> &argv, i32 increment) throws -> Maybe<i32>
{
  if (argv.is_empty()) return None;
  let const raw_argv = make_os_args(argv);

  return run_through_error_pipe(raw_argv, [increment](int) {
    errno = 0;
    let current = getpriority(PRIO_PROCESS, 0);
    if (current == -1 && errno != 0) current = 0;
    let target = static_cast<i64>(current) + increment;
    if (target < -20) target = -20;
    if (target > 19) target = 19;
    unused(setpriority(PRIO_PROCESS, 0, static_cast<int>(target)));
  });
}

fn run_nohup(const ArrayList<String> &argv, const nohup_options &options) throws
    -> Maybe<i32>
{
  if (argv.is_empty()) return None;
  let const raw_argv = make_os_args(argv);
  let home_output = String{heap_allocator(), options.home};
  if (!home_output.is_empty() && home_output.back() != '/') home_output += '/';
  home_output += "nohup.out";

  return run_through_error_pipe(raw_argv, [&](int error_descriptor) {
    signal(SIGHUP, SIG_IGN);
    let child_input = options.input;
    let child_output = options.output;
    let child_error = options.error;
    int null_input = -1;
    int nohup_output = -1;
    if (isatty(child_input)) {
      null_input = open("/dev/null", O_RDONLY);
      if (null_input != -1) child_input = null_input;
    }
    if (isatty(child_output)) {
      nohup_output = open("nohup.out", O_WRONLY | O_APPEND | O_CREAT, 0600);
      if (nohup_output == -1 && !options.home.is_empty())
        nohup_output =
            open(home_output.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
      if (nohup_output == -1) {
        let const child_errno = errno;
        unused(
            os::write_all(error_descriptor, &child_errno, sizeof(child_errno)));
        _exit(127);
      }
      child_output = nohup_output;
    }
    if (isatty(child_error)) child_error = child_output;
    if (child_input != STDIN_FILENO) dup2(child_input, STDIN_FILENO);
    if (child_output != STDOUT_FILENO) dup2(child_output, STDOUT_FILENO);
    if (child_error != STDERR_FILENO) dup2(child_error, STDERR_FILENO);
    if (null_input > STDERR_FILENO) close(null_input);
    if (nohup_output > STDERR_FILENO) close(nohup_output);
  });
}

} /* namespace os */

} /* namespace koshka */

fn kosh_main(int argc, char **argv) -> int;

fn main(int argc, char **argv) -> int { return kosh_main(argc, argv); }
