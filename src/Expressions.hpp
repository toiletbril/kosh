#pragma once

#include "Common.hpp"
#include "Diagnostics.hpp"
#include "Eval.hpp"
#include "Formatter.hpp"
#include "Tokens.hpp"

namespace koshka {

using namespace tokens;

class Token;
struct heredoc_contents;

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

/* A source handed to the analysis stage one top-level command at a time. The
   stream owns the parser and the arena mark, and the analysis stage releases
   each unit before it asks for the next, so a large script costs the memory of
   its widest command and not the memory of its whole syntax tree. */
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
class ForLoop;
class CStyleForLoop;
class Subshell;
} /* namespace expressions */

/* The getopts call whose result the enclosing loop body reads. The views point
   into the syntax tree, which outlives the analysis. */
struct active_getopts_call
{
  StringView optstring;
  StringView variable_name;
  SourceLocation location;
};

/* One call of a name this script defines as a function. The name is owned for
   the same reason the definition name is. */
struct function_call_record
{
  String name;
  SourceLocation location;
  bool has_arguments{false};
  bool is_inside_function_body{false};
};

/* One assignment whose value is a bare command name. The value points into a
   composed string that dies with the command, so both fields are owned. */
struct command_name_assignment_record
{
  String name;
  String value;
  SourceLocation location;
};

/* What put a value in the name, so a reader who asks about a name a command
   binds is told where the value comes from. */
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

/* One assignment a reader may ask about. The name and the folded value are
   owned, and the span is a plain byte range, since the language server releases
   the analysis arena before it answers. */
struct variable_assignment_record
{
  String name;
  Maybe<String> literal_value;
  bool is_conditional{false};
  bool is_append{false};
  bool is_array{false};
  usize position{0};
  usize length{0};
  assignment_binder binder{assignment_binder::Assignment};
};

enum class variable_occurrence_kind : u8
{
  Assignment,
  Reference,
  Unset,
};

struct variable_occurrence_record
{
  String name;
  usize position{0};
  usize length{0};
  variable_occurrence_kind kind{variable_occurrence_kind::Reference};
  bool is_unresolved{false};
  bool is_unused{false};
  usize function_definition_index{~usize{0}};
  bool has_resolved_function_path{false};
  bool has_unresolved_function_path{false};
  bool has_inherited_function_path{false};
};

struct variable_occurrence_state
{
  ArrayList<usize> assignment_indices{heap_allocator()};
  bool is_definitely_set{false};
  bool is_definitely_unset{false};
  bool has_unset_path{false};
  bool has_inherited_path{false};
};

/* One function this script defines. The whole-script sweep reads these after
   the walk, so the name is owned and never a slice of the syntax tree. */
struct function_definition_record
{
  String name;
  SourceLocation location;
  usize occurrence_start{0};
  usize occurrence_end{0};
  HashSet affected_names{heap_allocator()};
  HashSet local_names{heap_allocator()};
  StringMap<variable_occurrence_state> exit_states{heap_allocator()};
  bool has_positional_reads{false};
  bool has_been_called{false};
  bool is_analysis_complete{false};
};

/* One function definition a reader may ask about. The body span is recovered
   from the document source, in the shape declare -f prints. */
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

struct followed_source_effects
{
  HashSet defined_functions{heap_allocator()};
  HashSet known_aliases{heap_allocator()};
  HashSet assigned_names{heap_allocator()};
  HashSet global_assigned_names{heap_allocator()};
  HashSet array_valued_names{heap_allocator()};
  bool has_seen_runtime_definer{false};
  bool has_unknown_path{false};
  bool has_unknown_working_directory{false};
  bool should_silence_unresolved_commands{false};
  bool has_fatal{false};
};

/* Whether the name reads a positional parameter, so $1 through $9, $@, $*, or
   $#. A name carrying a modifier such as ${1:-default} supplies its own value
   and is left out. */
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

/* The sibling checks of a command list read the node that follows, and a
   streamed run holds one top-level command at a time. The carry keeps what
   those checks need from the unit before, so a finding that spans two
   top-level commands is still reported. Every field is owned, so a rewind of
   the syntax tree leaves it readable. */
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

class AnalysisContext
{
public:
  StringView source;
  bool has_fatal{false};
  usize reported_warning_count{0};
  usize reported_error_count{0};
  u8 warning_level{0};
  bool is_default_mood{true};
  bool are_koshkit_utilities_reachable{true};
  bool should_emit_annoying_diagnostics{true};
  const ArrayList<shellcheck_suppression> *shellcheck_suppressions{nullptr};
  bool has_seen_runtime_definer{false};
  HashSet defined_functions{heap_allocator()};
  HashSet known_aliases{heap_allocator()};
  ArrayList<String> defined_function_insertions{heap_allocator()};
  ArrayList<String> known_alias_insertions{heap_allocator()};
  /* The table is cleared at a conditional branch, a loop body, a function body,
     a subshell, and on any runtime definer, since a value recorded before such
     a boundary is no longer proven to hold past it. */
  StringMap<String> constant_variables{heap_allocator()};

  usize function_scope_depth{0};

  /* Saved and zeroed on entry to a function body, since a break inside the body
     cannot leave a loop that only surrounds the call. */
  usize loop_body_depth{0};

  /* Saved and cleared on entry to a function body and restored on exit. Each
     name carries the assignment that recorded it, read by the diagnostic that
     names a near miss. */
  StringMap<SourceLocation> function_local_names{heap_allocator()};

  StringMap<variable_occurrence_state> variable_occurrence_assignments{
      heap_allocator()};
  StringMap<variable_occurrence_state>
      inherited_variable_occurrence_assignments{heap_allocator()};

  /* An assignment inside a function to one of these updates an existing global
     rather than leaking a new binding, so the no-local warning stays quiet. */
  StringMap<SourceLocation> global_assigned_names{heap_allocator()};
  HashSet inherited_global_assigned_names{heap_allocator()};

  StringMap<SourceLocation> assigned_names_so_far{heap_allocator()};
  HashSet inherited_assigned_names{heap_allocator()};

  StringMap<SourceLocation> reads_before_assignment{heap_allocator()};

  /* An assignment holding a bare command name is only wrong when nothing runs
     that name, and the run may follow the assignment, so the finding waits for
     the end of the walk. */
  ArrayList<command_name_assignment_record> command_name_assignments{
      heap_allocator()};
  HashSet command_position_names{heap_allocator()};

  /* Every function the script defines and every call of one, gathered during
     the walk and swept once it ends. */
  ArrayList<function_definition_record> function_definitions{heap_allocator()};
  StringMap<usize> latest_function_definition_indices{heap_allocator()};
  ArrayList<function_call_record> function_calls{heap_allocator()};

  static constexpr usize NO_ACTIVE_FUNCTION_DEFINITION = ~usize{0};
  usize active_function_definition_index{NO_ACTIVE_FUNCTION_DEFINITION};

  bool is_direct_pipeline_stage{false};
  bool is_inside_loop_condition{false};
  bool is_command_status_observed{false};
  bool has_input_reading_loop_condition{false};
  bool is_inside_read_loop{false};
  HashSet pipeline_lost_names{heap_allocator()};
  HashSet external_input_names{heap_allocator()};

  /* A name proven to hold an array, so a bare expansion of it reads one element
     and a scalar assignment to it drops the rest. */
  HashSet array_valued_names{heap_allocator()};

  /* Where a name whose literal value carries quote bytes was assigned, read
     when that name is expanded as a command word. */
  StringMap<SourceLocation> quoted_literal_assignments{heap_allocator()};

  StringMap<SourceLocation> active_loop_variables{heap_allocator()};

  /* Saved before a while condition and restored after its body, so a case in
     the body sees the getopts call that fills its word. */
  active_getopts_call active_getopts{};

  /* The lookup is lazy, and null in a context with no live shell. */
  EvalContext *eval_context{nullptr};

  /* The SC3xxx bashism lints fire only behind this gate, since a kosh or bash
     shebang means the bash extension on purpose. */
  bool is_posix_sh_shebang{false};

  /* An interactive -W chunk runs the moment the analysis ends and the runtime
     resolution reports the same missing command, so the analysis copy would
     double the report. A script run keeps the check. */
  bool should_silence_unresolved_commands{false};
  bool has_unknown_path{false};
  bool has_unknown_working_directory{false};
  bool is_inside_subshell_analysis{false};

  HashSet tested_command_names{heap_allocator()};
  bool should_retain_tested_command_names{false};
  bool is_analyzing_condition{false};

  bool should_report_optimizer_diagnostics{false};
  usize optimizer_eliminated_count{0};
  HashSet *followed_source_paths{nullptr};
  StringMap<followed_source_effects> *followed_source_effects_cache{nullptr};
  followed_source_effects *current_source_effects{nullptr};

  /* Armed for one streamed top-level command, and the list that reads it
     takes it so a nested list keeps its own sibling state. */
  top_level_sibling_carry *stream_sibling_carry{nullptr};

  ArrayList<pending_analysis_warning> pending_warnings{heap_allocator()};
  ArrayList<source_diagnostic> *diagnostic_sink{nullptr};
  AnalysisSourceProvider *source_provider{nullptr};

  /* Null outside the language server. A record costs an owned name and a folded
     value, so an ordinary run pays one null test per assignment. */
  analysis_symbol_records *symbol_records{nullptr};

  explicit AnalysisContext(StringView source_view) : source(source_view) {}

  fn add_defined_function(StringView name) throws -> void
  {
    if (current_source_effects != nullptr)
      current_source_effects->defined_functions.add(name);
    if (!defined_functions.add(name)) return;
    defined_function_insertions.push(String{name});
  }

  fn add_known_alias(StringView name) throws -> void
  {
    if (current_source_effects != nullptr)
      current_source_effects->known_aliases.add(name);
    if (!known_aliases.add(name)) return;
    known_alias_insertions.push(String{name});
  }

  fn add_array_valued_name(StringView name) throws -> void
  {
    array_valued_names.add(name);
    if (current_source_effects != nullptr)
      current_source_effects->array_valued_names.add(name);
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
    has_unknown_path = true;
    should_silence_unresolved_commands =
        should_silence_unresolved_commands || should_silence_commands;
    if (current_source_effects != nullptr) {
      current_source_effects->has_unknown_path = true;
      current_source_effects->should_silence_unresolved_commands =
          current_source_effects->should_silence_unresolved_commands ||
          should_silence_commands;
    }
  }

  fn mark_working_directory_unknown() wontthrow -> void
  {
    has_unknown_working_directory = true;
    if (current_source_effects != nullptr)
      current_source_effects->has_unknown_working_directory = true;
  }

  fn mark_runtime_definer_seen() wontthrow -> void
  {
    has_seen_runtime_definer = true;
    if (current_source_effects != nullptr)
      current_source_effects->has_seen_runtime_definer = true;
  }

  template <class Definitions>
  fn apply_scope_definitions(const Definitions &definitions) throws -> void
  {
    for (let const &definition : definitions) {
      if (definition.is_alias) {
        add_known_alias(definition.name.view());
      } else {
        add_defined_function(definition.name.view());
      }
    }
  }

  fn rollback_defined_functions(usize insertion_count) throws -> void
  {
    while (defined_function_insertions.count() > insertion_count) {
      defined_functions.remove(defined_function_insertions.back().view());
      defined_function_insertions.pop_back();
    }
  }

  fn rollback_known_aliases(usize insertion_count) throws -> void
  {
    while (known_alias_insertions.count() > insertion_count) {
      known_aliases.remove(known_alias_insertions.back().view());
      known_alias_insertions.pop_back();
    }
  }

  /* The result is true when the message was delivered. A suppressed code must
     not suppress a later check. */
  fn report_diagnostic(
      diagnostic_id id, const SourceLocation &location,
      std::initializer_list<StringView> arguments = {},
      const Maybe<SourceLocation> &related_location = None) throws -> bool;
  fn flush_warnings() throws -> void;
  fn print_diagnostic_summary() const throws -> void;
  fn print_optimizer_summary() const throws -> void;
  pure fn is_diagnostic_suppressed(
      diagnostic_id id, const SourceLocation &location) const wontthrow -> bool;

  /* Whether the mood and the warning level let this code reach the output. A
     check that costs more than a comparison asks first, and the reporting
     funnel asks before it formats anything. */
  pure fn should_report(diagnostic_id id) const wontthrow -> bool;
  fn note_variable_assignment(StringView name,
                              const SourceLocation &location) throws -> void;

  /* A null value word means the assignment has no scalar word to fold, so an
     array element or a NAME=(...) list. */
  fn note_variable_assignment_record(StringView name, const Word *value_word,
                                     const SourceLocation &location,
                                     bool is_conditional, bool is_append) throws
      -> void;
  /* A command that binds a name supplies no value word and no literal, so the
     binder is what a reader is told. */
  fn note_variable_binding_record(StringView name,
                                  const SourceLocation &location,
                                  assignment_binder binder,
                                  bool is_conditional) throws -> void;
  fn note_variable_occurrence(StringView name, const SourceLocation &location,
                              variable_occurrence_kind kind,
                              bool is_unresolved = false,
                              bool is_append = false) throws -> void;
  fn apply_called_function(StringView name,
                           const SourceLocation &call_location) throws -> void;
  fn note_function_body_record(StringView name, usize name_position,
                               usize body_position,
                               usize body_end_position) throws -> void;

  /* A positional read inside a function body means the body uses the arguments
     its caller passes. */
  fn mark_positional_reference() wontthrow -> void
  {
    if (active_function_definition_index == NO_ACTIVE_FUNCTION_DEFINITION)
      return;

    function_definitions[active_function_definition_index]
        .has_positional_reads = true;
  }

  /* The sentinel check comes first, since the walk spends most of its time
     outside every function body. */
  fn note_positional_reference(StringView name) wontthrow -> void
  {
    if (active_function_definition_index == NO_ACTIVE_FUNCTION_DEFINITION)
      return;
    if (!reference_names_positional(name)) return;

    mark_positional_reference();
  }

  fn note_variable_read(StringView name, const SourceLocation &location,
                        bool is_top_level_unconditional) throws -> void;
  fn trace_optimizer_line(StringView message) const throws -> void;
  fn print_script_backtrace_if_rooted(
      const SourceLocation &location) const throws -> void;

private:
  pure fn should_report(diagnostic_tier tier) const wontthrow -> bool;
  fn warn(diagnostic_id id, const SourceLocation &location, StringView message,
          StringView suggestion, diagnostic_tier tier,
          const Maybe<SourceLocation> &related_location,
          StringView related_message) throws -> void;
  fn fail(diagnostic_id id, const SourceLocation &location, StringView message,
          StringView suggestion, diagnostic_tier tier,
          const Maybe<SourceLocation> &related_location,
          StringView related_message) throws -> void;
};

fn analyze_ast(
    const Expression *root, StringView source, const HashSet &known_functions,
    const HashSet &known_aliases, EvalContext *eval_context, u8 warning_level,
    bool silence_unresolved_commands, bool is_default_mood,
    bool should_emit_annoying_diagnostics,
    const ArrayList<shellcheck_suppression> &shellcheck_suppressions,
    const ArrayList<analysis_scope_definition> &scope_definitions,
    const ArrayList<shellcheck_directive_span> &directive_spans,
    const ArrayList<heredoc_terminator_miss> &heredoc_misses,
    bool is_named_script_file, bool should_report_optimizer_diagnostics = false,
    HashSet *followed_source_paths = nullptr,
    StringMap<followed_source_effects> *source_effects_cache = nullptr,
    AnalysisContext *parent_analysis_context = nullptr,
    analysis_diagnostic_totals *deferred_diagnostic_totals = nullptr,
    bool should_merge_parent_state = true,
    bool should_merge_parent_uncertainty = true,
    followed_source_effects *source_effects = nullptr,
    ArrayList<source_diagnostic> *diagnostic_sink = nullptr,
    AnalysisSourceProvider *source_provider = nullptr,
    analysis_symbol_records *symbol_records = nullptr,
    AnalysisUnitStream *unit_stream = nullptr) throws -> bool;

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
  /* The byte just past this node's source text. It defaults to the end of the
     opening token that source_location names, and a compound node widens it to
     its closing token so the whole span is recoverable. */
  pure fn source_end_position() const wontthrow -> usize;
  fn set_source_end_position(usize position) wontthrow -> void;
  fn evaluate(EvalContext &cxt) const throws -> i64;

  Expression(const Expression &) = delete;
  Expression(Expression &&) noexcept = delete;
  Expression &operator=(const Expression &) = delete;
  Expression &operator=(Expression &&) noexcept = delete;

  virtual fn to_string() const throws -> String = 0;
  virtual fn to_ast_string(usize layer = 0) const throws -> String;

  virtual fn is_simple_command() const wontthrow -> bool;
  virtual fn is_dummy() const wontthrow -> bool;

  /* The typed-node downcasts the optimizer rules use to match a node without
     RTTI, since the build links with -fno-rtti. The base returns nullptr and a
     node of the matching kind overrides its own hook to return this, so a rule
     reads a typed pointer or skips the node. */
  virtual fn as_if_clause() const wontthrow -> const expressions::IfClause *;
  virtual fn as_while_loop() const wontthrow -> const expressions::WhileLoop *;
  virtual fn as_assign_command() const wontthrow
      -> const expressions::AssignCommand *;
  virtual fn as_simple_command() const wontthrow
      -> const expressions::SimpleCommand *;
  virtual fn as_for_loop() const wontthrow -> const expressions::ForLoop *;
  virtual fn as_cstyle_for_loop() const wontthrow
      -> const expressions::CStyleForLoop *;
  virtual fn as_subshell() const wontthrow -> const expressions::Subshell *;

  /* This no-ops for arena storage and frees an ordinary heap node otherwise. */
  static fn operator delete(opaque *pointer) wontthrow->void;

  /* is_unconditional says whether this node is reached on every run, which
     decides a failure from a warning. */
  virtual fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void;

  virtual fn append_presence_tested_command_names(
      const AnalysisContext &actx, HashSet &names,
      bool status_is_success) const throws -> void;

  virtual fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws -> bool;

  /* Some(true) means the condition always succeeds with no side effect,
     Some(false) means it always fails, and None means the result is only known
     at run time. */
  virtual fn
  try_static_condition_verdict(const AnalysisContext &actx) const wontthrow
      -> Maybe<bool>;

protected:
  virtual fn evaluate_impl(EvalContext &cxt) const throws -> i64 = 0;

  SourceLocation m_location;
  /* This sits in the hole the twelve-byte location leaves, and a source beyond
     four gigabytes is already out of reach of the location itself. */
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;
};

/* Kept as an ordered list, so a repeated name accumulates. */
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
  pure fn is_append() const wontthrow -> bool { return token->is_append(); }
};

struct array_builtin_assignment
{
  String name;
  ArrayList<const Token *> elements;
  SourceLocation location;
  bool is_append;
};

/* The bash assignment builtins that parse a NAME=(...) argument as an array
   assignment. The parser recognizes the form by this classification and the
   evaluator applies the elements with the scope and the marks the same
   classification selects. */
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

  fn set_timed(bool posix_format, SourceLocation location) wontthrow -> void;
  pure fn is_timed() const wontthrow -> bool;
  pure fn time_uses_posix_format() const wontthrow -> bool;
  pure fn time_location() const wontthrow -> SourceLocation;

  virtual fn is_assignment() const wontthrow -> bool;
  virtual fn is_compound_command() const wontthrow -> bool;

  /* The default throws the unsupported error, only a node that takes a target
     overrides it. */
  virtual fn append_to(usize d, String &f, bool duplicate) throws -> void;
  virtual fn redirect_to(usize d, String &f, bool duplicate) throws -> void;

protected:
  bool m_is_async{false};
  bool m_is_negated{false};
  bool m_is_timed{false};
  bool m_is_time_posix_format{false};
  /* The keyword is always the literal `time` in the source the command itself
     is stamped with, so the length and the source name are recovered and only
     the position is retained. */
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  const Assignment *m_assignment;
};

class Redirection
{
public:
  enum class Kind : u8
  {
    TruncateOutput,         /* >    */
    TruncateOutputOverride, /* >|   */
    AppendOutput,           /* >>   */
    ReadInput,              /* <    */
    ReadWrite,              /* <>   */
    DuplicateOutput,        /* >&   */
    DuplicateInput,         /* <&   */
    Heredoc,                /* <<   */
    HereString              /* <<<  */
  };

  /* The dup_fd value that marks the close-descriptor form, as in 2>&- and <&-,
     which closes fd rather than copying another descriptor onto it. */
  static constexpr i32 DUP_FD_CLOSE = -2;

  /* Null for a duplication whose descriptor was a literal in the source. */
  const Token *target;
  const heredoc_contents *heredoc;
  const Token *fd_allocation_name_token;
  i32 fd;
  /* The literal descriptor to copy from, or DUP_FD_CLOSE for the close form,
     or -1 when the descriptor is a dynamic word held in target. */
  i32 dup_fd;
  Kind kind;
  bool should_expand_heredoc;
  /* True for a bare >&word outside POSIX mode, where a word that expands to
     neither a number nor a dash is the csh both-streams spelling bash reads
     as >word 2>&1, resolved after the expansion the way bash decides it. */
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

  /* A duplication and a close copy a descriptor that is already open, so only
     these forms compete with a pipe or with a second redirect for fd. */
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

  fn set_array_args(ArrayList<array_builtin_assignment> &&array_args) throws
      -> void;

  fn redirect_exec_context(ExecContext &ec, EvalContext &cxt) const throws
      -> void;

  fn is_simple_command() const wontthrow -> bool override;

  pure fn args() const wontthrow -> const ArrayList<const Token *> &;
  pure fn redirections() const wontthrow -> const SparseList<Redirection> &;

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

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  ArrayList<const Token *> m_args{heap_allocator()};

  mutable Maybe<bool> m_command_word_is_glob{};

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

  /* True when the command this node holds carries a leading !, which set -e
     exempts from its exit. */
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  Kind m_kind;
  const Command *m_cmd;
};

class CompoundList : public Expression
{
public:
  CompoundList();

  ~CompoundList() override;

  pure fn is_empty() const wontthrow -> bool;
  fn append_node(const CompoundListCondition *node) throws -> void;

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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  fn evaluate_with_compound_stages(EvalContext &cxt) const throws -> i64;

  ArrayList<const Command *> m_commands{heap_allocator()};

  mutable Maybe<bool> m_has_compound_stage{};
};

/* Redirections on a compound command are not supported yet. */
class CompoundCommand : public Command
{
public:
  CompoundCommand(SourceLocation location);

  fn is_compound_command() const wontthrow -> bool override;

  fn redirect_to(usize d, String &f, bool duplicate) throws -> void override;

  fn set_fully_eliminated() const wontthrow -> void;
  pure fn is_fully_eliminated() const wontthrow -> bool;

protected:
  mutable bool m_is_fully_eliminated{false};
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

  /* The branch count selects the else body or nothing. */
  fn set_folded_branch(usize index) const wontthrow -> void;
  pure fn has_folded_branch() const wontthrow -> bool;
  /* The branch index the dead-branch rule recorded, read by the compound-body
     elimination rule. An index at the branch count names the else body. Valid
     only when has_folded_branch is true. */
  pure fn folded_branch_index() const wontthrow -> usize;

  fn as_if_clause() const wontthrow -> const IfClause * override;

  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  ArrayList<if_branch> m_branches{heap_allocator()};
  const Expression *m_otherwise;

  /* The branch the analyze pass proved this if takes, when every condition up
     to it has a statically-decidable verdict. Some(i) selects branch i's body,
     a value past the last branch selects the else body or nothing. None means
     the branch is only known at run time and evaluate_impl runs the conditions.
   */
  mutable Maybe<usize> m_folded_branch{};
};

class WhileLoop : public CompoundCommand
{
public:
  WhileLoop(SourceLocation location, const Expression *condition,
            const Expression *body, bool is_until);
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  const Expression *m_condition;
  const Expression *m_body;
  bool m_is_until;

  /* A while true stays unfolded, since the loop is infinite and still runs. */
  mutable bool m_folded_to_skip{false};
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  /* The name is a slice of the arena that holds this node. */
  StringView m_variable_name;
  ArrayList<const Token *> m_words{heap_allocator()};
  const Expression *m_body;
  SourceLocation m_variable_location;
  bool m_has_in_clause;
};

/* How an arm ends. ;; stops the case, ;& falls into the next arm body without
   matching it, and ;;& resumes matching at the following arms. */
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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
  fn can_evaluate_in_process_substitution(
      const EvalContext &cxt, HashSet &active_functions) const throws
      -> bool override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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

  fn set_analysis_scope_definitions(
      ArrayList<analysis_scope_definition> definitions) throws -> void
  {
    m_analysis_scope_definitions.fill(steal(definitions));
  }

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  /* The expression is a slice of the arena that holds this node. */
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

  /* The init runs once before the condition even when the condition folds to
     zero, so the folding rule keeps the loop alive to run it. */
  pure fn init_clause() const wontthrow -> StringView;

  fn set_folded_condition(i64 value) const wontthrow -> void;
  pure fn has_folded_condition() const wontthrow -> bool;

  fn as_cstyle_for_loop() const wontthrow -> const CStyleForLoop * override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  /* The source position of the first byte of the init clause, so each clause
     base is recovered from the lengths that precede it. */
  usize m_header_position;
  /* The three clauses are slices of the arena that holds this node, so they
     stay readable for as long as the node does. */
  StringView m_init;
  StringView m_condition;
  StringView m_step;
  const Expression *m_body;

  mutable Maybe<i64> m_folded_condition{};

  /* A loop that is only analyzed never tokenizes a clause, so each cache is
     allocated on the first evaluation that needs it. */
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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  /* The name is a slice of the arena that holds this node. */
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

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

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
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  String m_name;
  FunctionBodyHandle m_body_storage;
  const Expression *m_body;
  SparseList<analysis_scope_definition> m_analysis_scope_definitions{};
};

class ConstantNumber : public Expression
{
public:
  ConstantNumber(SourceLocation location, i64 value);
  ~ConstantNumber() override;

  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn to_string() const throws -> String override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  const i64 m_value;
};

class UnaryExpression : public Expression
{
public:
  UnaryExpression(SourceLocation location, const Expression *rhs);
  ~UnaryExpression() override;

  fn to_ast_string(usize layer = 0) const throws -> String override;

protected:
  const Expression *m_rhs;
};

#define UNARY_EXPRESSION_STRUCT(e)                                             \
  class e : public UnaryExpression                                             \
  {                                                                            \
  public:                                                                      \
    e(SourceLocation location, const Expression *rhs);                         \
    String to_string() const throws override;                                  \
                                                                               \
  protected:                                                                   \
    i64 evaluate_impl(EvalContext &cxt) const throws override;                 \
  }

UNARY_EXPRESSION_STRUCT(Negate);
UNARY_EXPRESSION_STRUCT(Unnegate);
UNARY_EXPRESSION_STRUCT(LogicalNot);
UNARY_EXPRESSION_STRUCT(BinaryComplement);

class BinaryExpression : public Expression
{
public:
  BinaryExpression(SourceLocation location, const Expression *lhs,
                   const Expression *rhs);
  ~BinaryExpression() override;

  fn to_ast_string(usize layer = 0) const throws -> String override;

protected:
  const Expression *m_lhs;
  const Expression *m_rhs;
};

#define BINARY_EXPRESSION_STRUCT(e)                                            \
  class e : public BinaryExpression                                            \
  {                                                                            \
  public:                                                                      \
    e(SourceLocation location, const Expression *lhs, const Expression *rhs);  \
    String to_string() const throws override;                                  \
                                                                               \
  protected:                                                                   \
    i64 evaluate_impl(EvalContext &cxt) const throws override;                 \
  }

BINARY_EXPRESSION_STRUCT(BinaryDummyExpression);
BINARY_EXPRESSION_STRUCT(Add);
BINARY_EXPRESSION_STRUCT(Subtract);
BINARY_EXPRESSION_STRUCT(Multiply);
BINARY_EXPRESSION_STRUCT(Divide);
BINARY_EXPRESSION_STRUCT(Module);
BINARY_EXPRESSION_STRUCT(BinaryAnd);
BINARY_EXPRESSION_STRUCT(LogicalAnd);
BINARY_EXPRESSION_STRUCT(GreaterThan);
BINARY_EXPRESSION_STRUCT(GreaterOrEqual);
BINARY_EXPRESSION_STRUCT(RightShift);
BINARY_EXPRESSION_STRUCT(LeftShift);
BINARY_EXPRESSION_STRUCT(LessThan);
BINARY_EXPRESSION_STRUCT(LessOrEqual);
BINARY_EXPRESSION_STRUCT(BinaryOr);
BINARY_EXPRESSION_STRUCT(LogicalOr);
BINARY_EXPRESSION_STRUCT(Xor);
BINARY_EXPRESSION_STRUCT(Equal);
BINARY_EXPRESSION_STRUCT(NotEqual);

} /* namespace expressions */

} /* namespace koshka */
