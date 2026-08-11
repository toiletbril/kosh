#pragma once

#include "Common.hpp"
#include "Diagnostics.hpp"
#include "Eval.hpp"
#include "Tokens.hpp"

namespace koshka {

using namespace tokens;

class Token;
struct heredoc_contents;

struct pending_analysis_warning
{
  SourceLocation location;
  String message;
  String suggestion;
  Maybe<SourceLocation> related_location;
  String related_message;
};

namespace expressions {
class IfClause;
class WhileLoop;
class AssignCommand;
class SimpleCommand;
class ForLoop;
class CStyleForLoop;
} /* namespace expressions */

class AnalysisContext
{
public:
  StringView source;
  bool has_fatal{false};
  usize reported_warning_count{0};
  usize reported_error_count{0};
  u8 warning_level{0};
  bool is_default_mood{true};
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

  /* Saved and cleared on entry to a function body and restored on exit. */
  HashSet function_local_names{heap_allocator()};

  /* An assignment inside a function to one of these updates an existing global
     rather than leaking a new binding, so the no-local warning stays quiet. */
  HashSet global_assigned_names{heap_allocator()};

  HashSet assigned_names_so_far{heap_allocator()};

  StringMap<SourceLocation> reads_before_assignment{heap_allocator()};

  bool is_direct_pipeline_stage{false};
  bool is_inside_loop_condition{false};
  bool is_command_status_observed{false};
  bool loop_condition_reads_input{false};
  bool is_inside_read_loop{false};
  HashSet pipeline_lost_names{heap_allocator()};
  HashSet external_input_names{heap_allocator()};
  StringMap<SourceLocation> active_loop_variables{heap_allocator()};

  /* The lookup is lazy, and null in a context with no live shell. */
  EvalContext *eval_context{nullptr};

  /* The SC3xxx bashism lints fire only behind this gate, since a kosh or bash
     shebang means the bash extension on purpose. */
  bool shebang_is_posix_sh{false};

  /* An interactive -W chunk runs the moment the analysis ends and the runtime
     resolution reports the same missing command, so the analysis copy would
     double the report. A script run keeps the check. */
  bool should_silence_unresolved_commands{false};

  HashSet tested_command_names{heap_allocator()};
  bool should_retain_tested_command_names{false};
  bool is_analyzing_condition{false};

  bool should_trace_optimizer{false};
  usize optimizer_folded_arithmetic{0};
  usize optimizer_recorded_constants{0};
  usize optimizer_folded_branches{0};
  usize optimizer_folded_loops{0};
  usize optimizer_eliminated_compounds{0};

  bool should_print_optimizer_state{false};

  ArrayList<pending_analysis_warning> pending_warnings{heap_allocator()};

  explicit AnalysisContext(StringView source_view) : source(source_view) {}

  fn add_defined_function(StringView name) throws -> void
  {
    if (defined_functions.contains(name)) return;
    defined_functions.add(name);
    defined_function_insertions.push(String{name});
  }

  fn add_known_alias(StringView name) throws -> void
  {
    if (known_aliases.contains(name)) return;
    known_aliases.add(name);
    known_alias_insertions.push(String{name});
  }

  fn apply_scope_definitions(
      const ArrayList<analysis_scope_definition> &definitions) throws -> void
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

  fn report_diagnostic(diagnostic_id id, SourceLocation location,
                       std::initializer_list<StringView> arguments = {},
                       Maybe<SourceLocation> related_location = None) throws
      -> void;
  fn flush_warnings() throws -> void;
  fn print_diagnostic_summary() const throws -> void;
  pure fn is_diagnostic_suppressed(diagnostic_id id,
                                   SourceLocation location) const wontthrow
      -> bool;

  /* Whether the mood and the warning level let this code reach the output. A
     check that costs more than a comparison asks first, and the reporting
     funnel asks before it formats anything. */
  pure fn should_report(diagnostic_id id) const wontthrow -> bool;
  fn note_variable_assignment(StringView name) throws -> void;
  fn note_variable_read(StringView name, SourceLocation location,
                        bool is_top_level_unconditional) throws -> void;
  fn trace_optimizer_line(StringView message) const throws -> void;
  fn trace_eliminated_node(SourceLocation location,
                           StringView message) const throws -> void;
  fn print_script_backtrace_if_rooted(SourceLocation location) const throws
      -> void;

private:
  fn warn(SourceLocation location, StringView message, StringView suggestion,
          diagnostic_tier tier, Maybe<SourceLocation> related_location,
          StringView related_message) throws -> void;
  fn fail(SourceLocation location, StringView message, StringView suggestion,
          diagnostic_tier tier, Maybe<SourceLocation> related_location,
          StringView related_message) throws -> void;
};

fn analyze_ast(const Expression *root, StringView source,
               const HashSet &known_functions, const HashSet &known_aliases,
               EvalContext *eval_context, u8 warning_level,
               bool silence_unresolved_commands, bool is_default_mood,
               bool should_emit_annoying_diagnostics,
               const ArrayList<shellcheck_suppression> &shellcheck_suppressions,
               const ArrayList<analysis_scope_definition> &scope_definitions,
               bool show_optimizer_state = false) throws -> bool;

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
  usize m_source_end_position;
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
struct prefix_assignment
{
  String name;
  Word value;
  SourceLocation location;
  bool is_append;
};

struct array_builtin_assignment
{
  String name;
  ArrayList<const Token *> elements;
  SourceLocation location;
  bool is_append;
};

class Command : public Expression
{
public:
  Command(SourceLocation location);

  fn make_async() wontthrow -> void;
  pure fn is_async() const wontthrow -> bool;
  fn set_local_vars(ArrayList<prefix_assignment> &&vars) throws -> void;

  pure fn local_vars() const wontthrow -> const ArrayList<prefix_assignment> &;

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
  SourceLocation m_time_location{};
  ArrayList<prefix_assignment> m_local_vars{heap_allocator()};
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

  i32 fd;
  Kind kind;
  /* Null for a duplication whose descriptor was a literal in the source. */
  const Token *target;
  /* The literal descriptor to copy from, or DUP_FD_CLOSE for the close form,
     or -1 when the descriptor is a dynamic word held in target. */
  i32 dup_fd;
  const heredoc_contents *heredoc;
  bool should_expand_heredoc;
  /* True for a bare >&word outside POSIX mode, where a word that expands to
     neither a number nor a dash is the csh both-streams spelling bash reads
     as >word 2>&1, resolved after the expansion the way bash decides it. */
  bool can_dup_be_filename;
  bool is_both_streams_spelling;
  const Token *fd_allocation_name_token;
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
  pure fn redirections() const wontthrow -> const ArrayList<Redirection> &;

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

  ArrayList<Redirection> m_redirections{heap_allocator()};
  ArrayList<array_builtin_assignment> m_array_args{heap_allocator()};
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

  String m_variable_name;
  SourceLocation m_variable_location;
  ArrayList<const Token *> m_words{heap_allocator()};
  bool m_has_in_clause;
  const Expression *m_body;
};

/* How an arm ends. ;; stops the case, ;& falls into the next arm body without
   matching it, and ;;& resumes matching at the following arms. */
enum class case_terminator
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

  fn set_analysis_scope_definitions(
      ArrayList<analysis_scope_definition> definitions) wontthrow -> void
  {
    m_analysis_scope_definitions = steal(definitions);
  }

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  const Expression *m_body;
  ArrayList<analysis_scope_definition> m_analysis_scope_definitions{
      heap_allocator()};
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
  ArithmeticCommand(SourceLocation location, String expression);
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

  String m_expression;
};

class CStyleForLoop : public CompoundCommand
{
public:
  CStyleForLoop(SourceLocation location, String init, String condition,
                String step, const Expression *body);
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

  String m_init;
  String m_condition;
  String m_step;
  const Expression *m_body;

  mutable Maybe<i64> m_folded_condition{};

  /* A simple clause runs the token fast path, a complex clause falls back to
     the char parser, decided once when the tokens are filled. */
  mutable ArrayList<arith_token> m_condition_tokens{heap_allocator()};
  mutable ArrayList<arith_token> m_step_tokens{heap_allocator()};
  mutable bool m_condition_tokenized{false};
  mutable bool m_step_tokenized{false};
  mutable bool m_condition_simple{false};
  mutable bool m_step_simple{false};
};

class SelectLoop : public CompoundCommand
{
public:
  SelectLoop(SourceLocation location, StringView variable_name,
             ArrayList<const Token *> &&words, bool has_in_clause,
             const Expression *body);
  ~SelectLoop() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  String m_variable_name;
  ArrayList<const Token *> m_words{heap_allocator()};
  bool m_has_in_clause;
  const Expression *m_body;
};

class ArrayAssignCommand : public Command
{
public:
  ArrayAssignCommand(SourceLocation location, StringView name,
                     ArrayList<const Token *> elements, bool is_append);
  ~ArrayAssignCommand() override;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;

  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  String m_name;
  ArrayList<const Token *> m_elements{heap_allocator()};
  bool m_is_append;
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
  ArrayList<Redirection> m_redirections{heap_allocator()};
};

class FunctionDefinition : public CompoundCommand
{
public:
  FunctionDefinition(SourceLocation location, StringView name,
                     const Expression *body);
  ~FunctionDefinition() override;

  pure fn name() const wontthrow -> const String &;
  pure fn body() const wontthrow -> const Expression *;

  fn to_string() const throws -> String override;
  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn analyze(AnalysisContext &actx, bool is_unconditional) const throws
      -> void override;

  fn set_analysis_scope_definitions(
      ArrayList<analysis_scope_definition> definitions) wontthrow -> void
  {
    m_analysis_scope_definitions = steal(definitions);
  }

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  String m_name;
  const Expression *m_body;
  ArrayList<analysis_scope_definition> m_analysis_scope_definitions{
      heap_allocator()};
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

class ConstantString : public Expression
{
public:
  ConstantString(SourceLocation location, StringView value);
  ~ConstantString() override;

  fn to_ast_string(usize layer = 0) const throws -> String override;
  fn to_string() const throws -> String override;

protected:
  fn evaluate_impl(EvalContext &cxt) const throws -> i64 override;

  const String m_value;
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
