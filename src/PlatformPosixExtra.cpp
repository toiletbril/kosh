/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This routed POSIX source fragment exists because its native interfaces and
 * required headers differ across Linux, macOS, and other POSIX targets. It
 * implements hardware performance counters, heap and resource statistics, CPU
 * affinity, executable path discovery, process enumeration, ownership lookup,
 * and process file-user scans, including fallbacks for targets that lack a
 * facility. The separate fragment keeps target-specific conditionals out of
 * the common POSIX backend.
 */

#if defined __APPLE__
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#endif

#if defined __GLIBC__
#if __GLIBC_PREREQ(2, 33)
#define KOSH_HAS_MALLINFO2 1
#pragma weak mallinfo2
#endif
#endif

namespace koshka {
namespace os {
namespace {

fn open_current_directory_reference() wontthrow -> descriptor
{
#if defined __linux__
  return ::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
#else
  return ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#endif
}

#if defined __linux__

class PlatformPerfSession
{
public:
  static constexpr usize PERF_EVENT_COUNT = 5;

  int event_fds[PERF_EVENT_COUNT]{-1, -1, -1, -1, -1};

  ~PlatformPerfSession()
  {
    for (usize event_index = 0; event_index < PERF_EVENT_COUNT; event_index++) {
      if (event_fds[event_index] != -1) close(event_fds[event_index]);
    }
  }

  fn prepare(pid_t child_pid) wontthrow -> bool
  {
    struct perf_event_spec
    {
      u32 type;
      u64 config;
    };

    constexpr perf_event_spec EVENT_SPECS[PERF_EVENT_COUNT] = {
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES      },
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS    },
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES    },
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES   },
    };

    for (usize event_index = 0; event_index < PERF_EVENT_COUNT; event_index++) {
      struct perf_event_attr attributes{};
      attributes.size = sizeof(attributes);
      attributes.type = EVENT_SPECS[event_index].type;
      attributes.config = EVENT_SPECS[event_index].config;
      attributes.disabled = 1;
      attributes.exclude_kernel = 1;
      attributes.exclude_hv = 1;
      attributes.inherit = 1;
      attributes.inherit_stat = 1;
      attributes.enable_on_exec = 1;
      attributes.read_format =
          PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

      event_fds[event_index] =
          static_cast<int>(syscall(SYS_perf_event_open, &attributes, child_pid,
                                   -1, -1, PERF_FLAG_FD_CLOEXEC));
      if (event_fds[event_index] == -1) return false;
    }

    return true;
  }

  fn start() wontthrow -> bool { return true; }

  pure fn is_system_wide() const wontthrow -> bool { return false; }

  fn cancel() wontthrow -> void {}

  fn finish(perf_counts &counts) wontthrow -> bool
  {
    struct perf_reading
    {
      u64 value;
      u64 enabled_nanos;
      u64 running_nanos;
    };

    u64 *destinations[PERF_EVENT_COUNT] = {
        &counts.cpu_cycles, &counts.instructions, &counts.cache_references,
        &counts.cache_misses, &counts.branch_misses};

    for (usize event_index = 0; event_index < PERF_EVENT_COUNT; event_index++) {
      perf_reading reading{};
      ssize_t read_count;
      do {
        read_count = read(event_fds[event_index], &reading, sizeof(reading));
      } while (read_count == -1 && errno == EINTR);

      if (read_count != static_cast<ssize_t>(sizeof(reading)) ||
          reading.running_nanos == 0 ||
          reading.running_nanos > reading.enabled_nanos)
      {
        counts = {};
        return false;
      }

      if (reading.running_nanos == reading.enabled_nanos) {
        *destinations[event_index] = reading.value;
      } else {
        let const scaled_value = static_cast<u128>(reading.value) *
                                 reading.enabled_nanos / reading.running_nanos;
        if (scaled_value > UINT64_MAX) {
          counts = {};
          return false;
        }
        *destinations[event_index] = static_cast<u64>(scaled_value);
      }
    }

    return true;
  }
};

#elif defined __APPLE__ && defined __aarch64__

struct kpep_db;
struct kpep_event;
struct kpep_config;

template <typename Function>
fn load_platform_symbol(void *library, const char *name) wontthrow -> Function
{
  return reinterpret_cast<Function>(dlsym(library, name));
}

class PlatformPerfSession
{
public:
  static constexpr usize EVENT_COUNT = 5;
  static constexpr usize MAX_COUNTER_COUNT = 32;

  using force_get_fn = int (*)(int *);
  using force_set_fn = int (*)(int);
  using get_counting_fn = u32 (*)();
  using set_counting_fn = int (*)(u32);
  using get_config_fn = int (*)(u32, u64 *);
  using set_config_fn = int (*)(u32, u64 *);
  using get_config_count_fn = u32 (*)(u32);
  using get_counter_count_fn = u32 (*)(u32);
  using get_cpu_counters_fn = int (*)(bool, u32, int *, u64 *);
  using db_create_fn = int (*)(const char *, kpep_db **);
  using db_free_fn = void (*)(kpep_db *);
  using db_event_fn = int (*)(kpep_db *, const char *, kpep_event **);
  using config_create_fn = int (*)(kpep_db *, kpep_config **);
  using config_free_fn = void (*)(kpep_config *);
  using config_add_event_fn = int (*)(kpep_config *, kpep_event **, u32, u32 *);
  using config_force_counters_fn = int (*)(kpep_config *);
  using config_classes_fn = int (*)(kpep_config *, u32 *);
  using config_count_fn = int (*)(kpep_config *, usize *);
  using config_values_fn = int (*)(kpep_config *, u64 *, usize);
  using config_map_fn = int (*)(kpep_config *, usize *, usize);

  void *kperf_library{nullptr};
  void *kperfdata_library{nullptr};
  force_get_fn force_get{nullptr};
  force_set_fn force_set{nullptr};
  get_counting_fn get_counting{nullptr};
  set_counting_fn set_counting{nullptr};
  get_config_fn get_config{nullptr};
  set_config_fn set_config{nullptr};
  get_config_count_fn get_config_count{nullptr};
  get_counter_count_fn get_counter_count{nullptr};
  get_cpu_counters_fn get_cpu_counters{nullptr};
  db_create_fn db_create{nullptr};
  db_free_fn db_free{nullptr};
  db_event_fn db_event{nullptr};
  config_create_fn config_create{nullptr};
  config_free_fn config_free{nullptr};
  config_add_event_fn config_add_event{nullptr};
  config_force_counters_fn config_force_counters{nullptr};
  config_classes_fn config_classes{nullptr};
  config_count_fn config_count{nullptr};
  config_values_fn config_values{nullptr};
  config_map_fn config_map{nullptr};
  u32 counter_classes{0};
  u32 counter_count{0};
  u32 previous_counting_classes{0};
  u32 previous_config_count{0};
  usize logical_cpu_count{0};
  usize counter_map[EVENT_COUNT]{};
  u64 previous_config[MAX_COUNTER_COUNT]{};
  usize start_counter_count{0};
  u64 *start_counters{nullptr};
  u64 *end_counters{nullptr};
  bool has_acquired_force{false};
  bool has_changed_config{false};
  bool has_changed_counting{false};

  ~PlatformPerfSession()
  {
    restore();
    uncached_heap_allocator().free_array(start_counters, start_counter_count);
    uncached_heap_allocator().free_array(end_counters, start_counter_count);
    if (kperfdata_library != nullptr) dlclose(kperfdata_library);
    if (kperf_library != nullptr) dlclose(kperf_library);
  }

  fn load_libraries() wontthrow -> bool
  {
    kperf_library = dlopen(
        "/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    kperfdata_library = dlopen(
        "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata",
        RTLD_LAZY);
    if (kperf_library == nullptr || kperfdata_library == nullptr) return false;

    force_get = load_platform_symbol<force_get_fn>(kperf_library,
                                                   "kpc_force_all_ctrs_get");
    force_set = load_platform_symbol<force_set_fn>(kperf_library,
                                                   "kpc_force_all_ctrs_set");
    get_counting = load_platform_symbol<get_counting_fn>(kperf_library,
                                                         "kpc_get_counting");
    set_counting = load_platform_symbol<set_counting_fn>(kperf_library,
                                                         "kpc_set_counting");
    get_config =
        load_platform_symbol<get_config_fn>(kperf_library, "kpc_get_config");
    set_config =
        load_platform_symbol<set_config_fn>(kperf_library, "kpc_set_config");
    get_config_count = load_platform_symbol<get_config_count_fn>(
        kperf_library, "kpc_get_config_count");
    get_counter_count = load_platform_symbol<get_counter_count_fn>(
        kperf_library, "kpc_get_counter_count");
    get_cpu_counters = load_platform_symbol<get_cpu_counters_fn>(
        kperf_library, "kpc_get_cpu_counters");
    db_create =
        load_platform_symbol<db_create_fn>(kperfdata_library, "kpep_db_create");
    db_free =
        load_platform_symbol<db_free_fn>(kperfdata_library, "kpep_db_free");
    db_event =
        load_platform_symbol<db_event_fn>(kperfdata_library, "kpep_db_event");
    config_create = load_platform_symbol<config_create_fn>(
        kperfdata_library, "kpep_config_create");
    config_free = load_platform_symbol<config_free_fn>(kperfdata_library,
                                                       "kpep_config_free");
    config_add_event = load_platform_symbol<config_add_event_fn>(
        kperfdata_library, "kpep_config_add_event");
    config_force_counters = load_platform_symbol<config_force_counters_fn>(
        kperfdata_library, "kpep_config_force_counters");
    config_classes = load_platform_symbol<config_classes_fn>(
        kperfdata_library, "kpep_config_kpc_classes");
    config_count = load_platform_symbol<config_count_fn>(
        kperfdata_library, "kpep_config_kpc_count");
    config_values = load_platform_symbol<config_values_fn>(kperfdata_library,
                                                           "kpep_config_kpc");
    config_map = load_platform_symbol<config_map_fn>(kperfdata_library,
                                                     "kpep_config_kpc_map");

    return force_get != nullptr && force_set != nullptr &&
           get_counting != nullptr && set_counting != nullptr &&
           get_config != nullptr && set_config != nullptr &&
           get_config_count != nullptr && get_counter_count != nullptr &&
           get_cpu_counters != nullptr && db_create != nullptr &&
           db_free != nullptr && db_event != nullptr &&
           config_create != nullptr && config_free != nullptr &&
           config_add_event != nullptr && config_force_counters != nullptr &&
           config_classes != nullptr && config_count != nullptr &&
           config_values != nullptr && config_map != nullptr;
  }

  fn create_configuration(u64 (&configuration)[MAX_COUNTER_COUNT]) wontthrow
      -> bool
  {
    constexpr const char *EVENT_NAMES[EVENT_COUNT][3] = {
        {"FIXED_CYCLES",           nullptr,             nullptr},
        {"FIXED_INSTRUCTIONS",     nullptr,             nullptr},
        {"ARM_L1D_CACHE",          "INST_LDST",         nullptr},
        {"ARM_L1D_CACHE_REFILL",   "L1D_CACHE_MISS_LD", nullptr},
        {"BRANCH_MISPRED_NONSPEC", "ARM_BR_MIS_PRED",   nullptr},
    };

    kpep_db *database = nullptr;
    kpep_config *config = nullptr;
    if (db_create(nullptr, &database) != 0 || database == nullptr) return false;

    let const do_cleanup = [&]() wontthrow {
      if (config != nullptr) config_free(config);
      db_free(database);
    };

    if (config_create(database, &config) != 0 || config == nullptr ||
        config_force_counters(config) != 0)
    {
      do_cleanup();
      return false;
    }

    for (usize event_index = 0; event_index < EVENT_COUNT; event_index++) {
      kpep_event *event = nullptr;
      for (usize name_index = 0; name_index < 3; name_index++) {
        let const name = EVENT_NAMES[event_index][name_index];
        if (name == nullptr) break;
        if (db_event(database, name, &event) == 0) break;
      }
      if (event == nullptr || config_add_event(config, &event, 1, nullptr) != 0)
      {
        do_cleanup();
        return false;
      }
    }

    usize configuration_count = 0;
    usize complete_counter_map[MAX_COUNTER_COUNT]{};
    bool did_succeed =
        config_classes(config, &counter_classes) == 0 &&
        config_count(config, &configuration_count) == 0 &&
        configuration_count <= MAX_COUNTER_COUNT &&
        config_values(config, configuration, sizeof(configuration)) == 0 &&
        config_map(config, complete_counter_map,
                   sizeof(complete_counter_map)) == 0;
    if (did_succeed) {
      for (usize event_index = 0; event_index < EVENT_COUNT; event_index++)
        counter_map[event_index] = complete_counter_map[event_index];
    }

    do_cleanup();
    return did_succeed;
  }

  fn prepare(pid_t) wontthrow -> bool
  {
    if (!load_libraries()) return false;

    u64 configuration[MAX_COUNTER_COUNT]{};
    if (!create_configuration(configuration)) return false;

    int previous_force = 0;
    if (force_get(&previous_force) != 0 || previous_force != 0) return false;

    previous_counting_classes = get_counting();
    if (previous_counting_classes != 0) return false;

    if (force_set(1) != 0) return false;
    has_acquired_force = true;

    previous_config_count = get_config_count(counter_classes);
    if (previous_config_count > MAX_COUNTER_COUNT ||
        get_config(counter_classes, previous_config) != 0 ||
        set_config(counter_classes, configuration) != 0)
    {
      return false;
    }
    has_changed_config = true;
    if (set_counting(counter_classes) != 0) return false;
    has_changed_counting = true;

    counter_count = get_counter_count(counter_classes);
    int cpu_count = 0;
    usize cpu_count_size = sizeof(cpu_count);
    if (counter_count == 0 ||
        sysctlbyname("hw.ncpu", &cpu_count, &cpu_count_size, nullptr, 0) != 0 ||
        cpu_count <= 0 ||
        static_cast<usize>(cpu_count) > SIZE_MAX / counter_count)
    {
      return false;
    }
    logical_cpu_count = static_cast<usize>(cpu_count);
    start_counter_count = logical_cpu_count * counter_count;
    if (start_counter_count > SIZE_MAX / sizeof(u64)) return false;

    start_counters =
        uncached_heap_allocator().alloc_array<u64>(start_counter_count);
    end_counters =
        uncached_heap_allocator().alloc_array<u64>(start_counter_count);
    if (start_counters == nullptr || end_counters == nullptr) return false;
    std::memset(start_counters, 0, start_counter_count * sizeof(u64));
    std::memset(end_counters, 0, start_counter_count * sizeof(u64));

    for (usize event_index = 0; event_index < EVENT_COUNT; event_index++) {
      if (counter_map[event_index] >= counter_count) return false;
    }

    return true;
  }

  fn start() const wontthrow -> bool
  {
    return get_cpu_counters(true, counter_classes, nullptr, start_counters) ==
           0;
  }

  fn cancel() wontthrow -> void { restore(); }

  pure fn is_system_wide() const wontthrow -> bool { return true; }

  fn finish(perf_counts &counts) wontthrow -> bool
  {
    if (get_cpu_counters(true, counter_classes, nullptr, end_counters) != 0) {
      counts = {};
      restore();
      return false;
    }

    u64 *destinations[EVENT_COUNT] = {
        &counts.cpu_cycles, &counts.instructions, &counts.cache_references,
        &counts.cache_misses, &counts.branch_misses};
    for (usize event_index = 0; event_index < EVENT_COUNT; event_index++) {
      u128 total = 0;
      for (usize cpu_index = 0; cpu_index < logical_cpu_count; cpu_index++) {
        let const counter_index =
            cpu_index * counter_count + counter_map[event_index];
        if (end_counters[counter_index] < start_counters[counter_index]) {
          counts = {};
          restore();
          return false;
        }
        total += end_counters[counter_index] - start_counters[counter_index];
      }
      if (total > UINT64_MAX) {
        counts = {};
        restore();
        return false;
      }
      *destinations[event_index] = static_cast<u64>(total);
    }

    restore();
    return true;
  }

  fn restore() wontthrow -> void
  {
    if (!has_acquired_force) return;

    if (has_changed_counting) set_counting(0);
    if (has_changed_config) set_config(counter_classes, previous_config);
    if (has_changed_counting) set_counting(previous_counting_classes);
    force_set(0);
    has_acquired_force = false;
    has_changed_config = false;
    has_changed_counting = false;
  }
};

#else

class PlatformPerfSession
{
public:
  fn prepare(pid_t) wontthrow -> bool { return false; }
  fn start() wontthrow -> bool { return false; }
  pure fn is_system_wide() const wontthrow -> bool { return false; }
  fn cancel() wontthrow -> void {}
  fn finish(perf_counts &) wontthrow -> bool { return false; }
};

#endif

fn platform_peak_rss_bytes(long peak_rss) wontthrow -> u64
{
#if defined __linux__
  return static_cast<u64>(peak_rss) * 1024ULL;
#else
  return static_cast<u64>(peak_rss);
#endif
}

} /* namespace */

fn affinity_processor_count(usize online_count,
                            usize configured_count) wontthrow -> usize
{
#if defined __linux__
  usize affinity_capacity = configured_count;
  if (affinity_capacity < CPU_SETSIZE) affinity_capacity = CPU_SETSIZE;
  for (usize attempt_count = 0; attempt_count < 8; attempt_count++) {
    let const affinity_size = CPU_ALLOC_SIZE(affinity_capacity);
    cpu_set_t *affinity = CPU_ALLOC(affinity_capacity);
    if (affinity == nullptr) break;
    CPU_ZERO_S(affinity_size, affinity);
    let const affinity_result = sched_getaffinity(0, affinity_size, affinity);
    let const affinity_error = errno;
    if (affinity_result == 0) {
      let const affinity_count = CPU_COUNT_S(affinity_size, affinity);
      CPU_FREE(affinity);
      if (affinity_count > 0) return static_cast<usize>(affinity_count);
      break;
    }
    CPU_FREE(affinity);
    if (affinity_error != EINVAL) break;
    affinity_capacity *= 2;
  }
#else
  unused(configured_count);
#endif
  return online_count;
}

fn current_executable_path() wontthrow -> Maybe<String>
{
#if defined __APPLE__
  u32 capacity = 0;
  _NSGetExecutablePath(nullptr, &capacity);
  if (capacity == 0) return koshka::None;

  ArrayList<char> buffer{heap_allocator()};
  buffer.reserve(capacity);
  if (_NSGetExecutablePath(buffer.begin(), &capacity) != 0) return koshka::None;

  let const raw_path = StringView{buffer.begin()};
  if (let const canonical = canonical_path(Path{raw_path}); canonical)
    return String{canonical->text()};

  return String{raw_path};
#else
  let const raw_path = read_symlink("/proc/self/exe", heap_allocator());
  if (!raw_path.has_value()) return None;

  if (let const canonical = canonical_path(Path{raw_path->view()}); canonical)
    return String{canonical->text()};

  return raw_path;
#endif
}

#if defined __APPLE__

static fn process_state_letter(char state) wontthrow -> char
{
  switch (state) {
  case SIDL: return 'I';
  case SRUN: return 'R';
  case SSLEEP: return 'S';
  case SSTOP: return 'T';
  case SZOMB: return 'Z';
  default: return '?';
  }
}

fn enumerate_processes(process_detail detail) throws -> ArrayList<process_entry>
{
  let const include_resource_stats = detail == process_detail::ResourceStats;
  ArrayList<process_entry> processes{heap_allocator()};
  int name_mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  usize byte_length = 0;
  if (::sysctl(name_mib, 4, nullptr, &byte_length, nullptr, 0) != 0)
    return processes;

  ArrayList<struct kinfo_proc> records{heap_allocator()};
  records.reserve(byte_length / sizeof(struct kinfo_proc) + 1);
  if (::sysctl(name_mib, 4, records.begin(), &byte_length, nullptr, 0) != 0)
    return processes;

  let const entry_count = byte_length / sizeof(struct kinfo_proc);
  for (usize entry_index = 0; entry_index < entry_count; entry_index++) {
    let const &record = records.begin()[entry_index];
    process_entry process{};
    process.pid = static_cast<i64>(record.kp_proc.p_pid);
    process.parent_pid = static_cast<i64>(record.kp_eproc.e_ppid);
    process.name = String{StringView{record.kp_proc.p_comm}};
    process.owner_id = static_cast<u32>(record.kp_eproc.e_ucred.cr_uid);
    process.state = process_state_letter(record.kp_proc.p_stat);
    process.start_token =
        static_cast<u64>(record.kp_proc.p_starttime.tv_sec) * 1000000 +
        static_cast<u64>(record.kp_proc.p_starttime.tv_usec);

    if (include_resource_stats) {
      char path_buffer[PROC_PIDPATHINFO_MAXSIZE];
      if (::proc_pidpath(record.kp_proc.p_pid, path_buffer,
                         sizeof(path_buffer)) > 0)
        process.command_line = String{StringView{path_buffer}};

      struct proc_taskinfo task_info{};
      if (::proc_pidinfo(record.kp_proc.p_pid, PROC_PIDTASKINFO, 0, &task_info,
                         sizeof(task_info)) ==
          static_cast<int>(sizeof(task_info)))
      {
        process.resident_kib =
            static_cast<u64>(task_info.pti_resident_size) / 1024;
        process.virtual_kib =
            static_cast<u64>(task_info.pti_virtual_size) / 1024;
        process.cpu_milliseconds =
            static_cast<u64>(task_info.pti_total_user +
                             task_info.pti_total_system) /
            1000000;
      }
    }

    if (process.command_line.is_empty())
      process.command_line = "[" + process.name + "]";

    processes.push(steal(process));
  }

  return processes;
}

#elif defined __linux__

static donteliminate fn nth_space_field(StringView text, usize index) wontthrow
    -> StringView
{
  usize field = 0;
  usize position = 0;
  while (position < text.length) {
    while (position < text.length &&
           (text[position] == ' ' || text[position] == '\n'))
      position++;
    if (position >= text.length) break;

    let const start_position = position;
    while (position < text.length && text[position] != ' ' &&
           text[position] != '\n')
      position++;
    if (field == index)
      return text.substring_of_length(start_position,
                                      position - start_position);
    field++;
  }

  return StringView{};
}

static fn leading_digits(StringView line, usize offset) wontthrow -> StringView
{
  while (offset < line.length && (line[offset] == ' ' || line[offset] == '\t'))
    offset++;

  usize digit_end_position = offset;
  while (digit_end_position < line.length && line[digit_end_position] >= '0' &&
         line[digit_end_position] <= '9')
    digit_end_position++;

  return line.substring_of_length(offset, digit_end_position - offset);
}

static fn linux_process_real_uid(StringView process_directory,
                                 i64 *parent_pid_out = nullptr) throws
    -> Maybe<u32>
{
  let const status =
      Path{(String{process_directory} + "/status").view()}.read_entire_file();
  if (!status.has_value()) return None;
  let const text = status->view();
  usize line_start_position = 0;
  for (usize position = 0; position <= text.length; position++) {
    if (position != text.length && text[position] != '\n') continue;
    let const line = text.substring_of_length(line_start_position,
                                              position - line_start_position);
    line_start_position = position + 1;

    if (parent_pid_out != nullptr && line.length > 5 &&
        line.substring_of_length(0, 5) == StringView{"PPid:"})
    {
      if (let const parsed = leading_digits(line, 5).to<i64>();
          !parsed.is_error())
        *parent_pid_out = parsed.value();
      continue;
    }

    if (line.length < 5 ||
        line.substring_of_length(0, 5) != StringView{"Uid:\t"})
      continue;

    let const uid = leading_digits(line, 4).to<u32>();
    return uid.is_error() ? Maybe<u32>{None} : Maybe<u32>{uid.value()};
  }

  return None;
}

fn enumerate_processes(process_detail detail) throws -> ArrayList<process_entry>
{
  let const include_resource_stats = detail == process_detail::ResourceStats;
  ArrayList<process_entry> processes{heap_allocator()};
  DIR *proc_directory = ::opendir("/proc");
  if (proc_directory == nullptr) return processes;
  defer { ::closedir(proc_directory); };

  for (struct dirent *entry = ::readdir(proc_directory); entry != nullptr;
       entry = ::readdir(proc_directory))
  {
    StringView name{entry->d_name};
    if (name.is_empty() || !name.is_all_decimal_digits()) continue;

    let const parsed_pid = name.to<i64>();
    if (parsed_pid.is_error()) continue;

    const String process_directory = "/proc/" + name;
    let command_name =
        Path{(process_directory + "/comm").view()}.read_entire_file();
    if (!command_name.has_value()) continue;
    while (!command_name->is_empty() && command_name->back() == '\n')
      command_name->pop_back();

    process_entry process{};
    process.pid = parsed_pid.value();
    process.name = steal(*command_name);

    if (let const uid = linux_process_real_uid(process_directory.view(),
                                               &process.parent_pid))
      process.owner_id = *uid;

    if (let command_line =
            Path{(process_directory + "/cmdline").view()}.read_entire_file();
        command_line.has_value() && !command_line->is_empty())
    {
      let normalized_command_line = String{heap_allocator()};
      normalized_command_line.reserve(command_line->count());
      for (usize position = 0; position < command_line->count(); position++) {
        let const byte = command_line->view()[position];
        if (byte != '\0')
          normalized_command_line.push(byte);
        else if (position + 1 < command_line->count())
          normalized_command_line.push(' ');
      }
      process.command_line = steal(normalized_command_line);
    } else {
      process.command_line = "[" + process.name + "]";
    }

    if (include_resource_stats) {
      if (let stat =
              Path{(process_directory + "/stat").view()}.read_entire_file();
          stat.has_value())
      {
        let const text = stat->view();
        usize after_name_position = text.length;
        for (usize position = text.length; position > 0; position--)
          if (text[position - 1] == ')') {
            after_name_position = position;
            break;
          }
        if (after_name_position < text.length) {
          let const fields = text.substring(after_name_position);
          let const state = nth_space_field(fields, 0);
          if (!state.is_empty()) process.state = state[0];
          u64 cpu_tick_count = 0;
          if (let const user_ticks = nth_space_field(fields, 11).to<i64>();
              !user_ticks.is_error())
            cpu_tick_count += static_cast<u64>(user_ticks.value());
          if (let const system_ticks = nth_space_field(fields, 12).to<i64>();
              !system_ticks.is_error())
            cpu_tick_count += static_cast<u64>(system_ticks.value());
          if (let const start_ticks = nth_space_field(fields, 19).to<i64>();
              !start_ticks.is_error() && start_ticks.value() >= 0)
            process.start_token = static_cast<u64>(start_ticks.value());
          let const ticks_per_second = ::sysconf(_SC_CLK_TCK);
          if (ticks_per_second > 0)
            process.cpu_milliseconds =
                static_cast<u64>(static_cast<u128>(cpu_tick_count) * 1000 /
                                 static_cast<u64>(ticks_per_second));
        }
      }

      if (let statm =
              Path{(process_directory + "/statm").view()}.read_entire_file();
          statm.has_value())
      {
        let const page_kib = static_cast<u64>(sysconf(_SC_PAGESIZE)) / 1024;
        if (let const size = nth_space_field(statm->view(), 0).to<i64>();
            !size.is_error())
          process.virtual_kib = static_cast<u64>(size.value()) * page_kib;
        if (let const resident = nth_space_field(statm->view(), 1).to<i64>();
            !resident.is_error())
          process.resident_kib = static_cast<u64>(resident.value()) * page_kib;
      }
    }

    processes.push(steal(process));
  }

  return processes;
}

#else

fn enumerate_processes(process_detail) throws -> ArrayList<process_entry>
{
  return ArrayList<process_entry>{heap_allocator()};
}

#endif

fn scan_process_file_users(const ArrayList<process_file_query> &queries,
                           ArrayList<process_file_user> &users,
                           Allocator scratch) throws -> Maybe<u32>
{
#if defined __APPLE__
  unused(scratch);
  let const do_matches_vnode = [](const process_file_query &query,
                                  const struct vinfo_stat &status) {
    if (query.should_match_device)
      return query.device_id == static_cast<u64>(status.vst_dev);
    return query.device_id == static_cast<u64>(status.vst_dev) &&
           query.file_id == static_cast<u64>(status.vst_ino);
  };
  let const do_matches_status = [](const process_file_query &query,
                                   const struct stat &status) {
    if (query.should_match_device)
      return query.device_id == static_cast<u64>(status.st_dev);
    return query.device_id == static_cast<u64>(status.st_dev) &&
           query.file_id == static_cast<u64>(status.st_ino);
  };
  let const filesystems = mounted_filesystems();

  for (let const &query : queries) {
    let path = String{query.path};
    if (query.should_match_device)
      for (let const &filesystem : filesystems) {
        struct stat mounted_status{};
        if (::stat(filesystem.target.c_str(), &mounted_status) == 0 &&
            query.device_id == static_cast<u64>(mounted_status.st_dev))
        {
          path = filesystem.target.clone();
          break;
        }
      }
    let const flags =
        query.should_match_device ? PROC_LISTPIDSPATH_PATH_IS_VOLUME : 0;
    let byte_count =
        ::proc_listpidspath(PROC_ALL_PIDS, 0, path.c_str(), flags, nullptr, 0);
    if (byte_count < 0) return query.query_position;
    if (byte_count == 0) continue;

    ArrayList<pid_t> pids{heap_allocator()};
    pids.reserve(static_cast<usize>(byte_count) / sizeof(pid_t));
    byte_count = ::proc_listpidspath(PROC_ALL_PIDS, 0, path.c_str(), flags,
                                     pids.begin(), byte_count);
    if (byte_count < 0) return query.query_position;

    let const pid_count = static_cast<usize>(byte_count) / sizeof(pid_t);
    for (usize pid_position = 0; pid_position < pid_count; pid_position++) {
      let const pid = pids.begin()[pid_position];
      if (pid <= 0) continue;

      struct proc_bsdinfo process_info{};
      if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &process_info,
                         sizeof(process_info)) != sizeof(process_info))
        continue;

      u8 use_mask = 0;
      struct proc_vnodepathinfo vnode_paths{};
      if (::proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vnode_paths,
                         sizeof(vnode_paths)) == sizeof(vnode_paths))
      {
        if (do_matches_vnode(query, vnode_paths.pvi_cdir.vip_vi.vi_stat))
          use_mask |= static_cast<u8>(process_file_use::Cwd);
        if (do_matches_vnode(query, vnode_paths.pvi_rdir.vip_vi.vi_stat))
          use_mask |= static_cast<u8>(process_file_use::Root);
      }

      char executable_path[PROC_PIDPATHINFO_MAXSIZE];
      if (::proc_pidpath(pid, executable_path, sizeof(executable_path)) > 0) {
        struct stat executable_status{};
        if (::stat(executable_path, &executable_status) == 0 &&
            do_matches_status(query, executable_status))
          use_mask |= static_cast<u8>(process_file_use::Executable);
      }

      let descriptor_bytes =
          ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
      if (descriptor_bytes > 0) {
        ArrayList<struct proc_fdinfo> descriptors{heap_allocator()};
        descriptors.reserve(static_cast<usize>(descriptor_bytes) /
                            sizeof(struct proc_fdinfo));
        descriptor_bytes = ::proc_pidinfo(
            pid, PROC_PIDLISTFDS, 0, descriptors.begin(), descriptor_bytes);
        if (descriptor_bytes > 0) {
          let const descriptor_count =
              static_cast<usize>(descriptor_bytes) / sizeof(struct proc_fdinfo);
          for (usize descriptor_position = 0;
               descriptor_position < descriptor_count; descriptor_position++)
          {
            let const &descriptor = descriptors.begin()[descriptor_position];
            if (descriptor.proc_fdtype != PROX_FDTYPE_VNODE) continue;
            struct vnode_fdinfowithpath vnode{};
            if (::proc_pidfdinfo(pid, descriptor.proc_fd,
                                 PROC_PIDFDVNODEPATHINFO, &vnode,
                                 sizeof(vnode)) != sizeof(vnode))
              continue;
            if (do_matches_vnode(query, vnode.pvip.vip_vi.vi_stat)) {
              use_mask |= static_cast<u8>(process_file_use::File);
              break;
            }
          }
        }
      }

      u64 region_address = 0;
      loop
      {
        struct proc_regionwithpathinfo region{};
        if (::proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, region_address, &region,
                           sizeof(region)) != sizeof(region))
          break;
        if (do_matches_vnode(query, region.prp_vip.vip_vi.vi_stat)) {
          use_mask |= static_cast<u8>(process_file_use::Mapped);
          break;
        }
        let const next_address =
            region.prp_prinfo.pri_address + region.prp_prinfo.pri_size;
        if (next_address <= region_address) break;
        region_address = next_address;
      }

      if (use_mask != 0)
        users.push(process_file_user{static_cast<u32>(pid),
                                     static_cast<u32>(process_info.pbi_ruid),
                                     query.query_position, use_mask});
    }
  }
  return None;
#elif defined __linux__
  DIR *proc_directory = ::opendir("/proc");
  if (proc_directory == nullptr) return queries[0].query_position;
  defer { ::closedir(proc_directory); };

  let const do_matches = [](const process_file_query &query,
                            const struct stat &status) {
    if (query.should_match_device)
      return query.device_id == static_cast<u64>(status.st_dev);
    return query.device_id == static_cast<u64>(status.st_dev) &&
           query.file_id == static_cast<u64>(status.st_ino);
  };
  let const do_apply_status = [&](ArrayList<u8> &use_masks,
                                  const struct stat &status,
                                  process_file_use use) {
    for (usize query_position = 0; query_position < queries.count();
         query_position++)
      if (do_matches(queries[query_position], status))
        use_masks[query_position] |= static_cast<u8>(use);
  };

  ArrayList<u8> use_masks{scratch};
  use_masks.reserve(queries.count());
  for (usize query_position = 0; query_position < queries.count();
       query_position++)
    use_masks.push(0);

  for (struct dirent *entry = ::readdir(proc_directory); entry != nullptr;
       entry = ::readdir(proc_directory))
  {
    let const name = StringView{entry->d_name};
    if (name.is_empty() || !name.is_all_decimal_digits()) continue;
    let const parsed_pid = name.to<u32>();
    if (parsed_pid.is_error()) continue;

    char process_path[64];
    let const process_path_length = std::snprintf(
        process_path, sizeof(process_path), "/proc/%s", entry->d_name);
    if (process_path_length <= 0 ||
        static_cast<usize>(process_path_length) >= sizeof(process_path))
      continue;
    let const process_user_id = linux_process_real_uid(process_path);
    if (!process_user_id.has_value()) continue;

    std::memset(use_masks.begin(), 0, use_masks.count() * sizeof(u8));

    struct named_reference
    {
      StringView name;
      process_file_use use;
    };
    const named_reference references[] = {
        {"root", process_file_use::Root      },
        {"cwd",  process_file_use::Cwd       },
        {"exe",  process_file_use::Executable},
    };
    for (let const &reference : references) {
      char reference_path[80];
      let const length = std::snprintf(
          reference_path, sizeof(reference_path), "%s/%.*s", process_path,
          static_cast<int>(reference.name.length), reference.name.data);
      if (length <= 0 || static_cast<usize>(length) >= sizeof(reference_path))
        continue;
      struct stat reference_status{};
      if (::stat(reference_path, &reference_status) == 0)
        do_apply_status(use_masks, reference_status, reference.use);
    }

    char descriptor_path[80];
    let const descriptor_path_length = std::snprintf(
        descriptor_path, sizeof(descriptor_path), "%s/fd", process_path);
    if (descriptor_path_length > 0 &&
        static_cast<usize>(descriptor_path_length) < sizeof(descriptor_path))
    {
      if (DIR *descriptor_directory = ::opendir(descriptor_path);
          descriptor_directory != nullptr)
      {
        defer { ::closedir(descriptor_directory); };
        let const descriptor_directory_fd = ::dirfd(descriptor_directory);
        for (struct dirent *descriptor = ::readdir(descriptor_directory);
             descriptor != nullptr;
             descriptor = ::readdir(descriptor_directory))
        {
          if (descriptor->d_name[0] == '.') continue;
          struct stat descriptor_status{};
          if (::fstatat(descriptor_directory_fd, descriptor->d_name,
                        &descriptor_status, 0) == 0)
            do_apply_status(use_masks, descriptor_status,
                            process_file_use::File);
        }
      }
    }

    char maps_path[80];
    let const maps_path_length =
        std::snprintf(maps_path, sizeof(maps_path), "%s/maps", process_path);
    if (maps_path_length > 0 &&
        static_cast<usize>(maps_path_length) < sizeof(maps_path))
    {
      if (FILE *maps = std::fopen(maps_path, "r"); maps != nullptr) {
        defer { std::fclose(maps); };
        char line[4096];
        while (std::fgets(line, sizeof(line), maps) != nullptr) {
          char *field = line;
          for (usize field_position = 0; field_position < 3; field_position++) {
            while (*field != '\0' && *field != ' ')
              field++;
            while (*field == ' ')
              field++;
          }
          char *end = nullptr;
          let const major_id = std::strtoull(field, &end, 16);
          if (end == field || *end != ':') continue;
          field = end + 1;
          let const minor_id = std::strtoull(field, &end, 16);
          if (end == field || *end != ' ') continue;
          field = end;
          while (*field == ' ')
            field++;
          let const file_id = std::strtoull(field, &end, 10);
          if (end == field || file_id == 0) continue;

          struct stat mapped_status{};
          mapped_status.st_dev = makedev(major_id, minor_id);
          mapped_status.st_ino = static_cast<ino_t>(file_id);
          do_apply_status(use_masks, mapped_status, process_file_use::Mapped);
        }
      }
    }

    for (usize query_position = 0; query_position < queries.count();
         query_position++)
    {
      if (use_masks[query_position] == 0) continue;
      users.push(process_file_user{parsed_pid.value(), *process_user_id,
                                   queries[query_position].query_position,
                                   use_masks[query_position]});
    }
  }
  return None;
#else
  unused(scratch);
  unused(queries);
  unused(users);
  errno = ENOTSUP;
  return queries[0].query_position;
#endif
}

fn process_file_query_is_supported(const file_status &status,
                                   bool should_match_filesystem) wontthrow
    -> bool
{
  unused(status);
  unused(should_match_filesystem);
  return true;
}

fn process_owner_name(u32 pid, u32 owner_id, Allocator allocator) throws
    -> Maybe<String>
{
  unused(pid);
  if (let const name = uid_to_username(owner_id))
    return String{allocator, name->view()};
  if (owner_id == get_real_user_id()) {
    if (let const name = get_current_user())
      return String{allocator, name->view()};
  }

  return None;
}

fn read_malloc_heap_stats(malloc_heap_stats &stats) wontthrow -> bool
{
#if defined __GLIBC__
#if defined KOSH_HAS_MALLINFO2
  if (mallinfo2 != nullptr) {
    let const info = mallinfo2();
    stats.bytes_in_use = static_cast<usize>(info.uordblks);
    stats.arena_bytes = static_cast<usize>(info.arena);
    stats.mapped_bytes = static_cast<usize>(info.hblkhd);
  } else
#endif
  {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    let const info = mallinfo();
#pragma GCC diagnostic pop
    stats.bytes_in_use =
        info.uordblks < 0 ? 0 : static_cast<usize>(info.uordblks);
    stats.arena_bytes = info.arena < 0 ? 0 : static_cast<usize>(info.arena);
    stats.mapped_bytes = info.hblkhd < 0 ? 0 : static_cast<usize>(info.hblkhd);
  }
  return true;
#elif defined __APPLE__
  /* The default zone answers for every ordinary malloc, and the size allocated
     is the region total the zone holds from the kernel. */
  malloc_statistics_t zone_stats{};
  malloc_zone_statistics(malloc_default_zone(), &zone_stats);
  stats.bytes_in_use = zone_stats.size_in_use;
  stats.arena_bytes = zone_stats.size_allocated;
  stats.mapped_bytes = zone_stats.max_size_in_use;

  return true;
#else
  unused(stats);
  return false;
#endif
}

#if defined __linux__

static fn read_small_file(const char *path, char *buffer,
                          usize capacity) wontthrow -> usize
{
  let const file_fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (file_fd < 0) return 0;
  defer { ::close(file_fd); };

  usize total_length = 0;
  while (total_length + 1 < capacity) {
    let const read_length =
        ::read(file_fd, buffer + total_length, capacity - 1 - total_length);
    if (read_length <= 0) break;

    total_length += static_cast<usize>(read_length);
  }

  buffer[total_length] = '\0';
  return total_length;
}

static fn each_line(StringView text, usize &position) wontthrow -> StringView
{
  let const start_position = position;
  while (position < text.length && text[position] != '\n')
    position++;

  let const line =
      text.substring_of_length(start_position, position - start_position);
  if (position < text.length) position++;

  return line;
}

static fn parse_decimal_word(StringView word, u64 &value) wontthrow -> bool
{
  if (word.is_empty()) return false;

  u64 parsed = 0;
  for (usize index = 0; index < word.length; index++) {
    let const character = word[index];
    if (character < '0' || character > '9') return false;
    let const digit = static_cast<u64>(character - '0');
    if (parsed > (UINT64_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }

  value = parsed;
  return true;
}

static fn parse_decimal_words(StringView text, u64 *values,
                              usize value_capacity) wontthrow -> usize
{
  usize value_count = 0;
  usize position = 0;
  while (position < text.length && value_count < value_capacity) {
    let const word = text.next_ascii_whitespace_word(position);
    if (word.is_empty()) continue;
    if (!parse_decimal_word(word, values[value_count])) break;
    value_count++;
  }

  return value_count;
}

static fn read_pressure_totals(const char *path, u64 &some_microseconds,
                               u64 &full_microseconds, bool &has_some,
                               bool &has_full) wontthrow -> void
{
  has_some = false;
  has_full = false;
  char buffer[512];
  let const length = read_small_file(path, buffer, sizeof(buffer));
  if (length == 0) return;

  let const text = StringView{buffer, length};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    let const total_position = line.find_substring("total=");
    if (!total_position.has_value()) continue;
    let const total = leading_digits(line, *total_position + 6).to<u64>();
    if (total.is_error()) continue;

    if (line.starts_with("some ")) {
      some_microseconds = total.value();
      has_some = true;
    } else if (line.starts_with("full ")) {
      full_microseconds = total.value();
      has_full = true;
    }
  }
}

#endif

fn read_system_activity_status(system_activity_status &status) wontthrow -> bool
{
#if defined __APPLE__
  let const host_port = mach_host_self();
  host_cpu_load_info_data_t cpu{};
  mach_msg_type_number_t cpu_count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(host_port, HOST_CPU_LOAD_INFO,
                      reinterpret_cast<host_info_t>(&cpu),
                      &cpu_count) == KERN_SUCCESS)
  {
    status.cpu_user_units = static_cast<u64>(cpu.cpu_ticks[CPU_STATE_USER]) +
                            cpu.cpu_ticks[CPU_STATE_NICE];
    status.cpu_system_units = cpu.cpu_ticks[CPU_STATE_SYSTEM];
    status.cpu_idle_units = cpu.cpu_ticks[CPU_STATE_IDLE];
    status.available_fields |= static_cast<u32>(system_activity_field::Cpu);
  }

  vm_size_t page_bytes = 4096;
  unused(host_page_size(host_port, &page_bytes));
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host_port, HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm),
                        &vm_count) == KERN_SUCCESS)
  {
    status.page_input_bytes = vm.pageins * static_cast<u64>(page_bytes);
    status.page_output_bytes = vm.pageouts * static_cast<u64>(page_bytes);
    status.page_fault_count = vm.faults;
    status.available_fields |=
        static_cast<u32>(system_activity_field::PageInput) |
        static_cast<u32>(system_activity_field::PageOutput) |
        static_cast<u32>(system_activity_field::Faults);
  }

  return status.available_fields != 0;
#elif defined __linux__
  status.has_isolation_probes = true;
  char stat_buffer[16384];
  let const stat_length =
      read_small_file("/proc/stat", stat_buffer, sizeof(stat_buffer));
  if (stat_length != 0) {
    let const text = StringView{stat_buffer, stat_length};
    usize position = 0;
    while (position < text.length) {
      let const line = each_line(text, position);
      if (line.starts_with("cpu ")) {
        u64 values[10]{};
        let const value_count =
            parse_decimal_words(line.substring(4), values, countof(values));
        if (value_count >= 4) {
          status.cpu_user_units = values[0] + values[1];
          status.cpu_system_units = values[2];
          status.cpu_idle_units = values[3];
          if (value_count > 4) {
            status.cpu_wait_units = values[4];
            status.available_fields |=
                static_cast<u32>(system_activity_field::CpuWait);
          }
          if (value_count > 6) {
            status.cpu_system_units += values[5] + values[6];
          }
          if (value_count > 7) {
            status.cpu_stolen_units = values[7];
            status.available_fields |=
                static_cast<u32>(system_activity_field::CpuStolen);
          }
          status.available_fields |=
              static_cast<u32>(system_activity_field::Cpu);
        }
      } else if (line.starts_with("procs_running ")) {
        let const value = leading_digits(line, 14).to<u64>();
        if (!value.is_error()) {
          status.runnable_process_count = value.value();
          status.available_fields |=
              static_cast<u32>(system_activity_field::Runnable);
        }
      } else if (line.starts_with("procs_blocked ")) {
        let const value = leading_digits(line, 14).to<u64>();
        if (!value.is_error()) {
          status.blocked_process_count = value.value();
          status.available_fields |=
              static_cast<u32>(system_activity_field::Blocked);
        }
      } else {
        struct stat_scalar_field
        {
          StringView name;
          u64 system_activity_status::*value;
          system_activity_field availability;
        };
        static constexpr stat_scalar_field STAT_SCALAR_FIELDS[] = {
            {"intr ",      &system_activity_status::interrupt_count,
             system_activity_field::Interrupts      },
            {"ctxt ",      &system_activity_status::context_switch_count,
             system_activity_field::ContextSwitches },
            {"processes ", &system_activity_status::process_creation_count,
             system_activity_field::ProcessCreations},
            {"softirq ",   &system_activity_status::soft_interrupt_count,
             system_activity_field::SoftInterrupts  },
        };
        for (let const &field : STAT_SCALAR_FIELDS) {
          if (!line.starts_with(field.name)) continue;
          let const value = leading_digits(line, field.name.length).to<u64>();
          if (!value.is_error()) {
            status.*(field.value) = value.value();
            status.available_fields |= static_cast<u32>(field.availability);
          }
          break;
        }
      }
    }
  }

  char vm_buffer[32768];
  let const vm_length =
      read_small_file("/proc/vmstat", vm_buffer, sizeof(vm_buffer));
  if (vm_length != 0) {
    let const text = StringView{vm_buffer, vm_length};
    usize position = 0;
    while (position < text.length) {
      let const line = each_line(text, position);
      struct vm_field
      {
        StringView name;
        u64 system_activity_status::*field;
        u64 multiplier;
        system_activity_field capability;
        bool is_additive;
      };
      static constexpr vm_field FIELDS[] = {
          {"pgpgin ",             &system_activity_status::page_input_bytes,       1024,
           system_activity_field::PageInput,       false},
          {"pgpgout ",            &system_activity_status::page_output_bytes,      1024,
           system_activity_field::PageOutput,      false},
          {"pgfault ",            &system_activity_status::page_fault_count,       1,
           system_activity_field::Faults,          false},
          {"pgmajfault ",         &system_activity_status::major_page_fault_count, 1,
           system_activity_field::MajorFaults,     false},
          {"pgscan_kswapd ",      &system_activity_status::page_scan_count,        1,
           system_activity_field::PageScan,        true },
          {"pgscan_direct ",      &system_activity_status::page_scan_count,        1,
           system_activity_field::PageScan,        true },
          {"pgscan_khugepaged ",  &system_activity_status::page_scan_count,        1,
           system_activity_field::PageScan,        true },
          {"pgscan_proactive ",   &system_activity_status::page_scan_count,        1,
           system_activity_field::PageScan,        true },
          {"pgsteal_kswapd ",     &system_activity_status::page_steal_count,       1,
           system_activity_field::PageSteal,       true },
          {"pgsteal_direct ",     &system_activity_status::page_steal_count,       1,
           system_activity_field::PageSteal,       true },
          {"pgsteal_khugepaged ", &system_activity_status::page_steal_count,       1,
           system_activity_field::PageSteal,       true },
          {"pgsteal_proactive ",  &system_activity_status::page_steal_count,       1,
           system_activity_field::PageSteal,       true },
          {"compact_stall ",      &system_activity_status::compaction_stall_count, 1,
           system_activity_field::CompactionStall, false},
          {"nr_dirty ",           &system_activity_status::dirty_page_count,       1,
           system_activity_field::DirtyPages,      false},
          {"nr_writeback ",       &system_activity_status::writeback_page_count,   1,
           system_activity_field::WritebackPages,  false},
          {"oom_kill ",           &system_activity_status::oom_kill_count,         1,
           system_activity_field::OomKills,        false},
      };
      if (line.starts_with("allocstall_") || line.starts_with("allocstall ")) {
        let const space_position = line.find_character(' ');
        if (space_position.has_value()) {
          let const value = leading_digits(line, *space_position + 1).to<u64>();
          if (!value.is_error()) {
            status.direct_reclaim_count =
                status.direct_reclaim_count > UINT64_MAX - value.value()
                    ? UINT64_MAX
                    : status.direct_reclaim_count + value.value();
            status.available_fields |=
                static_cast<u32>(system_activity_field::DirectReclaim);
          }
        }
        continue;
      }
      for (let const &known : FIELDS) {
        if (!line.starts_with(known.name)) continue;
        let const value = leading_digits(line, known.name.length).to<u64>();
        if (!value.is_error()) {
          let const scaled = value.value() > UINT64_MAX / known.multiplier
                                 ? UINT64_MAX
                                 : value.value() * known.multiplier;
          if (known.is_additive) {
            status.*known.field = status.*known.field > UINT64_MAX - scaled
                                      ? UINT64_MAX
                                      : status.*known.field + scaled;
          } else {
            status.*known.field = scaled;
          }
          status.available_fields |= static_cast<u32>(known.capability);
        }
        break;
      }
    }
  }

  bool has_some = false;
  bool has_full = false;
  read_pressure_totals("/proc/pressure/cpu", status.cpu_some_stall_microseconds,
                       status.cpu_full_stall_microseconds, has_some, has_full);
  if (has_some) {
    status.available_fields |=
        static_cast<u32>(system_activity_field::CpuSomeStall);
  }
  if (has_full) {
    status.available_fields |=
        static_cast<u32>(system_activity_field::CpuFullStall);
  }
  read_pressure_totals(
      "/proc/pressure/memory", status.memory_some_stall_microseconds,
      status.memory_full_stall_microseconds, has_some, has_full);
  if (has_some) {
    status.available_fields |=
        static_cast<u32>(system_activity_field::MemorySomeStall);
  }
  if (has_full) {
    status.available_fields |=
        static_cast<u32>(system_activity_field::MemoryFullStall);
  }
  read_pressure_totals("/proc/pressure/io", status.io_some_stall_microseconds,
                       status.io_full_stall_microseconds, has_some, has_full);
  if (has_some) {
    status.available_fields |=
        static_cast<u32>(system_activity_field::IoSomeStall);
  }
  if (has_full) {
    status.available_fields |=
        static_cast<u32>(system_activity_field::IoFullStall);
  }

  if (DIR *namespace_directory = ::opendir("/proc/self/ns");
      namespace_directory != nullptr)
  {
    defer { ::closedir(namespace_directory); };
    errno = 0;
    u64 namespace_count = 0;
    bool is_bounded = true;
    for (struct dirent *entry = ::readdir(namespace_directory);
         entry != nullptr; entry = ::readdir(namespace_directory))
    {
      if (entry->d_name[0] == '.') continue;
      if (namespace_count == 64) {
        is_bounded = false;
        break;
      }
      namespace_count++;
    }
    if (is_bounded && errno == 0) {
      status.namespace_count = namespace_count;
      status.available_fields |=
          static_cast<u32>(system_activity_field::NamespaceCount);
    }
  }

  if (let const cgroup_fd = ::open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
      cgroup_fd >= 0)
  {
    defer { ::close(cgroup_fd); };
    char cgroup_buffer[8192];
    usize cgroup_length = 0;
    bool has_complete_read = false;
    while (cgroup_length < sizeof(cgroup_buffer)) {
      let const read_length = ::read(cgroup_fd, cgroup_buffer + cgroup_length,
                                     sizeof(cgroup_buffer) - cgroup_length);
      if (read_length > 0) {
        cgroup_length += static_cast<usize>(read_length);
        continue;
      }
      if (read_length == 0) {
        has_complete_read = true;
        break;
      }
      if (errno != EINTR) break;
    }
    if (has_complete_read && cgroup_length > 0 &&
        cgroup_buffer[cgroup_length - 1] == '\n')
    {
      let const text = StringView{cgroup_buffer, cgroup_length};
      usize position = 0;
      u64 membership_count = 0;
      bool is_valid = true;
      while (position < text.length) {
        let const line = each_line(text, position);
        let const first_colon = line.find_character(':');
        if (!first_colon.has_value() ||
            !line.substring(*first_colon + 1).find_character(':').has_value() ||
            membership_count == 64)
        {
          is_valid = false;
          break;
        }
        membership_count++;
      }
      if (is_valid) {
        status.cgroup_membership_count = membership_count;
        status.available_fields |=
            static_cast<u32>(system_activity_field::CgroupMembershipCount);
      }
    }
  }

  return status.available_fields != 0;
#else
  unused(status);
  return false;
#endif
}

fn read_network_interface_statistics() throws
    -> ArrayList<network_interface_statistics_entry>
{
  let result = ArrayList<network_interface_statistics_entry>{heap_allocator()};
#if defined __APPLE__
  int name_mib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
  usize byte_length = 0;
  if (::sysctl(name_mib, 6, nullptr, &byte_length, nullptr, 0) != 0) {
    return result;
  }
  let storage = ArrayList<u8>{heap_allocator()};
  storage.reserve(byte_length);
  if (::sysctl(name_mib, 6, storage.begin(), &byte_length, nullptr, 0) != 0) {
    return result;
  }

  usize position = 0;
  while (position + sizeof(if_msghdr) <= byte_length) {
    let const *header =
        reinterpret_cast<const if_msghdr *>(storage.begin() + position);
    if (header->ifm_msglen == 0 || position + header->ifm_msglen > byte_length)
      break;
    if (header->ifm_type == RTM_IFINFO2 &&
        header->ifm_msglen >= sizeof(if_msghdr2))
    {
      let const *info = reinterpret_cast<const if_msghdr2 *>(header);
      int data_mib[6] = {CTL_NET,      PF_LINK,         NETLINK_GENERIC,
                         IFMIB_IFDATA, info->ifm_index, IFDATA_GENERAL};
      struct ifmibdata data{};
      usize data_length = sizeof(data);
      if (::sysctl(data_mib, 6, &data, &data_length, nullptr, 0) == 0 &&
          data_length >= sizeof(data))
      {
        u32 available_fields =
            static_cast<u32>(network_statistics_field::ReceiveBytes) |
            static_cast<u32>(network_statistics_field::TransmitBytes) |
            static_cast<u32>(network_statistics_field::ReceivePackets) |
            static_cast<u32>(network_statistics_field::TransmitPackets) |
            static_cast<u32>(network_statistics_field::ReceiveErrors) |
            static_cast<u32>(network_statistics_field::TransmitErrors) |
            static_cast<u32>(network_statistics_field::TransmitDrops) |
            static_cast<u32>(network_statistics_field::TransmitQueueLength) |
            static_cast<u32>(network_statistics_field::TransmitQueueLimit);
        if (data.ifmd_data.ifi_baudrate != 0) {
          available_fields |=
              static_cast<u32>(network_statistics_field::ReceiveLinkSpeed) |
              static_cast<u32>(network_statistics_field::TransmitLinkSpeed);
        }
        result.push(network_interface_statistics_entry{
            String{data.ifmd_name},
            data.ifmd_data.ifi_ibytes,
            data.ifmd_data.ifi_obytes,
            data.ifmd_data.ifi_ipackets,
            data.ifmd_data.ifi_opackets,
            data.ifmd_data.ifi_ierrors,
            data.ifmd_data.ifi_oerrors,
            0,
            data.ifmd_snd_drops,
            data.ifmd_data.ifi_baudrate,
            data.ifmd_data.ifi_baudrate,
            data.ifmd_snd_len,
            data.ifmd_snd_maxlen,
            available_fields,
        });
      }
    }
    position += header->ifm_msglen;
  }
#elif defined __linux__
  char buffer[65536];
  let const length = read_small_file("/proc/net/dev", buffer, sizeof(buffer));
  if (length == 0) return result;

  let const text = StringView{buffer, length};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    let const colon_position = line.find_character(':');
    if (!colon_position.has_value()) continue;
    u64 values[16]{};
    if (parse_decimal_words(line.substring(*colon_position + 1), values,
                            countof(values)) < countof(values))
    {
      continue;
    }
    constexpr u32 AVAILABLE =
        static_cast<u32>(network_statistics_field::ReceiveBytes) |
        static_cast<u32>(network_statistics_field::TransmitBytes) |
        static_cast<u32>(network_statistics_field::ReceivePackets) |
        static_cast<u32>(network_statistics_field::TransmitPackets) |
        static_cast<u32>(network_statistics_field::ReceiveErrors) |
        static_cast<u32>(network_statistics_field::TransmitErrors) |
        static_cast<u32>(network_statistics_field::ReceiveDrops) |
        static_cast<u32>(network_statistics_field::TransmitDrops);
    result.push(network_interface_statistics_entry{
        String{line.substring_of_length(0, *colon_position).trim_blanks()},
        values[0],
        values[8],
        values[1],
        values[9],
        values[2],
        values[10],
        values[3],
        values[11],
        0,
        0,
        0,
        0,
        AVAILABLE,
    });
  }
#endif
  return result;
}

fn read_tcp_statistics(tcp_statistics &statistics) wontthrow -> bool
{
#if defined __APPLE__
  int name_mib[4] = {CTL_NET, PF_INET, IPPROTO_TCP, TCPCTL_STATS};
  struct tcpstat native{};
  usize native_length = sizeof(native);
  if (::sysctl(name_mib, 4, &native, &native_length, nullptr, 0) != 0 ||
      native_length < sizeof(native))
  {
    return false;
  }
  statistics.active_open_count = native.tcps_connattempt;
  statistics.passive_open_count = native.tcps_accepts;
  statistics.received_segment_count = native.tcps_rcvtotal;
  statistics.sent_segment_count = native.tcps_sndtotal;
  statistics.retransmitted_segment_count = native.tcps_sndrexmitpack;
  statistics.input_error_count = static_cast<u64>(native.tcps_rcvbadsum) +
                                 native.tcps_rcvbadoff + native.tcps_rcvshort;
  statistics.connection_drop_count = native.tcps_conndrops + native.tcps_drops;
  statistics.receive_memory_drop_count = native.tcps_rcvmemdrop;
  statistics.listen_drop_count = native.tcps_listendrop;
  statistics.retransmit_timeout_count = native.tcps_rexmttimeo;
  statistics.available_fields =
      static_cast<u32>(tcp_statistics_field::ActiveOpens) |
      static_cast<u32>(tcp_statistics_field::PassiveOpens) |
      static_cast<u32>(tcp_statistics_field::ReceivedSegments) |
      static_cast<u32>(tcp_statistics_field::SentSegments) |
      static_cast<u32>(tcp_statistics_field::RetransmittedSegments) |
      static_cast<u32>(tcp_statistics_field::InputErrors) |
      static_cast<u32>(tcp_statistics_field::ConnectionDrops) |
      static_cast<u32>(tcp_statistics_field::ReceiveMemoryDrops) |
      static_cast<u32>(tcp_statistics_field::ListenDrops) |
      static_cast<u32>(tcp_statistics_field::RetransmitTimeouts);
  return true;
#elif defined __linux__
  char buffer[32768];
  let const length = read_small_file("/proc/net/snmp", buffer, sizeof(buffer));
  if (length == 0) return false;

  let const text = StringView{buffer, length};
  StringView header;
  bool has_statistics = false;
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    if (!line.starts_with("Tcp:")) continue;
    if (header.is_empty()) {
      header = line.substring(4);
      continue;
    }

    usize name_position = 0;
    usize value_position = 4;
    while (name_position < header.length && value_position < line.length) {
      let const name = header.next_ascii_whitespace_word(name_position);
      let const value_word = line.next_ascii_whitespace_word(value_position);
      u64 value = 0;
      if (!parse_decimal_word(value_word, value)) continue;
      struct tcp_field
      {
        StringView name;
        u64 tcp_statistics::*field;
        tcp_statistics_field capability;
      };
      static constexpr tcp_field FIELDS[] = {
          {"ActiveOpens",  &tcp_statistics::active_open_count,
           tcp_statistics_field::ActiveOpens          },
          {"PassiveOpens", &tcp_statistics::passive_open_count,
           tcp_statistics_field::PassiveOpens         },
          {"InSegs",       &tcp_statistics::received_segment_count,
           tcp_statistics_field::ReceivedSegments     },
          {"OutSegs",      &tcp_statistics::sent_segment_count,
           tcp_statistics_field::SentSegments         },
          {"RetransSegs",  &tcp_statistics::retransmitted_segment_count,
           tcp_statistics_field::RetransmittedSegments},
          {"InErrs",       &tcp_statistics::input_error_count,
           tcp_statistics_field::InputErrors          },
          {"AttemptFails", &tcp_statistics::attempt_failure_count,
           tcp_statistics_field::AttemptFailures      },
          {"EstabResets",  &tcp_statistics::established_reset_count,
           tcp_statistics_field::EstablishedResets    },
          {"CurrEstab",    &tcp_statistics::current_established_count,
           tcp_statistics_field::CurrentEstablished   },
          {"OutRsts",      &tcp_statistics::sent_reset_count,
           tcp_statistics_field::SentResets           },
      };
      for (let const &known : FIELDS) {
        if (name != known.name) continue;
        statistics.*known.field = value;
        statistics.available_fields |= static_cast<u32>(known.capability);
        has_statistics = true;
        break;
      }
    }
    break;
  }

  char netstat_buffer[65536];
  let const netstat_length = read_small_file(
      "/proc/net/netstat", netstat_buffer, sizeof(netstat_buffer));
  if (netstat_length == 0) return has_statistics;

  let const netstat_text = StringView{netstat_buffer, netstat_length};
  let tcp_extended_header = StringView{};
  usize netstat_position = 0;
  while (netstat_position < netstat_text.length) {
    let const line = each_line(netstat_text, netstat_position);
    if (!line.starts_with("TcpExt:")) continue;
    if (tcp_extended_header.is_empty()) {
      tcp_extended_header = line.substring(7);
      continue;
    }

    usize name_position = 0;
    usize value_position = 7;
    while (name_position < tcp_extended_header.length &&
           value_position < line.length)
    {
      let const name =
          tcp_extended_header.next_ascii_whitespace_word(name_position);
      let const value_word = line.next_ascii_whitespace_word(value_position);
      u64 value = 0;
      if (!parse_decimal_word(value_word, value)) continue;
      struct tcp_extended_field
      {
        StringView name;
        u64 tcp_statistics::*field;
        tcp_statistics_field capability;
      };
      static constexpr tcp_extended_field FIELDS[] = {
          {"ListenOverflows", &tcp_statistics::listen_overflow_count,
           tcp_statistics_field::ListenOverflows           },
          {"ListenDrops",     &tcp_statistics::listen_drop_count,
           tcp_statistics_field::ListenDrops               },
          {"TCPTimeouts",     &tcp_statistics::retransmit_timeout_count,
           tcp_statistics_field::RetransmitTimeouts        },
          {"TCPSynRetrans",   &tcp_statistics::syn_retransmit_count,
           tcp_statistics_field::SynRetransmits            },
          {"TCPFastRetrans",  &tcp_statistics::fast_retransmit_count,
           tcp_statistics_field::FastRetransmits           },
          {"TCPSpuriousRTOs",
           &tcp_statistics::spurious_retransmit_timeout_count,
           tcp_statistics_field::SpuriousRetransmitTimeouts},
          {"TCPRcvQDrop",     &tcp_statistics::receive_queue_drop_count,
           tcp_statistics_field::ReceiveQueueDrops         },
          {"TCPBacklogDrop",  &tcp_statistics::backlog_drop_count,
           tcp_statistics_field::BacklogDrops              },
          {"TCPReqQFullDrop", &tcp_statistics::request_queue_full_drop_count,
           tcp_statistics_field::RequestQueueFullDrops     },
          {"TCPRetransFail",  &tcp_statistics::retransmit_failure_count,
           tcp_statistics_field::RetransmitFailures        },
      };
      for (let const &known : FIELDS) {
        if (name != known.name) continue;
        statistics.*known.field = value;
        statistics.available_fields |= static_cast<u32>(known.capability);
        has_statistics = true;
        break;
      }
    }
    break;
  }
  return has_statistics;
#else
  unused(statistics);
  return false;
#endif
}

fn read_disk_io_snapshot(Allocator allocator) throws -> disk_io_snapshot
{
  disk_io_snapshot snapshot{ArrayList<disk_io_status>{allocator},
                            monotonic_nanos()};
#if defined __APPLE__
  let *matching = IOServiceMatching(kIOBlockStorageDriverClass);
  if (matching == nullptr) return snapshot;
  io_iterator_t iterator = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) !=
      KERN_SUCCESS)
  {
    return snapshot;
  }
  defer { IOObjectRelease(iterator); };

  let const do_read_number = [](CFDictionaryRef dictionary, const char *key,
                                u64 &value) wontthrow -> bool {
    let const key_text = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                   kCFStringEncodingUTF8);
    if (key_text == nullptr) return false;
    defer { CFRelease(key_text); };
    let const raw = CFDictionaryGetValue(dictionary, key_text);
    if (raw == nullptr || CFGetTypeID(raw) != CFNumberGetTypeID()) return false;
    i64 signed_value = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberSInt64Type,
                          &signed_value) ||
        signed_value < 0)
    {
      return false;
    }
    value = static_cast<u64>(signed_value);
    return true;
  };

  io_object_t service = IO_OBJECT_NULL;
  while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
    defer { IOObjectRelease(service); };
    let const raw_statistics = IORegistryEntryCreateCFProperty(
        service, CFSTR(kIOBlockStorageDriverStatisticsKey), kCFAllocatorDefault,
        0);
    if (raw_statistics == nullptr ||
        CFGetTypeID(raw_statistics) != CFDictionaryGetTypeID())
    {
      if (raw_statistics != nullptr) CFRelease(raw_statistics);
      continue;
    }
    defer { CFRelease(raw_statistics); };
    let const dictionary = static_cast<CFDictionaryRef>(raw_statistics);

    char name_buffer[256]{};
    let const raw_name = IORegistryEntrySearchCFProperty(
        service, kIOServicePlane, CFSTR("BSD Name"), kCFAllocatorDefault,
        kIORegistryIterateRecursively);
    if (raw_name != nullptr) {
      if (CFGetTypeID(raw_name) == CFStringGetTypeID()) {
        unused(CFStringGetCString(static_cast<CFStringRef>(raw_name),
                                  name_buffer, sizeof(name_buffer),
                                  kCFStringEncodingUTF8));
      }
      CFRelease(raw_name);
    }
    if (name_buffer[0] == '\0') {
      io_name_t fallback_name{};
      if (IORegistryEntryGetName(service, fallback_name) != KERN_SUCCESS) {
        continue;
      }
      std::snprintf(name_buffer, sizeof(name_buffer), "%s", fallback_name);
    }

    disk_io_status status{};
    status.name = String{allocator, name_buffer};
    struct disk_field
    {
      const char *name;
      u64 disk_io_status::*field;
      disk_io_field capability;
    };
    static constexpr disk_field FIELDS[] = {
        {kIOBlockStorageDriverStatisticsBytesReadKey,
         &disk_io_status::read_bytes,             disk_io_field::ReadBytes     },
        {kIOBlockStorageDriverStatisticsBytesWrittenKey,
         &disk_io_status::written_bytes,          disk_io_field::WrittenBytes  },
        {kIOBlockStorageDriverStatisticsReadsKey,
         &disk_io_status::read_operation_count,   disk_io_field::ReadOperations},
        {kIOBlockStorageDriverStatisticsWritesKey,
         &disk_io_status::write_operation_count,
         disk_io_field::WriteOperations                                        },
        {kIOBlockStorageDriverStatisticsTotalReadTimeKey,
         &disk_io_status::read_time_nanoseconds,  disk_io_field::ReadTime      },
        {kIOBlockStorageDriverStatisticsTotalWriteTimeKey,
         &disk_io_status::write_time_nanoseconds, disk_io_field::WriteTime     },
        {kIOBlockStorageDriverStatisticsReadErrorsKey,
         &disk_io_status::read_error_count,       disk_io_field::ReadErrors    },
        {kIOBlockStorageDriverStatisticsWriteErrorsKey,
         &disk_io_status::write_error_count,      disk_io_field::WriteErrors   },
        {kIOBlockStorageDriverStatisticsReadRetriesKey,
         &disk_io_status::read_retry_count,       disk_io_field::ReadRetries   },
        {kIOBlockStorageDriverStatisticsWriteRetriesKey,
         &disk_io_status::write_retry_count,      disk_io_field::WriteRetries  },
    };
    for (let const &field : FIELDS) {
      if (do_read_number(dictionary, field.name, status.*field.field)) {
        status.available_fields |= static_cast<u32>(field.capability);
      }
    }
    if (status.available_fields != 0) snapshot.disks.push(steal(status));
  }
#elif defined __linux__
  let const contents = Path{"/proc/diskstats"}.read_entire_file();
  if (!contents.has_value()) return snapshot;

  let const text = contents->view();
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    usize word_position = 0;
    unused(line.next_ascii_whitespace_word(word_position));
    unused(line.next_ascii_whitespace_word(word_position));
    let const name = line.next_ascii_whitespace_word(word_position);
    if (name.is_empty()) continue;
    u64 values[17]{};
    let const value_count = parse_decimal_words(line.substring(word_position),
                                                values, countof(values));
    if (value_count < 11) continue;

    let const do_scale = [](u64 value, u64 multiplier) wontthrow -> Maybe<u64> {
      if (value > UINT64_MAX / multiplier) return None;
      return value * multiplier;
    };
    let const read_bytes = do_scale(values[2], 512);
    let const written_bytes = do_scale(values[6], 512);
    let const read_time = do_scale(values[3], 1000000);
    let const write_time = do_scale(values[7], 1000000);
    let const busy_time = do_scale(values[9], 1000000);
    let const weighted_busy_time = do_scale(values[10], 1000000);
    u32 available_fields = static_cast<u32>(disk_io_field::ReadOperations) |
                           static_cast<u32>(disk_io_field::WriteOperations) |
                           static_cast<u32>(disk_io_field::QueueDepth);
    if (read_bytes.has_value())
      available_fields |= static_cast<u32>(disk_io_field::ReadBytes);
    if (written_bytes.has_value())
      available_fields |= static_cast<u32>(disk_io_field::WrittenBytes);
    if (read_time.has_value())
      available_fields |= static_cast<u32>(disk_io_field::ReadTime);
    if (write_time.has_value())
      available_fields |= static_cast<u32>(disk_io_field::WriteTime);
    if (busy_time.has_value())
      available_fields |= static_cast<u32>(disk_io_field::BusyTime);
    if (weighted_busy_time.has_value()) {
      available_fields |= static_cast<u32>(disk_io_field::WeightedBusyTime);
    }
    snapshot.disks.push(disk_io_status{
        String{allocator, name},
        read_bytes.value_or(0),
        written_bytes.value_or(0),
        values[0],
        values[4],
        read_time.value_or(0),
        write_time.value_or(0),
        busy_time.value_or(0),
        0,
        weighted_busy_time.value_or(0),
        values[8],
        0,
        0,
        0,
        0,
        available_fields,
    });
  }
#endif
  return snapshot;
}

fn system_uptime_seconds() wontthrow -> Maybe<u64>
{
#if defined __APPLE__
  struct timeval boot_time{};
  usize boot_time_length = sizeof(boot_time);
  int name_mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (::sysctl(name_mib, 2, &boot_time, &boot_time_length, nullptr, 0) != 0)
    return None;
  if (boot_time.tv_sec <= 0) return None;

  let const now = ::time(nullptr);
  if (now <= boot_time.tv_sec) return None;

  return static_cast<u64>(now - boot_time.tv_sec);
#elif defined __linux__
  char buffer[128];
  if (read_small_file("/proc/uptime", buffer, sizeof(buffer)) == 0) return None;

  return static_cast<u64>(std::strtoull(buffer, nullptr, 10));
#else
  return None;
#endif
}

fn read_memory_status(memory_status &status) wontthrow -> bool
{
#if defined __APPLE__
  u64 memory_bytes = 0;
  usize memory_bytes_length = sizeof(memory_bytes);
  if (::sysctlbyname("hw.memsize", &memory_bytes, &memory_bytes_length, nullptr,
                     0) != 0)
    return false;
  status.total_kib = memory_bytes / 1024;
  status.available_fields |= static_cast<u32>(memory_status_field::Total);

  let const host_port = mach_host_self();
  vm_size_t page_bytes = 0;
  if (host_page_size(host_port, &page_bytes) != KERN_SUCCESS ||
      page_bytes < 1024)
  {
    page_bytes = 4096;
  }

  vm_statistics64_data_t vm_stats{};
  mach_msg_type_number_t vm_stats_count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host_port, HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm_stats),
                        &vm_stats_count) == KERN_SUCCESS)
  {
    let const page_kib = static_cast<u64>(page_bytes) / 1024;
    status.free_kib = static_cast<u64>(vm_stats.free_count) * page_kib;
    status.available_kib = (static_cast<u64>(vm_stats.free_count) +
                            static_cast<u64>(vm_stats.inactive_count) +
                            static_cast<u64>(vm_stats.purgeable_count)) *
                           page_kib;
    status.available_fields |= static_cast<u32>(memory_status_field::Free) |
                               static_cast<u32>(memory_status_field::Available);
  }

  struct xsw_usage swap{};
  usize swap_length = sizeof(swap);
  if (::sysctlbyname("vm.swapusage", &swap, &swap_length, nullptr, 0) == 0) {
    status.swap_total_kib = swap.xsu_total / 1024;
    status.swap_free_kib = swap.xsu_avail / 1024;
    status.available_fields |=
        static_cast<u32>(memory_status_field::SwapTotal) |
        static_cast<u32>(memory_status_field::SwapFree);
  }

  return true;
#elif defined __linux__
  struct meminfo_field
  {
    StringView name;
    u64 memory_status::*field;
    memory_status_field availability;
  };

  static constexpr meminfo_field MEMINFO_FIELDS[] = {
      {"MemTotal",        &memory_status::total_kib,             memory_status_field::Total   },
      {"MemFree",         &memory_status::free_kib,              memory_status_field::Free    },
      {"MemAvailable",    &memory_status::available_kib,
       memory_status_field::Available                                                         },
      {"SwapTotal",       &memory_status::swap_total_kib,
       memory_status_field::SwapTotal                                                         },
      {"SwapFree",        &memory_status::swap_free_kib,
       memory_status_field::SwapFree                                                          },
      {"Buffers",         &memory_status::buffer_kib,            memory_status_field::Buffers },
      {"Cached",          &memory_status::cached_kib,            memory_status_field::Cached  },
      {"SReclaimable",    &memory_status::reclaimable_slab_kib,
       memory_status_field::ReclaimableSlab                                                   },
      {"Shmem",           &memory_status::shared_kib,            memory_status_field::Shared  },
      {"Slab",            &memory_status::slab_kib,              memory_status_field::Slab    },
      {"Active",          &memory_status::active_kib,            memory_status_field::Active  },
      {"Inactive",        &memory_status::inactive_kib,          memory_status_field::Inactive},
      {"CommitLimit",     &memory_status::commit_limit_kib,
       memory_status_field::CommitLimit                                                       },
      {"Committed_AS",    &memory_status::committed_kib,
       memory_status_field::Committed                                                         },
      {"HugePages_Total", &memory_status::huge_page_total_count,
       memory_status_field::HugePagesTotal                                                    },
      {"HugePages_Free",  &memory_status::huge_page_free_count,
       memory_status_field::HugePagesFree                                                     },
  };

  char buffer[8192];
  let const length = read_small_file("/proc/meminfo", buffer, sizeof(buffer));
  if (length == 0) return false;

  let const text = StringView{buffer, length};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    let const colon_position = line.find_character(':');
    if (!colon_position.has_value()) continue;

    let const name = line.substring_of_length(0, *colon_position);
    for (let const &known : MEMINFO_FIELDS) {
      if (name != known.name) continue;

      if (let const parsed =
              leading_digits(line, *colon_position + 1).to<u64>();
          !parsed.is_error())
      {
        status.*known.field = parsed.value();
        status.available_fields |= static_cast<u32>(known.availability);
      }

      break;
    }
  }

  if (!status.has_field(memory_status_field::Available) &&
      status.has_field(memory_status_field::Free))
    status.available_kib = status.free_kib;
  return status.has_field(memory_status_field::Total);
#else
  unused(status);
  return false;
#endif
}

fn read_swap_status(swap_status &status) wontthrow -> bool
{
#if defined __APPLE__
  struct xsw_usage swap{};
  usize swap_length = sizeof(swap);
  if (::sysctlbyname("vm.swapusage", &swap, &swap_length, nullptr, 0) != 0) {
    return false;
  }
  status.total_bytes = swap.xsu_total;
  status.used_bytes = swap.xsu_used;
  status.free_bytes = swap.xsu_avail;
  status.has_encryption_state = true;
  status.is_encrypted = swap.xsu_encrypted != 0;

  let const host_port = mach_host_self();
  vm_size_t page_bytes = 4096;
  unused(host_page_size(host_port, &page_bytes));
  vm_statistics64_data_t vm_stats{};
  mach_msg_type_number_t vm_stats_count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host_port, HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm_stats),
                        &vm_stats_count) == KERN_SUCCESS)
  {
    status.input_bytes = static_cast<u64>(vm_stats.swapins) * page_bytes;
    status.output_bytes = static_cast<u64>(vm_stats.swapouts) * page_bytes;
    status.has_activity = true;
  }

  return true;
#elif defined __linux__
  memory_status memory{};
  if (!read_memory_status(memory)) return false;
  if (!memory.has_field(memory_status_field::SwapTotal) ||
      !memory.has_field(memory_status_field::SwapFree))
    return false;
  status.total_bytes = memory.swap_total_kib * 1024;
  status.free_bytes = memory.swap_free_kib * 1024;
  status.used_bytes = status.total_bytes > status.free_bytes
                          ? status.total_bytes - status.free_bytes
                          : 0;

  char buffer[16384];
  let const length = read_small_file("/proc/vmstat", buffer, sizeof(buffer));
  if (length == 0) return true;

  u64 input_pages = 0;
  u64 output_pages = 0;
  let const text = StringView{buffer, length};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    if (line.starts_with("pswpin ")) {
      if (let const parsed = leading_digits(line, 7).to<u64>();
          !parsed.is_error())
        input_pages = parsed.value();
    } else if (line.starts_with("pswpout ")) {
      if (let const parsed = leading_digits(line, 8).to<u64>();
          !parsed.is_error())
        output_pages = parsed.value();
    }
  }

  let const page_bytes = ::sysconf(_SC_PAGESIZE);
  if (page_bytes > 0) {
    status.input_bytes = input_pages * static_cast<u64>(page_bytes);
    status.output_bytes = output_pages * static_cast<u64>(page_bytes);
    status.has_activity = true;
  }

  return true;
#else
  unused(status);
  return false;
#endif
}

#if defined __linux__
static fn parse_linux_process_io_status(StringView text,
                                        process_io_status &status) wontthrow
    -> void
{
  struct io_field
  {
    StringView name;
    u64 process_io_status::*field;
  };
  static constexpr io_field FIELDS[] = {
      {"read_bytes:",  &process_io_status::read_bytes           },
      {"write_bytes:", &process_io_status::written_bytes        },
      {"syscr:",       &process_io_status::read_operation_count },
      {"syscw:",       &process_io_status::write_operation_count},
  };

  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    for (let const &known : FIELDS) {
      if (!line.starts_with(known.name)) continue;
      if (let const parsed = leading_digits(line, known.name.length).to<u64>();
          !parsed.is_error())
        status.*known.field = parsed.value();
      if (known.name == "syscr:" || known.name == "syscw:") {
        status.has_operation_counts = true;
      }
      break;
    }
  }
}
#endif

fn read_process_io_status(i64 pid, process_io_status &status) wontthrow -> bool
{
#if defined __APPLE__
  rusage_info_v2 usage{};
  if (::proc_pid_rusage(static_cast<int>(pid), RUSAGE_INFO_V2,
                        reinterpret_cast<rusage_info_t *>(&usage)) != 0)
  {
    return false;
  }
  status.read_bytes = usage.ri_diskio_bytesread;
  status.written_bytes = usage.ri_diskio_byteswritten;
  return true;
#elif defined __linux__
  char path[64];
  let const path_length = std::snprintf(path, sizeof(path), "/proc/%lld/io",
                                        static_cast<long long>(pid));
  if (path_length <= 0 || static_cast<usize>(path_length) >= sizeof(path)) {
    return false;
  }

  char buffer[2048];
  let const length = read_small_file(path, buffer, sizeof(buffer));
  if (length == 0) return false;

  parse_linux_process_io_status(StringView{buffer, length}, status);

  return true;
#else
  unused(pid);
  unused(status);
  return false;
#endif
}

fn read_process_io_statuses(const ArrayList<i64> &process_ids,
                            ArrayList<process_io_status> &statuses,
                            ArrayList<u8> &availability) throws -> void
{
  statuses.clear();
  availability.clear();
  statuses.reserve(process_ids.count());
  availability.reserve(process_ids.count());
  for (usize index = 0; index < process_ids.count(); index++) {
    statuses.push({});
    availability.push(0);
  }

#if defined __linux__
  constexpr usize PROCESS_IO_BATCH_COUNT = 64;
  struct process_io_probe
  {
    descriptor fd{KOSH_INVALID_FD};
    usize process_position{0};
    char bytes[2048]{};
  };

  process_io_probe probes[PROCESS_IO_BATCH_COUNT]{};
  usize probe_count = 0;
  let batch = Batch{statuses.allocator()};
  let results = ArrayList<batch_result>{statuses.allocator()};
  batch.reserve(PROCESS_IO_BATCH_COUNT);
  results.reserve(PROCESS_IO_BATCH_COUNT);
  let const do_flush_probes = [&]() throws -> void {
    batch.clear();
    for (usize index = 0; index < probe_count; index++) {
      batch.add(batch_operation::read(probes[index].fd, probes[index].bytes,
                                      sizeof(probes[index].bytes)));
    }
    batch.execute(results);
    for (usize index = 0; index < probe_count; index++) {
      let const &result = results[index];
      if (result.error_number == 0 && result.transferred_byte_count != 0) {
        let const process_position = probes[index].process_position;
        parse_linux_process_io_status(
            StringView{probes[index].bytes, result.transferred_byte_count},
            statuses[process_position]);
        availability[process_position] = 1;
      }
      unused(close_fd(probes[index].fd));
    }
    probe_count = 0;
  };

  for (usize process_position = 0; process_position < process_ids.count();
       process_position++)
  {
    char path[64];
    let const path_length =
        std::snprintf(path, sizeof(path), "/proc/%lld/io",
                      static_cast<long long>(process_ids[process_position]));
    if (path_length <= 0 || static_cast<usize>(path_length) >= sizeof(path)) {
      continue;
    }

    let const opened = open_file_descriptor(path, file_open_mode::Read);
    if (!opened.has_value()) continue;

    probes[probe_count].fd = *opened;
    probes[probe_count].process_position = process_position;
    probe_count++;
    if (probe_count == PROCESS_IO_BATCH_COUNT) do_flush_probes();
  }
  if (probe_count != 0) do_flush_probes();
#else
  for (usize index = 0; index < process_ids.count(); index++) {
    availability[index] =
        read_process_io_status(process_ids[index], statuses[index]) ? 1 : 0;
  }
#endif
}

fn processor_model_name(Allocator allocator) throws -> Maybe<String>
{
#if defined __APPLE__
  usize name_length = 0;
  if (::sysctlbyname("machdep.cpu.brand_string", nullptr, &name_length, nullptr,
                     0) != 0 ||
      name_length == 0)
  {
    return None;
  }

  ArrayList<char> buffer{allocator};
  buffer.reserve(name_length + 1);
  if (::sysctlbyname("machdep.cpu.brand_string", buffer.begin(), &name_length,
                     nullptr, 0) != 0)
    return None;

  buffer.begin()[name_length] = '\0';
  return String{allocator, StringView{buffer.begin()}};
#elif defined __linux__
  static constexpr StringView KNOWN_KEYS[] = {
      "model name", "Model name", "Hardware", "cpu model", "Processor"};
  char buffer[16384];
  let const length = read_small_file("/proc/cpuinfo", buffer, sizeof(buffer));
  if (length == 0) return None;

  let const text = StringView{buffer, length};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    let const colon_position = line.find_character(':');
    if (!colon_position.has_value()) continue;

    let name = line.substring_of_length(0, *colon_position);
    while (!name.is_empty() &&
           (name[name.length - 1] == ' ' || name[name.length - 1] == '\t'))
      name = name.substring_of_length(0, name.length - 1);

    bool is_known = false;
    for (let const &known : KNOWN_KEYS) {
      if (name != known) continue;

      is_known = true;
      break;
    }
    if (!is_known) continue;

    let value = line.substring(*colon_position + 1);
    while (!value.is_empty() && (value[0] == ' ' || value[0] == '\t'))
      value = value.substring(1);
    if (value.is_empty()) continue;

    return String{allocator, value};
  }

  return None;
#else
  unused(allocator);
  return None;
#endif
}

#if defined __APPLE__

static pure fn open_flags_access(int open_flags) wontthrow -> char
{
  switch (open_flags & O_ACCMODE) {
  case O_RDONLY: return 'r';
  case O_WRONLY: return 'w';
  default: break;
  }

  return 'u';
}

#endif

#if defined __linux__

fn linux_socket_endpoint(StringView process_path, u64 inode,
                         Allocator allocator) throws -> String
{
  let const path = String{process_path} + "/net/unix";
  char buffer[65536];
  let const length = read_small_file(path.c_str(), buffer, sizeof(buffer));
  if (length == 0) return String{allocator};

  let const text = StringView{buffer, length};
  usize position = 0;
  while (position < text.length) {
    let const line = each_line(text, position);
    usize token_position = 0;
    StringView inode_token;
    StringView endpoint;
    for (usize token_index = 0; token_index < 8; token_index++) {
      while (token_position < line.length &&
             (line[token_position] == ' ' || line[token_position] == '\t'))
        token_position++;
      if (token_position == line.length) break;

      let const token_start = token_position;
      while (token_position < line.length && line[token_position] != ' ' &&
             line[token_position] != '\t')
        token_position++;
      let const token =
          line.substring_of_length(token_start, token_position - token_start);
      if (token_index == 6) inode_token = token;
      if (token_index == 7) endpoint = token;
    }

    if (inode_token.is_empty() || endpoint.is_empty()) continue;
    let const parsed = inode_token.to<u64>();
    if (!parsed.is_error() && parsed.value() == inode)
      return String{allocator, endpoint};
  }

  return String{allocator};
}

#endif

fn has_process_open_file_listing() wontthrow -> bool
{
#if defined __APPLE__ || defined __linux__
  return true;
#else
  return false;
#endif
}

fn list_process_open_files(i64 pid, Allocator allocator,
                           process_open_file_detail detail) throws
    -> ArrayList<process_open_file>
{
  ArrayList<process_open_file> files{allocator};
  let const do_push =
      [&files, allocator](StringView path, i64 descriptor_number, u64 size,
                          u64 file_id, u64 offset, u32 mode,
                          process_file_use use, char access, bool is_deleted,
                          StringView socket_endpoint) throws -> void {
    if (path.is_empty()) return;

    files.push(process_open_file{
        String{allocator, path           },
        descriptor_number, size, file_id, offset, mode,
        use, access, is_deleted, false, String{allocator, socket_endpoint}
    });
  };

#if defined __APPLE__
  if (detail == process_open_file_detail::IncludeMappings) {
    files.push(process_open_file{
        String{allocator, "[inaccessible]"},
        -1, 0, 0, 0, 0,
        process_file_use::Mapped, 'u', false, true, String{allocator}
    });
  }
  let const process_id = static_cast<pid_t>(pid);
  struct proc_vnodepathinfo vnode_paths{};
  if (::proc_pidinfo(process_id, PROC_PIDVNODEPATHINFO, 0, &vnode_paths,
                     sizeof(vnode_paths)) == sizeof(vnode_paths))
  {
    do_push(StringView{vnode_paths.pvi_cdir.vip_path}, -1, 0,
            static_cast<u64>(vnode_paths.pvi_cdir.vip_vi.vi_stat.vst_ino), 0,
            vnode_paths.pvi_cdir.vip_vi.vi_stat.vst_mode, process_file_use::Cwd,
            'r', false, {});
    do_push(StringView{vnode_paths.pvi_rdir.vip_path}, -1, 0,
            static_cast<u64>(vnode_paths.pvi_rdir.vip_vi.vi_stat.vst_ino), 0,
            vnode_paths.pvi_rdir.vip_vi.vi_stat.vst_mode,
            process_file_use::Root, 'r', false, {});
  }

  char executable_path[PROC_PIDPATHINFO_MAXSIZE];
  if (::proc_pidpath(process_id, executable_path, sizeof(executable_path)) > 0)
    do_push(StringView{executable_path}, -1, 0, 0, 0, 0,
            process_file_use::Executable, 'r', false, {});

  let descriptor_bytes =
      ::proc_pidinfo(process_id, PROC_PIDLISTFDS, 0, nullptr, 0);
  if (descriptor_bytes <= 0) return files;

  ArrayList<struct proc_fdinfo> descriptors{allocator};
  descriptors.reserve(static_cast<usize>(descriptor_bytes) /
                      sizeof(struct proc_fdinfo));
  descriptor_bytes = ::proc_pidinfo(process_id, PROC_PIDLISTFDS, 0,
                                    descriptors.begin(), descriptor_bytes);
  if (descriptor_bytes <= 0) return files;

  let const descriptor_count =
      static_cast<usize>(descriptor_bytes) / sizeof(struct proc_fdinfo);
  for (usize descriptor_position = 0; descriptor_position < descriptor_count;
       descriptor_position++)
  {
    let const &descriptor = descriptors.begin()[descriptor_position];
    let const descriptor_number = static_cast<i64>(descriptor.proc_fd);

    switch (descriptor.proc_fdtype) {
    case PROX_FDTYPE_VNODE: {
      struct vnode_fdinfowithpath vnode{};
      if (::proc_pidfdinfo(process_id, descriptor.proc_fd,
                           PROC_PIDFDVNODEPATHINFO, &vnode,
                           sizeof(vnode)) != sizeof(vnode))
        break;

      do_push(StringView{vnode.pvip.vip_path}, descriptor_number,
              static_cast<u64>(vnode.pvip.vip_vi.vi_stat.vst_size),
              static_cast<u64>(vnode.pvip.vip_vi.vi_stat.vst_ino), 0,
              vnode.pvip.vip_vi.vi_stat.vst_mode, process_file_use::File,
              open_flags_access(vnode.pfi.fi_openflags), false, {});
      break;
    }

    case PROX_FDTYPE_SOCKET:
      do_push("[socket]", descriptor_number, 0, 0, 0, 0, process_file_use::File,
              'u', false, {});
      break;

    case PROX_FDTYPE_PIPE:
      do_push("[pipe]", descriptor_number, 0, 0, 0, 0, process_file_use::File,
              'u', false, {});
      break;

    default:
      do_push("[other]", descriptor_number, 0, 0, 0, 0, process_file_use::File,
              'u', false, {});
      break;
    }
  }

  return files;
#elif defined __linux__
  char process_path[64];
  let const process_path_length =
      std::snprintf(process_path, sizeof(process_path), "/proc/%lld",
                    static_cast<long long>(pid));
  if (process_path_length <= 0 ||
      static_cast<usize>(process_path_length) >= sizeof(process_path))
    return files;

  struct named_reference
  {
    StringView name;
    process_file_use use;
  };

  static constexpr named_reference REFERENCES[] = {
      {"cwd",  process_file_use::Cwd       },
      {"root", process_file_use::Root      },
      {"exe",  process_file_use::Executable},
  };

  for (let const &reference : REFERENCES) {
    let const reference_path =
        String{process_path} + "/" + String{reference.name};
    let const target = read_symlink(reference_path.view(), allocator);
    if (!target.has_value()) continue;

    constexpr StringView DELETED_SUFFIX = " (deleted)";
    let path = target->view();
    let const is_deleted =
        path.length >= DELETED_SUFFIX.length &&
        path.substring(path.length - DELETED_SUFFIX.length) == DELETED_SUFFIX;
    if (is_deleted)
      path = path.substring_of_length(0, path.length - DELETED_SUFFIX.length);
    do_push(path, -1, 0, 0, 0, 0, reference.use, 'r', is_deleted, {});
  }

  if (detail == process_open_file_detail::IncludeMappings) {
    bool did_read_mappings = false;
    let maps_path = String{allocator, process_path};
    maps_path += "/maps";
    if (let maps_descriptor =
            open_file_descriptor(maps_path.view(), file_open_mode::Read);
        maps_descriptor.has_value())
    {
      defer { close_fd(*maps_descriptor); };
      if (let maps = read_fd_to_string(*maps_descriptor, allocator);
          maps.has_value())
      {
        did_read_mappings = true;
        usize line_position = 0;
        while (line_position < maps->length()) {
          let const line = each_line(maps->view(), line_position);
          usize field_position = 0;
          u64 file_id = 0;
          bool is_valid = true;
          for (usize field_index = 0; field_index < 5; field_index++) {
            while (
                field_position < line.length &&
                (line[field_position] == ' ' || line[field_position] == '\t'))
              field_position++;
            let const field_start = field_position;
            while (field_position < line.length &&
                   line[field_position] != ' ' && line[field_position] != '\t')
              field_position++;
            if (field_start == field_position) {
              is_valid = false;
              break;
            }
            if (field_index == 4) {
              let const parsed =
                  line.substring_of_length(field_start,
                                           field_position - field_start)
                      .to<u64>();
              if (parsed.is_error()) {
                is_valid = false;
                break;
              }
              file_id = parsed.value();
            }
          }
          if (!is_valid) continue;
          while (field_position < line.length &&
                 (line[field_position] == ' ' || line[field_position] == '\t'))
            field_position++;
          if (field_position == line.length) continue;

          let path = line.substring(field_position);
          if (path[0] == '[') continue;

          constexpr StringView DELETED_SUFFIX = " (deleted)";
          let const is_deleted =
              path.length >= DELETED_SUFFIX.length &&
              path.substring(path.length - DELETED_SUFFIX.length) ==
                  DELETED_SUFFIX;
          if (is_deleted)
            path = path.substring_of_length(0, path.length -
                                                   DELETED_SUFFIX.length);

          bool is_duplicate = false;
          for (let const &file : files) {
            if (file.use == process_file_use::Mapped &&
                file.file_id == file_id && file.path.view() == path)
            {
              is_duplicate = true;
              break;
            }
          }
          if (is_duplicate) continue;

          do_push(path, -1, 0, file_id, 0, 0, process_file_use::Mapped, 'r',
                  is_deleted, {});
        }
      }
    }
    if (!did_read_mappings) {
      files.push(process_open_file{
          String{allocator, "[inaccessible]"},
          -1, 0, 0, 0, 0,
          process_file_use::Mapped, 'u', false, true, String{allocator}
      });
    }
  }

  let const descriptor_root = String{process_path} + "/fd";
  DIR *descriptor_directory = ::opendir(descriptor_root.c_str());
  if (descriptor_directory == nullptr) {
    if (errno == EACCES || errno == EPERM) {
      files.push(process_open_file{
          String{allocator, "[inaccessible]"},
          -1, 0, 0, 0, 0,
          process_file_use::File, 'u', false, true, String{allocator}
      });
    }
    return files;
  }
  defer { ::closedir(descriptor_directory); };

  let const descriptor_directory_fd = ::dirfd(descriptor_directory);
  for (struct dirent *entry = ::readdir(descriptor_directory); entry != nullptr;
       entry = ::readdir(descriptor_directory))
  {
    let const name = StringView{entry->d_name};
    if (name.is_empty() || !name.is_all_decimal_digits()) continue;

    let const parsed_number = name.to<i64>();
    if (parsed_number.is_error()) continue;

    let const link_path = descriptor_root + "/" + String{name};
    let const target = read_symlink(link_path.view(), allocator);
    if (!target.has_value()) continue;

    struct stat descriptor_status{};
    u64 size = 0;
    u64 file_id = 0;
    if (::fstatat(descriptor_directory_fd, entry->d_name, &descriptor_status,
                  0) == 0)
    {
      size = static_cast<u64>(descriptor_status.st_size);
      file_id = static_cast<u64>(descriptor_status.st_ino);
    }

    char access = 'u';
    u64 offset = 0;
    let const info_path = String{process_path} + "/fdinfo/" + String{name};
    char info_buffer[512];
    if (read_small_file(info_path.c_str(), info_buffer, sizeof(info_buffer)) !=
        0)
    {
      let const info_text = StringView{info_buffer};
      usize info_position = 0;
      while (info_position < info_text.length) {
        let const line = each_line(info_text, info_position);
        if (line.length >= 4 &&
            line.substring_of_length(0, 4) == StringView{"pos:"})
        {
          let const parsed = line.substring(4).to<u64>();
          if (!parsed.is_error()) offset = parsed.value();
          continue;
        }
        if (line.length < 6 ||
            line.substring_of_length(0, 6) != StringView{"flags:"})
          continue;

        let const flags = std::strtol(line.data + 6, nullptr, 8);
        switch (flags & O_ACCMODE) {
        case O_RDONLY: access = 'r'; break;
        case O_WRONLY: access = 'w'; break;
        default: break;
        }

        break;
      }
    }

    let const deleted_suffix = StringView{" (deleted)"};
    let const is_deleted =
        target->view().length >= deleted_suffix.length &&
        target->view().substring(target->view().length -
                                 deleted_suffix.length) == deleted_suffix;
    let const mode = descriptor_status.st_mode;
    let socket_endpoint = String{allocator};
    if (target->view().starts_with("socket:[")) {
      let const open = target->view().find_character('[');
      let const close = target->view().find_character(']');
      if (open.has_value() && close.has_value() && *close > *open + 1) {
        let const socket_inode = target->view()
                                     .substring(*open + 1)
                                     .substring_of_length(0, *close - *open - 1)
                                     .to<u64>();
        if (!socket_inode.is_error())
          socket_endpoint = linux_socket_endpoint(
              process_path, socket_inode.value(), allocator);
      }
    }
    do_push(target->view(), parsed_number.value(), size, file_id, offset, mode,
            process_file_use::File, access, is_deleted, socket_endpoint.view());
  }

  return files;
#else
  unused(pid);
  unused(detail);
  return files;
#endif
}

} /* namespace os */
} /* namespace koshka */
