/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines lightweight evaluator enums and value records for
 * argument lifetimes, execution modes, status propagation, restrictions, and
 * glob fields. It also names the value the exported set stores beside each
 * name and owns the grouped job state and the composite-key array storage kept
 * by EvalContext. It prevents common evaluator types from depending on
 * Eval.hpp.
 */

#pragma once

#include "Builtin.hpp"
#include "Errors.hpp"
#include "MimicMood.hpp"
#include "Platform.hpp"
#include "base/Arena.hpp"
#include "base/Bitset.hpp"
#include "base/Common.hpp"
#include "base/Containers.hpp"
#include "base/Hive.hpp"
#include "base/Maybe.hpp"
#include "base/Path.hpp"

namespace koshka {

enum class assignment_update_mode : u8
{
  Replace,
  Append,
};

using exported_name_value =
    std::conditional_t<os::ENVIRONMENT_IS_CASE_SENSITIVE, Nothing, String>;

class CompositeKeyArrays
{
public:
  pure fn has(StringView name) const wontthrow -> bool
  {
    return m_names.contains(name);
  }
  fn declare(StringView name) throws -> void { m_names.add(name); }
  fn forget(StringView name) throws -> void { m_names.remove(name); }
  fn names() wontthrow -> HashSet & { return m_names; }
  pure fn names() const wontthrow -> const HashSet & { return m_names; }
  fn values() wontthrow -> StringMap<String> & { return m_values; }
  pure fn values() const wontthrow -> const StringMap<String> &
  {
    return m_values;
  }

  fn put_ordered(StringView composite, StringView value) throws -> void
  {
    if (!m_values.find(composite).has_value())
      m_sequences.set(composite, m_next_sequence++);
    m_values.set(composite, value);
  }
  fn erase_ordered(StringView composite) throws -> void
  {
    m_values.erase(composite);
    m_sequences.erase(composite);
  }
  pure fn sequence_of(StringView composite) const wontthrow -> u64
  {
    let const found = m_sequences.find(composite);
    return found.has_value() ? *found.value() : u64{0};
  }

private:
  HashSet m_names{heap_allocator()};
  StringMap<String> m_values{heap_allocator()};
  StringMap<u64> m_sequences{heap_allocator()};
  u64 m_next_sequence{1};
};

enum class argument_lifetime : u8
{
  Persistent,
  Transient,
};

enum class argument_context : u8
{
  Command,
  ArrayLiteral,
  AssociativeLiteral,
};

enum class execution_mode : u8
{
  Foreground,
  Background,
};

enum class script_isolation : u8
{
  Shared,
  Isolated,
};

enum class shell_identity_mode : u8
{
  Native,
  Bash,
};

enum class return_handling : u8
{
  Propagate,
  Consume,
  Reject,
};

enum class history_recording : u8
{
  Disabled,
  Enabled,
};

enum class syntax_error_reach : u8
{
  Command,
  PosixScript,
};

enum class source_tilde_expansion : u8
{
  Disabled,
  Enabled,
};

enum class status_flag : u32
{
  ErrResolved = 1U << 0,
  ExitCodeReported = 1U << 1,
};

struct status_result
{
  i32 status{0};
  u32 flags{0};

  constexpr status_result() wontthrow = default;
  constexpr status_result(i64 exit_status, u32 status_flags = 0) wontthrow
      : status{static_cast<i32>(exit_status)},
        flags{status_flags}
  {}

  pure fn has(status_flag flag) const wontthrow -> bool
  {
    return (flags & static_cast<u32>(flag)) != 0;
  }

  fn set(status_flag flag) wontthrow -> void
  {
    flags |= static_cast<u32>(flag);
  }
};

static_assert(sizeof(status_result) == 8);

enum class restricted_path_use : u8
{
  Command,
  Source,
  History,
  Hash,
};

enum class variable_attribute : u8
{
  Readonly = 1U << 0,
  Integer = 1U << 1,
  Lowercase = 1U << 2,
  Uppercase = 1U << 3,
  Declared = 1U << 4,
  Nameref = 1U << 5,
};

struct variable_entry
{
  String value{heap_allocator()};
  ArrayList<String> elements{heap_allocator()};
  u8 attribute_bits{0};
  bool has_value{false};
  bool is_indexed_array{false};
};

class VariableTable
{
public:
  VariableTable() = default;
  VariableTable(VariableTable &&other) wontthrow
      : m_index{steal(other.m_index)},
        m_entries{steal(other.m_entries)},
        m_pipestatus_entry{other.m_pipestatus_entry},
        m_value_count{other.m_value_count},
        m_indexed_array_count{other.m_indexed_array_count},
        m_has_namerefs{other.m_has_namerefs},
        m_has_declared_marks{other.m_has_declared_marks}
  {
    other.forget_moved_entries();
  }
  fn operator=(VariableTable &&other) wontthrow -> VariableTable &
  {
    if (this == &other) return *this;

    m_index = steal(other.m_index);
    m_entries = steal(other.m_entries);
    m_pipestatus_entry = other.m_pipestatus_entry;
    m_value_count = other.m_value_count;
    m_indexed_array_count = other.m_indexed_array_count;
    m_has_namerefs = other.m_has_namerefs;
    m_has_declared_marks = other.m_has_declared_marks;
    other.forget_moved_entries();

    return *this;
  }

  VariableTable(const VariableTable &other) throws
      : m_index{other.m_index},
        m_value_count{other.m_value_count},
        m_indexed_array_count{other.m_indexed_array_count},
        m_has_namerefs{other.m_has_namerefs},
        m_has_declared_marks{other.m_has_declared_marks}
  {
    m_index.for_each([&](StringView, variable_entry *&entry) throws {
      entry = m_entries.emplace(*entry);
    });
  }
  fn operator=(const VariableTable &other) throws -> VariableTable &
  {
    if (this != &other) *this = VariableTable{other};
    return *this;
  }

  hot pure fn find(StringView name) const wontthrow -> Maybe<variable_entry *>
  {
    let const found = m_index.find(name);
    if (!found.has_value()) return None;
    return *found.value();
  }

  fn get_or_create(StringView name) throws -> variable_entry &
  {
    if (let const found = m_index.find(name); found.has_value())
      return **found.value();

    let *const created = m_entries.emplace();
    try {
      m_index.set(name, created);
    } catch (...) {
      m_entries.erase(created);
      throw;
    }
    return *created;
  }

  fn release_if_unused(StringView name, variable_entry &entry) throws -> void
  {
    if (entry.has_value || entry.is_indexed_array || entry.attribute_bits != 0)
      return;

    if (&entry == m_pipestatus_entry) m_pipestatus_entry = nullptr;
    m_index.erase(name);
    m_entries.erase(&entry);
  }

  fn set_elements(StringView name, ArrayList<String> elements) throws
      -> ArrayList<String> *
  {
    let &entry = get_or_create(name);
    if (!entry.is_indexed_array) m_indexed_array_count++;
    entry.is_indexed_array = true;
    entry.elements = steal(elements);
    return &entry.elements;
  }

  fn erase_elements(StringView name) throws -> void
  {
    let const found = find(name);
    if (!found.has_value() || !(*found)->is_indexed_array) return;

    (*found)->is_indexed_array = false;
    (*found)->elements.clear();
    m_indexed_array_count--;
    release_if_unused(name, **found);
  }

  fn find_pipestatus() throws -> Maybe<variable_entry *>
  {
    if (m_pipestatus_entry != nullptr) return m_pipestatus_entry;

    let const found = find(StringView{"PIPESTATUS", 10});
    if (found.has_value()) m_pipestatus_entry = *found;
    return found;
  }

  fn set_value(StringView name, StringView value) throws -> String *
  {
    return assign_value(get_or_create(name), value);
  }

  fn assign_value(variable_entry &entry, StringView value) throws -> String *
  {
    if (!entry.has_value) m_value_count++;
    entry.has_value = true;
    let const is_buffer_wasteful = entry.value.count() > 256 &&
                                   value.length < entry.value.count() / 2;
    if (is_buffer_wasteful) {
      entry.value = String{heap_allocator(), value};
      return &entry.value;
    }
    entry.value.clear();
    entry.value.append(value);
    return &entry.value;
  }

  fn erase_value(StringView name) throws -> void
  {
    let const found = find(name);
    if (!found.has_value() || !(*found)->has_value) return;

    (*found)->has_value = false;
    (*found)->value.clear();
    m_value_count--;
    release_if_unused(name, **found);
  }

  fn set_attribute_bits(StringView name, u8 bits) throws -> void
  {
    note_attribute_bits(bits);
    if (bits == 0) {
      if (let const found = find(name); found.has_value()) {
        (*found)->attribute_bits = 0;
        release_if_unused(name, **found);
      }
      return;
    }

    get_or_create(name).attribute_bits = bits;
  }

  template <typename Callback>
  fn for_each(Callback do_callback) const throws -> void
  {
    m_index.for_each([&](StringView name, variable_entry *const &entry) throws {
      do_callback(name, *entry);
    });
  }

  pure fn value_count() const wontthrow -> usize { return m_value_count; }
  pure fn indexed_array_count() const wontthrow -> usize
  {
    return m_indexed_array_count;
  }
  pure fn has_namerefs() const wontthrow -> bool { return m_has_namerefs; }
  pure fn has_declared_marks() const wontthrow -> bool
  {
    return m_has_declared_marks;
  }
  fn note_attribute_bits(u8 bits) wontthrow -> void
  {
    if ((bits & static_cast<u8>(variable_attribute::Nameref)) != 0)
      m_has_namerefs = true;
    if ((bits & static_cast<u8>(variable_attribute::Declared)) != 0)
      m_has_declared_marks = true;
  }

private:
  fn forget_moved_entries() wontthrow -> void
  {
    m_index.clear();
    m_pipestatus_entry = nullptr;
    m_value_count = 0;
    m_indexed_array_count = 0;
  }

  StringMap<variable_entry *> m_index{heap_allocator()};
  Hive<variable_entry> m_entries;
  variable_entry *m_pipestatus_entry{nullptr};
  usize m_value_count{0};
  usize m_indexed_array_count{0};
  bool m_has_namerefs{false};
  bool m_has_declared_marks{false};
};

struct glob_field
{
  explicit glob_field(Allocator allocator)
      : text(allocator), glob_active(heap_allocator())
  {}

  String text;
  Bitset glob_active;
  bool has_literal_glob{false};
};

enum class glob_expansion_mode : u8
{
  Files,
  Directories,
};

enum class extglob_mode : u8
{
  Disabled,
  Enabled,
};

enum class glob_charset : u8
{
  Bytes,
  Utf8,
};

hot pure fn first_active_glob(StringView text, const Bitset &mask,
                              extglob_mode mode) wontthrow -> Maybe<usize>;

inline pure fn is_colon_modifier_operator(char c) wontthrow -> bool
{
  return c == '-' || c == '+' || c == '=' || c == '?';
}

class Token;
class Word;
class WordSegment;
class Expression;
struct arith_token;

struct conditional_element
{
  enum class Kind : u8
  {
    Operand,
    And,
    Or,
    Not,
    OpenParen,
    CloseParen,
    Less,
    Greater,
  };

  const Token *word{nullptr};
  SourceLocation location{};
  Kind kind;
  bool is_bare_unquoted{false};
};

static_assert(sizeof(usize) != 8 || sizeof(conditional_element) == 24);

pure fn is_runtime_dynamic_variable_name(StringView name) wontthrow -> bool;
pure fn is_bash_only_dynamic_variable_name(StringView name) wontthrow -> bool;
pure fn is_process_dynamic_variable_name(StringView name) wontthrow -> bool;

struct control_flow
{
  enum class Kind : u8
  {
    Normal,
    Break,
    Continue,
    Return,
    Exit,
  };

  i64 value{0};
  const String *source{nullptr};
  String origin{heap_allocator()};
  SourceLocation location{0, 0};
  Kind kind{Kind::Normal};
};

static constexpr u64 EXTERNAL_SOURCE_GENERATION = UINT64_MAX;

enum class source_frame_kind : u8
{
  Ordinary,
  CliRoot,
  SourcedFile,
};

struct trap_definition
{
  String action_text;
  String line_source;
  SourceLocation location;
  isize line_offset{0};
  bool has_location{false};
};

struct trap_action_frame
{
  usize trigger_line_number{0};
  usize source_frame_count{0};
  usize function_depth{0};
  Maybe<i32> saved_exit_status{None};
  u32 depth{0};
  u8 running_conditions{0};

  pure fn get_trigger_line_number(usize current_source_frame_count,
                                  usize current_function_depth) const wontthrow
      -> Maybe<usize>
  {
    if (depth == 0) return None;
    if (current_source_frame_count != source_frame_count) return None;
    if (current_function_depth != function_depth) return None;

    return trigger_line_number;
  }
};

struct startup_options
{
  bool should_disable_path_expansion{false};
  bool should_echo{false};
  bool should_echo_expanded{false};
  bool is_interactive{false};
  bool should_error_exit{false};
};

struct coprocess_descriptors
{
  i32 read_fd{-1};
  i32 write_fd{-1};
  i64 process_id{-1};
  String name{heap_allocator()};

  pure fn has_any() const wontthrow -> bool
  {
    return read_fd >= 0 || write_fd >= 0;
  }
};

struct trap_install_state
{
  usize debug_active_depth{0};
  usize err_active_depth{0};
  bool did_reset_inherited_signal_traps{false};
};

struct embedded_source
{
  StringView text;
  const String *parent;
  SourceLocation parent_location;
  usize inner_offset;
  const String *body{nullptr};
  usize function_call_depth{0};
  usize call_depth_floor{0};
  bool is_mapped{true};
};

struct source_line_base
{
  const String *source;
  usize preceding_line_count;
};

struct source_frame
{
  source_frame(String origin, SourceLocation call_site,
               const String *parent_source, u64 parent_source_generation,
               String source_path, source_frame_kind kind)
      : origin(steal(origin)), source_path(steal(source_path)),
        parent_source(parent_source),
        parent_source_generation(parent_source_generation),
        call_site(steal(call_site)), kind(kind)
  {}

  pure fn has_bash_source_row() const wontthrow -> bool
  {
    return kind == source_frame_kind::SourcedFile;
  }

  String origin;
  String source_path;
  const String *parent_source;
  u64 parent_source_generation;
  const trap_definition *definition{nullptr};
  usize function_call_depth{0};
  SourceLocation call_site;
  Maybe<SourceLocation> deferred_trace_location;
  source_frame_kind kind;
  bool was_printed{false};
  bool was_definition_printed{false};
  bool is_source_changing : 1 {true};
  bool should_defer_trace : 1 {false};
  bool has_deferred_trace : 1 {false};
};

struct local_binding
{
  String name;
  Maybe<String> previous_value;
  Maybe<SourceLocation> previous_special_definition_location;
  Maybe<ArrayList<String>> previous_indexed_array;
  ArrayList<String> previous_associative_keys{heap_allocator()};
  ArrayList<String> previous_associative_values{heap_allocator()};
  ArrayList<usize> previous_sparse_indices{heap_allocator()};
  ArrayList<String> previous_sparse_values{heap_allocator()};
  u8 previous_attributes{0};
  bool previous_was_associative{false};
  bool previous_was_exported{false};
  bool is_self_reference{false};
};

static_assert(sizeof(usize) != 8 || sizeof(local_binding) == 272);

struct job
{
  job() = default;
  explicit job(Allocator allocator)
      : earlier_pipeline_processes(allocator), command(allocator)
  {}

  enum class State : u8
  {
    Running,
    Stopped,
    Done,
  };

  ArrayList<os::process> earlier_pipeline_processes{heap_allocator()};
  String command{heap_allocator()};
  i64 process_id{0};
  i64 process_group_id{0};
  i32 id{0};
  os::process pid{KOSH_INVALID_PROCESS};
  i32 last_status{0};
  i32 stopped_status{0};
  os::process_termination termination{};
  State state{State::Running};
  bool is_primary_process_active{true};
  bool has_unreported_state_change{false};
  bool is_inherited{false};
  bool was_waited{false};
};

struct job_line_format
{
  bool should_show_process_id{false};
  bool is_posix{false};
  StringView state_color{};
  StringView color_reset{};
};

struct finished_process_status
{
  i64 process_id{0};
  i32 status{0};
};

struct next_job_wait
{
  Maybe<i32> job_id{None};
  i64 process_id{0};
  i32 status{127};
  bool was_interrupted{false};
};

struct job_table_snapshot
{
  Maybe<i64> last_background_pid;
  ArrayList<job> jobs;
  ArrayList<os::process> detached_job_processes;
  ArrayList<finished_process_status> finished_statuses;
  usize next_finished_status_slot;
  i32 next_job_id;
};

struct subshell_bootstrap_reader;

struct job_table_wire
{
  Maybe<i64> last_background_pid{None};
  i32 next_job_id{1};
  ArrayList<job> jobs{heap_allocator()};
  ArrayList<os::process> detached_processes{heap_allocator()};
  ArrayList<u32> process_references{heap_allocator()};

  fn bind_processes(const os::subshell_bootstrap &bootstrap) wontthrow -> bool;
};

class JobTable
{
  friend class EvalContext;

public:
  explicit JobTable(Allocator allocator)
      : m_finished_statuses(allocator), m_jobs(allocator),
        m_detached_job_processes(allocator)
  {}

  fn set_last_background_pid(i64 pid) wontthrow -> void;
  fn remember_finished_status(i64 process_id, i32 status) wontthrow -> void;
  pure fn find_finished_status(i64 process_id) const wontthrow -> Maybe<i32>;
  fn forget_finished_statuses() wontthrow -> void;
  fn register_job(os::process pid, StringView command,
                  i64 process_group_id) throws -> i32;
  fn register_pipeline_job(const ArrayList<os::process> &processes,
                           os::process primary_process, StringView command,
                           i64 process_group_id) throws -> i32;
  fn register_stopped_job(os::process pid, StringView command, i32 status,
                          i64 process_group_id) throws -> i32;
  fn notify_stopped_job(i32 id) throws -> void;
  fn append_status_line(String &out, usize index,
                        const job_line_format &format) const throws -> void;
  fn update_jobs() throws -> void;
  fn wait_for_job_processes(job &entry, bool *was_stopped = nullptr,
                            bool should_wait_for_termination = false) throws
      -> i32;
  fn wait_for_next_job(const ArrayList<i32> &job_ids,
                       bool should_wait_for_termination) throws
      -> next_job_wait;
  fn find_job_index_by_spec(StringView spec) throws -> Maybe<usize>;
  fn find_job_by_spec(StringView spec) throws -> job *;
  fn most_recent_job() wontthrow -> job *;
  fn forget_done_jobs() throws -> void;
  fn forget_done_job(i32 id) throws -> void;
  fn mark_job_waited(i32 id, bool should_remember_status) wontthrow -> void;
  fn mark_job_waited(job &entry, bool should_remember_status) wontthrow
      -> void;
  fn forget_waited_jobs() throws -> void
  {
    if (m_has_waited_jobs) forget_marked_waited_jobs();
  }
  fn inherit_parent_jobs(bool should_keep_finished_statuses) wontthrow -> void;
  fn remove_job(i32 id) throws -> bool;
  fn format_done_job_notifications(StringView line_ending, bool is_posix) throws
      -> String;
  fn take_snapshot() throws -> job_table_snapshot;
  fn restore_snapshot(job_table_snapshot snapshot) throws -> void;
  fn append_wire(String &output, os::subshell_bootstrap &bootstrap) const throws
      -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      job_table_wire &wire) throws -> bool;
  fn apply_wire(job_table_wire wire) wontthrow -> void;

  fn last_background_pid() wontthrow -> Maybe<i64> &
  {
    return m_last_background_pid;
  }
  pure fn last_background_pid() const wontthrow -> const Maybe<i64> &
  {
    return m_last_background_pid;
  }
  fn jobs() wontthrow -> ArrayList<job> & { return m_jobs; }
  pure fn jobs() const wontthrow -> const ArrayList<job> & { return m_jobs; }
  fn detached_job_processes() wontthrow -> ArrayList<os::process> &
  {
    return m_detached_job_processes;
  }
  pure fn detached_job_processes() const wontthrow
      -> const ArrayList<os::process> &
  {
    return m_detached_job_processes;
  }
  fn next_job_id() wontthrow -> i32 & { return m_next_job_id; }
  pure fn next_job_id() const wontthrow -> i32 { return m_next_job_id; }

  fn foreground_program_title_buffer() wontthrow -> String &
  {
    return m_foreground_program_title_buffer;
  }
  fn set_in_pipeline_stage(bool in_stage) wontthrow -> void
  {
    m_is_in_pipeline_stage = in_stage;
  }
  pure fn is_in_pipeline_stage() const wontthrow -> bool
  {
    return m_is_in_pipeline_stage;
  }
  fn set_stage_boundary_published(bool published) wontthrow -> void
  {
    m_was_stage_boundary_published = published;
  }
  pure fn was_stage_boundary_published() const wontthrow -> bool
  {
    return m_was_stage_boundary_published;
  }

private:
  static constexpr usize REMEMBERED_FINISHED_STATUS_COUNT = 1024;

  fn claim_job_id() throws -> i32;
  fn forget_marked_waited_jobs() throws -> void;

  Maybe<i64> m_last_background_pid{};
  ArrayList<finished_process_status> m_finished_statuses;
  usize m_next_finished_status_slot{0};
  ArrayList<job> m_jobs;
  ArrayList<os::process> m_detached_job_processes;
  i32 m_next_job_id{1};
  String m_foreground_program_title_buffer{heap_allocator()};
  bool m_is_in_pipeline_stage{false};
  bool m_was_stage_boundary_published{false};
  bool m_has_waited_jobs{false};
};

struct environment_undo_entry
{
  String name;
  Maybe<String> previous_value;
  Maybe<SourceLocation> previous_special_definition_location;
};

struct process_substitution
{
  os::descriptor shell_fd;
  os::process child;
  i64 process_id;
  opaque *platform_cleanup;
  SourceLocation location;
  StringView source;
};

struct process_substitution_mark
{
  usize pending{0};
};

struct loop_redirect_fd
{
  i32 target_fd{-1};
  os::file_open_mode mode{};
  String path;
  os::descriptor fd{};
};

struct loop_redirect_fd_mark
{
  usize count{0};
};

struct subshell_saved_descriptor
{
  usize depth;
  os::saved_descriptor saved;
};

struct function_definition_info
{
  usize body_start_position{0};
  usize header_length{0};
  usize definition_line{0};
  isize line_offset{0};
  usize enclosing_line_count{0};
  u32 source_name_index{0};
  u32 body_name_index{0};
  String line_prefix{heap_allocator()};
  String line_suffix{heap_allocator()};
  mutable String render_source{heap_allocator()};
  mutable bool has_render_source{false};
  bool is_numbered_apart{false};
  definition_state defining_state;
};

struct function_arena_stats
{
  usize bytes_used{0};
  usize bytes_capacity{0};
  usize block_count{0};
  usize destructor_count{0};
  usize destructor_capacity{0};
};

struct function_body_storage
{
  explicit function_body_storage(BumpArena *arena);
  ~function_body_storage();

  BumpArena *arena{nullptr};
  const Expression *body{nullptr};
  String source{heap_allocator()};
  function_definition_info definition_info;
  u32 reference_count{1};
  function_body_storage *previous_live{nullptr};
  function_body_storage *next_live{nullptr};
};

pure fn live_function_storage_stats() wontthrow -> function_arena_stats;

class FunctionBodyHandle
{
public:
  FunctionBodyHandle() = default;
  FunctionBodyHandle(const FunctionBodyHandle &other);
  FunctionBodyHandle(FunctionBodyHandle &&other) noexcept;
  ~FunctionBodyHandle();

  fn operator=(const FunctionBodyHandle &other)->FunctionBodyHandle &;
  fn operator=(FunctionBodyHandle &&other) noexcept -> FunctionBodyHandle &;

  static fn create(usize source_length_hint) throws -> FunctionBodyHandle;

  pure fn has_value() const wontthrow -> bool { return m_storage != nullptr; }
  pure fn get_arena() const wontthrow -> BumpArena *;
  pure fn get_body() const wontthrow -> const Expression *;
  pure fn get_source() const wontthrow -> const String *;
  pure fn get_definition_info() const wontthrow
      -> const function_definition_info *;
  fn set_body(const Expression *body) wontthrow -> void;
  fn set_definition(StringView source,
                    function_definition_info definition_info) const throws
      -> void;

private:
  explicit FunctionBodyHandle(function_body_storage *storage)
      : m_storage(storage)
  {}

  fn retain() wontthrow -> void;
  fn release() wontthrow -> void;

  function_body_storage *m_storage{nullptr};

  friend struct function_body_storage;
};

struct shell_option_mutations
{
  u64 revision{0};
  u64 last_revision[static_cast<usize>(shell_option_id::Count)]{};

  fn note(shell_option_id option) wontthrow -> void
  {
    revision++;
    last_revision[static_cast<usize>(option)] = revision;
  }

  pure fn touched_since(shell_option_id option,
                        u64 prior_revision) const wontthrow -> bool
  {
    return last_revision[static_cast<usize>(option)] > prior_revision;
  }
};

enum class reporting_field : u8
{
  Mood = 1U << 0,
  Warning = 1U << 1,
  Diagnostics = 1U << 2,
  Annoying = 1U << 3,
};

struct reporting_revisions
{
  bool was_mood_set_explicitly{false};
  u64 mood{0};
  u64 warning{0};
  u64 diagnostics{0};
  u64 annoying{0};

  pure fn changed_fields_since(const reporting_revisions &prior) const wontthrow
      -> u8
  {
    u8 fields = 0;
    if (mood != prior.mood) fields |= static_cast<u8>(reporting_field::Mood);
    if (warning != prior.warning)
      fields |= static_cast<u8>(reporting_field::Warning);
    if (diagnostics != prior.diagnostics)
      fields |= static_cast<u8>(reporting_field::Diagnostics);
    if (annoying != prior.annoying)
      fields |= static_cast<u8>(reporting_field::Annoying);
    return fields;
  }

  static constexpr pure fn has_field(u8 fields, reporting_field field) wontthrow
      -> bool
  {
    return (fields & static_cast<u8>(field)) != 0;
  }
};

struct control_mutations
{
  reporting_revisions reporting;
  shell_option_mutations options;
};

enum class definition_state_exit : u8
{
  PropagateMutations,
  RestoreCaller,
};

struct function_runtime_state
{
  RuntimeState previous;
  RuntimeState entered;
  control_mutations entry_mutations;
};

struct saved_frame_trap
{
  Maybe<trap_definition> definition;
  usize active_depth{0};
};

fn record_directory_access(StringView directory, Allocator allocator) throws
    -> void;
fn z_completion_candidates(StringView query, Allocator allocator) throws
    -> ArrayList<String>;

enum class suppressible_warning : u8
{
  UnsetReference,
  UnsetTestOperand,
};

} /* namespace koshka */
