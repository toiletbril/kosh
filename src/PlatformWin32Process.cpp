#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "EvalVariablesInternal.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {
namespace os {

static fn windows_priority_class(i32 priority) wontthrow -> DWORD
{
  if (priority <= -10) return HIGH_PRIORITY_CLASS;
  if (priority < 0) return ABOVE_NORMAL_PRIORITY_CLASS;
  if (priority <= 5) return NORMAL_PRIORITY_CLASS;
  if (priority <= 10) return BELOW_NORMAL_PRIORITY_CLASS;
  return IDLE_PRIORITY_CLASS;
}

static fn priority_from_windows_class(DWORD priority_class) wontthrow
    -> Maybe<i32>
{
  switch (priority_class) {
  case REALTIME_PRIORITY_CLASS: return -20;
  case HIGH_PRIORITY_CLASS: return -10;
  case ABOVE_NORMAL_PRIORITY_CLASS: return -1;
  case NORMAL_PRIORITY_CLASS: return 0;
  case BELOW_NORMAL_PRIORITY_CLASS: return 10;
  case IDLE_PRIORITY_CLASS: return 19;
  default: return None;
  }
}

struct windows_measured_launch_options
{
  DWORD creation_flags{};
  Maybe<descriptor> inherited_handle;
  Maybe<descriptor> input;
  Maybe<descriptor> output;
  Maybe<descriptor> error;
};

static fn
run_measured_with_options(const ArrayList<String> &argv, measured_output output,
                          const windows_measured_launch_options &options) throws
    -> Maybe<measured_result>;

fn get_priority(priority_target target, i64 id) wontthrow -> Maybe<i32>
{
  if (target != priority_target::Process || id < 0 || id > UINT32_MAX) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return None;
  }
  let const process_id =
      id == 0 ? GetCurrentProcessId() : static_cast<DWORD>(id);
  let const process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) return None;
  defer { CloseHandle(process); };
  let const priority_class = GetPriorityClass(process);
  if (priority_class == 0) return None;

  return priority_from_windows_class(priority_class);
}

fn set_priority(priority_target target, i64 id, i32 priority) wontthrow -> bool
{
  if (target != priority_target::Process || id < 0 || id > UINT32_MAX) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
  }
  let const process_id =
      id == 0 ? GetCurrentProcessId() : static_cast<DWORD>(id);
  let const process = OpenProcess(PROCESS_SET_INFORMATION, FALSE, process_id);
  if (process == nullptr) return false;
  defer { CloseHandle(process); };
  return SetPriorityClass(process, windows_priority_class(priority)) != FALSE;
}

fn run_nice(const ArrayList<String> &argv, i32 increment) throws -> Maybe<i32>
{
  let const current_priority_class = GetPriorityClass(GetCurrentProcess());
  if (current_priority_class == 0) return None;
  let const current_priority =
      priority_from_windows_class(current_priority_class);
  if (!current_priority.has_value()) return None;
  let adjusted_priority = static_cast<i64>(*current_priority) + increment;
  if (adjusted_priority < -20) adjusted_priority = -20;
  if (adjusted_priority > 19) adjusted_priority = 19;
  windows_measured_launch_options options{};
  options.creation_flags =
      windows_priority_class(static_cast<i32>(adjusted_priority));
  let const result =
      run_measured_with_options(argv, measured_output::Inherit, options);
  if (!result.has_value()) return None;
  return static_cast<i32>(result->exit_status);
}

fn run_nohup(const ArrayList<String> &argv, descriptor input, descriptor output,
             descriptor error, StringView home) throws -> Maybe<i32>
{
  if (argv.is_empty()) return None;

  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;
  HANDLE null_input = INVALID_HANDLE_VALUE;
  HANDLE nohup_output = INVALID_HANDLE_VALUE;
  let child_input = input;
  let child_output = output;
  let child_error = error;
  defer
  {
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (nohup_output != INVALID_HANDLE_VALUE) CloseHandle(nohup_output);
  };

  if (is_fd_a_tty(child_input)) {
    null_input = CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) return None;
    child_input = null_input;
  }
  if (is_fd_a_tty(child_output)) {
    nohup_output = CreateFileW(L"nohup.out", FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nohup_output == INVALID_HANDLE_VALUE && !home.is_empty()) {
      let home_output = String{heap_allocator(), home};
      if (home_output.back() != '/' && home_output.back() != '\\') {
        home_output += '/';
      }
      home_output += "nohup.out";
      let const wide_home_output =
          utf8_to_wide(home_output.view(), heap_allocator());
      if (!wide_home_output.has_value()) return None;
      nohup_output =
          CreateFileW(wide_home_output->begin(), FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (nohup_output == INVALID_HANDLE_VALUE) return None;
    child_output = nohup_output;
  }
  if (is_fd_a_tty(child_error)) child_error = child_output;
  windows_measured_launch_options options{};
  options.creation_flags = CREATE_NEW_PROCESS_GROUP;
  options.input = child_input;
  options.output = child_output;
  options.error = child_error;
  let const result =
      run_measured_with_options(argv, measured_output::Inherit, options);
  if (!result.has_value()) return None;
  return static_cast<i32>(result->exit_status);
}

volatile sig_atomic_t INTERRUPT_REQUESTED = 0;
volatile sig_atomic_t CHILD_STATE_CHANGED = 0;
volatile sig_atomic_t SIGNAL_PENDING = 0;

static constexpr i32 SIGNAL_FLAG_COUNT = 128;
static volatile sig_atomic_t PENDING_SIGNAL_FLAGS[SIGNAL_FLAG_COUNT] = {};
static volatile LONG64 CHILD_USER_TICKS = 0;
static volatile LONG64 CHILD_SYSTEM_TICKS = 0;
static volatile LONG64 CHILD_PEAK_RSS_BYTES = 0;
static volatile LONG INTERNAL_PIPE_SEQUENCE = 0;

static fn filetime_ticks(FILETIME time) wontthrow -> u64
{
  ULARGE_INTEGER ticks{};
  ticks.LowPart = time.dwLowDateTime;
  ticks.HighPart = time.dwHighDateTime;
  return ticks.QuadPart;
}

static fn record_child_process_usage(process child) wontthrow -> void
{
  FILETIME creation_time{};
  FILETIME exit_time{};
  FILETIME kernel_time{};
  FILETIME user_time{};
  if (GetProcessTimes(child, &creation_time, &exit_time, &kernel_time,
                      &user_time) != FALSE)
  {
    InterlockedAdd64(&CHILD_USER_TICKS,
                     static_cast<LONG64>(filetime_ticks(user_time)));
    InterlockedAdd64(&CHILD_SYSTEM_TICKS,
                     static_cast<LONG64>(filetime_ticks(kernel_time)));
  }

  PROCESS_MEMORY_COUNTERS memory_counters{};
  memory_counters.cb = sizeof(memory_counters);
  if (GetProcessMemoryInfo(child, &memory_counters, sizeof(memory_counters)) ==
      FALSE)
    return;

  let const peak_rss_bytes =
      static_cast<LONG64>(memory_counters.PeakWorkingSetSize);
  let previous_peak_rss_bytes =
      InterlockedCompareExchange64(&CHILD_PEAK_RSS_BYTES, 0, 0);
  while (peak_rss_bytes > previous_peak_rss_bytes) {
    let const observed_peak_rss_bytes = InterlockedCompareExchange64(
        &CHILD_PEAK_RSS_BYTES, peak_rss_bytes, previous_peak_rss_bytes);
    if (observed_peak_rss_bytes == previous_peak_rss_bytes) break;
    previous_peak_rss_bytes = observed_peak_rss_bytes;
  }
}

} /* namespace os */
} /* namespace koshka */

namespace koshka {

namespace os {

static fn append_windows_quoted_arg(String &out, StringView arg) throws -> void;

static pure fn is_batch_program(StringView path) wontthrow -> bool;

static fn create_process_utf8(StringView application_path,
                              StringView command_line,
                              StringView working_directory,
                              DWORD creation_flags, LPVOID environment_block,
                              const STARTUPINFOW &startup_info,
                              const HANDLE *inherited_handles,
                              usize inherited_handle_count,
                              PROCESS_INFORMATION &process_info) throws -> bool;

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
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &inheritable, OPEN_EXISTING, 0, nullptr);
  if (null_input == INVALID_HANDLE_VALUE) return None;
  defer { CloseHandle(null_input); };

  let application_path = argv[0].c_str();
  let resolved_program_path_storage = String{heap_allocator()};
  if (let resolved_program_path = canonical_path(Path{argv[0].view()})) {
    resolved_program_path_storage = resolved_program_path->text().clone();
    application_path = resolved_program_path_storage.c_str();
  }

  let application_path_storage = String{heap_allocator()};
  let command_line = make_os_args(argv);
  if (is_batch_program(argv[0].view())) {
    let batch_command = String{heap_allocator()};
    append_windows_quoted_arg(batch_command,
                              resolved_program_path_storage.is_empty()
                                  ? argv[0].view()
                                  : resolved_program_path_storage.view());
    for (usize argument_position = 1; argument_position < argv.count();
         argument_position++)
    {
      batch_command += ' ';
      append_windows_quoted_arg(batch_command, argv[argument_position].view());
    }

    let command_processor = get_environment_variable("COMSPEC");
    application_path_storage = command_processor.has_value()
                                   ? command_processor.take()
                                   : String{"cmd.exe"};
    application_path = application_path_storage.c_str();
    let processor_command_line = String{heap_allocator()};
    append_windows_quoted_arg(processor_command_line,
                              application_path_storage.view());
    processor_command_line += " /d /s /c \"";
    processor_command_line += batch_command;
    processor_command_line += '"';
    command_line = steal(processor_command_line);
  }
  LOG(Debug, "capturing output from '%s' with command line '%s'",
      application_path, command_line.c_str());

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = null_input;
  startup_info.hStdOutput = write_end;
  startup_info.hStdError = write_end;

  PROCESS_INFORMATION process_info{};
  HANDLE inherited_handles[] = {null_input, write_end};
  if (!create_process_utf8(StringView{application_path}, command_line.view(),
                           {}, CREATE_NO_WINDOW, nullptr, startup_info,
                           inherited_handles, 2, process_info))
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
    record_child_process_usage(process_info.hProcess);
    return None;
  }
  record_child_process_usage(process_info.hProcess);
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
    -> bool
{
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;

  DWORD flags = 0;
  if (GetHandleInformation(handle, &flags) == FALSE) return false;
  if ((flags & HANDLE_FLAG_INHERIT) != 0) return true;
  if (SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ==
      FALSE)
    return false;

  state.handle = handle;
  state.original_flags = flags;
  state.should_restore = true;
  return true;
}

static fn
restore_handle_inheritance(const inherited_handle_state &state) wontthrow
    -> bool
{
  if (!state.should_restore) return true;
  return SetHandleInformation(state.handle, HANDLE_FLAG_INHERIT,
                              state.original_flags & HANDLE_FLAG_INHERIT) !=
         FALSE;
}

static fn ensure_valid_standard_handles(STARTUPINFOW &startup_info,
                                        HANDLE &null_handle) wontthrow -> bool
{
  let const are_standard_handles_valid =
      startup_info.hStdInput != nullptr &&
      startup_info.hStdInput != INVALID_HANDLE_VALUE &&
      startup_info.hStdOutput != nullptr &&
      startup_info.hStdOutput != INVALID_HANDLE_VALUE &&
      startup_info.hStdError != nullptr &&
      startup_info.hStdError != INVALID_HANDLE_VALUE;
  if (are_standard_handles_valid) return true;

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                            OPEN_EXISTING, 0, nullptr);
  if (null_handle == INVALID_HANDLE_VALUE) return false;
  if (startup_info.hStdInput == nullptr ||
      startup_info.hStdInput == INVALID_HANDLE_VALUE)
    startup_info.hStdInput = null_handle;
  if (startup_info.hStdOutput == nullptr ||
      startup_info.hStdOutput == INVALID_HANDLE_VALUE)
    startup_info.hStdOutput = null_handle;
  if (startup_info.hStdError == nullptr ||
      startup_info.hStdError == INVALID_HANDLE_VALUE)
    startup_info.hStdError = null_handle;
  return true;
}

static fn create_process_utf8(StringView application_path,
                              StringView command_line,
                              StringView working_directory,
                              DWORD creation_flags, LPVOID environment_block,
                              const STARTUPINFOW &startup_info,
                              const HANDLE *inherited_handles,
                              usize inherited_handle_count,
                              PROCESS_INFORMATION &process_info) throws -> bool
{
  if ((startup_info.dwFlags & STARTF_USESTDHANDLES) != 0 &&
      (startup_info.hStdInput == nullptr ||
       startup_info.hStdInput == INVALID_HANDLE_VALUE ||
       startup_info.hStdOutput == nullptr ||
       startup_info.hStdOutput == INVALID_HANDLE_VALUE ||
       startup_info.hStdError == nullptr ||
       startup_info.hStdError == INVALID_HANDLE_VALUE))
  {
    SetLastError(ERROR_INVALID_HANDLE);
    return false;
  }

  let wide_application = application_path.is_empty()
                             ? Maybe<ArrayList<wchar_t>>{}
                             : utf8_to_wide(application_path, heap_allocator());
  if (!application_path.is_empty() && !wide_application.has_value())
    return false;
  let wide_command_line = utf8_to_wide(command_line, heap_allocator());
  if (!wide_command_line.has_value()) return false;
  let wide_working_directory =
      working_directory.is_empty()
          ? Maybe<ArrayList<wchar_t>>{}
          : utf8_to_wide(working_directory, heap_allocator());
  if (!working_directory.is_empty() && !wide_working_directory.has_value())
    return false;

  scan_inherited_shell_fds();
  let unique_handles = ArrayList<HANDLE>{heap_allocator()};
  unique_handles.reserve(inherited_handle_count +
                         static_cast<usize>(HIGHEST_OPEN_SHELL_FD + 1));
  let const do_add_unique_handle = [&](HANDLE handle) throws {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      return;
    }
    for (let const existing : unique_handles)
      if (existing == handle) return;
    unique_handles.push(handle);
  };
  for (usize handle_position = 0; handle_position < inherited_handle_count;
       handle_position++)
    do_add_unique_handle(inherited_handles[handle_position]);

  let inherited_fd_storage = ArrayList<u8>{heap_allocator()};
  if (HIGHEST_OPEN_SHELL_FD > 2) {
    let const inherited_fd_count = HIGHEST_OPEN_SHELL_FD + 1;
    let const storage_size = sizeof(i32) +
                             static_cast<usize>(inherited_fd_count) +
                             sizeof(intptr_t) * inherited_fd_count;
    if (storage_size > UINT16_MAX) {
      SetLastError(ERROR_TOO_MANY_OPEN_FILES);
      return false;
    }
    inherited_fd_storage.reserve(storage_size);
    for (usize byte_position = 0; byte_position < storage_size; byte_position++)
      inherited_fd_storage.push(0);
    __builtin_memcpy(inherited_fd_storage.begin(), &inherited_fd_count,
                     sizeof(inherited_fd_count));
    let *flags = inherited_fd_storage.begin() + sizeof(inherited_fd_count);
    let *handles = flags + inherited_fd_count;
    for (i32 shell_fd = 0; shell_fd < inherited_fd_count; shell_fd++) {
      let const handle = descriptor_for_shell_fd(shell_fd);
      let const handle_value = reinterpret_cast<intptr_t>(handle);
      __builtin_memcpy(handles + sizeof(handle_value) * shell_fd, &handle_value,
                       sizeof(handle_value));
      if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        continue;
      }
      flags[shell_fd] = 1;
      do_add_unique_handle(handle);
    }
  }

  static SRWLOCK launch_lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&launch_lock);
  defer { ReleaseSRWLockExclusive(&launch_lock); };

  let inheritance_states = ArrayList<inherited_handle_state>{heap_allocator()};
  inheritance_states.reserve(unique_handles.count());
  for (usize handle_position = 0; handle_position < unique_handles.count();
       handle_position++)
    inheritance_states.push(inherited_handle_state{});
  usize inheritable_handle_count = 0;
  DWORD preserved_error = ERROR_SUCCESS;
  bool did_restore_inheritance = false;
  defer
  {
    if (!did_restore_inheritance)
      for (usize restore_position = 0;
           restore_position < inheritable_handle_count; restore_position++)
        if (!restore_handle_inheritance(inheritance_states[restore_position]) &&
            preserved_error == ERROR_SUCCESS)
          preserved_error = GetLastError();
    if (preserved_error != ERROR_SUCCESS) SetLastError(preserved_error);
  };
  for (usize handle_position = 0; handle_position < unique_handles.count();
       handle_position++)
    if (!make_handle_inheritable(unique_handles[handle_position],
                                 inheritance_states[handle_position]))
    {
      preserved_error = GetLastError();
      return false;
    } else {
      inheritable_handle_count = handle_position + 1;
    }

  SIZE_T attribute_size = 0;
  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
  ArrayList<u8> attribute_storage{heap_allocator()};
  STARTUPINFOEXW extended_startup{};
  extended_startup.StartupInfo.cb = unique_handles.is_empty()
                                        ? sizeof(STARTUPINFOW)
                                        : sizeof(extended_startup);
  extended_startup.StartupInfo.dwFlags = startup_info.dwFlags;
  extended_startup.StartupInfo.wShowWindow = startup_info.wShowWindow;
  extended_startup.StartupInfo.hStdInput = startup_info.hStdInput;
  extended_startup.StartupInfo.hStdOutput = startup_info.hStdOutput;
  extended_startup.StartupInfo.hStdError = startup_info.hStdError;
  if (!inherited_fd_storage.is_empty()) {
    extended_startup.StartupInfo.cbReserved2 =
        static_cast<WORD>(inherited_fd_storage.count());
    extended_startup.StartupInfo.lpReserved2 = inherited_fd_storage.begin();
  }
  bool is_attribute_list_initialized = false;
  defer
  {
    if (is_attribute_list_initialized)
      DeleteProcThreadAttributeList(attribute_list);
  };
  if (!unique_handles.is_empty()) {
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    if (attribute_size == 0) {
      preserved_error = GetLastError();
      return false;
    }
    attribute_storage.reserve(static_cast<usize>(attribute_size));
    attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.begin());
    if (InitializeProcThreadAttributeList(attribute_list, 1, 0,
                                          &attribute_size) == FALSE)
    {
      preserved_error = GetLastError();
      return false;
    }
    is_attribute_list_initialized = true;
    if (UpdateProcThreadAttribute(
            attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            unique_handles.begin(), sizeof(HANDLE) * unique_handles.count(),
            nullptr, nullptr) == FALSE)
    {
      preserved_error = GetLastError();
      return false;
    }
    extended_startup.lpAttributeList = attribute_list;
    creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
  }

  let const did_create =
      CreateProcessW(
          wide_application.has_value() ? wide_application->begin() : nullptr,
          wide_command_line->begin(), nullptr, nullptr,
          !unique_handles.is_empty(), creation_flags, environment_block,
          wide_working_directory.has_value() ? wide_working_directory->begin()
                                             : nullptr,
          &extended_startup.StartupInfo, &process_info) != FALSE;
  if (!did_create) preserved_error = GetLastError();

  DWORD restoration_error = ERROR_SUCCESS;
  for (usize restore_position = 0; restore_position < inheritable_handle_count;
       restore_position++)
    if (!restore_handle_inheritance(inheritance_states[restore_position]) &&
        restoration_error == ERROR_SUCCESS)
      restoration_error = GetLastError();
  did_restore_inheritance = true;
  if (restoration_error == ERROR_SUCCESS) return did_create;

  if (did_create) {
    TerminateProcess(process_info.hProcess, 127);
    WaitForSingleObject(process_info.hProcess, INFINITE);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    process_info = PROCESS_INFORMATION{};
  }
  preserved_error = restoration_error;
  return false;
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

    let command_processor = get_environment_variable("COMSPEC");
    application_path_storage = command_processor.has_value()
                                   ? command_processor.take()
                                   : String{"cmd.exe"};
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
  STARTUPINFOW startup_info{};

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
    if (!ensure_valid_standard_handles(startup_info, null_handle))
      throw ErrorWithLocation{ec.source_location(),
                              last_system_error_message()};

    startup_info.dwFlags = STARTF_USESTDHANDLES;
  }

  /* An empty CreateProcess environment block is two nulls, a null pointer would
     inherit the shell's environment. */
  wchar_t empty_environment_block[] = {L'\0', L'\0'};
  LPVOID environment_block =
      ec.should_use_empty_environment ? empty_environment_block : nullptr;

  DWORD creation_flags =
      should_create_new_process_group ? CREATE_NEW_PROCESS_GROUP : 0;
  if (should_start_suspended) creation_flags |= CREATE_SUSPENDED;
  if (ec.should_use_empty_environment)
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;

  HANDLE inherited_handles[] = {startup_info.hStdInput, startup_info.hStdOutput,
                                startup_info.hStdError};
  let const inherited_handle_count = should_use_standard_handles ? 3 : 0;
  if (!create_process_utf8(
          StringView{application_path}, command_line.view(),
          working_directory != nullptr ? StringView{working_directory}
                                       : StringView{},
          creation_flags, environment_block, startup_info, inherited_handles,
          inherited_handle_count, process_info))
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

static fn spawn_subshell_stage(
    StringView source, Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
    Maybe<descriptor> err_fd, mimic_mood mood, process_group_mode process_group,
    bool source_traces_enabled = true, StringView bootstrap_source = {},
    StringView shell_name = {}, i32 previous_exit_status = 0,
    i64 shell_process_id = 0, usize subshell_depth = 0) throws
    -> Maybe<process>;

static fn make_internal_pipe_path() throws -> String
{
  let path = String{"\\\\.\\pipe\\kosh-"};
  path += String::from(GetCurrentProcessId(), heap_allocator()) + "-" +
          String::from(InterlockedIncrement(&INTERNAL_PIPE_SEQUENCE),
                       heap_allocator());
  return path;
}

static fn send_internal_pipe(StringView path, StringView content,
                             process child) throws -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let const deadline = GetTickCount64() + 5000;
  for (;;) {
    if (WaitNamedPipeW(wide_path->begin(), 50) != FALSE) {
      let const client = CreateFileW(wide_path->begin(), GENERIC_WRITE, 0,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
      if (client == INVALID_HANDLE_VALUE) return false;
      let const was_written = write_all(client, content.data, content.length);
      CloseHandle(client);
      return was_written;
    }
    if (WaitForSingleObject(child, 0) == WAIT_OBJECT_0) return false;
    if (GetTickCount64() >= deadline) return false;
    Sleep(1);
  }
}

fn launch_process_substitution(StringView source, bool command_writes_pipe,
                               mimic_mood mood, bool source_traces_enabled,
                               StringView bootstrap_source,
                               StringView shell_name, i32 previous_exit_status,
                               i64 shell_process_id,
                               usize subshell_depth) throws
    -> process_substitution_launch
{
  let path = make_internal_pipe_path();
  let const wide_path = utf8_to_wide(path.view(), heap_allocator());
  if (!wide_path.has_value())
    throw Error{"Unable to name the process substitution pipe"};
  let cleanup_path = String{heap_allocator(), path.view()};
  let cleanup = heap_allocator().alloc_array<String>(1);
  if (cleanup == nullptr) throw std::bad_alloc{};
  try {
    new (cleanup) String{steal(cleanup_path)};
  } catch (...) {
    heap_allocator().free_array(cleanup, 1);
    throw;
  }
  defer
  {
    if (cleanup == nullptr) return;
    cleanup->~String();
    heap_allocator().free_array(cleanup, 1);
  };
  let const previous_connection =
      get_environment_variable(internal::CONNECT_NAMED_PIPE);
  let connection = String{command_writes_pipe ? "stdout:" : "stdin:"};
  connection += path;
  set_environment_variable(internal::CONNECT_NAMED_PIPE, connection.view());
  defer
  {
    if (previous_connection.has_value())
      set_environment_variable(internal::CONNECT_NAMED_PIPE,
                               previous_connection->view());
    else
      unset_environment_variable(internal::CONNECT_NAMED_PIPE);
  };
  let const child = spawn_subshell_stage(
      source, None, None, None, mood, process_group_mode::Inherit,
      source_traces_enabled, bootstrap_source, shell_name, previous_exit_status,
      shell_process_id, subshell_depth);
  if (!child.has_value())
    throw Error{"Unable to run the process substitution because the inner "
                "shell could not be spawned: " +
                last_system_error_message()};
  bool is_pipe_ready = false;
  let const deadline = GetTickCount64() + 5000;
  for (;;) {
    if (WaitNamedPipeW(wide_path->begin(), 50) != FALSE) {
      is_pipe_ready = true;
      break;
    }
    if (WaitForSingleObject(*child, 0) == WAIT_OBJECT_0) break;
    if (GetTickCount64() >= deadline) break;
    Sleep(1);
  }
  if (!is_pipe_ready) {
    TerminateProcess(*child, 1);
    WaitForSingleObject(*child, INFINITE);
    record_child_process_usage(*child);
    CloseHandle(*child);
    throw Error{"Unable to connect the process substitution pipe: " +
                last_system_error_message()};
  }
  let launch = process_substitution_launch{
      .path = steal(path),
      .retained_fd = KOSH_INVALID_FD,
      .child = *child,
      .cleanup = cleanup,
  };
  cleanup = nullptr;
  return launch;
}

fn release_unused_process_substitution(opaque *cleanup) wontthrow -> void
{
  if (cleanup == nullptr) return;
  let path = static_cast<String *>(cleanup);
  defer
  {
    path->~String();
    heap_allocator().free_array(path, 1);
  };
  let const wide_path = utf8_to_wide(path->view(), heap_allocator());
  if (!wide_path.has_value()) return;
  constexpr DWORD ACCESS_MODES[] = {GENERIC_READ, GENERIC_WRITE};
  for (let const access : ACCESS_MODES) {
    let const client = CreateFileW(wide_path->begin(), access, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE) continue;
    CloseHandle(client);
    return;
  }
}

static fn spawn_subshell_stage(
    StringView source, Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
    Maybe<descriptor> err_fd, mimic_mood mood, process_group_mode process_group,
    bool source_traces_enabled, StringView bootstrap_source,
    StringView shell_name, i32 previous_exit_status, i64 shell_process_id,
    usize subshell_depth) throws -> Maybe<process>
{
  /* Windows has no fork, so a compound pipeline stage re-parses its source in a
     fresh shell, returned unwaited for the pipeline to reap. */
  let const module_path = current_executable_path();
  if (!module_path.has_value()) return koshka::None;

  let arguments = ArrayList<String>{heap_allocator()};
  arguments.push(String{heap_allocator(), module_path->view()});
  arguments.push(String{heap_allocator(), StringView{"--privileged"}});
  arguments.push(String{heap_allocator(), StringView{"--no-init-files"}});
  if (mood != mimic_mood::Default) {
    arguments.push(String{heap_allocator(), StringView{"--mood"}});
    arguments.push(String{heap_allocator(), mood_name(mood)});
  }
  arguments.push(String{heap_allocator(), StringView{"--no-diagnostics"}});
  if (!source_traces_enabled)
    arguments.push(String{heap_allocator(), StringView{"--no-traces"}});
  arguments.push(String{heap_allocator(), StringView{"-c"}});
  arguments.push(String{heap_allocator(), source});
  if (!shell_name.is_empty())
    arguments.push(String{heap_allocator(), shell_name});
  let command_line = make_os_args(arguments);

  let const previous_status_value =
      get_environment_variable(internal::PREVIOUS_EXIT_STATUS);
  set_environment_variable(
      internal::PREVIOUS_EXIT_STATUS,
      String::from(previous_exit_status, heap_allocator()).view());
  defer
  {
    if (previous_status_value.has_value())
      set_environment_variable(internal::PREVIOUS_EXIT_STATUS,
                               previous_status_value->view());
    else
      unset_environment_variable(internal::PREVIOUS_EXIT_STATUS);
  };

  let const previous_parent_process_id =
      get_environment_variable(internal::PARENT_PROCESS_ID);
  set_environment_variable(
      internal::PARENT_PROCESS_ID,
      String::from(GetCurrentProcessId(), heap_allocator()).view());
  defer
  {
    if (previous_parent_process_id.has_value())
      set_environment_variable(internal::PARENT_PROCESS_ID,
                               previous_parent_process_id->view());
    else
      unset_environment_variable(internal::PARENT_PROCESS_ID);
  };

  let const previous_shell_process_id =
      get_environment_variable(internal::SHELL_PROCESS_ID);
  set_environment_variable(
      internal::SHELL_PROCESS_ID,
      String::from(shell_process_id, heap_allocator()).view());
  defer
  {
    if (previous_shell_process_id.has_value())
      set_environment_variable(internal::SHELL_PROCESS_ID,
                               previous_shell_process_id->view());
    else
      unset_environment_variable(internal::SHELL_PROCESS_ID);
  };

  let const previous_subshell_depth =
      get_environment_variable(internal::SUBSHELL_DEPTH);
  set_environment_variable(
      internal::SUBSHELL_DEPTH,
      String::from(subshell_depth, heap_allocator()).view());
  defer
  {
    if (previous_subshell_depth.has_value())
      set_environment_variable(internal::SUBSHELL_DEPTH,
                               previous_subshell_depth->view());
    else
      unset_environment_variable(internal::SUBSHELL_DEPTH);
  };

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = in_fd ? *in_fd : GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = out_fd ? *out_fd : GetStdHandle(STD_OUTPUT_HANDLE);
  startup_info.hStdError = err_fd ? *err_fd : GetStdHandle(STD_ERROR_HANDLE);
  HANDLE null_handle = INVALID_HANDLE_VALUE;
  defer
  {
    if (null_handle != INVALID_HANDLE_VALUE) CloseHandle(null_handle);
  };
  if (!ensure_valid_standard_handles(startup_info, null_handle)) return None;

  let bootstrap_pipe_path = String{heap_allocator()};
  let previous_bootstrap_pipe = Maybe<String>{};
  if (!bootstrap_source.is_empty()) {
    bootstrap_pipe_path = make_internal_pipe_path();
    previous_bootstrap_pipe =
        get_environment_variable(internal::STATE_NAMED_PIPE);
    set_environment_variable(internal::STATE_NAMED_PIPE,
                             bootstrap_pipe_path.view());
  }
  defer
  {
    if (!bootstrap_source.is_empty()) {
      if (previous_bootstrap_pipe.has_value())
        set_environment_variable(internal::STATE_NAMED_PIPE,
                                 previous_bootstrap_pipe->view());
      else
        unset_environment_variable(internal::STATE_NAMED_PIPE);
    }
  };

  PROCESS_INFORMATION process_info{};
  let const creation_flags = process_group == process_group_mode::Inherit
                                 ? static_cast<DWORD>(0)
                                 : CREATE_NEW_PROCESS_GROUP;
  HANDLE inherited_handles[] = {startup_info.hStdInput, startup_info.hStdOutput,
                                startup_info.hStdError};
  if (!create_process_utf8(module_path->view(), command_line.view(), {},
                           creation_flags, nullptr, startup_info,
                           inherited_handles, 3, process_info))
    return koshka::None;
  CloseHandle(process_info.hThread);
  if (!bootstrap_source.is_empty() &&
      !send_internal_pipe(bootstrap_pipe_path.view(), bootstrap_source,
                          process_info.hProcess))
  {
    TerminateProcess(process_info.hProcess, 1);
    WaitForSingleObject(process_info.hProcess, INFINITE);
    record_child_process_usage(process_info.hProcess);
    CloseHandle(process_info.hProcess);
    return koshka::None;
  }
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
                         process_group_mode process_group, i64 process_group_id,
                         StringView bootstrap_source, StringView shell_name,
                         i32 previous_exit_status, i64 shell_process_id,
                         usize subshell_depth) throws -> compound_stage_launch
{
  unused(diagnostic_source);
  if (source.is_empty())
    throw ErrorWithLocation{
        steal(location),
        "A compound command in a pipeline is not supported on this platform"};

  unused(process_group_id);
  let child = spawn_subshell_stage(source, in_fd, out_fd, err_fd, mood,
                                   process_group, true, bootstrap_source,
                                   shell_name, previous_exit_status,
                                   shell_process_id, subshell_depth);
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
  record_child_process_usage(p);

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
  record_child_process_usage(p);
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
  record_child_process_usage(p);

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

  if (process_is_pid_reference(p) &&
      pid_from_reference(p) == GetCurrentProcessId() && signal_number == SIGINT)
  {
    return raise(SIGINT) == 0;
  }

  if (!is_process_signal_supported(signal_number)) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
  }
  let const exit_status = static_cast<UINT>(128 + signal_number);

  if (process_is_group_reference(p)) {
    let const process_handle = process_from_group_reference(p);
    char job_name[64];
    if (!timeout_job_name(process_handle, job_name)) return false;
    let const job = OpenJobObjectA(JOB_OBJECT_TERMINATE, FALSE, job_name);
    if (job == nullptr) return false;
    let const did_terminate = TerminateJobObject(job, exit_status) != FALSE;
    CloseHandle(job);
    return did_terminate;
  }

  if (!process_is_pid_reference(p))
    return TerminateProcess(p, exit_status) != 0;

  let const target =
      OpenProcess(PROCESS_TERMINATE, FALSE, pid_from_reference(p));
  if (target == nullptr) return false;
  let const did_terminate = TerminateProcess(target, exit_status) != 0;
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
  LPWSTR errno_text{};
  let const win_errno = GetLastError();
  if (win_errno == ERROR_FILE_NOT_FOUND || win_errno == ERROR_PATH_NOT_FOUND)
    return String{"No such file or directory"};

  let const ret = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, win_errno, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&errno_text), 0, nullptr); /* NOLINT */

  if (ret == 0) {
    return String::from(win_errno, heap_allocator()) +
           StringView{" (Error message could not be processed due to "
                      "a FormatMessage() failure)"};
  }
  defer { LocalFree(errno_text); };

  let converted =
      wide_to_utf8(errno_text, static_cast<usize>(ret), heap_allocator());
  if (!converted.has_value()) {
    return String::from(win_errno, heap_allocator()) +
           StringView{" (Error message could not be converted to UTF-8)"};
  }
  let view = converted->view();
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
    if (view[i] == '%' && i + 1 < view.length && view[i + 1] >= '0' &&
        view[i + 1] <= '9')
    {
      err += err.is_empty() ? StringView{"Input"} : StringView{"input"};
      i++;
      continue;
    }
    let const byte = view[i];
    err.push(err.is_empty() && byte >= 'a' && byte <= 'z'
                 ? static_cast<char>(byte - 'a' + 'A')
                 : byte);
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

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry) == FALSE) return 0;

  let const process_id = GetCurrentProcessId();
  do {
    if (entry.th32ProcessID == process_id)
      return static_cast<i64>(entry.th32ParentProcessID);
  } while (Process32NextW(snapshot, &entry) != FALSE);

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

fn get_effective_group_id() wontthrow -> i64 { return 0; }

fn get_supplementary_group_ids(Allocator allocator) throws -> ArrayList<u32>
{
  let groups = ArrayList<u32>{allocator};
  groups.push(0);
  return groups;
}

fn child_max() wontthrow -> i64 { return 0; }

fn machine_type() throws -> String
{
  SYSTEM_INFO information{};
  GetNativeSystemInfo(&information);
  switch (information.wProcessorArchitecture) {
  case PROCESSOR_ARCHITECTURE_AMD64: return String{"x86_64"};
  case PROCESSOR_ARCHITECTURE_ARM64: return String{"arm64"};
  case PROCESSOR_ARCHITECTURE_INTEL: return String{"i686"};
  case PROCESSOR_ARCHITECTURE_ARM: return String{"arm"};
  default: return String{"unknown"};
  }
}

fn executable_system_name() throws -> String { return String{"Windows"}; }

static fn windows_version() wontthrow -> OSVERSIONINFOW
{
  static const OSVERSIONINFOW version = [] {
    OSVERSIONINFOW information{};
    information.dwOSVersionInfoSize = sizeof(information);
    using rtl_get_version_fn = LONG(WINAPI *)(OSVERSIONINFOW *);
    let const address =
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    rtl_get_version_fn get_version = nullptr;
    static_assert(sizeof(get_version) == sizeof(address));
    __builtin_memcpy(&get_version, &address, sizeof(get_version));
    if (get_version != nullptr && get_version(&information) != 0) {
      information.dwMajorVersion = 0;
    }
    return information;
  }();
  return version;
}

fn system_release_name() throws -> String
{
  let const version = windows_version();
  if (version.dwMajorVersion == 0) return String{"unknown"};
  return String::from(version.dwMajorVersion, heap_allocator()) + "." +
         String::from(version.dwMinorVersion, heap_allocator()) + "." +
         String::from(version.dwBuildNumber, heap_allocator());
}

fn system_version_name() throws -> String
{
  let const version = windows_version();
  if (version.dwMajorVersion == 0) return String{"unknown"};
  return String::from(version.dwBuildNumber, heap_allocator());
}

fn machine_target_name() throws -> String
{
  return machine_type() + "-pc-msys";
}

fn ostype_name() wontthrow -> StringView { return "msys"; }

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
  user_seconds = static_cast<double>(
                     InterlockedCompareExchange64(&CHILD_USER_TICKS, 0, 0)) /
                 10000000.0;
  system_seconds = static_cast<double>(InterlockedCompareExchange64(
                       &CHILD_SYSTEM_TICKS, 0, 0)) /
                   10000000.0;
}

fn children_peak_rss_bytes() wontthrow -> u64
{
  return static_cast<u64>(
      InterlockedCompareExchange64(&CHILD_PEAK_RSS_BYTES, 0, 0));
}

fn read_process_cpu_times() wontthrow -> cpu_times
{
  cpu_times result{};
  FILETIME creation_time{};
  FILETIME exit_time{};
  FILETIME kernel_time{};
  FILETIME user_time{};
  if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                      &kernel_time, &user_time) != FALSE)
  {
    result.self_user_seconds =
        static_cast<double>(filetime_ticks(user_time)) / 10000000.0;
    result.self_system_seconds =
        static_cast<double>(filetime_ticks(kernel_time)) / 10000000.0;
  }
  children_cpu_seconds(result.child_user_seconds, result.child_system_seconds);

  return result;
}

fn read_malloc_heap_stats(malloc_heap_stats &stats) wontthrow -> bool
{
  unused(stats);
  return false;
}

static fn
run_measured_with_options(const ArrayList<String> &argv, measured_output output,
                          const windows_measured_launch_options &options) throws
    -> Maybe<measured_result>
{
  let const suppress_output = output == measured_output::Suppress;
  if (argv.is_empty()) return None;

  /* Windows has no hardware perf counters, only wall time and peak working set.
   */
  measured_result result{};

  let command_line = make_os_args(argv);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.hStdInput = options.input.value_or(GetStdHandle(STD_INPUT_HANDLE));
  startup.hStdOutput = options.output.value_or(GetStdHandle(STD_OUTPUT_HANDLE));
  startup.hStdError = options.error.value_or(GetStdHandle(STD_ERROR_HANDLE));

  HANDLE null_handle = INVALID_HANDLE_VALUE;
  if (suppress_output) {
    SECURITY_ATTRIBUTES inherit_sa{};
    inherit_sa.nLength = sizeof(inherit_sa);
    inherit_sa.bInheritHandle = TRUE;
    null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
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
      (suppress_output || options.inherited_handle.has_value() ||
       options.input.has_value() || options.output.has_value() ||
       options.error.has_value());
  if (should_use_standard_handles) startup.dwFlags = STARTF_USESTDHANDLES;

  const u64 start_nanos = monotonic_nanos();
  HANDLE inherited_handles[] = {
      should_use_standard_handles ? startup.hStdInput : INVALID_HANDLE_VALUE,
      should_use_standard_handles ? startup.hStdOutput : INVALID_HANDLE_VALUE,
      should_use_standard_handles ? startup.hStdError : INVALID_HANDLE_VALUE,
      options.inherited_handle.value_or(INVALID_HANDLE_VALUE)};
  if (!create_process_utf8({}, command_line.view(), {}, options.creation_flags,
                           nullptr, startup, inherited_handles, 4,
                           process_info))
  {
    return None;
  }
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };

  if (WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0)
    return None;
  record_child_process_usage(process_info.hProcess);

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

fn run_measured(const ArrayList<String> &argv, measured_output output,
                const Maybe<descriptor> &inherited_handle) throws
    -> Maybe<measured_result>
{
  windows_measured_launch_options options{};
  options.inherited_handle = inherited_handle;
  return run_measured_with_options(argv, output, options);
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

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry) == 0) return processes;
  do {
    let name = wide_to_utf8(entry.szExeFile,
                            static_cast<usize>(lstrlenW(entry.szExeFile)),
                            heap_allocator());
    if (!name.has_value()) continue;

    process_entry process{};
    process.pid = static_cast<i64>(entry.th32ProcessID);
    process.name = name.take();
    /* The snapshot exposes only the executable name, used as the command line.
     */
    process.command_line = process.name.clone();
    processes.push(steal(process));
  } while (Process32NextW(snapshot, &entry) != 0);
  return processes;
}

static fn utf8_to_absolute_wide_path(StringView path,
                                     Allocator allocator) throws
    -> Maybe<ArrayList<wchar_t>>
{
  let wide_path = utf8_to_wide(path, allocator);
  if (!wide_path.has_value()) return None;

  let absolute_length =
      GetFullPathNameW(wide_path->begin(), 0, nullptr, nullptr);
  if (absolute_length == 0) return None;
  ArrayList<wchar_t> absolute_path{allocator};
  for (usize attempt_count = 0; attempt_count < 4; attempt_count++) {
    absolute_path.reserve(static_cast<usize>(absolute_length));
    let const written = GetFullPathNameW(wide_path->begin(), absolute_length,
                                         absolute_path.begin(), nullptr);
    if (written == 0) return None;
    if (written < absolute_length) return absolute_path;
    if (written == MAXDWORD) return None;
    absolute_length = written + 1;
  }
  SetLastError(ERROR_INSUFFICIENT_BUFFER);
  return None;
}

fn scan_process_file_users(const ArrayList<process_file_query> &queries,
                           ArrayList<process_file_user> &users,
                           Allocator scratch) throws -> Maybe<u32>
{
  for (let const &query : queries) {
    let wide_path = utf8_to_absolute_wide_path(query.path, scratch);
    if (!wide_path.has_value()) return query.query_position;

    DWORD session_handle = 0;
    wchar_t session_key[CCH_RM_SESSION_KEY + 1]{};
    let result = RmStartSession(&session_handle, 0, session_key);
    if (result != ERROR_SUCCESS) {
      SetLastError(result);
      return query.query_position;
    }
    defer { RmEndSession(session_handle); };

    LPCWSTR filenames[] = {wide_path->begin()};
    result = RmRegisterResources(session_handle, 1, filenames, 0, nullptr, 0,
                                 nullptr);
    if (result != ERROR_SUCCESS) {
      SetLastError(result);
      return query.query_position;
    }

    UINT required_count = 0;
    UINT process_count = 0;
    DWORD reboot_reasons = 0;
    result = RmGetList(session_handle, &required_count, &process_count, nullptr,
                       &reboot_reasons);
    if (result == ERROR_SUCCESS && required_count == 0) {
      continue;
    }
    if (result != ERROR_MORE_DATA) {
      SetLastError(result);
      return query.query_position;
    }

    ArrayList<RM_PROCESS_INFO> process_infos{scratch};
    for (usize attempt_count = 0; attempt_count < 4; attempt_count++) {
      process_infos.reserve(required_count);
      process_count = required_count;
      result = RmGetList(session_handle, &required_count, &process_count,
                         process_infos.begin(), &reboot_reasons);
      if (result == ERROR_SUCCESS) break;
      if (result != ERROR_MORE_DATA || attempt_count == 3) {
        SetLastError(result);
        return query.query_position;
      }
    }

    let const query_user_start = users.count();
    for (UINT process_position = 0; process_position < process_count;
         process_position++)
    {
      let const pid =
          process_infos.begin()[process_position].Process.dwProcessId;
      bool is_duplicate = false;
      for (usize user_position = query_user_start;
           user_position < users.count(); user_position++)
      {
        let const &user = users[user_position];
        if (user.query_position == query.query_position && user.pid == pid) {
          is_duplicate = true;
          break;
        }
      }
      if (is_duplicate) continue;
      let use_mask = static_cast<u8>(process_file_use::File);
      let const process =
          OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (process != nullptr) {
        defer { CloseHandle(process); };
        ArrayList<wchar_t> image_path{scratch};
        constexpr DWORD MAXIMUM_IMAGE_PATH_LENGTH = 32768;
        image_path.reserve(MAXIMUM_IMAGE_PATH_LENGTH);
        DWORD image_path_length = MAXIMUM_IMAGE_PATH_LENGTH;
        if (QueryFullProcessImageNameW(process, 0, image_path.begin(),
                                       &image_path_length) != FALSE)
        {
          let image_path_utf8 =
              wide_to_utf8(image_path.begin(),
                           static_cast<usize>(image_path_length), scratch);
          file_status image_status{};
          if (image_path_utf8.has_value() &&
              stat_path_following(image_path_utf8->view(), image_status) &&
              image_status.device_id == query.device_id &&
              image_status.file_id == query.file_id)
          {
            use_mask |= static_cast<u8>(process_file_use::Executable);
          }
        }
      }
      users.push(process_file_user{pid, 0, query.query_position, use_mask});
    }
  }
  return None;
}

fn process_file_query_is_supported(const file_status &status,
                                   bool should_match_filesystem) wontthrow
    -> bool
{
  return !should_match_filesystem && file_type_letter(status.mode) != 'd';
}

fn process_owner_name(u32 pid, u32 owner_id, Allocator allocator) throws
    -> Maybe<String>
{
  unused(owner_id);
  let const process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return None;
  defer { CloseHandle(process); };

  HANDLE token = nullptr;
  if (OpenProcessToken(process, TOKEN_QUERY, &token) == FALSE) return None;
  defer { CloseHandle(token); };

  DWORD token_length = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &token_length);
  if (token_length == 0) return None;
  ArrayList<u8> token_buffer{allocator};
  token_buffer.reserve(token_length);
  if (GetTokenInformation(token, TokenUser, token_buffer.begin(), token_length,
                          &token_length) == FALSE)
    return None;
  let const *token_user =
      reinterpret_cast<const TOKEN_USER *>(token_buffer.begin());

  DWORD name_length = 0;
  DWORD domain_length = 0;
  SID_NAME_USE sid_type{};
  LookupAccountSidW(nullptr, token_user->User.Sid, nullptr, &name_length,
                    nullptr, &domain_length, &sid_type);
  if (name_length == 0) return None;
  ArrayList<wchar_t> name{allocator};
  ArrayList<wchar_t> domain{allocator};
  name.reserve(name_length);
  domain.reserve(domain_length == 0 ? 1 : domain_length);
  if (LookupAccountSidW(nullptr, token_user->User.Sid, name.begin(),
                        &name_length, domain.begin(), &domain_length,
                        &sid_type) == FALSE)
    return None;
  if (name_length > 0 && name.begin()[name_length - 1] == L'\0') {
    name_length--;
  }
  if (domain_length > 0 && domain.begin()[domain_length - 1] == L'\0') {
    domain_length--;
  }

  let user_name = wide_to_utf8(name.begin(), name_length, allocator);
  if (!user_name.has_value()) return None;
  if (domain_length == 0) return user_name;
  let domain_name = wide_to_utf8(domain.begin(), domain_length, allocator);
  if (!domain_name.has_value()) return None;
  let qualified_name = String{allocator, domain_name->view()};
  qualified_name += '\\';
  qualified_name += user_name->view();
  return qualified_name;
}

} /* namespace os */

} /* namespace koshka */
