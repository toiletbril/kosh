/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the shell syntax tree, analysis context, symbol records,
 * variable dataflow, and every expression and command node. Parser,
 * optimizer, diagnostics, language server, and evaluator code share this
 * interface.
 */

#pragma once

#include "Diagnostics.hpp"
#include "Eval.hpp"
#include "Formatter.hpp"
#include "Tokens.hpp"
#include "base/Common.hpp"

namespace koshka {

using namespace tokens;

class Token;
struct heredoc_contents;
struct parsed_format_document;

struct pending_analysis_warning
{
  diagnostic_id id;
  SourceLocation location;
  String message;
  String suggestion;
  Maybe<SourceLocation> related_location;
  String related_message;
};

struct source_diagnostic
{
  Maybe<diagnostic_id> id;
  error_severity severity;
  SourceLocation location;
  String source_name;
  String message;
  String suggestion;
  Maybe<SourceLocation> related_location;
  String related_source_name;
  String related_message;
  ArrayList<source_fix> fixes;
};

class AnalysisSourceProvider
{
public:
  virtual ~AnalysisSourceProvider() = default;
  virtual fn read_source(const Path &canonical_path) throws
      -> Maybe<String> = 0;
};

class AnalysisUnitStream
{
public:
  virtual ~AnalysisUnitStream() = default;
  virtual fn next_unit() throws -> const Expression * = 0;
  virtual fn release_unit() throws -> void = 0;
};

namespace expressions {
class IfClause;
class WhileLoop;
class AssignCommand;
class SimpleCommand;
class CompoundList;
class ForLoop;
class CStyleForLoop;
class Subshell;
class RedirectedCommand;
} /* namespace expressions */

struct active_getopts_call
{
  StringView optstring;
  StringView variable_name;
  SourceLocation location;
};

struct function_call_record
{
  String name;
  SourceLocation location;
  bool has_arguments{false};
  bool is_inside_function_body{false};
};

struct command_name_assignment_record
{
  String name;
  String value;
  SourceLocation location;
};

enum class assignment_binder : u8
{
  Assignment,
  ForLoop,
  SelectLoop,
  Arithmetic,
  ReadInput,
  MappedLines,
  ParsedOption,
  FormattedText,
  Declaration,
};

pure fn binder_description(assignment_binder binder) wontthrow -> StringView;

struct diagnostic_assignment_trace
{
  SourceLocation location;
  assignment_binder binder{assignment_binder::Assignment};
};

struct variable_assignment_record
{
  String name;
  Maybe<String> literal_value;
  u32 position{0};
  u32 length{0};
  assignment_binder binder{assignment_binder::Assignment};
  bool is_conditional{false};
  assignment_update_mode update_mode{assignment_update_mode::Replace};
  bool is_array{false};
};

static_assert(sizeof(usize) != 8 || sizeof(variable_assignment_record) == 136);

enum class variable_occurrence_kind : u8
{
  Assignment,
  Reference,
  Unset,
};

struct variable_occurrence_record
{
  String name;
  u32 position{0};
  u32 length{0};
  usize function_definition_index{~usize{0}};
  variable_occurrence_kind kind{variable_occurrence_kind::Reference};
  bool is_unresolved{false};
  bool is_unused{false};
  bool has_resolved_function_path{false};
  bool has_unresolved_function_path{false};
  bool has_inherited_function_path{false};
};

static_assert(sizeof(usize) != 8 || sizeof(variable_occurrence_record) == 80);

struct assignment_index_storage;

class AssignmentIndexSet
{
public:
  AssignmentIndexSet() = default;
  AssignmentIndexSet(const AssignmentIndexSet &other) wontthrow;
  AssignmentIndexSet(AssignmentIndexSet &&other) noexcept;
  ~AssignmentIndexSet();

  fn operator=(const AssignmentIndexSet &other) wontthrow->AssignmentIndexSet &;
  fn operator=(AssignmentIndexSet &&other) noexcept -> AssignmentIndexSet &;

  static fn singleton(usize index) throws -> AssignmentIndexSet;
  fn merge(const AssignmentIndexSet &other) throws -> void;

  pure fn count() const wontthrow -> usize;
  pure fn is_empty() const wontthrow -> bool;
  pure fn operator[](usize index) const wontthrow->usize;
  pure fn begin() const wontthrow -> const usize *;
  pure fn end() const wontthrow -> const usize *;

private:
  explicit AssignmentIndexSet(assignment_index_storage *storage)
      : m_storage(storage)
  {}

  fn retain() wontthrow -> void;
  fn release() wontthrow -> void;

  assignment_index_storage *m_storage{nullptr};
};

struct variable_occurrence_state
{
  AssignmentIndexSet assignment_indices;
  bool is_definitely_set{false};
  bool is_definitely_unset{false};
  bool has_unset_path{false};
  bool has_inherited_path{false};
};

struct variable_occurrence_map_entry
{
  variable_occurrence_state state;
  bool is_present{false};
};

struct variable_occurrence_map_storage;

class VariableOccurrenceStateMap
{
public:
  VariableOccurrenceStateMap() = default;
  VariableOccurrenceStateMap(const VariableOccurrenceStateMap &other);
  VariableOccurrenceStateMap(VariableOccurrenceStateMap &&other) noexcept;
  ~VariableOccurrenceStateMap();

  fn operator=(const VariableOccurrenceStateMap &other) throws
      ->VariableOccurrenceStateMap &;
  fn operator=(VariableOccurrenceStateMap &&other) noexcept
      -> VariableOccurrenceStateMap &;

  fn snapshot() throws -> VariableOccurrenceStateMap;
  pure fn find(StringView name) const wontthrow
      -> const variable_occurrence_state *;
  fn set(StringView name, variable_occurrence_state state) throws -> void;
  fn erase(StringView name) throws -> void;
  fn clear() wontthrow -> void;
  fn merge(const VariableOccurrenceStateMap &other) throws -> void;

private:
  fn get_private_head() throws -> variable_occurrence_map_storage *;
  fn collapse_exclusive_layers() throws -> void;

  variable_occurrence_map_storage *m_head{nullptr};
};

struct variable_occurrence_pair
{
  VariableOccurrenceStateMap assigned;
  VariableOccurrenceStateMap inherited;

  fn snapshot() throws -> variable_occurrence_pair
  {
    return variable_occurrence_pair{assigned.snapshot(), inherited.snapshot()};
  }

  fn merge(const variable_occurrence_pair &other) throws -> void
  {
    assigned.merge(other.assigned);
    inherited.merge(other.inherited);
  }

  pure fn find(StringView name) const wontthrow
      -> const variable_occurrence_state *
  {
    let const *state = assigned.find(name);

    return state != nullptr ? state : inherited.find(name);
  }

  fn replace(StringView name, variable_occurrence_state state) throws -> void
  {
    assigned.set(name, steal(state));
    inherited.erase(name);
  }
};

struct function_global_assignment
{
  String name;
  SourceLocation location;
};

struct function_definition_record
{
  String name;
  usize occurrence_start{0};
  usize occurrence_end{0};
  HashSet affected_names{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
  HashSet local_names{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
  VariableOccurrenceStateMap exit_states;
  String first_positional_read;
  Maybe<usize> previous_definition_index{};
  usize recursive_call_count{0};
  usize async_recursive_call_count{0};
  SourceLocation location;
  SourceLocation first_positional_read_location{};
  SourceLocation first_recursive_call_location{};
  bool has_been_called{false};
  bool is_analysis_complete{false};
};

struct analysis_name_insertion
{
  HashSet *names;
  String name;
};

struct analysis_function_mark
{
  usize definition_count{0};
  usize function_insertion_count{0};
  usize alias_insertion_count{0};
};

struct analysis_function_table
{
  HashSet defined{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
  HashSet aliases{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
  ArrayList<function_definition_record> records{heap_allocator()};
  StringMap<usize> latest_indices{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
  ArrayList<String> function_insertions{heap_allocator()};
  ArrayList<String> alias_insertions{heap_allocator()};

  fn add_function(StringView name) throws -> void
  {
    if (!defined.add(name)) return;

    function_insertions.push(String{name});
  }

  fn add_alias(StringView name) throws -> void
  {
    if (!aliases.add(name)) return;

    alias_insertions.push(String{name});
  }

  pure fn get_mark() const wontthrow -> analysis_function_mark
  {
    return analysis_function_mark{records.count(), function_insertions.count(),
                                  alias_insertions.count()};
  }

  fn rollback(const analysis_function_mark &mark) throws -> void
  {
    for (usize index = records.count(); index > mark.definition_count; index--)
    {
      let const &definition = records[index - 1];
      if (!definition.is_analysis_complete) continue;

      if (definition.previous_definition_index.has_value()) {
        latest_indices.set(definition.name.view(),
                           *definition.previous_definition_index);
      } else {
        latest_indices.erase(definition.name.view());
      }
    }

    while (function_insertions.count() > mark.function_insertion_count) {
      defined.remove(function_insertions.back().view());
      function_insertions.pop_back();
    }

    while (alias_insertions.count() > mark.alias_insertion_count) {
      aliases.remove(alias_insertions.back().view());
      alias_insertions.pop_back();
    }
  }
};

struct function_body_record
{
  String name;
  usize name_position{0};
  usize body_position{0};
  usize body_end_position{0};
};

struct analysis_symbol_records
{
  ArrayList<variable_assignment_record> assignments{heap_allocator()};
  ArrayList<variable_occurrence_record> variable_occurrences{heap_allocator()};
  ArrayList<function_body_record> functions{heap_allocator()};

  fn clear() wontthrow -> void
  {
    assignments.clear();
    variable_occurrences.clear();
    functions.clear();
  }
};

struct analysis_diagnostic_totals
{
  usize warning_count{0};
  usize error_count{0};
};

struct analysis_walk_flags
{
  bool is_direct_pipeline_stage{false};
  bool is_inside_loop_condition{false};
  bool is_command_status_observed{false};
  bool has_input_reading_loop_condition{false};
  bool is_inside_read_loop{false};
  bool is_inside_subshell_analysis{false};
  bool should_retain_tested_command_names{false};
  bool is_analyzing_condition{false};
  bool is_bare_read_substitution{false};
  bool is_inside_substitution_subshell{false};
};

struct analysis_effects
{
  bool has_seen_runtime_definer{false};
  bool has_unknown_path{false};
  bool has_unknown_working_directory{false};
  bool should_silence_unresolved_commands{false};

  fn raise(const analysis_effects &delta, analysis_effects *mirror) wontthrow
      -> void
  {
    has_seen_runtime_definer =
        has_seen_runtime_definer || delta.has_seen_runtime_definer;
    has_unknown_path = has_unknown_path || delta.has_unknown_path;
    has_unknown_working_directory =
        has_unknown_working_directory || delta.has_unknown_working_directory;
    should_silence_unresolved_commands =
        should_silence_unresolved_commands ||
        delta.should_silence_unresolved_commands;

    if (mirror != nullptr) mirror->raise(delta, nullptr);
  }
};

struct followed_source_effects
{
  HashSet defined_functions{heap_allocator()};
  HashSet known_aliases{heap_allocator()};
  HashSet assigned_names{heap_allocator()};
  HashSet global_assigned_names{heap_allocator()};
  HashSet array_valued_names{heap_allocator()};
  analysis_effects effects;
  bool has_fatal{false};
};

inline pure fn reference_names_positional(StringView name) wontthrow -> bool
{
  if (name.is_empty()) return false;

  switch (name[0]) {
  case '@':
  case '*':
  case '#': return name.length == 1;

  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9': return name.is_all_decimal_digits();

  default: return false;
  }
}

struct top_level_sibling_carry
{
  Maybe<SourceLocation> first_directory_change{};
  Maybe<SourceLocation> pending_unchecked_cd{};
  Maybe<SourceLocation> pending_exec_replacement{};
  Maybe<SourceLocation> pending_negated_command{};

  String repeated_append_target{heap_allocator()};
  SourceLocation repeated_append_location{};
  usize repeated_append_count{0};
};

enum class missing_shebang_policy : u8
{
  Suppress,
  Report,
};

struct analysis_options
{
  u8 warning_level{0};
  bool is_default_mood{true};
  bool should_emit_annoying{true};
  bool should_silence_unresolved_commands{false};
  bool should_report_optimizer_diagnostics{false};
  missing_shebang_policy shebang_policy{missing_shebang_policy::Suppress};
  StringView source_base_directory{};

  static fn from_runtime(const RuntimeState &runtime) wontthrow
      -> analysis_options
  {
    analysis_options options{};
    options.warning_level = runtime.get_warning_level();
    options.is_default_mood = runtime.get_mood() == mimic_mood::Default;
    options.should_emit_annoying = runtime.is_annoying_diagnostics_enabled();

    return options;
  }
};

struct analysis_directives
{
  ArrayList<shellcheck_suppression> shellcheck_suppressions{heap_allocator()};
  ArrayList<analysis_scope_definition> scope_definitions{heap_allocator()};
  ArrayList<shellcheck_directive_span> directive_spans{heap_allocator()};
  ArrayList<heredoc_terminator_miss> heredoc_misses{heap_allocator()};
  u32 source_name_index{0};
};

struct analysis_followed_sources
{
  HashSet *paths{nullptr};
  StringMap<followed_source_effects> *effects_cache{nullptr};
};

class AnalysisContext;

struct analysis_report_site
{
  StringView source;
  EvalContext *eval_context;
};

struct analysis_reporter
{
  const analysis_options &options;
  const ArrayList<shellcheck_suppression> *suppressions{nullptr};
  u32 source_name_index{0};
  analysis_diagnostic_totals totals{};
  ArrayList<pending_analysis_warning> pending{heap_allocator()};
  ArrayList<source_diagnostic> *sink{nullptr};
  bool has_fatal{false};

  explicit analysis_reporter(const analysis_options &analysis)
      : options(analysis)
  {}

  fn report(const analysis_report_site &site, diagnostic_id id,
            const SourceLocation &location,
            std::initializer_list<StringView> arguments,
            const Maybe<SourceLocation> &related_location) throws -> bool;
  fn flush(const analysis_report_site &site) throws -> void;
  pure fn should_report(diagnostic_id id) const wontthrow -> bool;
  pure fn should_report(diagnostic_tier tier) const wontthrow -> bool;
  pure fn is_suppressed(const analysis_report_site &site, diagnostic_id id,
                        const SourceLocation &location) const wontthrow -> bool;

private:
  fn warn(diagnostic_id id, const SourceLocation &location, StringView message,
          StringView suggestion, diagnostic_tier tier,
          const Maybe<SourceLocation> &related_location,
          StringView related_message) throws -> void;
  fn fail(const analysis_report_site &site, diagnostic_id id,
          const SourceLocation &location, StringView message,
          StringView suggestion, diagnostic_tier tier,
          const Maybe<SourceLocation> &related_location,
          StringView related_message) throws -> void;
};

struct analysis_parent_link
{
  AnalysisContext *context{nullptr};
  bool should_merge_state{true};
  bool should_merge_uncertainty{true};
  followed_source_effects *source_effects{nullptr};
};

struct analysis_outputs
{
  analysis_diagnostic_totals *deferred_totals{nullptr};
  ArrayList<source_diagnostic> *diagnostic_sink{nullptr};
  analysis_symbol_records *symbol_records{nullptr};
};

class AnalysisContext
{
public:
  StringView source;
  const analysis_options options;
  analysis_reporter reporter;
  bool are_koshkit_utilities_reachable{true};
  analysis_function_table functions;
  StringMap<String> constant_variables{heap_allocator(),
                                       SMALL_MAP_FIRST_CAPACITY};

  usize function_scope_depth{0};

  usize loop_body_depth{0};
  usize conditional_branch_depth{0};

  StringMap<SourceLocation> function_local_names{heap_allocator(),
                                                 SMALL_MAP_FIRST_CAPACITY};

  variable_occurrence_pair occurrences;

  StringMap<SourceLocation> global_assigned_names{heap_allocator()};
  HashSet inherited_global_assigned_names{heap_allocator(),
                                          SMALL_MAP_FIRST_CAPACITY};

  HashSet always_exiting_function_names{heap_allocator()};

  StringMap<SourceLocation> assigned_names_so_far{heap_allocator()};
  HashSet inherited_assigned_names{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};

  StringMap<SourceLocation> reads_before_assignment{heap_allocator()};
  StringMap<diagnostic_assignment_trace> diagnostic_assignment_traces{
      heap_allocator()};
  HashSet readonly_assigned_names{heap_allocator()};

  ArrayList<command_name_assignment_record> command_name_assignments{
      heap_allocator()};
  HashSet command_position_names{heap_allocator()};

  ArrayList<function_call_record> function_calls{heap_allocator()};

  static constexpr usize NO_ACTIVE_FUNCTION_DEFINITION = ~usize{0};
  usize active_function_definition_index{NO_ACTIVE_FUNCTION_DEFINITION};

  analysis_walk_flags walk;
  HashSet pipeline_lost_names{heap_allocator()};
  HashSet external_input_names{heap_allocator()};

  HashSet array_valued_names{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
  ArrayList<analysis_name_insertion> scoped_name_insertions{heap_allocator()};

  StringMap<SourceLocation> quoted_literal_assignments{heap_allocator()};

  StringMap<SourceLocation> active_loop_variables{heap_allocator()};

  active_getopts_call active_getopts{};

  EvalContext *eval_context{nullptr};

  bool is_posix_sh_shebang{false};

  ArrayList<function_global_assignment> function_global_assignments{
      heap_allocator()};
  StringMap<usize> variable_first_scopes{heap_allocator()};
  HashSet shared_scope_variable_names{heap_allocator()};
  HashSet top_level_assigned_names{heap_allocator()};

  analysis_effects effects;
  const parsed_format_document *format_document{nullptr};

  HashSet generated_relative_executable_paths{heap_allocator(),
                                              SMALL_MAP_FIRST_CAPACITY};

  HashSet tested_command_names{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};

  usize optimizer_eliminated_count{0};
  HashSet *followed_source_paths{nullptr};
  StringMap<followed_source_effects> *followed_source_effects_cache{nullptr};
  followed_source_effects *current_source_effects{nullptr};

  top_level_sibling_carry *stream_sibling_carry{nullptr};

  AnalysisSourceProvider *source_provider{nullptr};

  analysis_symbol_records *symbol_records{nullptr};

  BumpArena substitution_arena;
  usize substitution_analysis_depth{0};

  AnalysisContext(StringView source_view, const analysis_options &analysis)
      : source(source_view), options(analysis), reporter(options)
  {
    effects.should_silence_unresolved_commands =
        analysis.should_silence_unresolved_commands;
  }

  AnalysisContext(const AnalysisContext &) = delete;
  AnalysisContext &operator=(const AnalysisContext &) = delete;

  fn add_defined_function(StringView name) throws -> void
  {
    if (current_source_effects != nullptr)
      current_source_effects->defined_functions.add(name);
    functions.add_function(name);
  }

  fn add_known_alias(StringView name) throws -> void
  {
    if (current_source_effects != nullptr)
      current_source_effects->known_aliases.add(name);
    functions.add_alias(name);
  }

  fn add_array_valued_name(StringView name) throws -> void
  {
    add_scoped_name(array_valued_names, name);
    if (current_source_effects != nullptr)
      current_source_effects->array_valued_names.add(name);
  }

  fn add_scoped_name(HashSet &names, StringView name) throws -> void
  {
    if (!names.add(name)) return;

    scoped_name_insertions.push(analysis_name_insertion{&names, String{name}});
  }

  pure fn get_scoped_name_mark() const wontthrow -> usize
  {
    return scoped_name_insertions.count();
  }

  fn rollback_scoped_names(usize mark) throws -> void
  {
    while (scoped_name_insertions.count() > mark) {
      let const &insertion = scoped_name_insertions.back();
      insertion.names->remove(insertion.name.view());
      scoped_name_insertions.pop_back();
    }
  }

  fn add_global_assigned_name(StringView name, SourceLocation location) throws
      -> void
  {
    global_assigned_names.set(name, steal(location));
    if (current_source_effects != nullptr)
      current_source_effects->global_assigned_names.add(name);
  }

  fn mark_path_unknown(bool should_silence_commands) wontthrow -> void
  {
    let delta = analysis_effects{};
    delta.has_unknown_path = true;
    delta.should_silence_unresolved_commands = should_silence_commands;
    raise_effects(delta);
  }

  fn mark_working_directory_unknown() wontthrow -> void
  {
    let delta = analysis_effects{};
    delta.has_unknown_working_directory = true;
    generated_relative_executable_paths =
        HashSet{heap_allocator(), SMALL_MAP_FIRST_CAPACITY};
    raise_effects(delta);
  }

  fn mark_runtime_definer_seen() wontthrow -> void
  {
    let delta = analysis_effects{};
    delta.has_seen_runtime_definer = true;
    raise_effects(delta);
  }

  template <class Definitions>
  fn apply_scope_definitions(const Definitions &definitions) throws -> void
  {
    for (let const &definition : definitions) {
      switch (definition.kind) {
      case analysis_scope_definition_kind::Function:
        add_defined_function(definition.name.view());
        break;
      case analysis_scope_definition_kind::Alias:
        add_known_alias(definition.name.view());
        break;
      }
    }
  }

  fn report_diagnostic(
      diagnostic_id id, const SourceLocation &location,
      std::initializer_list<StringView> arguments = {},
      const Maybe<SourceLocation> &related_location = None) throws -> bool
  {
    return reporter.report(get_report_site(), id, location, arguments,
                           related_location);
  }
  fn flush_warnings() throws -> void { reporter.flush(get_report_site()); }
  fn print_diagnostic_summary() const throws -> void;
  fn print_optimizer_summary() const throws -> void;

  pure fn should_report(diagnostic_id id) const wontthrow -> bool
  {
    return reporter.should_report(id);
  }
  fn note_variable_assignment(StringView name, const SourceLocation &location,
                              bool is_proven_unconditional) throws -> void;

  fn note_variable_assignment_record(StringView name, const Word *value_word,
                                     const SourceLocation &location,
                                     bool is_conditional,
                                     assignment_update_mode update_mode) throws
      -> void;
  fn note_variable_binding_record(StringView name,
                                  const SourceLocation &location,
                                  assignment_binder binder,
                                  bool is_conditional) throws -> void;
  fn note_variable_occurrence(StringView name, const SourceLocation &location,
                              variable_occurrence_kind kind,
                              bool is_unresolved = false,
                              assignment_update_mode update_mode =
                                  assignment_update_mode::Replace) throws
      -> void;
  fn apply_called_function(StringView name,
                           const SourceLocation &call_location) throws -> void;
  fn note_function_body_record(StringView name, usize name_position,
                               usize body_position,
                               usize body_end_position) throws -> void;

  fn note_positional_reference(StringView name,
                               const SourceLocation &location) throws -> void
  {
    if (active_function_definition_index == NO_ACTIVE_FUNCTION_DEFINITION)
      return;
    if (!reference_names_positional(name)) return;

    let &definition = functions.records[active_function_definition_index];
    if (!definition.first_positional_read.is_empty()) return;

    let spelling = location.get_source_text(source).value_or(name);
    definition.first_positional_read = String{spelling};
    definition.first_positional_read_location = location;
  }

  fn note_variable_read(StringView name, const SourceLocation &location,
                        bool is_top_level_unconditional) throws -> void;
  fn note_variable_scope(StringView name) throws -> void;
  pure fn is_posix_mood() const wontthrow -> bool;
  fn trace_optimizer_line(StringView message) const throws -> void;
  pure fn should_silence_unresolved_command_at(usize position) const wontthrow
      -> bool;

private:
  fn raise_effects(const analysis_effects &delta) wontthrow -> void
  {
    effects.raise(delta, current_source_effects != nullptr
                             ? &current_source_effects->effects
                             : nullptr);
  }

  pure fn get_report_site() const wontthrow -> analysis_report_site
  {
    return analysis_report_site{source, eval_context};
  }
};

fn analyze_ast(const Expression *root, StringView source,
               const HashSet &known_functions, const HashSet &known_aliases,
               EvalContext *eval_context, const analysis_options &options,
               const analysis_directives &directives,
               const analysis_followed_sources &followed_sources = {},
               const analysis_parent_link &parent = {},
               const analysis_outputs &outputs = {},
               AnalysisSourceProvider *source_provider = nullptr,
               AnalysisUnitStream *unit_stream = nullptr,
               const parsed_format_document *format_document = nullptr) throws
    -> bool;

mustuse pure fn is_source_location_variable(StringView name) wontthrow -> bool;

fn print_analysis_diagnostic_summary(
    const analysis_diagnostic_totals &totals) throws -> void;

class Expression
{
public:
  Expression() = delete;
  Expression(SourceLocation location);

  virtual ~Expression() = default;

  pure fn source_location() const wontthrow -> SourceLocation;
  pure fn source_end_position() const wontthrow -> usize;
  fn set_source_end_position(usize position) wontthrow -> void;
  virtual fn error_report_location() const wontthrow -> SourceLocation;
  fn evaluate_root_status(EvalContext &cxt,
                          root_evaluation_mode mode) const throws
      -> status_result;
  fn evaluate_status(EvalContext &cxt) const throws -> status_result
  {
    return evaluate_root_status(cxt, root_evaluation_mode::Normal);
  }
  fn evaluate_root(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> i64
  {
    return evaluate_root_status(cxt, mode).status;
  }
  fn evaluate(EvalContext &cxt) const throws -> i64
  {
    return evaluate_root_status(cxt, root_evaluation_mode::Normal).status;
  }

  Expression(const Expression &) = delete;
  Expression(Expression &&) noexcept = delete;
  Expression &operator=(const Expression &) = delete;
  Expression &operator=(Expression &&) noexcept = delete;

  virtual fn to_string() const throws -> String = 0;
  virtual fn to_ast_string(usize layer = 0) const throws -> String;

  virtual fn is_simple_command() const wontthrow -> bool;
  virtual fn is_compound_command() const wontthrow -> bool;
  virtual fn is_dummy() const wontthrow -> bool;

  virtual fn as_if_clause() const wontthrow -> const expressions::IfClause *;
  virtual fn as_while_loop() const wontthrow -> const expressions::WhileLoop *;
  virtual fn as_assign_command() const wontthrow
      -> const expressions::AssignCommand *;
  virtual fn as_simple_command() const wontthrow
      -> const expressions::SimpleCommand *;
  virtual fn as_compound_list() const wontthrow
      -> const expressions::CompoundList *;
  virtual fn as_for_loop() const wontthrow -> const expressions::ForLoop *;
  virtual fn as_cstyle_for_loop() const wontthrow
      -> const expressions::CStyleForLoop *;
  virtual fn as_subshell() const wontthrow -> const expressions::Subshell *;
  virtual fn as_redirected_command() const wontthrow
      -> const expressions::RedirectedCommand *;

  virtual fn always_exits(const AnalysisContext &actx) const wontthrow -> bool;

  static fn operator delete(opaque *pointer) wontthrow->void;

  virtual fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void;

  virtual fn append_presence_tested_command_names(
      const AnalysisContext &actx, HashSet &names,
      bool status_is_success) const throws -> void;

  virtual fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws -> bool;

  virtual fn
  try_static_condition_verdict(const AnalysisContext &actx) const wontthrow
      -> Maybe<bool>;

protected:
  virtual fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const
      throws -> status_result = 0;

  SourceLocation m_location;
  u32 m_source_end_position;
};

namespace expressions {

class IfStatement : public Expression
{
public:
  IfStatement(SourceLocation location, const Expression *condition,
              const Expression *then, const Expression *otherwise);

  ~IfStatement() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  const Expression *m_condition;
  const Expression *m_then;
  const Expression *m_otherwise;
};

class DummyExpression : public Expression
{
public:
  DummyExpression(SourceLocation location);

  fn is_dummy() const wontthrow -> bool override;

  fn to_string() const throws -> String override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;
};

class PrefixAssignment
{
public:
  const Assignment *token;

  pure fn get_name() const wontthrow -> StringView
  {
    return token->key().view();
  }
  pure fn get_value() const wontthrow -> const Word &
  {
    return token->value_word();
  }
  pure fn get_location() const wontthrow -> SourceLocation
  {
    return token->source_location();
  }
  pure fn get_update_mode() const wontthrow -> assignment_update_mode
  {
    return token->get_update_mode();
  }
};

struct array_builtin_assignment
{
  String name;
  ArrayList<const Token *> elements;
  SourceLocation location;
  u32 end_position;
  assignment_update_mode update_mode;
};

enum class assignment_builtin : u8
{
  None,
  Local,
  Declare,
  Readonly,
  Export,
};

pure fn classify_assignment_builtin(StringView name) wontthrow
    -> assignment_builtin;

enum class time_format_mode : u8
{
  Default,
  Posix,
};

enum class time_rss_mode : u8
{
  Omit,
  Include,
};

class Command : public Expression
{
public:
  Command(SourceLocation location);

  fn make_async() wontthrow -> void;
  pure fn is_async() const wontthrow -> bool;
  fn set_local_vars(ArrayList<PrefixAssignment> &&vars) throws -> void;

  pure fn local_vars() const wontthrow -> const SparseList<PrefixAssignment> &;

  fn set_negated() wontthrow -> void;
  pure fn is_negated() const wontthrow -> bool;

  fn set_timed(SourceLocation location, time_format_mode format,
               time_rss_mode rss) wontthrow -> void;
  pure fn is_timed() const wontthrow -> bool;
  pure fn get_time_format_mode() const wontthrow -> time_format_mode;
  pure fn get_time_rss_mode() const wontthrow -> time_rss_mode;
  pure fn time_location() const wontthrow -> SourceLocation;

  virtual fn is_assignment() const wontthrow -> bool;

  fn evaluate_async(EvalContext &cxt) const throws -> i64;

protected:
  using async_body = i64 (*)(void *context, EvalContext &cxt);

  virtual fn evaluate_async_body(EvalContext &cxt) const throws -> i64;
  fn evaluate_async_with(
      EvalContext &cxt, async_body body, void *context,
      StringView expanded_child_source = StringView{}) const throws -> i64;

  fn append_ast_execution_flags(String &label) const throws -> void;

  enum class ExecutionFlag : u8
  {
    Async = 1U << 0,
    Negated = 1U << 1,
    Timed = 1U << 2,
    TimePosixFormat = 1U << 3,
    TimeReportRss = 1U << 4,
    FullyEliminated = 1U << 5,
    UntilLoop = 1U << 6,
    FoldedLoopToSkip = 1U << 7,
  };

  pure fn has_execution_flag(ExecutionFlag flag) const wontthrow -> bool
  {
    return (m_execution_flags & static_cast<u8>(flag)) != 0;
  }
  fn set_execution_flag(ExecutionFlag flag, bool enabled = true) const wontthrow
      -> void
  {
    if (enabled)
      m_execution_flags |= static_cast<u8>(flag);
    else
      m_execution_flags &= static_cast<u8>(~static_cast<u8>(flag));
  }

  mutable u8 m_execution_flags{0};
  u32 m_time_position{0};
  SparseList<PrefixAssignment> m_local_vars{};
};

class AssignCommand : public Command
{
public:
  AssignCommand(SourceLocation location, const Assignment *a);
  ~AssignCommand() override;

  pure fn assignment() const wontthrow -> const Assignment *;

  fn is_assignment() const wontthrow -> bool override;

  fn to_string() const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn as_assign_command() const wontthrow -> const AssignCommand * override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;
  fn evaluate_assignment(EvalContext &cxt) const throws -> i64;

  const Assignment *m_assignment;
  mutable Maybe<String> m_published_command_text{};
  mutable bool m_was_published_text_rendered_with_bash_additions{false};
};

class Redirection
{
public:
  enum class Kind : u8
  {
    TruncateOutput,
    TruncateOutputOverride,
    AppendOutput,
    ReadInput,
    ReadWrite,
    DuplicateOutput,
    DuplicateInput,
    Heredoc,
    HereString
  };

  static constexpr i32 DUP_FD_CLOSE = -2;

  const Token *target;
  const heredoc_contents *heredoc;
  const Token *fd_allocation_name_token;
  const Token *heredoc_delimiter;
  i32 fd;
  i32 dup_fd;
  Kind kind;
  bool should_expand_heredoc;
  bool should_strip_heredoc_tabs;
  bool is_dup_filename_allowed;
  bool is_both_streams_spelling;

  pure fn opens_output_file() const wontthrow -> bool
  {
    switch (kind) {
    case Kind::TruncateOutput:
    case Kind::TruncateOutputOverride:
    case Kind::AppendOutput: return true;

    default: return false;
    }
  }

  pure fn opens_input_source() const wontthrow -> bool
  {
    switch (kind) {
    case Kind::ReadInput:
    case Kind::ReadWrite:
    case Kind::Heredoc:
    case Kind::HereString: return true;

    default: return false;
    }
  }

  pure fn claims_descriptor() const wontthrow -> bool
  {
    return opens_output_file() || opens_input_source();
  }
};

class SimpleCommand : public Command
{
public:
  SimpleCommand(SourceLocation location, ArrayList<const Token *> &&args);
  ~SimpleCommand() override;

  fn set_redirections(ArrayList<Redirection> &&redirections) throws -> void;

  fn append_redirection(const Redirection &redirection,
                        Allocator allocator) throws -> void;

  fn set_array_args(ArrayList<array_builtin_assignment> &&array_args) throws
      -> void;

  fn set_full_source_end_position(usize position) wontthrow -> void;

  pure fn full_source_start_position() const wontthrow -> usize;
  pure fn full_source_end_position() const wontthrow -> usize;
  pure fn assignments_source_end_position() const wontthrow -> usize;

  fn redirect_exec_context(ExecContext &ec, EvalContext &cxt) const throws
      -> void;

  fn is_simple_command() const wontthrow -> bool override;

  pure fn args() const wontthrow -> const ArrayList<const Token *> &;
  pure fn redirections() const wontthrow -> const SparseList<Redirection> &;

  fn get_published_command_text(EvalContext &cxt) const throws -> StringView;

  fn to_string() const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn append_presence_tested_command_names(const AnalysisContext &actx,
                                          HashSet &names,
                                          bool status_is_success) const throws
      -> void override;

  fn try_static_condition_verdict(const AnalysisContext &actx) const wontthrow
      -> Maybe<bool> override;

  fn as_simple_command() const wontthrow -> const SimpleCommand * override;

  fn always_exits(const AnalysisContext &actx) const wontthrow -> bool override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  ArrayList<const Token *> m_args{heap_allocator()};

  mutable Maybe<bool> m_command_word_is_glob{};

  struct literal_command_lookup
  {
    Maybe<Builtin::Kind> builtin{};
    bool is_special{false};
  };
  mutable Maybe<literal_command_lookup> m_literal_command_lookup{};
  mutable Maybe<String> m_published_command_text{};
  mutable bool m_was_published_text_rendered_with_bash_additions{false};

  fn get_literal_command_lookup(const ArrayList<String> &program_args)
      const throws -> const literal_command_lookup *;

  u32 m_full_source_end_position{0};

  SparseList<Redirection> m_redirections{};
  SparseList<array_builtin_assignment> m_array_args{};
};

class CompoundListCondition : public Expression
{
public:
  enum class Kind : u8
  {
    None,
    And,
    Or,
  };

  CompoundListCondition(SourceLocation location, Kind kind,
                        const Command *expr);
  ~CompoundListCondition() override;

  pure fn kind() const wontthrow -> Kind;
  pure fn command() const wontthrow -> const Command *;

  pure fn is_negated() const wontthrow -> bool;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn append_presence_tested_command_names(const AnalysisContext &actx,
                                          HashSet &names,
                                          bool status_is_success) const throws
      -> void override;
  fn try_static_condition_verdict(const AnalysisContext &actx) const wontthrow
      -> Maybe<bool> override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  Kind m_kind;
  const Command *m_cmd;
};

class CompoundList : public Expression
{
public:
  CompoundList();

  ~CompoundList() override;

  pure fn is_empty() const wontthrow -> bool;
  fn has_single_test_command() const throws -> bool;
  fn append_node(const CompoundListCondition *node) throws -> void;
  pure fn node_count() const wontthrow -> usize;
  fn move_nodes_from(usize first_index, CompoundList &destination) throws
      -> void;

  fn single_unconditional_command() const wontthrow -> const Command *;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  fn append_presence_tested_command_names(const AnalysisContext &actx,
                                          HashSet &names,
                                          bool status_is_success) const throws
      -> void override;
  fn try_static_condition_verdict(const AnalysisContext &actx) const wontthrow
      -> Maybe<bool> override;
  fn as_compound_list() const wontthrow -> const CompoundList * override;
  fn always_exits(const AnalysisContext &actx) const wontthrow -> bool override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  ArrayList<const CompoundListCondition *> m_nodes{heap_allocator()};
};

class Pipeline : public Command
{
public:
  Pipeline(SourceLocation location);

  ~Pipeline() override;

  pure fn is_empty() const wontthrow -> bool;
  fn append_command(const Command *node) throws -> void;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn append_presence_tested_command_names(const AnalysisContext &actx,
                                          HashSet &names,
                                          bool status_is_success) const throws
      -> void override;

  fn as_simple_command() const wontthrow -> const SimpleCommand * override;
  fn error_report_location() const wontthrow -> SourceLocation override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  fn evaluate_with_compound_stages(EvalContext &cxt) const throws -> i64;

  ArrayList<const Command *> m_commands{heap_allocator()};

  mutable Maybe<bool> m_has_compound_stage{};
  mutable bool m_has_assignment_only_stage{false};
};

class CompoundCommand : public Command
{
public:
  CompoundCommand(SourceLocation location);

  fn is_compound_command() const wontthrow -> bool override;

  fn set_fully_eliminated() const wontthrow -> void;
  pure fn is_fully_eliminated() const wontthrow -> bool;
};

struct if_branch
{
  const Expression *condition;
  const Expression *body;
};

class IfClause : public CompoundCommand
{
public:
  IfClause(SourceLocation location, ArrayList<if_branch> &&branches,
           const Expression *otherwise);
  ~IfClause() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  pure fn branches() const wontthrow -> const ArrayList<if_branch> &;
  pure fn otherwise() const wontthrow -> const Expression *;

  fn set_folded_branch(usize index) const wontthrow -> void;
  pure fn has_folded_branch() const wontthrow -> bool;
  pure fn folded_branch_index() const wontthrow -> usize;

  fn as_if_clause() const wontthrow -> const IfClause * override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  ArrayList<if_branch> m_branches{heap_allocator()};
  const Expression *m_otherwise;

  mutable Maybe<usize> m_folded_branch{};
};

enum class loop_kind : u8
{
  While,
  Until,
};

class WhileLoop : public CompoundCommand
{
public:
  WhileLoop(SourceLocation location, const Expression *condition,
            const Expression *body, loop_kind kind);
  ~WhileLoop() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  pure fn condition() const wontthrow -> const Expression *;
  pure fn is_until() const wontthrow -> bool;

  fn set_folded_to_skip() const wontthrow -> void;
  pure fn is_folded_to_skip() const wontthrow -> bool;

  fn as_while_loop() const wontthrow -> const WhileLoop * override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  const Expression *m_condition;
  const Expression *m_body;
};

class ForLoop : public CompoundCommand
{
public:
  ForLoop(SourceLocation location, SourceLocation variable_location,
          StringView variable_name, ArrayList<const Token *> &&words,
          bool has_in_clause, const Expression *body);
  ~ForLoop() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  fn as_for_loop() const wontthrow -> const ForLoop * override;

  pure fn has_in_clause() const wontthrow -> bool;
  pure fn words() const wontthrow -> const ArrayList<const Token *> &;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  StringView m_variable_name;
  ArrayList<const Token *> m_words{heap_allocator()};
  const Expression *m_body;
  SourceLocation m_variable_location;
  bool m_has_in_clause;
};

enum class case_terminator : u8
{
  Break,
  FallThrough,
  ContinueMatch,
};

struct case_item
{
  ArrayList<const Token *> patterns;
  const Expression *body;
  case_terminator terminator;
};

class CaseClause : public CompoundCommand
{
public:
  CaseClause(SourceLocation location, const Token *word,
             ArrayList<case_item> &&items);
  ~CaseClause() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  const Token *m_word;
  ArrayList<case_item> m_items{heap_allocator()};
};

class BraceGroup : public CompoundCommand
{
public:
  BraceGroup(SourceLocation location, const Expression *body);
  ~BraceGroup() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  fn always_exits(const AnalysisContext &actx) const wontthrow -> bool override;
  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  const Expression *m_body;
};

class CoprocCommand : public CompoundCommand
{
public:
  CoprocCommand(SourceLocation location, StringView name,
                const Expression *body);
  ~CoprocCommand() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  StringView m_name;
  const Expression *m_body;
};

class Subshell : public CompoundCommand
{
public:
  Subshell(SourceLocation location, const Expression *body);
  ~Subshell() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  fn as_subshell() const wontthrow -> const Subshell * override;
  fn error_report_location() const wontthrow -> SourceLocation override;

  fn set_analysis_scope_definitions(
      ArrayList<analysis_scope_definition> definitions) throws -> void
  {
    m_analysis_scope_definitions.fill(steal(definitions));
  }

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  fn collapsed_body() const wontthrow -> const Expression *;

  const Expression *m_body;
  SparseList<analysis_scope_definition> m_analysis_scope_definitions{};
};

class ConditionalCommand : public CompoundCommand
{
public:
  ConditionalCommand(SourceLocation location,
                     ArrayList<conditional_element> elements);
  ~ConditionalCommand() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  ArrayList<conditional_element> m_elements;
};

class ArithmeticCommand : public CompoundCommand
{
public:
  ArithmeticCommand(SourceLocation location, StringView expression);
  ~ArithmeticCommand() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  StringView m_expression;
};

class CStyleForLoop : public CompoundCommand
{
public:
  CStyleForLoop(SourceLocation location, usize header_position, StringView init,
                StringView condition, StringView step, const Expression *body);
  ~CStyleForLoop() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;
  pure fn condition_clause() const wontthrow -> StringView;

  pure fn init_clause() const wontthrow -> StringView;

  fn set_folded_condition(i64 compatibility_value,
                          bool is_exact_nonzero) const wontthrow -> void;
  pure fn has_folded_condition() const wontthrow -> bool;

  fn as_cstyle_for_loop() const wontthrow -> const CStyleForLoop * override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  usize m_header_position;
  StringView m_init;
  StringView m_condition;
  StringView m_step;
  const Expression *m_body;

  mutable Maybe<i64> m_folded_condition{};
  mutable bool m_is_exact_folded_condition_nonzero{false};

  mutable arith_token_cache *m_condition_cache{nullptr};
  mutable arith_token_cache *m_step_cache{nullptr};

  static fn get_clause_cache(arith_token_cache *&slot) throws
      -> arith_token_cache &;
};

class SelectLoop : public CompoundCommand
{
public:
  SelectLoop(SourceLocation location, SourceLocation variable_location,
             StringView variable_name, ArrayList<const Token *> &&words,
             bool has_in_clause, const Expression *body);
  ~SelectLoop() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  StringView m_variable_name;
  ArrayList<const Token *> m_words{heap_allocator()};
  const Expression *m_body;
  SourceLocation m_variable_location;
  bool m_has_in_clause;
};

class RedirectedCommand : public Command
{
public:
  RedirectedCommand(SourceLocation location, const Command *child,
                    ArrayList<Redirection> &&redirections);
  ~RedirectedCommand() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn as_redirected_command() const wontthrow
      -> const RedirectedCommand * override;

  pure fn child() const wontthrow -> const Command *;

  pure fn redirections() const wontthrow -> const SparseList<Redirection> &;

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;
  fn evaluate_async_body(EvalContext &cxt) const throws -> i64 override;

  fn evaluate_redirected(EvalContext &cxt) const throws -> status_result;

  const Command *m_child;
  SparseList<Redirection> m_redirections{};
};

class FunctionDefinition : public CompoundCommand
{
public:
  FunctionDefinition(SourceLocation location, StringView name,
                     FunctionBodyHandle body);
  ~FunctionDefinition() override;

  pure fn name() const wontthrow -> const String &;
  pure fn body() const wontthrow -> const Expression *;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn set_analysis_scope_definitions(
      ArrayList<analysis_scope_definition> definitions) throws -> void
  {
    m_analysis_scope_definitions.fill(steal(definitions));
  }

protected:
  fn evaluate_impl(EvalContext &cxt, root_evaluation_mode mode) const throws
      -> status_result override;

  String m_name;
  FunctionBodyHandle m_body_storage;
  const Expression *m_body;
  SparseList<analysis_scope_definition> m_analysis_scope_definitions{};
};

} /* namespace expressions */

} /* namespace koshka */
