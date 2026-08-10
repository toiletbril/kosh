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
    free(start_counters);
    free(end_counters);
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
        static_cast<u64 *>(calloc(start_counter_count, sizeof(u64)));
    end_counters = static_cast<u64 *>(calloc(start_counter_count, sizeof(u64)));
    if (start_counters == nullptr || end_counters == nullptr) return false;

    for (usize event_index = 0; event_index < EVENT_COUNT; event_index++) {
      if (counter_map[event_index] >= counter_count) return false;
    }

    return true;
  }

  fn start() wontthrow -> bool
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
  let const raw_path = read_symlink("/proc/self/exe");
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

    if (let status =
            Path{(process_directory + "/status").view()}.read_entire_file();
        status.has_value())
    {
      let const text = status->view();
      usize line_start_position = 0;
      for (usize position = 0; position <= text.length; position++) {
        if (position != text.length && text[position] != '\n') continue;
        let const line = text.substring_of_length(
            line_start_position, position - line_start_position);
        line_start_position = position + 1;
        if (line.length < 5 ||
            line.substring_of_length(0, 5) != StringView{"Uid:\t"})
          continue;

        usize digit_end_position = 5;
        while (digit_end_position < line.length &&
               line[digit_end_position] >= '0' &&
               line[digit_end_position] <= '9')
          digit_end_position++;
        let const uid_text =
            line.substring_of_length(5, digit_end_position - 5);
        if (let const uid = uid_text.to<i64>(); !uid.is_error())
          process.owner_id = static_cast<u32>(uid.value());
        break;
      }
    }

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
#else
  unused(stats);
  return false;
#endif
}

} /* namespace os */
} /* namespace koshka */
