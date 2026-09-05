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
    process.name = String{StringView{record.kp_proc.p_comm}};
    process.owner_id = static_cast<u32>(record.kp_eproc.e_ucred.cr_uid);
    process.state = process_state_letter(record.kp_proc.p_stat);

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
        process.cpu_ticks = static_cast<u64>(task_info.pti_total_user +
                                             task_info.pti_total_system);
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

static fn linux_process_real_uid(StringView process_directory) throws
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
    if (line.length < 5 ||
        line.substring_of_length(0, 5) != StringView{"Uid:\t"})
      continue;
    usize digit_end_position = 5;
    while (digit_end_position < line.length &&
           line[digit_end_position] >= '0' && line[digit_end_position] <= '9')
      digit_end_position++;
    let const uid =
        line.substring_of_length(5, digit_end_position - 5).to<u32>();
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

    if (let const uid = linux_process_real_uid(process_directory.view()))
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
          if (let const user_ticks = nth_space_field(fields, 11).to<i64>();
              !user_ticks.is_error())
            process.cpu_ticks += static_cast<u64>(user_ticks.value());
          if (let const system_ticks = nth_space_field(fields, 12).to<i64>();
              !system_ticks.is_error())
            process.cpu_ticks += static_cast<u64>(system_ticks.value());
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

} /* namespace os */
} /* namespace koshka */
