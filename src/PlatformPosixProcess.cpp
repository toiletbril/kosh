#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace os {

static fn fork_job_process() throws -> process;

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
fn close_process_group(process group) wontthrow -> void { unused(group); }
fn process_has_id(process p, i64 id) wontthrow -> bool
{
  return p == static_cast<process>(id);
}

/* posix_spawn reports an exec failure through its return value with no waitable
   pid, so a child is forked to give the caller the same pid and status. */
cold fn spawn_failure_child(SourceLocation location, const Path &program_path,
                            int spawn_error, StringView source,
                            process_group_mode process_group,
                            i64 process_group_id) throws -> process
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
    /* 127 for a missing file, 126 for a resolved but unexecutable program. */
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

hot fn execute_program(ExecContext &ec, script_fallback_policy fallback,
                       process_group_mode process_group, StringView source,
                       terminal_handoff handoff, i64 process_group_id) throws
    -> process
{
  let const allow_script_fallback = fallback == script_fallback_policy::Allow;
  let const new_process_group = process_group != process_group_mode::Inherit;
  let const should_hand_off_controlling_terminal_before_start =
      handoff == terminal_handoff::BeforeStart;
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

    koshka::flush();
    let const child = fork_job_process();
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
        show_message(error.to_string(source));
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

  /* A descriptor already on its target slot is left in place, the close would
     shut the live descriptor. */
  if (ec.in_fd && *ec.in_fd != STDIN_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.in_fd, STDIN_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.in_fd);
  }
  if (ec.out_fd && *ec.out_fd != STDOUT_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.out_fd, STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.out_fd);
  }
  if (ec.err_fd && *ec.err_fd != STDERR_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.err_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.err_fd);
  }
  /* The dups come after the files are placed, so 2>&1 sees the final stdout. */
  ec.apply_dup_routing(
      [&]() {
        posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO,
                                         STDERR_FILENO);
      },
      [&]() {
        posix_spawn_file_actions_adddup2(&file_actions, STDERR_FILENO,
                                         STDOUT_FILENO);
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
  /* SIGPIPE is reset so a pipe producer dies rather than inheriting the shell's
     ignore. */
  sigaddset(&default_signals, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &default_signals);

  short spawn_flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  if (new_process_group) {
    ASSERT(process_group != process_group_mode::Join || process_group_id > 0);
    posix_spawnattr_setpgroup(&attr, process_group == process_group_mode::Join
                                         ? static_cast<pid_t>(process_group_id)
                                         : 0);
    spawn_flags |= POSIX_SPAWN_SETPGROUP;
  }
  posix_spawnattr_setflags(&attr, spawn_flags);

  pid_t child_pid = 0;
  char *const empty_environment[] = {nullptr};
  const int spawn_error =
      posix_spawn(&child_pid, ec.program_path().c_str(), &file_actions, &attr,
                  const_cast<char *const *>(child_args.begin()),
                  ec.should_use_empty_environment
                      ? const_cast<char *const *>(empty_environment)
                      : environ);

  /* An ENOEXEC file with no shebang runs as a shell script in place, the POSIX
     behavior. The check runs before the fds close so the script keeps them. */
  if (spawn_error == ENOEXEC && allow_script_fallback) {
    was_fds_handed_to_fallback = true;
    return KOSH_INVALID_PROCESS;
  }

  ec.close_fds();

  if (spawn_error != 0)
    return spawn_failure_child(ec.source_location(), ec.program_path(),
                               spawn_error, source, process_group,
                               process_group_id);

  return child_pid;
}

fn shell_has_controlling_terminal() wontthrow -> bool
{
  return isatty(STDIN_FILENO) == 1;
}

fn capture_program_output(const ArrayList<String> &argv,
                          u64 timeout_nanos) wontthrow -> Maybe<String>
{
  if (argv.is_empty()) return None;

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return None;
  const int read_end = pipe_fds[0];
  const int write_end = pipe_fds[1];

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

  /* The shell ignores SIGPIPE. The spawn restores the default in the child, so
     a child that keeps writing after the read end closes on the timeout dies on
     SIGPIPE rather than seeing EPIPE. */
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t default_signals;
  sigemptyset(&default_signals);
  sigaddset(&default_signals, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &default_signals);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);

  pid_t child_pid = 0;
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

  let captured = String{heap_allocator()};
  const u64 deadline_nanos = monotonic_nanos() + timeout_nanos;
  bool was_timed_out = false;
  loop
  {
    const u64 now_nanos = monotonic_nanos();
    if (now_nanos >= deadline_nanos) {
      was_timed_out = true;
      break;
    }
    int remaining_millis =
        static_cast<int>((deadline_nanos - now_nanos) / 1'000'000);
    if (remaining_millis <= 0) remaining_millis = 1;

    struct pollfd watch;
    watch.fd = read_end;
    watch.events = POLLIN;
    watch.revents = 0;
    const int ready = poll(&watch, 1, remaining_millis);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) {
      was_timed_out = true;
      break;
    }

    char buffer[4096];
    const ssize_t read_count = read(read_end, buffer, sizeof(buffer));
    if (read_count < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (read_count == 0) break;
    captured.append(StringView{buffer, static_cast<usize>(read_count)});
  }
  close(read_end);

  if (was_timed_out) signal_process(child_pid, SIGKILL);
  int wait_status = 0;
  waitpid(child_pid, &wait_status, 0);

  if (was_timed_out) return None;
  return captured;
}

fn give_controlling_terminal_to(process p) wontthrow -> void
{
  if (!shell_has_controlling_terminal()) return;
  /* The handoff itself raises SIGTTOU, so it is ignored across the change. */
  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, p);
  signal(SIGTTOU, previous);
}

fn give_controlling_terminal_to_process_group(i64 process_group_id) wontthrow
    -> void
{
  if (!shell_has_controlling_terminal() || process_group_id <= 0) return;
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
    process_group_mode process_group = process_group_mode::Inherit,
    i64 process_group_id = 0) throws -> process
{
  LOG(Debug, "forking a compound pipeline stage");

  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    /* A throw would unwind into the parent's evaluator, the child must exit
       directly. */
    try {
      if (process_group != process_group_mode::Inherit) {
        ASSERT(process_group != process_group_mode::Join ||
               process_group_id > 0);
        let const target_group = process_group == process_group_mode::Join
                                     ? static_cast<pid_t>(process_group_id)
                                     : 0;
        check_syscall(setpgid(0, target_group));
      }

      if (in_fd) {
        check_syscall(dup2(*in_fd, STDIN_FILENO));
        check_syscall(close(*in_fd));
      }
      if (out_fd) {
        check_syscall(dup2(*out_fd, STDOUT_FILENO));
        check_syscall(close(*out_fd));
      }
      if (err_fd) {
        check_syscall(dup2(*err_fd, STDERR_FILENO));
        check_syscall(close(*err_fd));
      }

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

fn try_fork_compound_stage(Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
                           Maybe<descriptor> err_fd, SourceLocation location,
                           StringView source, process_group_mode process_group,
                           i64 process_group_id) throws -> Maybe<process>
{
  return fork_compound_stage(steal(in_fd), steal(out_fd), steal(err_fd),
                             steal(location), source, process_group,
                             process_group_id);
}

fn try_fork_job_process() throws -> Maybe<process>
{
  return fork_job_process();
}

fn can_fork_evaluator() wontthrow -> bool { return true; }

fn launch_process_substitution(StringView source, bool command_writes_pipe,
                               bool bash_compatible,
                               bool source_traces_enabled) throws
    -> process_substitution_launch
{
  unused(source);
  unused(bash_compatible);
  unused(source_traces_enabled);

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

  const descriptor retained_fd = command_writes_pipe ? pipe->in : pipe->out;
  close_fd(command_writes_pipe ? pipe->out : pipe->in);
  make_fd_inheritable(retained_fd);

  let path = String{"/dev/fd/"};
  path += String::from(static_cast<i64>(retained_fd), heap_allocator());
  return process_substitution_launch{
      .path = steal(path),
      .retained_fd = retained_fd,
      .child = child,
  };
}

fn launch_compound_stage(StringView source, Maybe<descriptor> in_fd,
                         Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
                         mimic_mood mood, SourceLocation location,
                         StringView diagnostic_source,
                         process_group_mode process_group,
                         i64 process_group_id) throws -> compound_stage_launch
{
  unused(source);
  unused(mood);
  const process child = fork_compound_stage(
      steal(in_fd), steal(out_fd), steal(err_fd), steal(location),
      diagnostic_source, process_group, process_group_id);
  return compound_stage_launch{
      .child = child,
      .should_evaluate_child = child == 0,
  };
}

[[noreturn]] fn exit_process_immediately(i32 status) wontthrow -> void
{
  _exit(status);
}

fn replace_process(ExecContext &&ec) throws -> void
{
  ASSERT(ec.args().count() > 0, "a program needs at least argv[0]");

  LOG(Debug, "replacing the shell process with '%s'",
      ec.program_path().c_str());

  let const child_args = make_os_args(ec.args());

  if (ec.in_fd) {
    check_syscall(dup2(*ec.in_fd, STDIN_FILENO));
    if (*ec.in_fd != STDIN_FILENO) check_syscall(close(*ec.in_fd));
  }
  if (ec.out_fd) {
    check_syscall(dup2(*ec.out_fd, STDOUT_FILENO));
    if (*ec.out_fd != STDOUT_FILENO) check_syscall(close(*ec.out_fd));
  }
  if (ec.err_fd) {
    check_syscall(dup2(*ec.err_fd, STDERR_FILENO));
    if (*ec.err_fd != STDERR_FILENO) check_syscall(close(*ec.err_fd));
  }
  ec.apply_dup_routing(
      [&]() { check_syscall(dup2(STDOUT_FILENO, STDERR_FILENO)); },
      [&]() { check_syscall(dup2(STDERR_FILENO, STDOUT_FILENO)); });

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

  /* exec -c replaces the inherited environ with a single null, so the program
     starts with an empty environment. execve takes the envp explicitly where
     execv would have read environ. */
  char *const empty_environment[] = {nullptr};
  execve(ec.program_path().c_str(),
         const_cast<char *const *>(child_args.begin()),
         ec.should_use_empty_environment
             ? const_cast<char *const *>(empty_environment)
             : environ);

  let const exec_error = errno;
  if (exec_error == ENOEXEC) return;
  /* The reason is read before the concatenation, which allocates and could
     clobber errno. */
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

  if (pipe(p) != 0) {
    return koshka::None;
  }

  for (descriptor end : p) {
    const int flags = fcntl(end, F_GETFD);
    if (flags != -1) fcntl(end, F_SETFD, flags | FD_CLOEXEC);
  }

  return Pipe{p[0], p[1]};
}

struct thread_start_context
{
  void (*entry)(opaque *);
  opaque *context;
};

fn thread_trampoline(opaque *raw_context) wontthrow -> opaque *
{
  let const start = static_cast<thread_start_context *>(raw_context);
  let const entry = start->entry;
  let const context = start->context;
  os::free_aligned(start);
  entry(context);
  return nullptr;
}

fn start_thread(void (*entry)(opaque *), opaque *context) wontthrow
    -> Maybe<thread>
{
  let const storage = os::allocate_aligned(sizeof(thread_start_context),
                                           alignof(thread_start_context));
  if (storage == nullptr) return koshka::None;
  let const start = new (storage) thread_start_context{entry, context};
  pthread_t handle{};
  if (pthread_create(&handle, nullptr, thread_trampoline, start) != 0) {
    os::free_aligned(start);
    return koshka::None;
  }
  return thread{handle};
}

fn join_thread(thread t) wontthrow -> void { pthread_join(t.handle, nullptr); }

fn wait_and_monitor_process(process pid, bool *was_stopped) throws -> i32
{
  ASSERT(pid >= 0);

  LOG(Debug, "waiting on process %lld", static_cast<long long>(pid));

  i32 status{};
  const int wait_flags = was_stopped != nullptr ? WUNTRACED : 0;
  pid_t changed_pid = 0;

  loop
  {
    changed_pid = waitpid(pid, &status, wait_flags);
    /* A signal interrupted the wait. Retry instead of failing. */
    if (changed_pid == -1 && errno == EINTR) {
      continue;
    }
    check_syscall(changed_pid);
    break;
  }

  if (was_stopped != nullptr && WIFSTOPPED(status)) {
    *was_stopped = true;
    return 128 + WSTOPSIG(status);
  }

  if (WIFSIGNALED(status)) {
    const i32 sig = WTERMSIG(status);
    const char *sig_str = strsignal(sig);
    const String sig_desc =
        (sig_str != nullptr) ? String{sig_str} : String{"Unknown"};

    /* SIGPIPE is reaped silently the way bash and dash do, Ctrl-C prints a bare
       newline, every other signal prints the located process message. */
    if (sig == SIGPIPE) {
    } else if (sig != SIGINT) {
      koshka::print("[Process " + String::from(changed_pid, heap_allocator()) +
                    ": " + sig_desc + ", signal " +
                    String::from(sig, heap_allocator()) + "]\n");
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

  while (CHILD_STATE_CHANGED == 0) {
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
    const pid_t w = waitpid(pid, &status, 0);
    if (w == -1 && errno == EINTR) {
      continue;
    }
    /* The SIGCHLD handler may already have reaped it, a missing child is fine.
     */
    if (w == -1 && errno == ECHILD) {
      return 0;
    }
    if (check_syscall(w) == pid) break;
  }

  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

fn poll_process(process p, i32 &status_out) wontthrow -> process_state
{
  i32 status = 0;
  pid_t result;
  do {
    result = waitpid(p, &status, WNOHANG | WUNTRACED | WCONTINUED);
  } while (result == -1 && errno == EINTR);

  if (result == 0) return process_state::Unchanged;
  if (result == -1) {
    status_out = 0;
    return process_state::Exited;
  }

  if (WIFSTOPPED(status)) {
    status_out = 128 + WSTOPSIG(status);
    return process_state::Stopped;
  }
  if (WIFCONTINUED(status)) return process_state::Running;
  if (WIFSIGNALED(status)) {
    status_out = 128 + WTERMSIG(status);
    return process_state::Exited;
  }
  status_out = WEXITSTATUS(status);
  return process_state::Exited;
}

fn signal_process(process p, i32 signal_number) wontthrow -> bool
{
  return kill(p, signal_number) == 0;
}

fn process_group_has_members(process group) wontthrow -> bool
{
  if (kill(group, 0) == 0) return true;
  return errno == EPERM;
}

fn is_process_signal_supported(i32 signal_number) wontthrow -> bool
{
  return signal_number >= 0 && signal_number < NSIG;
}

fn process_from_pid(i64 pid) wontthrow -> process
{
  return static_cast<process>(pid);
}

static const utils::signal_pair SIGNAL_PAIRS[] = {
    {SIGHUP,  "HUP" },
    {SIGINT,  "INT" },
    {SIGQUIT, "QUIT"},
    {SIGKILL, "KILL"},
    {SIGTERM, "TERM"},
    {SIGSTOP, "STOP"},
    {SIGTSTP, "TSTP"},
    {SIGCONT, "CONT"},
    {SIGUSR1, "USR1"},
    {SIGUSR2, "USR2"},
    {SIGABRT, "ABRT"},
    {SIGALRM, "ALRM"},
    {SIGPIPE, "PIPE"},
};

fn signal_number_from_name(StringView name) throws -> Maybe<i32>
{
  return utils::find_signal_number(SIGNAL_PAIRS, countof(SIGNAL_PAIRS), name);
}

fn signal_name_from_number(i32 number) throws -> Maybe<String>
{
  return utils::find_signal_name(SIGNAL_PAIRS, countof(SIGNAL_PAIRS), number);
}

fn signal_names() throws -> const ArrayList<StringView> &
{
  static ArrayList<StringView> names =
      utils::collect_signal_names(SIGNAL_PAIRS, countof(SIGNAL_PAIRS));

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

fn child_max() wontthrow -> i64
{
  return static_cast<i64>(sysconf(_SC_CHILD_MAX));
}

fn machine_type() throws -> String
{
  static const String cached = []() -> String {
    struct utsname info{};
    if (uname(&info) != 0) return String{"unknown"};
    return String{
        StringView{info.machine, std::strlen(info.machine)}
    };
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
  /* A negative epoch is the current time, so a fixed value renders a fixed time
     while the bash -1 and -2 magic values track the clock. */
  const time_t when = epoch < 0 ? time(nullptr) : static_cast<time_t>(epoch);
  struct tm broken_down{};
  /* localtime_r returns null and leaves the struct unspecified for an epoch
     outside the representable range, so an unchecked struct would feed strftime
     garbage. An out-of-range time renders as empty rather than a wrong date. */
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

fn children_cpu_seconds(double &user_seconds, double &system_seconds) wontthrow
    -> void
{
  struct rusage usage{};
  if (getrusage(RUSAGE_CHILDREN, &usage) != 0) {
    user_seconds = 0;
    system_seconds = 0;
    return;
  }
  user_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                 static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0;
  system_seconds = static_cast<double>(usage.ru_stime.tv_sec) +
                   static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
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

fn transfer_barrier_byte(int descriptor, bool should_write) wontthrow -> bool
{
  char byte = 1;
  ssize_t transfer_count;
  do {
    transfer_count =
        should_write ? write(descriptor, &byte, 1) : read(descriptor, &byte, 1);
  } while (transfer_count == -1 && errno == EINTR);

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

    let const is_ready = transfer_barrier_byte(ready_descriptors[1], true);
    close(ready_descriptors[1]);
    let const should_start = transfer_barrier_byte(start_descriptors[0], false);
    close(start_descriptors[0]);
    if (!is_ready || !should_start) _exit(127);

    execvp(raw_argv[0], const_cast<char *const *>(raw_argv.begin()));
    _exit(127);
  }

  close(ready_descriptors[1]);
  close(start_descriptors[0]);
  let const is_ready = transfer_barrier_byte(ready_descriptors[0], false);
  close(ready_descriptors[0]);
  if (!is_ready) {
    close(start_descriptors[1]);
    while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {}
    return false;
  }

  child_out = {child_pid, start_descriptors[1]};
  return true;
}

fn wait_for_measured_child(pid_t child_pid, i64 &status_out,
                           u64 &peak_rss_out) wontthrow -> bool
{

  int status = 0;
  struct rusage usage{};
  pid_t waited = -1;
  loop
  {
    waited = wait4(child_pid, &status, 0, &usage);
    if (waited == -1 && errno == EINTR) {
      continue;
    }
    break;
  }

  if (waited != child_pid) {
    if (waited == -1 && errno != ECHILD) {
      kill(child_pid, SIGKILL);
      while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {}
    }
    return false;
  }

  if (WIFEXITED(status))
    status_out = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    status_out = 128 + WTERMSIG(status);
  else
    status_out = -1;

  peak_rss_out = platform_peak_rss_bytes(usage.ru_maxrss);

  return true;
}

} /* namespace */

fn run_measured(const ArrayList<String> &argv, measured_output output,
                const Maybe<descriptor> &) throws -> Maybe<measured_result>
{
  if (argv.is_empty()) return None;

  measured_result result{};

  measured_child child{};
  if (!spawn_measured_child(argv, output, child)) return None;

  PlatformPerfSession perf_session;
  bool has_perf = perf_session.prepare(child.pid);
  if (has_perf) has_perf = perf_session.start();
  if (!has_perf) perf_session.cancel();

  const u64 start_nanos = monotonic_nanos();
  let const did_release = transfer_barrier_byte(child.start_descriptor, true);
  close(child.start_descriptor);
  if (!did_release) {
    kill(child.pid, SIGKILL);
    while (waitpid(child.pid, nullptr, 0) == -1 && errno == EINTR) {}
    return None;
  }

  if (!wait_for_measured_child(child.pid, result.exit_status,
                               result.peak_rss_bytes))
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

fn get_priority(priority_target target, i64 id) wontthrow -> Maybe<i32>
{
  errno = 0;
  let const priority =
      getpriority(native_priority_target(target), static_cast<id_t>(id));
  if (priority == -1 && errno != 0) return None;
  return priority;
}

fn set_priority(priority_target target, i64 id, i32 priority) wontthrow -> bool
{
  return setpriority(native_priority_target(target), static_cast<id_t>(id),
                     priority) == 0;
}

fn run_nice(const ArrayList<String> &argv, i32 increment) throws -> Maybe<i32>
{
  if (argv.is_empty()) return None;
  let const raw_argv = make_os_args(argv);
  let const child = fork();
  if (child == -1) return None;
  if (child == 0) {
    errno = 0;
    let current = getpriority(PRIO_PROCESS, 0);
    if (current == -1 && errno != 0) current = 0;
    let target = static_cast<i64>(current) + increment;
    if (target < -20) target = -20;
    if (target > 19) target = 19;
    unused(setpriority(PRIO_PROCESS, 0, static_cast<int>(target)));
    execvp(raw_argv[0], const_cast<char *const *>(raw_argv.begin()));
    _exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  if (waited != child) return None;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return None;
}

fn run_nohup(const ArrayList<String> &argv, descriptor input, descriptor output,
             descriptor error, StringView home) throws -> Maybe<i32>
{
  if (argv.is_empty()) return None;
  let const raw_argv = make_os_args(argv);
  let home_output = String{heap_allocator(), home};
  if (!home_output.is_empty() && home_output.back() != '/') home_output += '/';
  home_output += "nohup.out";

  let const child = fork();
  if (child == -1) return None;
  if (child == 0) {
    signal(SIGHUP, SIG_IGN);
    let child_input = input;
    let child_output = output;
    let child_error = error;
    int null_input = -1;
    int nohup_output = -1;
    if (isatty(child_input)) {
      null_input = open("/dev/null", O_RDONLY);
      if (null_input != -1) child_input = null_input;
    }
    if (isatty(child_output)) {
      nohup_output = open("nohup.out", O_WRONLY | O_APPEND | O_CREAT, 0600);
      if (nohup_output == -1 && !home.is_empty())
        nohup_output =
            open(home_output.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
      if (nohup_output == -1) _exit(127);
      child_output = nohup_output;
    }
    if (isatty(child_error)) child_error = child_output;
    if (child_input != STDIN_FILENO) dup2(child_input, STDIN_FILENO);
    if (child_output != STDOUT_FILENO) dup2(child_output, STDOUT_FILENO);
    if (child_error != STDERR_FILENO) dup2(child_error, STDERR_FILENO);
    if (null_input > STDERR_FILENO) close(null_input);
    if (nohup_output > STDERR_FILENO) close(nohup_output);
    execvp(raw_argv[0], const_cast<char *const *>(raw_argv.begin()));
    _exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  if (waited != child) return None;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return None;
}

} /* namespace os */

} /* namespace koshka */
