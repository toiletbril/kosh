#include "Arena.hpp"
#include "Builtin.hpp"
#include "Cli.hpp"
#include "Colors.hpp"
#include "Common.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

pure fn find_pipe_overriding_redirection(const SimpleCommand *stage,
                                         bool wants_output) wontthrow
    -> const Redirection *
{
  for (let const &redirection : stage->redirections()) {
    let const claims_stream = wants_output ? redirection.opens_output_file()
                                           : redirection.opens_input_source();
    if (claims_stream && redirection.fd == (wants_output ? 1 : 0))
      return &redirection;
  }

  return nullptr;
}

fn Pipeline::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  /* POSIX sh reads time as a utility, so it receives the first stage alone and
     the report covers nothing else, shellcheck SC2176. */
  if (actx.is_posix_sh_shebang && is_timed())
    actx.report_diagnostic(diagnostic_id::sc2176, time_location());

  /* A multi-stage pipeline runs each stage in a forked child, so a stage
     assignment must not be recorded as a straight-line constant. A single
     command keeps the caller's unconditional context. */
  let const has_multiple_stages = m_commands.count() > 1;
  let const stage_is_unconditional = is_unconditional && !has_multiple_stages;
  for (let const command : m_commands) {
    ASSERT(command != nullptr);
    let const was_direct_pipeline_stage = actx.is_direct_pipeline_stage;
    actx.is_direct_pipeline_stage =
        has_multiple_stages && (command->as_simple_command() != nullptr ||
                                command->as_assign_command() != nullptr);

    if (!has_multiple_stages) {
      command->analyze(actx, stage_is_unconditional);
      actx.is_direct_pipeline_stage = was_direct_pipeline_stage;
      continue;
    }

    let const saved_has_seen_runtime_definer = actx.has_seen_runtime_definer;
    let const saved_has_unknown_path = actx.has_unknown_path;
    let const saved_has_unknown_working_directory =
        actx.has_unknown_working_directory;
    let const saved_should_silence_unresolved_commands =
        actx.should_silence_unresolved_commands;
    let const defined_function_insertion_count =
        actx.defined_function_insertions.count();
    let const known_alias_insertion_count = actx.known_alias_insertions.count();
    let saved_inherited_assigned_names = actx.inherited_assigned_names.clone();
    let saved_inherited_global_assigned_names =
        actx.inherited_global_assigned_names.clone();
    let saved_array_valued_names = actx.array_valued_names.clone();
    let saved_occurrence_assignments =
        actx.variable_occurrence_assignments.clone();
    let saved_inherited_occurrence_assignments =
        actx.inherited_variable_occurrence_assignments.clone();
    let *saved_source_effects = actx.current_source_effects;
    actx.current_source_effects = nullptr;

    command->analyze(actx, stage_is_unconditional);

    actx.current_source_effects = saved_source_effects;
    actx.is_direct_pipeline_stage = was_direct_pipeline_stage;
    actx.has_unknown_working_directory = saved_has_unknown_working_directory;
    actx.has_unknown_path = saved_has_unknown_path;
    actx.has_seen_runtime_definer = saved_has_seen_runtime_definer;
    actx.should_silence_unresolved_commands =
        saved_should_silence_unresolved_commands;
    actx.array_valued_names = steal(saved_array_valued_names);
    actx.inherited_global_assigned_names =
        steal(saved_inherited_global_assigned_names);
    actx.inherited_assigned_names = steal(saved_inherited_assigned_names);
    actx.variable_occurrence_assignments = steal(saved_occurrence_assignments);
    actx.inherited_variable_occurrence_assignments =
        steal(saved_inherited_occurrence_assignments);
    actx.rollback_defined_functions(defined_function_insertion_count);
    actx.rollback_known_aliases(known_alias_insertion_count);
  }

  /* cat feeding a single named file into the next stage runs an extra process,
     shellcheck SC2002. The first stage must be cat with one plain file operand
     and a later stage must follow. */
  if (m_commands.count() > 1) {
    ASSERT(m_commands[0] != nullptr);
    const SimpleCommand *first_stage = m_commands[0]->as_simple_command();

    /* An assignment-only first stage runs beside the pipeline and reads nothing
       from it, shellcheck SC2036. The parser splits a lone assignment into an
       AssignCommand and keeps several as prefixes on an empty command. */
    if (m_commands[0]->is_assignment()) {
      const AssignCommand *assign = m_commands[0]->as_assign_command();
      if (assign != nullptr) {
        actx.report_diagnostic(diagnostic_id::sc2036,
                               m_commands[0]->source_location(),
                               {assign->assignment()->key().view()});
      }
    } else if (first_stage != nullptr && first_stage->args().is_empty() &&
               !first_stage->local_vars().is_empty())
    {
      actx.report_diagnostic(diagnostic_id::sc2036,
                             first_stage->local_vars()[0].get_location(),
                             {first_stage->local_vars()[0].get_name()});
    }

    if (first_stage != nullptr) {
      let const &cat_args = first_stage->args();
      if (cat_args.count() == 2) {
        let const name = static_command_name(cat_args[0]);
        let raw_operand_storage = String{heap_allocator()};
        let const raw_operand =
            borrowed_token_text(cat_args[1], raw_operand_storage);
        let const file_is_plain_operand =
            cat_args[1]->kind() == Token::Kind::Word &&
            !raw_operand.is_empty() && raw_operand[0] != '-';
        if (name.has_value() && *name == "cat" &&
            !actx.defined_functions.contains(*name) &&
            !actx.known_aliases.contains(*name) && file_is_plain_operand)
        {
          actx.report_diagnostic(diagnostic_id::sc2002,
                                 cat_args[0]->source_location());
        }
      }
    }
  }

  /* The stage-pair lints. A pipe into a non-stdin command is SC2216, and the
     remaining codes are keyed on the stage that feeds the pipe. */
  for (usize i = 0; i + 1 < m_commands.count(); i++) {
    const SimpleCommand *stage = m_commands[i]->as_simple_command();
    const SimpleCommand *next = m_commands[i + 1]->as_simple_command();
    if (stage == nullptr || next == nullptr) {
      continue;
    }
    if (stage->args().is_empty() || next->args().is_empty()) {
      continue;
    }

    /* One descriptor cannot hold both a pipe and a file, and the redirection
       wins, shellcheck SC2259 and SC2260. Each stage is visited once as the
       feeding side and once as the receiving side. */
    let const output_override = find_pipe_overriding_redirection(stage, true);
    if (output_override != nullptr && output_override->target != nullptr) {
      actx.report_diagnostic(
          diagnostic_id::sc2260, output_override->target->source_location(),
          {stage->args()[0]->raw_view().value_or(StringView{})});
    }

    let const input_override = find_pipe_overriding_redirection(next, false);
    if (input_override != nullptr && input_override->target != nullptr) {
      actx.report_diagnostic(
          diagnostic_id::sc2259, input_override->target->source_location(),
          {next->args()[0]->raw_view().value_or(StringView{})});
    }

    let const stage_name = static_command_name(stage->args()[0]);
    let const next_name = static_command_name(next->args()[0]);
    if (!stage_name.has_value() || !next_name.has_value()) {
      continue;
    }
    let const next_is_user = actx.defined_functions.contains(*next_name) ||
                             actx.known_aliases.contains(*next_name);
    let const stage_is_user = actx.defined_functions.contains(*stage_name) ||
                              actx.known_aliases.contains(*stage_name);
    let const stage_info = get_analysis_command_info(*stage_name);
    let const next_info = get_analysis_command_info(*next_name);
    let const next_is_pattern_matcher =
        next_info.is_in_group(COMMAND_GROUP_PATTERN_MATCHER);
    let const next_is_xargs =
        next_info.id == command_name_id::Xargs && !next_is_user;

    if (!next_is_user &&
        next_info.is_in_group(COMMAND_GROUP_NON_STDIN_READER) &&
        !args_have_stdin_operand(next->args()))
    {
      if (next_info.id == command_name_id::Echo) {
        actx.report_diagnostic(diagnostic_id::sc2008,
                               next->args()[0]->source_location());
      } else {
        actx.report_diagnostic(diagnostic_id::sc2216,
                               next->args()[0]->source_location(),
                               {*next_name});
      }
    }

    if (stage_is_user) continue;

    switch (stage_info.id) {
    /* echo feeding wc -c measures a string whose length the shell already
       knows, shellcheck SC2000. */
    case command_name_id::Echo:
      if (next_info.id == command_name_id::Wc && !next_is_user &&
          stage->args().count() == 2 && next->args().count() == 2)
      {
        let count_flag_storage = String{heap_allocator()};
        let const count_flag =
            borrowed_token_text(next->args()[1], count_flag_storage);
        if (count_flag == "-c" || count_flag == "-m") {
          actx.report_diagnostic(diagnostic_id::sc2000,
                                 stage->args()[0]->source_location());
        }
      }
      break;

    /* find piped into xargs splits a name at every blank, shellcheck SC2038. */
    case command_name_id::Find: {
      if (!next_is_xargs) break;

      let has_null_flag = false;
      let raw_storage = String{heap_allocator()};
      for (usize a = 1; a < stage->args().count() && !has_null_flag; a++)
        if (borrowed_token_text(stage->args()[a], raw_storage) == "-print0")
          has_null_flag = true;
      for (usize a = 1; a < next->args().count() && !has_null_flag; a++) {
        let const raw = borrowed_token_text(next->args()[a], raw_storage);
        if (raw == "-0" || raw == "--null") {
          has_null_flag = true;
        }
      }

      if (!has_null_flag)
        actx.report_diagnostic(diagnostic_id::sc2038,
                               next->args()[0]->source_location());
      break;
    }

    /* The ls listing loses a name that holds a space or a newline. Grep reading
       it is shellcheck SC2010, xargs reading it is SC2011, and any other reader
       is SC2012. */
    case command_name_id::Ls:
      if (next_is_pattern_matcher) {
        actx.report_diagnostic(diagnostic_id::sc2010,
                               next->args()[0]->source_location());
      } else if (next_is_xargs) {
        actx.report_diagnostic(diagnostic_id::sc2011,
                               next->args()[0]->source_location());
      } else {
        actx.report_diagnostic(diagnostic_id::sc2012,
                               stage->args()[0]->source_location());
      }
      break;

    /* ps piped into grep races the process table and matches the grep itself,
       shellcheck SC2009. */
    case command_name_id::Ps:
      if (next_is_pattern_matcher)
        actx.report_diagnostic(diagnostic_id::sc2009,
                               next->args()[0]->source_location());
      break;

    /* grep feeding wc -l counts matches with a second process, shellcheck
       SC2126. */
    case command_name_id::Grep:
      if (next_info.id == command_name_id::Wc && !next_is_user &&
          next->args().count() == 2)
      {
        let flag_storage = String{heap_allocator()};
        if (borrowed_token_text(next->args()[1], flag_storage) == "-l") {
          actx.report_diagnostic(diagnostic_id::sc2126,
                                 stage->args()[0]->source_location());
        }
      }
      break;

    default: break;
    }
  }

  /* The table cannot prove a value across the fork, so a multi-stage pipeline
     forgets any recorded constant. */
  if (m_commands.count() > 1) actx.constant_variables.clear();
}

fn Pipeline::append_presence_tested_command_names(
    const AnalysisContext &actx, HashSet &names,
    bool status_is_success) const throws -> void
{
  if (m_commands.count() != 1 || m_commands[0] == nullptr) {
    return;
  }
  m_commands[0]->append_presence_tested_command_names(
      actx, names, status_is_success != is_negated());
}

fn Pipeline::as_simple_command() const wontthrow -> const SimpleCommand *
{
  if (m_commands.count() != 1 || m_commands[0] == nullptr) {
    return nullptr;
  }
  return m_commands[0]->as_simple_command();
}

/* The opening [ of a bracket test the node leaves unclosed, null when the node
   holds anything else. */
cold static fn
node_unclosed_test_bracket(const CompoundListCondition *node) wontthrow
    -> const Token *
{
  const Command *held = node->command();
  if (held == nullptr) return nullptr;

  const SimpleCommand *command = held->as_simple_command();
  if (command == nullptr) return nullptr;

  let const &args = command->args();
  if (args.is_empty()) return nullptr;

  let const name = args[0]->raw_view();
  if (!name.has_value() || *name != StringView{"["}) {
    return nullptr;
  }

  let const closer = args.back()->raw_view();
  if (closer.has_value() && *closer == StringView{"]"}) {
    return nullptr;
  }

  return args[0];
}

fn CompoundListCondition::analyze(AnalysisContext &actx,
                                  bool is_unconditional) const throws -> void
{
  ASSERT(m_cmd != nullptr);

  m_cmd->analyze(actx, is_unconditional);
}

fn CompoundListCondition::append_presence_tested_command_names(
    const AnalysisContext &actx, HashSet &names,
    bool status_is_success) const throws -> void
{
  ASSERT(m_cmd != nullptr);
  m_cmd->append_presence_tested_command_names(actx, names, status_is_success);
}

cold fn CompoundListCondition::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  ASSERT(m_cmd != nullptr);

  /* An && or || node depends on the command before it, so only a plain sequence
     node carries its own verdict. */
  if (m_kind != Kind::None) return koshka::None;
  return m_cmd->try_static_condition_verdict(actx);
}

/* The X token of a bracketed X != Y test the node holds, null when the node
   holds anything else. */
cold static fn
node_inequality_left_operand(const CompoundListCondition *node) wontthrow
    -> const Token *
{
  const Command *held = node->command();
  if (held == nullptr) return nullptr;

  const SimpleCommand *command = held->as_simple_command();
  if (command == nullptr) return nullptr;

  let const &args = command->args();
  if (args.count() != 5) return nullptr;

  let const name = args[0]->raw_view();
  if (!name.has_value() || *name != StringView{"["}) {
    return nullptr;
  }

  let const closer = args[4]->raw_view();
  if (!closer.has_value() || *closer != StringView{"]"}) {
    return nullptr;
  }

  let const op = args[2]->raw_view();
  if (!op.has_value() || *op != StringView{"!="}) {
    return nullptr;
  }

  return args[1];
}

fn CompoundList::analyze(AnalysisContext &actx,
                         bool is_unconditional) const throws -> void
{
  top_level_sibling_carry *const carry = actx.stream_sibling_carry;
  actx.stream_sibling_carry = nullptr;
  defer { actx.stream_sibling_carry = carry; };

  /* A top-level unit ends at a newline, so the node that follows always joins
     with Kind::None. The checks that only wanted to know that a next node
     exists are answered here. */
  if (carry != nullptr && !m_nodes.is_empty()) {
    if (carry->pending_unchecked_cd.has_value())
      actx.report_diagnostic(diagnostic_id::sc2164,
                             *carry->pending_unchecked_cd);

    if (carry->pending_exec_replacement.has_value()) {
      let const next_command = m_nodes[0]->command();
      ASSERT(next_command != nullptr);
      actx.report_diagnostic(diagnostic_id::sc2093,
                             *carry->pending_exec_replacement, {},
                             next_command->source_location());
    }

    if (carry->pending_negated_command.has_value())
      actx.report_diagnostic(diagnostic_id::sc2251,
                             *carry->pending_negated_command);
  }
  if (carry != nullptr) {
    carry->pending_unchecked_cd = None;
    carry->pending_exec_replacement = None;
    carry->pending_negated_command = None;
  }

  Maybe<SourceLocation> first_directory_change =
      carry != nullptr ? carry->first_directory_change : None;
  defer
  {
    if (carry != nullptr)
      carry->first_directory_change = first_directory_change;
  };

  for (usize i = 0; i < m_nodes.count(); i++) {
    ASSERT(m_nodes[i] != nullptr);
    let const command = m_nodes[i]->command();

    /* POSIX sh reads time as a utility, so it receives the compound keyword as
       an operand and the report covers nothing, shellcheck SC2177. */
    if (actx.is_posix_sh_shebang && command != nullptr && command->is_timed() &&
        command->is_compound_command())
    {
      actx.report_diagnostic(diagnostic_id::sc2177, command->time_location());
    }

    let const simple =
        command != nullptr ? command->as_simple_command() : nullptr;
    if (simple != nullptr && !simple->args().is_empty()) {
      let const name = static_command_name(simple->args()[0]);
      if (name.has_value() && !actx.defined_functions.contains(*name) &&
          !actx.known_aliases.contains(*name))
      {
        if (*name == "cd") {
          if (i + 1 < m_nodes.count() &&
              m_nodes[i + 1]->kind() == CompoundListCondition::Kind::None)
          {
            actx.report_diagnostic(diagnostic_id::sc2164,
                                   simple->args()[0]->source_location());
          } else if (carry != nullptr && i + 1 == m_nodes.count()) {
            carry->pending_unchecked_cd = simple->args()[0]->source_location();
          }

          /* A subshell restores the directory on its own, so the return trip is
             work the shell already does, shellcheck SC2103. */
          let is_return_trip = false;
          if (simple->args().count() == 2 &&
              simple->args()[1]->kind() == Token::Kind::Word)
          {
            is_return_trip =
                static_cast<const tokens::WordToken *>(simple->args()[1])
                    ->word()
                    .to_literal_string()
                    .view() == "-";
          }

          if (!is_return_trip) {
            first_directory_change = simple->args()[0]->source_location();
          } else if (first_directory_change.has_value()) {
            actx.report_diagnostic(diagnostic_id::sc2103,
                                   *first_directory_change, {},
                                   simple->args()[0]->source_location());
            first_directory_change = None;
          }
        }
        if (*name == "exec" && simple->args().count() > 1) {
          if (i + 1 < m_nodes.count()) {
            let const next_command = m_nodes[i + 1]->command();
            ASSERT(next_command != nullptr);
            actx.report_diagnostic(diagnostic_id::sc2093,
                                   simple->args()[0]->source_location(), {},
                                   next_command->source_location());
          } else if (carry != nullptr) {
            carry->pending_exec_replacement =
                simple->args()[0]->source_location();
          }
        }
      }
    }

    if (i > 0 && i + 1 < m_nodes.count() &&
        m_nodes[i]->kind() == CompoundListCondition::Kind::And &&
        m_nodes[i + 1]->kind() == CompoundListCondition::Kind::Or)
    {
      let const middle_command = m_nodes[i]->command();
      let const middle_simple = middle_command != nullptr
                                    ? middle_command->as_simple_command()
                                    : nullptr;
      let is_test_command = false;
      if (middle_simple != nullptr && !middle_simple->args().is_empty()) {
        let const middle_name = static_command_name(middle_simple->args()[0]);
        is_test_command =
            middle_name.has_value() && get_analysis_command_info(*middle_name)
                                           .is_in_group(COMMAND_GROUP_TEST);
      }
      if (!is_test_command)
        actx.report_diagnostic(diagnostic_id::sc2015,
                               m_nodes[i]->source_location());
    }
  }

  usize repeated_append_count =
      carry != nullptr ? carry->repeated_append_count : 0;
  String repeated_append_target{
      heap_allocator(),
      carry != nullptr ? carry->repeated_append_target.view() : StringView{}};
  SourceLocation repeated_append_location =
      carry != nullptr ? carry->repeated_append_location : SourceLocation{};
  defer
  {
    if (carry != nullptr) {
      carry->repeated_append_count = repeated_append_count;
      carry->repeated_append_target = String{repeated_append_target.view()};
      carry->repeated_append_location = repeated_append_location;
    }
  };

  for (let const node : m_nodes) {
    let const command = node->command();
    let const simple =
        command != nullptr ? command->as_simple_command() : nullptr;
    String current_target{heap_allocator()};
    SourceLocation current_location{};
    if (simple != nullptr) {
      for (let const &redirection : simple->redirections()) {
        if (redirection.kind != Redirection::Kind::AppendOutput ||
            redirection.target == nullptr)
        {
          continue;
        }
        current_target = redirection.target->raw_string();
        current_location = redirection.target->source_location();
        break;
      }
    }
    if (!current_target.is_empty() &&
        current_target.view() == repeated_append_target.view())
    {
      repeated_append_count++;
    } else {
      repeated_append_target = steal(current_target);
      repeated_append_count = repeated_append_target.is_empty() ? 0 : 1;
      repeated_append_location = current_location;
    }
    if (repeated_append_count == 3)
      actx.report_diagnostic(diagnostic_id::sc2129, repeated_append_location,
                             {}, current_location);
  }

  let saved_tested_command_names = actx.tested_command_names.clone();
  const CompoundListCondition *previous_node = nullptr;
  for (usize i = 0; i < m_nodes.count(); i++) {
    let const node = m_nodes[i];
    ASSERT(node != nullptr);

    /* A [ test ends at its own bracket, so a joiner written inside one reaches
       the shell instead, shellcheck SC2107 and SC2109. */
    if (previous_node != nullptr &&
        node->kind() != CompoundListCondition::Kind::None)
    {
      const Token *unclosed_bracket = node_unclosed_test_bracket(previous_node);
      if (unclosed_bracket != nullptr) {
        actx.report_diagnostic(node->kind() == CompoundListCondition::Kind::And
                                   ? diagnostic_id::sc2107
                                   : diagnostic_id::sc2109,
                               unclosed_bracket->source_location());
      }
    }

    if (node->kind() != CompoundListCondition::Kind::And) {
      actx.tested_command_names = saved_tested_command_names.clone();
      if (node->kind() == CompoundListCondition::Kind::Or &&
          previous_node != nullptr)
      {
        previous_node->append_presence_tested_command_names(
            actx, actx.tested_command_names, false);

        /* Two inequalities on the same operand hold for every value, shellcheck
           SC2252. */
        let const before = node_inequality_left_operand(previous_node);
        let const after = node_inequality_left_operand(node);
        if (before != nullptr && after != nullptr) {
          let const before_view = before->raw_view();
          let const after_view = after->raw_view();
          if (before_view.has_value() && after_view.has_value() &&
              *before_view == *after_view)
          {
            actx.report_diagnostic(diagnostic_id::sc2252,
                                   after->source_location(), {*after_view});
          }
        }
      }
    }

    let const next_node_joins =
        i + 1 < m_nodes.count() &&
        m_nodes[i + 1]->kind() != CompoundListCondition::Kind::None;

    /* A subshell costs a process, and a brace group gives the same grouping in
       the current shell, shellcheck SC2235. */
    if ((node->kind() != CompoundListCondition::Kind::None ||
         next_node_joins) &&
        node->command() != nullptr && node->command()->as_subshell() != nullptr)
    {
      actx.report_diagnostic(diagnostic_id::sc2235,
                             node->command()->source_location());
    }

    /* A negated command outside a condition inhibits errexit and leaves its
       status unread, shellcheck SC2251. */
    if (!actx.is_analyzing_condition && node->is_negated() &&
        node->kind() == CompoundListCondition::Kind::None && !next_node_joins &&
        node->command() != nullptr)
    {
      if (i + 1 < m_nodes.count()) {
        actx.report_diagnostic(diagnostic_id::sc2251,
                               node->command()->source_location());
      } else if (carry != nullptr) {
        carry->pending_negated_command = node->command()->source_location();
      }
    }

    /* A semicolon or newline node runs whenever the list runs, an && or || node
       is conditional. */
    let const node_unconditional =
        is_unconditional && node->kind() == CompoundListCondition::Kind::None;
    let const was_command_status_observed = actx.is_command_status_observed;
    actx.is_command_status_observed =
        was_command_status_observed || next_node_joins;
    node->analyze(actx, node_unconditional);
    actx.is_command_status_observed = was_command_status_observed;
    previous_node = node;
  }
  if (!actx.should_retain_tested_command_names)
    actx.tested_command_names = steal(saved_tested_command_names);
}

fn CompoundList::append_presence_tested_command_names(
    const AnalysisContext &actx, HashSet &names,
    bool status_is_success) const throws -> void
{
  if (m_nodes.count() != 1 || m_nodes[0] == nullptr) {
    return;
  }
  m_nodes[0]->append_presence_tested_command_names(actx, names,
                                                   status_is_success);
}

cold fn CompoundList::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  /* Only a condition list of exactly one command has a verdict the whole
     condition takes. */
  if (m_nodes.count() != 1) return koshka::None;
  ASSERT(m_nodes[0] != nullptr);
  return m_nodes[0]->try_static_condition_verdict(actx);
}

fn IfStatement::analyze(AnalysisContext &actx,
                        bool is_unconditional) const throws -> void
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_then != nullptr);

  m_condition->analyze(actx, is_unconditional);
  let before_then = actx.variable_occurrence_assignments.clone();
  let before_then_inherited =
      actx.inherited_variable_occurrence_assignments.clone();
  m_then->analyze(actx, false);
  let const after_then = actx.variable_occurrence_assignments.clone();
  let const after_then_inherited =
      actx.inherited_variable_occurrence_assignments.clone();

  actx.variable_occurrence_assignments = steal(before_then);
  actx.inherited_variable_occurrence_assignments = steal(before_then_inherited);
  if (m_otherwise != nullptr) m_otherwise->analyze(actx, false);

  let do_merge_occurrence_states = [](auto &merged_states,
                                      const auto &then_states) throws -> void {
    merged_states.for_each(
        [&](StringView name, variable_occurrence_state &state) {
          if (then_states.find(name) == nullptr)
            state.is_definitely_set = false;
        });
    then_states.for_each(
        [&](StringView name, const variable_occurrence_state &then_state) {
          let *state = merged_states.find(name);
          if (state == nullptr) {
            let merged_state = then_state;
            merged_state.is_definitely_set = false;
            merged_states.set(name, steal(merged_state));
            return;
          }

          for (let const assignment_index : then_state.assignment_indices) {
            if (!state->assignment_indices.find(assignment_index).has_value())
              state->assignment_indices.push(assignment_index);
          }
          state->is_definitely_set =
              state->is_definitely_set && then_state.is_definitely_set;
        });
  };
  do_merge_occurrence_states(actx.variable_occurrence_assignments, after_then);
  do_merge_occurrence_states(actx.inherited_variable_occurrence_assignments,
                             after_then_inherited);

  actx.constant_variables.clear();
}

} /* namespace expressions */

} /* namespace koshka */
