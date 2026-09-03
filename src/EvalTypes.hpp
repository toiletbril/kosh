#pragma once

#include "Arena.hpp"
#include "Bitset.hpp"
#include "Builtin.hpp"
#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ResolvedCommand.hpp"
#include "RuntimeState.hpp"

namespace koshka {

enum class argument_lifetime : u8
{
  Persistent,
  Transient,
};

enum class argument_context : u8
{
  Command,
  ArrayLiteral,
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

enum class return_handling : u8
{
  Propagate,
  Consume,
  Reject,
};

enum class status_flag : u32
{
  ErrResolved = 1U << 0,
};

struct status_result
{
  i32 status{0};
  u32 flags{0};

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

/* A candidate argument after variable expansion and field splitting. The
   parallel mask marks which characters may act as glob metacharacters. */
struct glob_field
{
  explicit glob_field(Allocator allocator)
      : text(allocator), glob_active(heap_allocator())
  {}

  String text;
  Bitset glob_active;
};

/* The index of the first active glob metacharacter in a field, or None when the
   field is all literal. The argument expander reads it to push a glob-free
   field straight through, skipping the directory scan that expand_path would
   run. */
hot pure fn first_active_glob(StringView text, const Bitset &mask,
                              bool extglob) wontthrow -> Maybe<usize>;

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

  Kind kind;
  const Token *word{nullptr};
  bool is_bare_unquoted{false};
  Maybe<SourceLocation> location;
};

pure fn is_runtime_dynamic_variable_name(StringView name) wontthrow -> bool;
pure fn is_bash_only_dynamic_variable_name(StringView name) wontthrow -> bool;
pure fn is_process_dynamic_variable_name(StringView name) wontthrow -> bool;

/* A pending non-local jump the evaluator carries instead of throwing, consumed
   at the matching boundary or left pending for an outer node. */
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

  Kind kind{Kind::Normal};
  i64 value{0};
  SourceLocation location{0, 0};
  const String *source{nullptr};
  String origin{heap_allocator()};
};

struct source_frame
{
  source_frame(String origin, SourceLocation call_site,
               const String *parent_source, String source_path,
               bool is_cli_root, bool is_only_root_source)
      : origin(steal(origin)), call_site(steal(call_site)),
        parent_source(parent_source), source_path(steal(source_path)),
        parent_source_length(parent_source != nullptr ? parent_source->length()
                                                      : 0),
        is_cli_root(is_cli_root), is_only_root_source(is_only_root_source)
  {}

  String origin;
  SourceLocation call_site;
  const String *parent_source;
  String source_path;
  usize parent_source_length;
  bool is_cli_root;
  bool is_only_root_source;
  bool was_printed{false};
  bool should_defer_trace{false};
  bool has_deferred_trace{false};
  Maybe<SourceLocation> deferred_trace_location;
};

/* A variable binding saved when a local shadows it. A None previous value means
   the name was unset, so leaving the scope restores the unset state. */
struct local_binding
{
  String name;
  Maybe<String> previous_value;
  Maybe<ArrayList<String>> previous_indexed_array;
  bool previous_was_associative{false};
  ArrayList<String> previous_associative_keys{heap_allocator()};
  ArrayList<String> previous_associative_values{heap_allocator()};
  ArrayList<usize> previous_sparse_indices{heap_allocator()};
  ArrayList<String> previous_sparse_values{heap_allocator()};
  bool previous_was_integer{false};
  /* The read-only mark, so a local -r marks only this scope and the caller's
     later reassignment is not rejected. */
  bool previous_was_readonly{false};
  bool previous_was_exported{false};
};

struct job
{
  enum class State : u8
  {
    Running,
    Stopped,
    Done,
  };

  i32 id;
  os::process pid;
  ArrayList<os::process> earlier_pipeline_processes{heap_allocator()};
  String command{heap_allocator()};
  State state{State::Running};
  i32 last_status{0};
  i32 stopped_status{0};
  i64 process_id{0};
  i64 process_group_id{0};
  bool is_primary_process_active{true};
  bool has_unreported_state_change{false};
};

struct environment_undo_entry
{
  String name;
  Maybe<String> previous_value;
};

struct process_substitution
{
  os::descriptor shell_fd;
  os::process child;
  SourceLocation location;
  StringView source;
};

struct process_substitution_mark
{
  usize pending{0};
  usize temp{0};
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

/* How a function body's absolute source positions map onto the stored
   definition copy. The copy holds a "name () " header then the body verbatim,
   so an absolute position rebases by the body start and header length. */
struct function_definition_info
{
  usize body_start_position{0};
  usize header_length{0};
  usize line_offset{0};
  u32 source_name_index{0};
  RuntimeState defining_runtime;
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

  static fn create() throws -> FunctionBodyHandle;

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
  pure fn get_stats() const wontthrow -> function_arena_stats;

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

enum class definition_state_exit : u8
{
  PropagateMutations,
  RestoreCaller,
};

struct function_runtime_state
{
  RuntimeState previous;
  RuntimeState entered;
  shell_option_mutations previous_shell_option_mutations;
  u64 shell_option_mutation_revision;
  u64 mood_mutation_revision;
  u64 warning_mutation_revision;
  u64 diagnostics_mutation_revision;
  u64 annoying_diagnostics_mutation_revision;
  bool was_mood_set_explicitly;
};

fn record_directory_access(StringView directory, Allocator allocator) throws
    -> void;

/* A warning the evaluator can silence for the span of a construct.
   UnsetReference exempts an unset name entirely, so neither the warning nor the
   set -u abort fires. UnsetTestOperand silences only the advisory unset warning
   while a test or [ expands its operands, the set -u abort still fires. */
enum class suppressible_warning : u8
{
  UnsetReference,
  UnsetTestOperand,
};

} /* namespace koshka */
