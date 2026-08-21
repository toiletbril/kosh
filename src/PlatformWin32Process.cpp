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

volatile sig_atomic_t INTERRUPT_REQUESTED = 0;
volatile sig_atomic_t CHILD_STATE_CHANGED = 0;
volatile sig_atomic_t SIGNAL_PENDING = 0;

static constexpr i32 SIGNAL_FLAG_COUNT = 128;
static volatile sig_atomic_t PENDING_SIGNAL_FLAGS[SIGNAL_FLAG_COUNT] = {};

} /* namespace os */
} /* namespace koshka */

namespace koshka {

namespace os {

static fn append_windows_quoted_arg(String &out, StringView arg) throws -> void;

static pure fn is_batch_program(StringView path) wontthrow -> bool;

fn capture_program_output(const ArrayList<String> &argv,
                          u64 timeout_nanos) wontthrow -> Maybe<String>
{
  if (argv.is_empty()) return None;

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_end = INVALID_HANDLE_VALUE;
  HANDLE write_end = INVALID_HANDLE_VALUE;
  if (CreatePipe(&read_end, &write_end, &inheritable, 0) == FALSE) return None;
  defer { CloseHandle(read_end); };
  defer { CloseHandle(write_end); };
  if (SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0) == FALSE)
    return None;

  let const null_input =
      CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &inheritable, OPEN_EXISTING, 0, nullptr);
  if (null_input == INVALID_HANDLE_VALUE) return None;
  defer { CloseHandle(null_input); };

  let application_path = argv[0].c_str();
  let application_path_storage = String{heap_allocator()};
  let command_line = make_os_args(argv);
  if (is_batch_program(argv[0].view())) {
    let batch_command = String{heap_allocator()};
    append_windows_quoted_arg(batch_command, argv[0].view());
    for (usize argument_position = 1; argument_position < argv.count();
         argument_position++)
    {
      batch_command += ' ';
      append_windows_quoted_arg(batch_command, argv[argument_position].view());
    }

    let const command_processor = std::getenv("COMSPEC");
    application_path_storage =
        String{command_processor != nullptr ? command_processor : "cmd.exe"};
    application_path = application_path_storage.c_str();
    let processor_command_line = String{heap_allocator()};
    append_windows_quoted_arg(processor_command_line,
                              application_path_storage.view());
    processor_command_line += " /d /s /c \"";
    processor_command_line += batch_command;
    processor_command_line += '"';
    command_line = steal(processor_command_line);
  }
  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = null_input;
  startup_info.hStdOutput = write_end;
  startup_info.hStdError = write_end;

  PROCESS_INFORMATION process_info{};
  if (CreateProcessA(application_path, const_cast<LPSTR>(command_line.data()),
                     nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                     &startup_info, &process_info) == FALSE)
  {
    return None;
  }
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };
  CloseHandle(write_end);
  write_end = INVALID_HANDLE_VALUE;

  let captured = String{heap_allocator()};
  const u64 deadline_nanos = monotonic_nanos() + timeout_nanos;
  bool was_timed_out = false;
  loop
  {
    DWORD available_byte_count = 0;
    if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available_byte_count,
                      nullptr) == FALSE)
    {
      let const error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) break;
      return None;
    }

    if (available_byte_count != 0) {
      char buffer[4096];
      let const requested_count = available_byte_count < sizeof(buffer)
                                      ? available_byte_count
                                      : sizeof(buffer);
      DWORD read_count = 0;
      if (ReadFile(read_end, buffer, requested_count, &read_count, nullptr) ==
          FALSE)
      {
        return None;
      }
      captured.append(StringView{buffer, static_cast<usize>(read_count)});
      continue;
    }

    if (WaitForSingleObject(process_info.hProcess, 0) == WAIT_OBJECT_0) {
      /* The pipe can report empty just before the final child write arrives. */
      if (WaitForSingleObject(read_end, 1) == WAIT_OBJECT_0) continue;
      break;
    }
    if (monotonic_nanos() >= deadline_nanos) {
      was_timed_out = true;
      break;
    }
    Sleep(1);
  }

  if (was_timed_out) {
    TerminateProcess(process_info.hProcess, 1);
    WaitForSingleObject(process_info.hProcess, INFINITE);
    return None;
  }
  captured.normalize_crlf_line_endings();
  return captured;
}
static pure fn process_is_pid_reference(process p) wontthrow -> bool
{
  return (reinterpret_cast<uintptr_t>(p) & PROCESS_REFERENCE_MASK) ==
         PID_REFERENCE_TAG;
}

static pure fn process_is_group_reference(process p) wontthrow -> bool
{
  return (reinterpret_cast<uintptr_t>(p) & PROCESS_REFERENCE_MASK) ==
         PROCESS_GROUP_REFERENCE_TAG;
}

static pure fn pid_from_reference(process p) wontthrow -> DWORD
{
  return static_cast<DWORD>(reinterpret_cast<uintptr_t>(p) >> 2u);
}

static pure fn process_from_group_reference(process p) wontthrow -> process
{
  return reinterpret_cast<process>(reinterpret_cast<uintptr>(p) &
                                   ~PROCESS_REFERENCE_MASK);
}

fn is_child_process() wontthrow -> bool
{
  return GetCurrentProcessId() != PARENT_SHELL_PID;
}

/* Windows has no setuid or setgid notion. */
fn is_running_setuid() wontthrow -> bool { return false; }
fn drop_elevated_identity() wontthrow -> bool { return true; }

fn process_id_of(process p) wontthrow -> i64
{
  if (process_is_pid_reference(p)) return pid_from_reference(p);
  if (process_is_group_reference(p)) p = process_from_group_reference(p);
  return static_cast<i64>(GetProcessId(p));
}

fn process_group_of(process p) throws -> process
{
  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (DuplicateHandle(GetCurrentProcess(), p, GetCurrentProcess(), &duplicate,
                      0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
  {
    let message = last_system_error_message();
    let const group = reinterpret_cast<process>(reinterpret_cast<uintptr>(p) |
                                                PROCESS_GROUP_REFERENCE_TAG);
    signal_process(group, 9);
    throw Error{"Could not retain the timeout process group: " + message};
  }

  return reinterpret_cast<process>(reinterpret_cast<uintptr>(duplicate) |
                                   PROCESS_GROUP_REFERENCE_TAG);
}

fn close_process_group(process group) wontthrow -> void
{
  if (group == nullptr || !process_is_group_reference(group)) return;
  CloseHandle(process_from_group_reference(group));
}
fn process_has_id(process p, i64 id) wontthrow -> bool
{
  return process_id_of(p) == id;
}

struct inherited_handle_state
{
  HANDLE handle{INVALID_HANDLE_VALUE};
  DWORD original_flags{0};
  bool should_restore{false};
};

static fn make_handle_inheritable(HANDLE handle,
                                  inherited_handle_state &state) wontthrow
    -> void
{
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;

  DWORD flags = 0;
  if (GetHandleInformation(handle, &flags) == FALSE) return;
  if ((flags & HANDLE_FLAG_INHERIT) != 0) return;
  if (SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ==
      FALSE)
    return;

  state.handle = handle;
  state.original_flags = flags;
  state.should_restore = true;
}

static fn
restore_handle_inheritance(const inherited_handle_state &state) wontthrow
    -> void
{
  if (!state.should_restore) return;
  SetHandleInformation(state.handle, HANDLE_FLAG_INHERIT,
                       state.original_flags & HANDLE_FLAG_INHERIT);
}

static fn timeout_job_name(HANDLE process_handle, char (&name)[64]) wontthrow
    -> bool
{
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME kernel_time;
  FILETIME user_time;
  if (GetProcessTimes(process_handle, &creation_time, &exit_time, &kernel_time,
                      &user_time) == FALSE)
    return false;

  let const creation_ticks =
      (static_cast<u64>(creation_time.dwHighDateTime) << 32u) |
      creation_time.dwLowDateTime;
  ::snprintf(name, sizeof(name), "kosh-timeout-%lu-%llu",
             static_cast<unsigned long>(GetProcessId(process_handle)),
             static_cast<unsigned long long>(creation_ticks));
  return true;
}

enum class timeout_job_lifetime : u8
{
  DescendantOwned,
  LeaderOwned,
};

static fn attach_timeout_job(const PROCESS_INFORMATION &process_info,
                             timeout_job_lifetime lifetime) throws -> void
{
  char job_name[64];
  if (!timeout_job_name(process_info.hProcess, job_name))
    throw Error{last_system_error_message()};
  let const job = CreateJobObjectA(nullptr, job_name);
  if (job == nullptr) throw Error{last_system_error_message()};
  defer { CloseHandle(job); };
  if (GetLastError() == ERROR_ALREADY_EXISTS)
    throw Error{"timeout job already exists"};

  if (lifetime == timeout_job_lifetime::LeaderOwned) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &job_limits, sizeof(job_limits)) == FALSE)
      throw Error{last_system_error_message()};
  }

  if (AssignProcessToJobObject(job, process_info.hProcess) == FALSE)
    throw Error{last_system_error_message()};

  HANDLE child_job_handle = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), job, process_info.hProcess,
                      &child_job_handle, 0,
                      lifetime == timeout_job_lifetime::DescendantOwned,
                      DUPLICATE_SAME_ACCESS) == FALSE)
    throw Error{last_system_error_message()};
}

static pure fn is_batch_program(StringView path) wontthrow -> bool
{
  if (path.length < 4) return false;
  let const suffix = path.substring_of_length(path.length - 4, 4);
  return suffix[0] == '.' && utils::ascii_to_lower(suffix[1]) == 'b' &&
         utils::ascii_to_lower(suffix[2]) == 'a' &&
         utils::ascii_to_lower(suffix[3]) == 't';
}

fn execute_program(ExecContext &ec, script_fallback_policy fallback,
                   process_group_mode process_group, StringView,
                   terminal_handoff handoff, i64 process_group_id) -> process
{
  let const allow_script_fallback = fallback == script_fallback_policy::Allow;
  unused(process_group_id);
  let const should_create_new_process_group =
      process_group == process_group_mode::New ||
      process_group == process_group_mode::NewBackground ||
      process_group == process_group_mode::NewLeaderOwned;
  let const should_attach_timeout_job =
      should_create_new_process_group &&
      process_group != process_group_mode::NewBackground;
  let const job_lifetime = process_group == process_group_mode::NewLeaderOwned
                               ? timeout_job_lifetime::LeaderOwned
                               : timeout_job_lifetime::DescendantOwned;
  let const should_hand_off_controlling_terminal_before_start =
      handoff == terminal_handoff::BeforeStart;
  let const should_start_suspended =
      should_attach_timeout_job ||
      should_hand_off_controlling_terminal_before_start;
  LOG(Debug, "spawning '%s' with %zu arguments", ec.program_path().c_str(),
      ec.args().count());

  let application_path = ec.program_path().c_str();
  String resolved_program_path_storage{heap_allocator()};
  if (let resolved_program_path = canonical_path(ec.program_path())) {
    resolved_program_path_storage = resolved_program_path->text().clone();
    application_path = resolved_program_path_storage.c_str();
  }

  String application_path_storage{heap_allocator()};
  String working_directory_storage{heap_allocator()};
  const char *working_directory = nullptr;
  String command_line = make_os_args(ec.args());
  if (is_batch_program(ec.program_path().text().view())) {
    let batch_command = String{heap_allocator()};
    append_windows_quoted_arg(batch_command,
                              resolved_program_path_storage.is_empty()
                                  ? ec.program_path().text().view()
                                  : resolved_program_path_storage.view());
    for (usize argument_index = 1; argument_index < ec.args().count();
         argument_index++)
    {
      batch_command += ' ';
      append_windows_quoted_arg(batch_command,
                                ec.args()[argument_index].view());
    }

    let const command_processor = std::getenv("COMSPEC");
    application_path_storage =
        String{command_processor != nullptr ? command_processor : "cmd.exe"};
    application_path = application_path_storage.c_str();
    let processor_command_line = String{heap_allocator()};
    append_windows_quoted_arg(processor_command_line,
                              application_path_storage.view());
    processor_command_line += " /d /s /c \"";
    processor_command_line += batch_command;
    processor_command_line += '"';
    command_line = steal(processor_command_line);

    let const current_directory = read_current_directory();
    let current_directory_text = current_directory.text().view();
    if (current_directory_text.length >= 7 &&
        current_directory_text.starts_with(StringView{"\\\\?\\"}) &&
        current_directory_text[5] == ':' &&
        is_directory_separator(current_directory_text[6]))
    {
      current_directory_text = current_directory_text.substring(4);
      working_directory_storage.append(current_directory_text);
      working_directory = working_directory_storage.c_str();
    }
  }

  PROCESS_INFORMATION process_info{};
  STARTUPINFOA startup_info{};

  startup_info.cb = sizeof(startup_info);
  startup_info.hStdInput = ec.in_fd.value_or(GetStdHandle(STD_INPUT_HANDLE));
  startup_info.hStdOutput = ec.out_fd.value_or(GetStdHandle(STD_OUTPUT_HANDLE));
  startup_info.hStdError = ec.err_fd.value_or(GetStdHandle(STD_ERROR_HANDLE));

  /* Each dup reads the current target of its source, so the source order
     decides a mixed 2>&1 1>&2. */
  ec.apply_dup_routing(
      [&]() { startup_info.hStdError = startup_info.hStdOutput; },
      [&]() { startup_info.hStdOutput = startup_info.hStdError; });

  bool were_handles_handed_to_fallback = false;
  defer
  {
    if (!were_handles_handed_to_fallback) ec.close_fds();
  };

  inherited_handle_state input_inheritance{};
  inherited_handle_state output_inheritance{};
  inherited_handle_state error_inheritance{};
  let const has_command_redirection =
      ec.in_fd.has_value() || ec.out_fd.has_value() || ec.err_fd.has_value() ||
      ec.should_duplicate_error_to_output ||
      ec.should_duplicate_output_to_error;
  let const should_use_standard_handles =
      has_command_redirection || !is_fd_a_tty(startup_info.hStdInput) ||
      !is_fd_a_tty(startup_info.hStdOutput) ||
      !is_fd_a_tty(startup_info.hStdError);

  HANDLE null_handle = INVALID_HANDLE_VALUE;
  defer
  {
    if (null_handle != INVALID_HANDLE_VALUE) CloseHandle(null_handle);
  };
  if (should_use_standard_handles) {
    if (startup_info.hStdInput == nullptr ||
        startup_info.hStdInput == INVALID_HANDLE_VALUE ||
        startup_info.hStdOutput == nullptr ||
        startup_info.hStdOutput == INVALID_HANDLE_VALUE ||
        startup_info.hStdError == nullptr ||
        startup_info.hStdError == INVALID_HANDLE_VALUE)
    {
      SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                      TRUE};
      null_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inheritable, OPEN_EXISTING, 0, nullptr);
      if (null_handle == INVALID_HANDLE_VALUE)
        throw ErrorWithLocation{ec.source_location(),
                                last_system_error_message()};
      if (startup_info.hStdInput == nullptr ||
          startup_info.hStdInput == INVALID_HANDLE_VALUE)
        startup_info.hStdInput = null_handle;
      if (startup_info.hStdOutput == nullptr ||
          startup_info.hStdOutput == INVALID_HANDLE_VALUE)
        startup_info.hStdOutput = null_handle;
      if (startup_info.hStdError == nullptr ||
          startup_info.hStdError == INVALID_HANDLE_VALUE)
        startup_info.hStdError = null_handle;
    }

    startup_info.dwFlags = STARTF_USESTDHANDLES;
    make_handle_inheritable(startup_info.hStdInput, input_inheritance);
    make_handle_inheritable(startup_info.hStdOutput, output_inheritance);
    make_handle_inheritable(startup_info.hStdError, error_inheritance);
  }
  defer { restore_handle_inheritance(input_inheritance); };
  defer { restore_handle_inheritance(output_inheritance); };
  defer { restore_handle_inheritance(error_inheritance); };

  /* An empty CreateProcess environment block is two nulls, a null pointer would
     inherit the shell's environment. */
  char empty_environment_block[] = {'\0', '\0'};
  LPVOID environment_block =
      ec.should_use_empty_environment ? empty_environment_block : nullptr;

  DWORD creation_flags =
      should_create_new_process_group ? CREATE_NEW_PROCESS_GROUP : 0;
  if (should_start_suspended) creation_flags |= CREATE_SUSPENDED;

  /* CreateProcessA may rewrite lpCommandLine in place, so it is passed mutable.
   */
  if (CreateProcessA(application_path, const_cast<LPSTR>(command_line.data()),
                     nullptr, nullptr, should_use_standard_handles,
                     creation_flags, environment_block, working_directory,
                     &startup_info, &process_info) == 0)
  {
    if (allow_script_fallback && GetLastError() == ERROR_BAD_EXE_FORMAT) {
      if (!resolved_program_path_storage.is_empty())
        ec.set_program_path(Path{resolved_program_path_storage.view()});
      were_handles_handed_to_fallback = true;
      return KOSH_INVALID_PROCESS;
    }
    throw ErrorWithLocation{ec.source_location(), last_system_error_message()};
  }

  if (should_attach_timeout_job) {
    try {
      attach_timeout_job(process_info, job_lifetime);
    } catch (const ErrorBase &error) {
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      relocate_error(error, ec.source_location());
    } catch (...) {
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      throw;
    }
  }
  if (should_hand_off_controlling_terminal_before_start)
    give_controlling_terminal_to(process_info.hProcess);
  if (should_start_suspended) {
    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
      let const message = last_system_error_message();
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      throw ErrorWithLocation{ec.source_location(), steal(message)};
    }
  }

  CloseHandle(process_info.hThread);
  return process_info.hProcess;
}

struct process_substitution_temp_result
{
  String path{heap_allocator()};
  String diagnostic_output{heap_allocator()};
  bool has_shell_diagnostic{false};
};

static fn run_substitution_to_temp(StringView source, bool bash_compatible,
                                   bool source_traces_enabled) throws
    -> Maybe<process_substitution_temp_result>
{
  /* Windows has no fork, so <(cmd) spawns a fresh shell that writes its output
     into a temp file the consumer reads by path. The whole output is written
     before the path returns. */
  let const module_path = current_executable_path();
  if (!module_path.has_value()) return koshka::None;

  char temp_dir[MAX_PATH];
  let const temp_directory_length = GetTempPathA(MAX_PATH, temp_dir);
  if (temp_directory_length == 0 || temp_directory_length >= MAX_PATH) {
    return koshka::None;
  }
  char temp_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, temp_path) == 0) return koshka::None;
  bool should_delete_temp_path = true;
  defer
  {
    if (should_delete_temp_path) DeleteFileA(temp_path);
  };

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const HANDLE temp_file =
      CreateFileA(temp_path, GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (temp_file == INVALID_HANDLE_VALUE) return koshka::None;
  defer { CloseHandle(temp_file); };

  char diagnostic_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, diagnostic_path) == 0)
    return koshka::None;
  defer { DeleteFileA(diagnostic_path); };
  const HANDLE diagnostic_file =
      CreateFileA(diagnostic_path, GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (diagnostic_file == INVALID_HANDLE_VALUE) return koshka::None;
  bool is_diagnostic_file_open = true;
  defer
  {
    if (is_diagnostic_file_open) CloseHandle(diagnostic_file);
  };

  char diagnostic_marker_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, diagnostic_marker_path) == 0)
    return koshka::None;
  defer { DeleteFileA(diagnostic_marker_path); };

  let arguments = ArrayList<String>{heap_allocator()};
  arguments.push(String{heap_allocator(), module_path->view()});
  if (bash_compatible) {
    arguments.push(String{heap_allocator(), StringView{"--mood"}});
    arguments.push(String{heap_allocator(), StringView{"bash"}});
  }
  arguments.push(String{heap_allocator(), StringView{"--no-diagnostics"}});
  if (!source_traces_enabled)
    arguments.push(String{heap_allocator(), StringView{"--no-traces"}});
  arguments.push(String{heap_allocator(), StringView{"-c"}});
  arguments.push(String{heap_allocator(), source});
  let command_line = make_os_args(arguments);

  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = temp_file;
  startup_info.hStdError = diagnostic_file;
  inherited_handle_state input_inheritance{};
  make_handle_inheritable(startup_info.hStdInput, input_inheritance);
  defer { restore_handle_inheritance(input_inheritance); };

  PROCESS_INFORMATION process_info{};
  let const previous_root_trace_marker =
      get_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE");
  let const previous_diagnostic_marker =
      get_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER");
  set_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE", "1");
  set_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER",
                           diagnostic_marker_path);
  defer
  {
    if (previous_root_trace_marker.has_value())
      set_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE",
                               previous_root_trace_marker->view());
    else
      unset_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE");
    if (previous_diagnostic_marker.has_value())
      set_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER",
                               previous_diagnostic_marker->view());
    else
      unset_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER");
  };
  if (CreateProcessA(module_path->c_str(),
                     const_cast<LPSTR>(command_line.data()), nullptr, nullptr,
                     TRUE, 0, nullptr, nullptr, &startup_info,
                     &process_info) == 0)
    return koshka::None;
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };
  WaitForSingleObject(process_info.hProcess, INFINITE);

  CloseHandle(diagnostic_file);
  is_diagnostic_file_open = false;
  let diagnostic_output = Path{diagnostic_path}.read_entire_file();

  /* A backslash would read as an escape in the target word, so slashes are
     returned, which CreateFile accepts the same. */
  let result = process_substitution_temp_result{};
  for (const char *byte = temp_path; *byte != '\0'; byte++)
    result.path += *byte == '\\' ? '/' : *byte;
  if (diagnostic_output.has_value())
    result.diagnostic_output = diagnostic_output.take();
  let const marker_size = Path{diagnostic_marker_path}.file_size();
  result.has_shell_diagnostic = marker_size.has_value() && *marker_size != 0;
  should_delete_temp_path = false;
  return result;
}

fn launch_process_substitution(StringView source, bool command_writes_pipe,
                               bool bash_compatible,
                               bool source_traces_enabled) throws
    -> process_substitution_launch
{
  if (!command_writes_pipe)
    throw Error{"Unable to run a >(cmd) process substitution because it is not "
                "supported on this platform"};

  let result =
      run_substitution_to_temp(source, bash_compatible, source_traces_enabled);
  if (!result.has_value())
    throw Error{"Unable to run the process substitution because the inner "
                "shell could not be spawned: " +
                last_system_error_message()};

  return process_substitution_launch{
      .path = steal(result->path),
      .diagnostic_output = steal(result->diagnostic_output),
      .is_temporary_file = true,
      .has_shell_diagnostic = result->has_shell_diagnostic,
  };
}

static fn spawn_subshell_stage(StringView source, Maybe<descriptor> in_fd,
                               Maybe<descriptor> out_fd,
                               Maybe<descriptor> err_fd, mimic_mood mood,
                               process_group_mode process_group) throws
    -> Maybe<process>
{
  /* Windows has no fork, so a compound pipeline stage re-parses its source in a
     fresh shell, returned unwaited for the pipeline to reap. */
  let const module_path = current_executable_path();
  if (!module_path.has_value()) return koshka::None;

  let arguments = ArrayList<String>{heap_allocator()};
  arguments.push(String{heap_allocator(), module_path->view()});
  arguments.push(String{heap_allocator(), StringView{"--privileged"}});
  if (mood != mimic_mood::Default) {
    arguments.push(String{heap_allocator(), StringView{"--mood"}});
    arguments.push(String{heap_allocator(), mood_name(mood)});
  }
  arguments.push(String{heap_allocator(), StringView{"--no-diagnostics"}});
  arguments.push(String{heap_allocator(), StringView{"-c"}});
  arguments.push(String{heap_allocator(), source});
  let command_line = make_os_args(arguments);

  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = in_fd ? *in_fd : GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = out_fd ? *out_fd : GetStdHandle(STD_OUTPUT_HANDLE);
  startup_info.hStdError = err_fd ? *err_fd : GetStdHandle(STD_ERROR_HANDLE);
  inherited_handle_state input_inheritance{};
  inherited_handle_state output_inheritance{};
  inherited_handle_state error_inheritance{};
  make_handle_inheritable(startup_info.hStdInput, input_inheritance);
  make_handle_inheritable(startup_info.hStdOutput, output_inheritance);
  make_handle_inheritable(startup_info.hStdError, error_inheritance);
  defer { restore_handle_inheritance(input_inheritance); };
  defer { restore_handle_inheritance(output_inheritance); };
  defer { restore_handle_inheritance(error_inheritance); };

  PROCESS_INFORMATION process_info{};
  let const creation_flags = process_group == process_group_mode::Inherit
                                 ? static_cast<DWORD>(0)
                                 : CREATE_NEW_PROCESS_GROUP;
  if (CreateProcessA(module_path->c_str(),
                     const_cast<LPSTR>(command_line.data()), nullptr, nullptr,
                     TRUE, creation_flags, nullptr, nullptr, &startup_info,
                     &process_info) == 0)
    return koshka::None;
  CloseHandle(process_info.hThread);
  return process_info.hProcess;
}

fn try_fork_compound_stage(Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
                           Maybe<descriptor> err_fd, SourceLocation location,
                           StringView source, process_group_mode process_group,
                           i64 process_group_id) -> Maybe<process>
{
  unused(in_fd);
  unused(out_fd);
  unused(err_fd);
  unused(location);
  unused(source);
  unused(process_group);
  unused(process_group_id);
  return koshka::None;
}

fn try_fork_job_process() -> Maybe<process> { return koshka::None; }

fn can_fork_evaluator() wontthrow -> bool { return false; }

fn launch_compound_stage(StringView source, Maybe<descriptor> in_fd,
                         Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
                         mimic_mood mood, SourceLocation location,
                         StringView diagnostic_source,
                         process_group_mode process_group,
                         i64 process_group_id) throws -> compound_stage_launch
{
  unused(diagnostic_source);
  if (source.is_empty())
    throw ErrorWithLocation{
        steal(location),
        "A compound command in a pipeline is not supported on this platform"};

  unused(process_group_id);
  let child =
      spawn_subshell_stage(source, in_fd, out_fd, err_fd, mood, process_group);
  if (!child.has_value())
    throw ErrorWithLocation{steal(location),
                            "Could not spawn the compound pipeline stage"};

  return compound_stage_launch{
      .child = *child,
      .should_evaluate_child = false,
  };
}

[[noreturn]] fn exit_process_immediately(i32 status) wontthrow -> void
{
  ExitProcess(static_cast<UINT>(status));
  unreachable("ExitProcess returned while exiting immediately");
}

fn replace_process(ExecContext &&ec) -> void
{
  /* Windows cannot exec in place, so the program runs to completion and the
     shell exits with its status. */
  LOG(Debug, "running '%s' to completion in place of an exec",
      ec.program_path().c_str());
  process child = execute_program(ec, script_fallback_policy::Allow);
  if (child == KOSH_INVALID_PROCESS) {
    redirect_self(ec);
    ec.close_fds();
    return;
  }

  i32 status = wait_and_monitor_process(child);
  ExitProcess(static_cast<UINT>(status));
  unreachable("ExitProcess returned after process replacement");
}

fn redirect_self(const ExecContext &ec) -> void
{
  if (ec.in_fd) replace_descriptor(0, *ec.in_fd);
  if (ec.out_fd) replace_descriptor(1, *ec.out_fd);
  if (ec.err_fd) replace_descriptor(2, *ec.err_fd);
  ec.apply_dup_routing(
      [&]() { replace_descriptor(2, GetStdHandle(STD_OUTPUT_HANDLE)); },
      [&]() { replace_descriptor(1, GetStdHandle(STD_ERROR_HANDLE)); });
}

fn make_pipe() wontthrow -> Maybe<Pipe>
{
  SECURITY_ATTRIBUTES attributes{};

  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  /* Both ends non-inheritable, the child receives only what
     STARTF_USESTDHANDLES names. */
  attributes.bInheritHandle = FALSE;
  attributes.lpSecurityDescriptor = nullptr; /* NOLINT */

  HANDLE in = INVALID_HANDLE_VALUE;
  HANDLE out = INVALID_HANDLE_VALUE;

  if (CreatePipe(&in, &out, &attributes, 0) == 0) {
    if (in != INVALID_HANDLE_VALUE) close_fd(in);
    if (out != INVALID_HANDLE_VALUE) close_fd(out);

    return koshka::None;
  }

  return Pipe{in, out};
}

struct thread_start_context
{
  void (*entry)(opaque *);
  opaque *context;
};

fn thread_trampoline(LPVOID raw_context) -> DWORD
{
  let const start = static_cast<thread_start_context *>(raw_context);
  let const entry = start->entry;
  let const context = start->context;
  os::free_aligned(start);
  entry(context);
  return 0;
}

fn start_thread(void (*entry)(opaque *), opaque *context) wontthrow
    -> Maybe<thread>
{
  let const storage = os::allocate_aligned(sizeof(thread_start_context),
                                           alignof(thread_start_context));
  if (storage == nullptr) return koshka::None;
  let const start = new (storage) thread_start_context{entry, context};
  HANDLE handle =
      CreateThread(nullptr, 0, thread_trampoline, start, 0, nullptr);
  if (handle == nullptr) {
    os::free_aligned(start);
    return koshka::None;
  }
  return thread{handle};
}

fn join_thread(thread t) wontthrow -> void
{
  WaitForSingleObject(t.handle, INFINITE);
  CloseHandle(t.handle);
}

fn wait_and_monitor_process(process p, bool *was_stopped) -> i32
{
  unused(was_stopped);
  defer { CloseHandle(p); };
  if (WaitForSingleObject(p, INFINITE) != WAIT_OBJECT_0)
    throw Error{"Could not wait for the process to finish: " +
                last_system_error_message()};

  DWORD code = -1;
  if (GetExitCodeProcess(p, &code) == 0)
    throw Error{"Could not read the process exit code: " +
                last_system_error_message()};

  return code;
}

fn wait_for_child_state_change() wontthrow -> void {}

fn reap_process_quietly(process p) -> i32
{
  defer { CloseHandle(p); };
  if (WaitForSingleObject(p, INFINITE) != WAIT_OBJECT_0)
    throw Error{"Could not wait for the process to finish: " +
                last_system_error_message()};
  DWORD code = 1;
  GetExitCodeProcess(p, &code);
  return static_cast<i32>(code);
}

fn poll_process(process p, i32 &status_out) wontthrow -> process_state
{
  let const wait_result = WaitForSingleObject(p, 0);
  if (wait_result == WAIT_TIMEOUT) return process_state::Running;
  if (wait_result != WAIT_OBJECT_0) {
    status_out = 0;
    CloseHandle(p);
    return process_state::Exited;
  }

  DWORD code = 0;
  if (GetExitCodeProcess(p, &code) == 0) {
    status_out = 0;
    CloseHandle(p);
    return process_state::Exited;
  }
  status_out = static_cast<i32>(code);
  CloseHandle(p);
  return process_state::Exited;
}

fn signal_process(process p, i32 signal_number) wontthrow -> bool
{
  if (signal_number == 0) {
    if (process_is_group_reference(p)) {
      let const has_members = process_group_has_members(p);
      if (!has_members) SetLastError(ERROR_NOT_FOUND);
      return has_members;
    }
    if (!process_is_pid_reference(p)) {
      let const wait_result = WaitForSingleObject(p, 0);
      if (wait_result == WAIT_TIMEOUT) return true;
      if (wait_result == WAIT_OBJECT_0) SetLastError(ERROR_NOT_FOUND);
      return false;
    }

    let const target = OpenProcess(SYNCHRONIZE, FALSE, pid_from_reference(p));
    if (target == nullptr) return false;
    let const wait_result = WaitForSingleObject(target, 0);
    CloseHandle(target);
    if (wait_result == WAIT_TIMEOUT) return true;
    if (wait_result == WAIT_OBJECT_0) SetLastError(ERROR_NOT_FOUND);
    return false;
  }

  if (!is_process_signal_supported(signal_number)) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
  }

  if (process_is_group_reference(p)) {
    let const process_handle = process_from_group_reference(p);
    char job_name[64];
    if (!timeout_job_name(process_handle, job_name)) return false;
    let const job = OpenJobObjectA(JOB_OBJECT_TERMINATE, FALSE, job_name);
    if (job == nullptr) return false;
    let const did_terminate = TerminateJobObject(job, 1) != FALSE;
    CloseHandle(job);
    return did_terminate;
  }

  if (!process_is_pid_reference(p)) return TerminateProcess(p, 1) != 0;

  let const target =
      OpenProcess(PROCESS_TERMINATE, FALSE, pid_from_reference(p));
  if (target == nullptr) return false;
  let const did_terminate = TerminateProcess(target, 1) != 0;
  CloseHandle(target);
  return did_terminate;
}

fn process_group_has_members(process group) wontthrow -> bool
{
  if (!process_is_group_reference(group)) return false;

  let const process_handle = process_from_group_reference(group);
  char job_name[64];
  if (!timeout_job_name(process_handle, job_name)) return false;

  let const job = OpenJobObjectA(JOB_OBJECT_QUERY, FALSE, job_name);
  if (job == nullptr) return false;

  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
  let const did_query =
      QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                &accounting, sizeof(accounting), nullptr);
  CloseHandle(job);
  return did_query != FALSE && accounting.ActiveProcesses > 0;
}

fn is_process_signal_supported(i32 signal_number) wontthrow -> bool
{
  return signal_number == 0 || signal_number == 9 || signal_number == 15;
}

fn process_from_pid(i64 pid) wontthrow -> process
{
  if (pid <= 0 || static_cast<u64>(pid) > UINT32_MAX) return nullptr;
  let const encoded = (static_cast<uintptr_t>(pid) << 2u) | PID_REFERENCE_TAG;
  return reinterpret_cast<process>(encoded);
}

/* The numbers are the POSIX values the shell scripts name, and the Windows
   runtime raises none of them, so the table is the whole of what this platform
   answers. */
static const utils::signal_pair SIGNAL_PAIRS[] = {
    {1,  "HUP" },
    {2,  "INT" },
    {3,  "QUIT"},
    {9,  "KILL"},
    {15, "TERM"},
};

fn signal_number_from_name(StringView name) -> Maybe<i32>
{
  return utils::find_signal_number(SIGNAL_PAIRS, countof(SIGNAL_PAIRS), name);
}

fn signal_name_from_number(i32 number) -> Maybe<String>
{
  return utils::find_signal_name(SIGNAL_PAIRS, countof(SIGNAL_PAIRS), number);
}

fn signal_names() throws -> const ArrayList<StringView> &
{
  static ArrayList<StringView> names =
      utils::collect_signal_names(SIGNAL_PAIRS, countof(SIGNAL_PAIRS));

  return names;
}

/* Quotes and escapes the way CommandLineToArgvW parses back, so an argument
   with a space, tab, or quote cannot inject further arguments. A backslash run
   is doubled only before a quote, an empty argument is quoted so it is kept. */
static fn append_windows_quoted_arg(String &out, StringView arg) -> void
{
  bool should_quote_arg = arg.count() == 0;
  for (usize i = 0; i < arg.count() && !should_quote_arg; i++) {
    const char c = arg[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
      should_quote_arg = true;
    }
  }
  if (!should_quote_arg) {
    out.append(arg);
    return;
  }

  out += '"';
  for (usize i = 0; i < arg.count();) {
    usize backslash_count = 0;
    while (i < arg.count() && arg[i] == '\\') {
      i++;
      backslash_count++;
    }
    if (i == arg.count()) {
      /* Trailing backslashes precede the closing quote, so they are doubled to
         stay literal rather than escaping the quote. */
      for (usize k = 0; k < backslash_count * 2; k++)
        out += '\\';
      break;
    }
    if (arg[i] == '"') {
      /* The backslashes before a quote are doubled and the quote is escaped. */
      for (usize k = 0; k < backslash_count * 2 + 1; k++)
        out += '\\';
      out += '"';
      i++;
    } else {
      for (usize k = 0; k < backslash_count; k++)
        out += '\\';
      out += arg[i];
      i++;
    }
  }
  out += '"';
}

fn make_os_args(const ArrayList<String> &args) -> os_args
{
  ASSERT(args.count() > 0);

  String command_line{heap_allocator()};
  append_windows_quoted_arg(command_line, args[0].view());
  for (usize i = 1; i < args.count(); i++) {
    command_line += ' ';
    append_windows_quoted_arg(command_line, args[i].view());
  }

  return command_line;
}

cold fn last_system_error_message() throws -> String
{
  LPSTR errno_str{};
  DWORD win_errno = GetLastError();

  DWORD ret = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, win_errno, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&errno_str), 0, nullptr); /* NOLINT */

  if (ret == 0) {
    return String::from(win_errno, heap_allocator()) +
           StringView{" (Error message could not be processed due to "
                      "a FormatMessage() failure)"};
  }
  defer { LocalFree(errno_str); };

  StringView view{static_cast<char *>(errno_str)};
  /* FormatMessage ends with a period, spacing, and a CRLF, trimmed here. */
  while (view.length > 0) {
    let const last_byte = view[view.length - 1];
    if (last_byte != '.' && last_byte != ' ' && last_byte != '\r' &&
        last_byte != '\n')
    {
      break;
    }
    view = view.substring_of_length(0, view.length - 1);
  }

  String err{heap_allocator()};
  for (usize i = 0; i < view.length; i++) {
    /* A %N placeholder is replaced with a word since no argument is passed. */
    if (view[i] == '%' && i + 1 < view.length && isdigit(view[i + 1])) {
      err += StringView{"input"};
      i++;
      continue;
    }
    err.push(view[i]);
  }

  if (err.length() > 0) {
    String capitalized{heap_allocator()};
    capitalized.push(static_cast<char>(toupper(err[0])));
    capitalized += err.substring(1);
    err = steal(capitalized);
  }

  return err;
}

fn last_system_error_is_missing_file() wontthrow -> bool
{
  let const error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static fn handle_interrupt(int s) -> void
{
  unused(s);
  INTERRUPT_REQUESTED = 1;
  signal(SIGINT, handle_interrupt);
}

fn set_default_signal_handlers(signal_profile profile) -> void
{
  let const is_interactive = profile == signal_profile::Interactive;
  /* The interactive shell ignores SIGTERM so a stray terminate does not close
     the prompt. */
  if (is_interactive && signal(SIGTERM, SIG_IGN) == SIG_ERR) {
    throw Error{"Could not install the signal handlers: " +
                last_system_error_message()};
  }

  if (signal(SIGINT, handle_interrupt) == SIG_ERR) {
    throw Error{"Could not install the signal handlers: " +
                last_system_error_message()};
  }
}

fn reset_signal_handlers() -> void
{
  if (signal(SIGTERM, SIG_DFL) == SIG_ERR || signal(SIGINT, SIG_DFL) == SIG_ERR)
  {
    throw Error{"Could not restore the default signal handlers: " +
                last_system_error_message()};
  }

  /* A stale inherited flag would throw Interrupted before the child runs. */
  INTERRUPT_REQUESTED = 0;
}

static fn handle_trapped_signal(int signal_number) -> void
{
  if (is_trappable_signal(signal_number))
    PENDING_SIGNAL_FLAGS[signal_number] = 1;
  SIGNAL_PENDING = 1;
  /* The C runtime resets the disposition, so it is reinstalled for the next. */
  signal(signal_number, handle_trapped_signal);
}

fn set_trap_handler(i32 signal_number) -> void
{
  if (!is_trappable_signal(signal_number)) return;
  signal(signal_number, handle_trapped_signal);
}

fn set_trap_ignore(i32 signal_number) -> void
{
  if (!is_trappable_signal(signal_number)) return;
  signal(signal_number, SIG_IGN);
}

fn clear_trap_handler(i32 signal_number) -> void
{
  if (!is_trappable_signal(signal_number)) return;
  if (signal_number == SIGINT)
    signal(signal_number, handle_interrupt);
  else
    signal(signal_number, SIG_DFL);
}

fn monotonic_nanos() wontthrow -> u64
{
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER counter;
  if (frequency.QuadPart == 0) return 0;
  if (QueryPerformanceCounter(&counter) == 0) return 0;
  /* The counter is scaled to nanoseconds through the frequency, splitting the
     whole seconds from the remainder so the multiply never overflows the way a
     raw counter times a billion would. */
  const u64 whole_seconds = counter.QuadPart / frequency.QuadPart;
  const u64 remainder = counter.QuadPart % frequency.QuadPart;
  return whole_seconds * 1000000000ULL +
         (remainder * 1000000000ULL) / static_cast<u64>(frequency.QuadPart);
}

static fn query_parent_process_id() wontthrow -> i64
{
  let const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  defer { CloseHandle(snapshot); };

  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry) == FALSE) return 0;

  let const process_id = GetCurrentProcessId();
  do {
    if (entry.th32ProcessID == process_id)
      return static_cast<i64>(entry.th32ParentProcessID);
  } while (Process32Next(snapshot, &entry) != FALSE);

  return 0;
}

fn get_parent_process_id() wontthrow -> i64
{
  static const i64 parent_process_id = query_parent_process_id();
  return parent_process_id;
}

fn get_real_user_id() wontthrow -> i64 { return 0; }

fn get_effective_user_id() wontthrow -> i64 { return 0; }

fn get_real_group_id() wontthrow -> i64 { return 0; }

fn child_max() wontthrow -> i64 { return 0; }

fn machine_type() throws -> String { return String{"x86_64"}; }

fn executable_system_name() throws -> String { return String{"Windows"}; }

fn executable_machine_name() throws -> String
{
#if defined _M_ARM64 || defined __aarch64__
  return String{"arm64"};
#elif defined _M_X64 || defined __x86_64__
  return String{"x86_64"};
#else
  return machine_type();
#endif
}

fn realtime_microseconds() wontthrow -> u64
{
  FILETIME file_time;
  GetSystemTimePreciseAsFileTime(&file_time);
  ULARGE_INTEGER ticks;
  ticks.LowPart = file_time.dwLowDateTime;
  ticks.HighPart = file_time.dwHighDateTime;
  /* FILETIME counts 100ns intervals since 1601, so the 1970 offset is removed.
   */
  const u64 epoch_offset_100ns = 116444736000000000ULL;
  if (ticks.QuadPart < epoch_offset_100ns) return 0;
  return (ticks.QuadPart - epoch_offset_100ns) / 10ULL;
}

fn format_local_time(StringView format, i64 epoch) throws -> String
{
  const time_t when = epoch < 0 ? time(nullptr) : static_cast<time_t>(epoch);
  struct tm broken_down{};
  localtime_s(&broken_down, &when);
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
  /* Windows has no RUSAGE_CHILDREN, only the wall time is meaningful. */
  user_seconds = 0;
  system_seconds = 0;
}

fn children_peak_rss_bytes() wontthrow -> u64 { return 0; }

fn read_malloc_heap_stats(malloc_heap_stats &stats) wontthrow -> bool
{
  unused(stats);
  return false;
}

fn run_measured(const ArrayList<String> &argv, measured_output output,
                const Maybe<descriptor> &inherited_handle) throws
    -> Maybe<measured_result>
{
  let const suppress_output = output == measured_output::Suppress;
  if (argv.is_empty()) return None;

  /* Windows has no hardware perf counters, only wall time and peak working set.
   */
  measured_result result{};

  let command_line = make_os_args(argv);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  HANDLE null_handle = INVALID_HANDLE_VALUE;
  if (suppress_output) {
    SECURITY_ATTRIBUTES inherit_sa{};
    inherit_sa.nLength = sizeof(inherit_sa);
    inherit_sa.bInheritHandle = TRUE;
    null_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit_sa,
                              OPEN_EXISTING, 0, nullptr);
    if (null_handle != INVALID_HANDLE_VALUE) {
      if (startup.hStdInput == nullptr ||
          startup.hStdInput == INVALID_HANDLE_VALUE)
        startup.hStdInput = null_handle;
      startup.hStdOutput = null_handle;
      startup.hStdError = null_handle;
    }
  }
  defer
  {
    if (null_handle != INVALID_HANDLE_VALUE) CloseHandle(null_handle);
  };

  PROCESS_INFORMATION process_info{};
  let const standard_handles_are_valid =
      startup.hStdInput != nullptr &&
      startup.hStdInput != INVALID_HANDLE_VALUE &&
      startup.hStdOutput != nullptr &&
      startup.hStdOutput != INVALID_HANDLE_VALUE &&
      startup.hStdError != nullptr && startup.hStdError != INVALID_HANDLE_VALUE;
  let const should_use_standard_handles =
      standard_handles_are_valid &&
      (suppress_output || inherited_handle.has_value());
  if (should_use_standard_handles) startup.dwFlags = STARTF_USESTDHANDLES;

  inherited_handle_state input_inheritance{};
  inherited_handle_state output_inheritance{};
  inherited_handle_state error_inheritance{};
  inherited_handle_state extra_inheritance{};
  if (should_use_standard_handles) {
    make_handle_inheritable(startup.hStdInput, input_inheritance);
    make_handle_inheritable(startup.hStdOutput, output_inheritance);
    make_handle_inheritable(startup.hStdError, error_inheritance);
  }
  if (inherited_handle.has_value())
    make_handle_inheritable(*inherited_handle, extra_inheritance);
  defer { restore_handle_inheritance(input_inheritance); };
  defer { restore_handle_inheritance(output_inheritance); };
  defer { restore_handle_inheritance(error_inheritance); };
  defer { restore_handle_inheritance(extra_inheritance); };

  const u64 start_nanos = monotonic_nanos();
  let const should_inherit_handles =
      should_use_standard_handles || inherited_handle.has_value();

  /* CreateProcessA may rewrite lpCommandLine in place. */
  if (CreateProcessA(nullptr, const_cast<LPSTR>(command_line.data()), nullptr,
                     nullptr, should_inherit_handles, 0, nullptr, nullptr,
                     &startup, &process_info) == 0)
    return None;
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };

  if (WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0)
    return None;

  result.wall_nanos = monotonic_nanos() - start_nanos;

  DWORD exit_code = 0;
  if (GetExitCodeProcess(process_info.hProcess, &exit_code) == FALSE)
    return None;
  result.exit_status = static_cast<i64>(exit_code);

  PROCESS_MEMORY_COUNTERS memory_counters{};
  memory_counters.cb = sizeof(memory_counters);
  if (GetProcessMemoryInfo(process_info.hProcess, &memory_counters,
                           sizeof(memory_counters)) != 0)
    result.peak_rss_bytes =
        static_cast<u64>(memory_counters.PeakWorkingSetSize);

  return result;
}
fn enumerate_processes(process_detail detail) throws -> ArrayList<process_entry>
{
  /* The snapshot has no per-process resource stats, so the BSD columns stay
   * zero. */
  unused(detail);
  ArrayList<process_entry> processes{heap_allocator()};
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return processes;
  defer { CloseHandle(snapshot); };

  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry) == 0) return processes;
  do {
    process_entry process{};
    process.pid = static_cast<i64>(entry.th32ProcessID);
    process.name = String{entry.szExeFile};
    /* The snapshot exposes only the executable name, used as the command line.
     */
    process.command_line = process.name.clone();
    processes.push(steal(process));
  } while (Process32Next(snapshot, &entry) != 0);
  return processes;
}

} /* namespace os */

} /* namespace koshka */
