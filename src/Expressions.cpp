#include "Expressions.hpp"

#include "Arena.hpp"
#include "Builtin.hpp"
#include "Cli.hpp"
#include "Colors.hpp"
#include "Common.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
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

fn indent_for_layer(usize layer) throws -> String
{
  let pad = String{heap_allocator()};
  for (usize i = 0; i < layer; i++)
    pad += EXPRESSION_AST_INDENT;
  return pad;
}

Expression::Expression(SourceLocation location)
    : m_location(steal(location)),
      m_source_end_position(m_location.position + m_location.length)
{}

pure fn Expression::source_location() const wontthrow -> SourceLocation
{
  return m_location;
}

pure fn Expression::source_end_position() const wontthrow -> usize
{
  return m_source_end_position;
}

fn Expression::set_source_end_position(usize position) wontthrow -> void
{
  m_source_end_position = static_cast<u32>(position);
}

cold fn Expression::to_ast_string(usize layer) const throws -> String
{
  return indent_for_layer(layer) + "[" + to_string() + "]";
}

hot flatten fn Expression::evaluate(EvalContext &cxt) const throws -> i64
{
  /* The check runs before every node, so a running command stops promptly and
     control returns to the prompt. */
  if (os::INTERRUPT_REQUESTED) {
    os::INTERRUPT_REQUESTED = 0;
    throw InterruptErrorWithLocation{source_location()};
  }
  /* A trapped signal runs its action here at the command boundary. */
  if (os::SIGNAL_PENDING) cxt.run_pending_traps();
  cxt.add_evaluated_expression();
  return evaluate_impl(cxt);
}

fn Expression::operator delete(opaque *pointer) wontthrow -> void
{
  if (is_arena_pointer(pointer)) return;
  ::operator delete(pointer);
}

fn AnalysisContext::warn(diagnostic_id id, const SourceLocation &location,
                         StringView message, StringView suggestion,
                         diagnostic_tier tier,
                         const Maybe<SourceLocation> &related_location,
                         StringView related_message) throws -> void
{
  if (!should_report(tier)) return;

  reported_warning_count++;

  pending_warnings.push(pending_analysis_warning{
      id, location, String{message}, String{suggestion}, related_location,
      String{related_message}});
}

fn AnalysisContext::flush_warnings() throws -> void
{
  if (pending_warnings.is_empty()) return;

  if (eval_context != nullptr && colors::stderr_wants_color()) {
    let positions = ArrayList<usize>{heap_allocator()};
    positions.reserve(pending_warnings.count() * 2);
    for (let const &warning : pending_warnings) {
      if (warning.location.position <= source.length)
        positions.push(warning.location.position);
      if (warning.related_location.has_value() &&
          warning.related_location->position <= source.length)
      {
        positions.push(warning.related_location->position);
      }
    }
    positions.sort();

    let *cache = eval_context->get_or_create_diagnostic_highlight_cache();
    Maybe<usize> previous_line_start;
    for (let const position : positions) {
      let const line = utils::source_line_position_at(source, position);
      if (previous_line_start.has_value() &&
          *previous_line_start == line.line_start)
      {
        continue;
      }
      cache->spans_for(source, line.line_start, line.line_end, *eval_context);
      previous_line_start = line.line_start;
    }
  }

  for (let const &warning : pending_warnings) {
    if (diagnostic_sink != nullptr) {
      let source_name = String{heap_allocator()};
      if (let const name = warning.location.get_filename(); name.has_value())
        source_name = String{*name};
      let related_source_name = String{heap_allocator()};
      if (warning.related_location.has_value()) {
        if (let const related_name = warning.related_location->get_filename();
            related_name.has_value())
        {
          related_source_name = String{*related_name};
        }
      }
      diagnostic_sink->push(source_diagnostic{
          warning.id, error_severity::Warning, warning.location,
          steal(source_name), warning.message.clone(),
          warning.suggestion.clone(), warning.related_location,
          steal(related_source_name), warning.related_message.clone(),
          source_fixes_for_diagnostic(warning.id, source, warning.location)});
      continue;
    }
    if (warning.related_location.has_value()) {
      let const located =
          WarningWithLocation{warning.location, warning.message};
      show_message(located.to_string(source, eval_context));

      let const related = ErrorWithLocationAndDetails{warning.location,
                                                      {},
                                                      *warning.related_location,
                                                      warning.related_message,
                                                      warning.suggestion};
      show_message(related.details_to_string(source, eval_context));
    } else {
      let const located = WarningWithLocationAndDetails{
          warning.location, warning.message, warning.suggestion};
      show_message(located.to_string(source, eval_context));
    }
    print_script_backtrace_if_rooted(warning.location);
  }
  pending_warnings.clear();
}

cold fn print_analysis_diagnostic_summary(
    const analysis_diagnostic_totals &totals) throws -> void
{
  if (totals.warning_count + totals.error_count < 2) return;

  let const wants_color = colors::stderr_wants_color();
  let const warning_color = wants_color ? colors::ansi::YELLOW : StringView{};
  let const error_color =
      wants_color ? colors::ansi::BOLD_BRIGHT_RED : StringView{};
  let const reset = wants_color ? colors::ansi::RESET : StringView{};

  let summary = String{"Encountered "};

  if (totals.warning_count > 0) {
    summary.append(warning_color);
    summary.append(String::from(totals.warning_count, heap_allocator()));
    summary.append(totals.warning_count == 1 ? " warning" : " warnings");
    summary.append(reset);
  }

  if (totals.warning_count > 0 && totals.error_count > 0) {
    summary.append(" and ");
  }

  if (totals.error_count > 0) {
    summary.append(error_color);
    summary.append(String::from(totals.error_count, heap_allocator()));
    summary.append(totals.error_count == 1 ? " error" : " errors");
    summary.append(reset);
  }

  summary.append(".");

  show_message(summary.view());
}

cold fn AnalysisContext::print_diagnostic_summary() const throws -> void
{
  print_analysis_diagnostic_summary(
      {reported_warning_count, reported_error_count});
}

cold fn AnalysisContext::print_optimizer_summary() const throws -> void
{
  if (!should_report_optimizer_diagnostics) return;

  let const wants_color = colors::stderr_wants_color();

  let summary = String{"Eliminated "};
  if (wants_color) summary.append(colors::ansi::BLUE);
  summary.append(String::from(optimizer_eliminated_count, heap_allocator()));
  summary.append(optimizer_eliminated_count == 1 ? " statement"
                                                 : " statements");
  if (wants_color) summary.append(colors::ansi::RESET);
  summary.append(".");

  show_message(summary.view());
}

pure fn AnalysisContext::should_report(diagnostic_id id) const wontthrow -> bool
{
  return should_report(get_diagnostic_definition(id).tier);
}

pure fn AnalysisContext::should_report(diagnostic_tier tier) const wontthrow
    -> bool
{
  if (tier == diagnostic_tier::Annoying && !should_emit_annoying_diagnostics) {
    return false;
  }
  if (is_default_mood) return true;

  u8 required_level = 0;
  switch (tier) {
  case diagnostic_tier::Strict: required_level = 1; break;
  case diagnostic_tier::Lenient: required_level = 2; break;
  case diagnostic_tier::Annoying: required_level = 3; break;
  }

  return warning_level >= required_level;
}

fn AnalysisContext::report_diagnostic(
    diagnostic_id id, const SourceLocation &location,
    std::initializer_list<StringView> arguments,
    const Maybe<SourceLocation> &related_location) throws -> bool
{
  if (!should_report(id)) return false;
  if (is_diagnostic_suppressed(id, location)) return false;

  let const &definition = get_diagnostic_definition(id);
  let message =
      format_diagnostic_template(definition.message_template, arguments);
  append_diagnostic_code(message, definition.shellcheck_code);

  let suggestion = String{heap_allocator()};
  let related_message = String{heap_allocator()};

  if (definition.suggestion_template.has_value()) {
    suggestion =
        format_diagnostic_template(*definition.suggestion_template, arguments);
  }
  if (definition.related_template.has_value()) {
    related_message =
        format_diagnostic_template(*definition.related_template, arguments);
  }

  switch (definition.delivery) {
  case diagnostic_delivery::Policy:
    fail(id, location, message.view(), suggestion.view(), definition.tier,
         related_location, related_message.view());
    break;
  case diagnostic_delivery::Warning:
    warn(id, location, message.view(), suggestion.view(), definition.tier,
         related_location, related_message.view());
    break;
  }

  return true;
}

cold fn AnalysisContext::trace_optimizer_line(StringView message) const throws
    -> void
{
  if (!should_report_optimizer_diagnostics) return;
  print_error("[optimizer] ");
  print_error(message);
  print_error("\n");
}

cold fn AnalysisContext::print_script_backtrace_if_rooted(
    const SourceLocation &location) const throws -> void
{
  if (eval_context != nullptr) eval_context->print_source_backtrace(location);
}

fn AnalysisContext::fail(diagnostic_id id, const SourceLocation &location,
                         StringView message, StringView suggestion,
                         diagnostic_tier tier,
                         const Maybe<SourceLocation> &related_location,
                         StringView related_message) throws -> void
{
  if (!is_default_mood) {
    if (should_report(tier))
      warn(id, location, message, suggestion, tier, related_location,
           related_message);
    return;
  }

  if (tier == diagnostic_tier::Annoying) {
    warn(id, location, message, suggestion, tier, related_location,
         related_message);
    return;
  }

  u8 demote_at_level = 0;
  switch (tier) {
  case diagnostic_tier::Strict: demote_at_level = 3; break;
  case diagnostic_tier::Lenient: demote_at_level = 2; break;
  case diagnostic_tier::Annoying: break;
  }

  if (warning_level >= demote_at_level) {
    warn(id, location, message, suggestion, tier, related_location,
         related_message);
    return;
  }

  flush_warnings();
  reported_error_count++;

  if (diagnostic_sink != nullptr) {
    let source_name = String{heap_allocator()};
    if (let const name = location.get_filename(); name.has_value())
      source_name = String{*name};
    let related_source_name = String{heap_allocator()};
    if (related_location.has_value()) {
      if (let const related_name = related_location->get_filename();
          related_name.has_value())
      {
        related_source_name = String{*related_name};
      }
    }
    diagnostic_sink->push(source_diagnostic{
        id, error_severity::Error, location, steal(source_name),
        String{message}, String{suggestion}, related_location,
        steal(related_source_name), String{related_message},
        source_fixes_for_diagnostic(id, source, location)});
    has_fatal = true;
    return;
  }

  if (related_location.has_value()) {
    let const located = ErrorWithLocation{location, message};
    show_message(located.to_string(source, eval_context));

    let const related = ErrorWithLocationAndDetails{
        location, {}, *related_location, related_message, suggestion};
    show_message(related.details_to_string(source, eval_context));
  } else {
    let const located =
        ErrorWithLocationAndDetails{location, message, suggestion};
    show_message(located.to_string(source, eval_context));
  }
  print_script_backtrace_if_rooted(location);
  has_fatal = true;
}

pure fn AnalysisContext::is_diagnostic_suppressed(
    diagnostic_id id, const SourceLocation &location) const wontthrow -> bool
{
  if (shellcheck_suppressions == nullptr) return false;

  for (let const &suppression : *shellcheck_suppressions) {
    if (location.position < suppression.start_position ||
        location.position >= suppression.end_position)
    {
      continue;
    }

    for (let const &selector : suppression.selectors) {
      if (shellcheck_selector_disables(selector, source, id)) return true;
    }
  }

  return false;
}

fn AnalysisContext::note_variable_assignment(
    StringView name, const SourceLocation &location) throws -> void
{
  if (name.is_empty()) return;

  assigned_names_so_far.set(name, location);
  if (current_source_effects != nullptr)
    current_source_effects->assigned_names.add(name);

  if (const SourceLocation *read_location = reads_before_assignment.find(name);
      read_location != nullptr)
  {
    report_diagnostic(diagnostic_id::use_before_assign, *read_location, {name},
                      location);
    reads_before_assignment.erase(name);
  }
}

/* One pathological value is a likelier memory hog than the record count. */
static constexpr usize RECORDED_LITERAL_LENGTH_LIMIT = 256;

fn AnalysisContext::note_variable_assignment_record(
    StringView name, const Word *value_word, const SourceLocation &location,
    bool is_conditional, bool is_append) throws -> void
{
  if (symbol_records == nullptr) return;
  if (name.is_empty()) return;

  let literal_value = Maybe<String>{None};
  if (value_word != nullptr) {
    let folded = optimizer::literal_word_value(*value_word);
    if (folded.has_value()) {
      literal_value = folded->count() > RECORDED_LITERAL_LENGTH_LIMIT
                          ? String{folded->view().substring_of_length(
                                0, RECORDED_LITERAL_LENGTH_LIMIT)}
                          : steal(*folded);
    }
  }

  symbol_records->assignments.push(variable_assignment_record{
      String{name}, steal(literal_value), is_conditional, is_append,
      value_word == nullptr, location.position, location.length,
      assignment_binder::Assignment});
}

fn AnalysisContext::note_variable_binding_record(StringView name,
                                                 const SourceLocation &location,
                                                 assignment_binder binder,
                                                 bool is_conditional) throws
    -> void
{
  if (symbol_records == nullptr) return;
  if (name.is_empty()) return;

  symbol_records->assignments.push(variable_assignment_record{
      String{name}, None, is_conditional, false, false, location.position,
      location.length, binder});
}

fn AnalysisContext::note_variable_occurrence(StringView name,
                                             const SourceLocation &location,
                                             variable_occurrence_kind kind,
                                             bool is_unresolved,
                                             bool is_append) throws -> void
{
  if (symbol_records == nullptr) return;
  if (name.is_empty() || location.length == 0) return;

  if (name.length > 1 && name[0] == '#') name = name.substring(1);
  if (!lexer::word_is_variable_name(name) && !reference_names_positional(name))
    name = expressions::operand_target_name(name);
  if (name.is_empty()) return;

  let const function_definition_index = active_function_definition_index;
  let const *current_state = kind == variable_occurrence_kind::Reference
                                 ? variable_occurrence_assignments.find(name)
                                 : nullptr;
  let const has_inherited_function_path =
      function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION &&
      kind == variable_occurrence_kind::Reference &&
      (current_state == nullptr || current_state->has_inherited_path);
  let occurrence = variable_occurrence_record{String{name},
                                              location.position,
                                              location.length,
                                              kind,
                                              is_unresolved,
                                              false,
                                              function_definition_index,
                                              false,
                                              false,
                                              has_inherited_function_path};

  if (kind == variable_occurrence_kind::Assignment) {
    if (is_append) {
      let const *prior_state = variable_occurrence_assignments.find(name);
      if (prior_state == nullptr)
        prior_state = inherited_variable_occurrence_assignments.find(name);
      if (prior_state != nullptr) {
        for (let const assignment_index : prior_state->assignment_indices)
          symbol_records->variable_occurrences[assignment_index].is_unused =
              false;
      }
    }

    let state = variable_occurrence_state{};
    state.assignment_indices.push(symbol_records->variable_occurrences.count());
    state.is_definitely_set = true;
    state.is_definitely_unset = false;
    state.has_inherited_path = false;
    variable_occurrence_assignments.set(name, steal(state));
    occurrence.is_unused = true;
    if (function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION)
      function_definitions[function_definition_index].affected_names.add(name);
  } else if (kind == variable_occurrence_kind::Reference) {
    let const *state = variable_occurrence_assignments.find(name);
    if (state != nullptr) {
      for (let const assignment_index : state->assignment_indices)
        symbol_records->variable_occurrences[assignment_index].is_unused =
            false;
    } else {
      state = inherited_variable_occurrence_assignments.find(name);
      if (state != nullptr) {
        for (let const assignment_index : state->assignment_indices)
          symbol_records->variable_occurrences[assignment_index].is_unused =
              false;
      }
    }

    occurrence.is_unresolved =
        is_unresolved || (state != nullptr && !state->is_definitely_set) ||
        (state == nullptr && !expressions::is_shell_maintained_variable(name) &&
         !(eval_context != nullptr && eval_context->has_variable_name(name)) &&
         !os::get_environment_variable(name).has_value());
  } else {
    let const *state = variable_occurrence_assignments.find(name);
    if (state == nullptr)
      state = inherited_variable_occurrence_assignments.find(name);
    occurrence.is_unresolved = state == nullptr || !state->is_definitely_set;

    let unset_state = variable_occurrence_state{};
    unset_state.is_definitely_unset = true;
    unset_state.has_unset_path = true;
    variable_occurrence_assignments.set(name, steal(unset_state));
    inherited_variable_occurrence_assignments.erase(name);
    if (function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION)
      function_definitions[function_definition_index].affected_names.add(name);
  }

  symbol_records->variable_occurrences.push(steal(occurrence));
}

fn AnalysisContext::apply_called_function(
    StringView name, const SourceLocation &call_location) throws -> void
{
  if (symbol_records == nullptr) return;

  if (active_function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION &&
      function_definitions[active_function_definition_index].name.view() ==
          name)
  {
    return;
  }

  let const *selected_definition_index =
      latest_function_definition_indices.find(name);
  if (selected_definition_index == nullptr) return;

  let const &selected_definition =
      function_definitions[*selected_definition_index];
  if (!selected_definition.is_analysis_complete ||
      selected_definition.location.position > call_location.position)
  {
    return;
  }

  let &definition = function_definitions[*selected_definition_index];
  definition.has_been_called = true;
  if (active_function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION) {
    let &active_definition =
        function_definitions[active_function_definition_index];
    definition.affected_names.for_each([&](StringView affected_name) {
      if (!definition.local_names.contains(affected_name))
        active_definition.affected_names.add(affected_name);
    });
  }

  for (usize occurrence_index = definition.occurrence_start;
       occurrence_index < definition.occurrence_end; occurrence_index++)
  {
    let &occurrence = symbol_records->variable_occurrences[occurrence_index];
    if (occurrence.kind != variable_occurrence_kind::Reference ||
        occurrence.function_definition_index != *selected_definition_index ||
        !occurrence.has_inherited_function_path)
    {
      continue;
    }
    if (active_function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION) {
      occurrence.function_definition_index = active_function_definition_index;
      continue;
    }

    let const *state =
        variable_occurrence_assignments.find(occurrence.name.view());
    if (state == nullptr)
      state = inherited_variable_occurrence_assignments.find(
          occurrence.name.view());
    if (state != nullptr && state->is_definitely_set) {
      occurrence.has_resolved_function_path = true;
      for (let const assignment_index : state->assignment_indices)
        symbol_records->variable_occurrences[assignment_index].is_unused =
            false;
    } else {
      occurrence.has_unresolved_function_path = true;
    }
  }

  definition.affected_names.for_each([&](StringView affected_name) {
    if (definition.local_names.contains(affected_name)) return;

    let const *exit_state = definition.exit_states.find(affected_name);
    if (exit_state == nullptr) return;

    if (exit_state->is_definitely_set || exit_state->is_definitely_unset) {
      variable_occurrence_assignments.set(affected_name, *exit_state);
      inherited_variable_occurrence_assignments.erase(affected_name);
      return;
    }

    let const *caller_state =
        variable_occurrence_assignments.find(affected_name);
    if (caller_state == nullptr) {
      caller_state =
          inherited_variable_occurrence_assignments.find(affected_name);
    }

    if (caller_state == nullptr) {
      variable_occurrence_assignments.set(affected_name, *exit_state);
      inherited_variable_occurrence_assignments.erase(affected_name);
      return;
    }

    let merged_state = *caller_state;
    if (merged_state.assignment_indices.is_empty()) {
      merged_state.assignment_indices = exit_state->assignment_indices.clone();
    } else if (!exit_state->assignment_indices.is_empty()) {
      let merged_assignment_indices = ArrayList<usize>{heap_allocator()};
      merged_assignment_indices.reserve(
          merged_state.assignment_indices.count() +
          exit_state->assignment_indices.count());
      usize caller_index = 0;
      usize exit_index = 0;
      while (caller_index < merged_state.assignment_indices.count() ||
             exit_index < exit_state->assignment_indices.count())
      {
        let const has_caller_index =
            caller_index < merged_state.assignment_indices.count();
        let const has_exit_index =
            exit_index < exit_state->assignment_indices.count();
        usize assignment_index;
        if (!has_exit_index || (has_caller_index &&
                                merged_state.assignment_indices[caller_index] <
                                    exit_state->assignment_indices[exit_index]))
        {
          assignment_index = merged_state.assignment_indices[caller_index++];
        } else if (!has_caller_index ||
                   exit_state->assignment_indices[exit_index] <
                       merged_state.assignment_indices[caller_index])
        {
          assignment_index = exit_state->assignment_indices[exit_index++];
        } else {
          assignment_index = merged_state.assignment_indices[caller_index++];
          exit_index++;
        }
        if (merged_assignment_indices.is_empty() ||
            merged_assignment_indices.back() != assignment_index)
          merged_assignment_indices.push(assignment_index);
      }
      merged_state.assignment_indices = steal(merged_assignment_indices);
    }
    merged_state.is_definitely_set =
        merged_state.is_definitely_set && exit_state->is_definitely_set;
    merged_state.is_definitely_unset = false;
    merged_state.has_unset_path =
        merged_state.has_unset_path || exit_state->has_unset_path;
    merged_state.has_inherited_path =
        merged_state.has_inherited_path || exit_state->has_inherited_path;
    variable_occurrence_assignments.set(affected_name, steal(merged_state));
    inherited_variable_occurrence_assignments.erase(affected_name);
  });
}

static fn resolve_function_occurrence_states(
    analysis_symbol_records &symbol_records) wontthrow -> void
{
  for (let &occurrence : symbol_records.variable_occurrences) {
    if (occurrence.kind != variable_occurrence_kind::Reference) continue;
    if (!occurrence.has_inherited_function_path) continue;

    occurrence.is_unresolved = occurrence.has_unresolved_function_path ||
                               !occurrence.has_resolved_function_path;
  }
}

fn AnalysisContext::note_function_body_record(StringView name,
                                              usize name_position,
                                              usize body_position,
                                              usize body_end_position) throws
    -> void
{
  if (symbol_records == nullptr) return;
  if (name.is_empty()) return;

  symbol_records->functions.push(function_body_record{
      String{name}, name_position, body_position, body_end_position});
}

/* The name an assign form ${name=value} or ${name:=value} writes back, or an
   empty view for every other expansion. */
static pure fn assign_form_target_name(StringView expansion_text) wontthrow
    -> StringView
{
  let const name = expressions::operand_target_name(expansion_text);
  if (!lexer::word_is_variable_name(name)) return StringView{};

  let remainder = expansion_text.substring(name.length);
  if (!remainder.is_empty() && remainder[0] == ':')
    remainder = remainder.substring(1);

  if (remainder.is_empty() || remainder[0] != '=') {
    return StringView{};
  }

  return name;
}

fn AnalysisContext::note_variable_read(StringView name,
                                       const SourceLocation &location,
                                       bool is_top_level_unconditional) throws
    -> void
{
  if (!is_top_level_unconditional) return;
  if (has_seen_runtime_definer) return;

  if (!lexer::word_is_variable_name(name)) {
    let const assigned = assign_form_target_name(name);
    if (!assigned.is_empty()) note_variable_assignment(assigned, location);

    return;
  }

  if (assigned_names_so_far.find(name) != nullptr) return;
  if (inherited_assigned_names.contains(name)) return;
  if (function_local_names.find(name) != nullptr) return;
  if (global_assigned_names.find(name) != nullptr) return;
  if (reads_before_assignment.find(name) != nullptr) return;
  if (expressions::is_shell_maintained_variable(name)) return;

  if (eval_context != nullptr &&
      (eval_context->is_exported(name) ||
       eval_context->lookup_shell_variable(name) != nullptr))
  {
    return;
  }

  reads_before_assignment.set(name, location);
}

cold fn report_command_resolution_error(
    EvalContext &cxt, const CommandResolutionErrorWithLocation &e) throws
    -> void
{
  const String *source = cxt.current_source();
  show_message(
      e.to_string(source != nullptr ? source->view() : StringView{}, &cxt));
  cxt.print_source_backtrace(e.location());
}

fn window_function_body_error(EvalContext &cxt,
                              ErrorWithLocation &error) wontthrow
    -> Maybe<StringView>
{
  let const resolved = cxt.resolve_render_source(error.location());
  if (!resolved.is_windowed || resolved.text == nullptr) {
    return None;
  }

  let rebased = error.location();
  rebased.position =
      static_cast<u32>(resolved.to_render_position(rebased.position));
  rebased.source_name_index = resolved.source_name_index;
  if (rebased.position > resolved.text->count()) return None;

  error.set_location(rebased);
  error.set_line_offset(resolved.line_offset);
  return resolved.text->view();
}

fn Expression::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  unused(actx);
  unused(is_unconditional);
}

fn Expression::append_presence_tested_command_names(
    const AnalysisContext &actx, HashSet &names,
    bool status_is_success) const throws -> void
{
  unused(actx);
  unused(names);
  unused(status_is_success);
}

fn Expression::is_simple_command() const wontthrow -> bool { return false; }

fn Expression::is_dummy() const wontthrow -> bool { return false; }

fn Expression::as_if_clause() const wontthrow -> const expressions::IfClause *
{
  return nullptr;
}

fn Expression::as_while_loop() const wontthrow -> const expressions::WhileLoop *
{
  return nullptr;
}

fn Expression::as_assign_command() const wontthrow
    -> const expressions::AssignCommand *
{
  return nullptr;
}

fn Expression::as_simple_command() const wontthrow
    -> const expressions::SimpleCommand *
{
  return nullptr;
}

fn Expression::as_for_loop() const wontthrow -> const expressions::ForLoop *
{
  return nullptr;
}

fn Expression::as_cstyle_for_loop() const wontthrow
    -> const expressions::CStyleForLoop *
{
  return nullptr;
}

fn Expression::as_subshell() const wontthrow -> const expressions::Subshell *
{
  return nullptr;
}

fn Expression::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  unused(actx);
  return koshka::None;
}

fn Expression::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  unused(cxt);
  unused(active_functions);
  return false;
}

fn static_command_name(const Token *token) throws -> Maybe<StringView>
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return koshka::None;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();

  for (let const &segment : word.segments) {
    /* Any expansion segment makes the name a runtime value, so its raw bytes
       must not pass for the program text. */
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText &&
        segment.kind != WordSegment::Kind::UnquotedText)
    {
      return koshka::None;
    }
    if (segment.kind == WordSegment::Kind::UnquotedText) {
      for (usize i = 0; i < segment.text.count(); i++) {
        if (lexer::is_expandable_char(segment.text[i])) return koshka::None;
      }
    }
  }

  return word.constant_value();
}

fn normalized_relative_executable_path(StringView path) throws -> Maybe<String>
{
  if (path.is_empty()) return None;

  let const typed_path = Path{path};
  if (!typed_path.is_relative()) return None;

  let normalized = typed_path.normalized().text();
  if (normalized == ".") return None;

  return normalized;
}

fn borrowed_token_text(const Token *token, String &storage) throws -> StringView
{
  ASSERT(token != nullptr);

  let const borrowed = token->raw_view();
  if (borrowed.has_value()) return *borrowed;

  storage = token->raw_string();

  return storage.view();
}

static constexpr PackedStringKey SOURCE_LOCATION_VARIABLE_KEYS[] = {
    SSK("HOME"),
    SSK("OLDPWD"),
    SSK("PWD"),
};
static constexpr StaticStringSet SOURCE_LOCATION_VARIABLES{
    SOURCE_LOCATION_VARIABLE_KEYS};

pure fn is_source_location_variable(StringView name) wontthrow -> bool
{
  return SOURCE_LOCATION_VARIABLES.contains(name);
}

fn expanded_command_path(StringView name, Allocator allocator) throws -> String
{
  if (let const expanded = utils::expand_leading_tilde_path(name);
      expanded.has_value())
  {
    return String{allocator, expanded->view()};
  }
  return String{allocator, name};
}

fn wrapped_command_index(command_name_id wrapper_id,
                         const ArrayList<const Token *> &args) throws
    -> Maybe<usize>
{
  if (args.count() < 2) return None;

  if (wrapper_id == command_name_id::Builtin) {
    let const first = static_command_name(args[1]);
    if (first.has_value() && *first == "--")
      return args.count() > 2 ? Maybe<usize>{2} : None;

    return 1;
  }

  if (wrapper_id != command_name_id::Command) return None;

  for (usize argument_index = 1; argument_index < args.count();
       argument_index++)
  {
    let const argument = static_command_name(args[argument_index]);
    if (!argument.has_value()) {
      return args[argument_index]->kind() == Token::Kind::Word
                 ? None
                 : Maybe<usize>{argument_index};
    }
    if (*argument == "--") {
      argument_index++;
      return argument_index < args.count() ? Maybe<usize>{argument_index}
                                           : None;
    }
    if (*argument == "-p") continue;
    if (argument->starts_with("-")) return None;
    return argument_index;
  }

  return None;
}

fn apply_followed_source_effects(AnalysisContext &actx,
                                 const followed_source_effects &effects,
                                 bool should_merge_parent_state,
                                 bool should_merge_parent_uncertainty) throws
    -> void
{
  if (should_merge_parent_state) {
    effects.defined_functions.for_each(
        [&actx](StringView name) { actx.add_defined_function(name); });
    effects.known_aliases.for_each(
        [&actx](StringView name) { actx.add_known_alias(name); });
    effects.assigned_names.for_each([&actx](StringView name) {
      actx.inherited_assigned_names.add(name);
      if (actx.current_source_effects != nullptr)
        actx.current_source_effects->assigned_names.add(name);
    });
    effects.global_assigned_names.for_each([&actx](StringView name) {
      actx.inherited_global_assigned_names.add(name);
      if (actx.current_source_effects != nullptr)
        actx.current_source_effects->global_assigned_names.add(name);
    });
    effects.array_valued_names.for_each(
        [&actx](StringView name) { actx.add_array_valued_name(name); });
  }

  if (should_merge_parent_uncertainty) {
    if (effects.has_seen_runtime_definer) actx.mark_runtime_definer_seen();
    if (effects.has_unknown_path)
      actx.mark_path_unknown(effects.should_silence_unresolved_commands);
    if (effects.has_unknown_working_directory)
      actx.mark_working_directory_unknown();
  }
  actx.has_fatal = actx.has_fatal || effects.has_fatal;
}

fn analyze_followed_source(AnalysisContext &actx,
                           const ArrayList<const Token *> &args,
                           usize command_index, bool should_merge_parent_state,
                           bool should_merge_parent_uncertainty) throws -> bool
{
  if (actx.followed_source_paths == nullptr ||
      actx.followed_source_effects_cache == nullptr ||
      actx.eval_context == nullptr || AST_ARENA == nullptr ||
      command_index + 1 >= args.count())
  {
    return true;
  }

  let path_index = command_index + 1;
  let const option = static_command_name(args[path_index]);
  if (option.has_value() && *option == "--help") return true;
  if (option.has_value() && *option == "--") path_index++;
  if (path_index >= args.count()) return true;

  /* An unread source may have edited the search path or the working
     directory. */
  let const do_give_up_on_source = [&actx]() throws -> bool {
    actx.mark_path_unknown(false);
    actx.mark_working_directory_unknown();
    return false;
  };

  let const literal_path = static_command_name(args[path_index]);
  if (!literal_path.has_value()) return do_give_up_on_source();

  bool should_expand_tilde = false;
  if (args[path_index]->kind() == Token::Kind::Word) {
    let const &word =
        static_cast<const tokens::WordToken *>(args[path_index])->word();
    if (!word.segments.is_empty() &&
        word.segments.front().is_tilde_candidate() &&
        !word.segments.front().text.is_empty() &&
        word.segments.front().text.first_character() == '~')
    {
      should_expand_tilde = true;
    }
  }
  if (should_expand_tilde && actx.has_unknown_working_directory) {
    return false;
  }
  let const source_path = Path{*literal_path};
  if (!should_expand_tilde && !source_path.is_absolute()) {
    if (os::has_directory_separator(*literal_path)) {
      if (actx.has_unknown_working_directory) return false;
    } else if (actx.has_unknown_path || actx.has_unknown_working_directory) {
      return false;
    }
  }
  let resolved_path = actx.eval_context->resolve_source_path(
      *literal_path, should_expand_tilde);
  if (!resolved_path.has_value()) return do_give_up_on_source();

  let canonical_path = os::canonical_path(*resolved_path);
  if (!canonical_path.has_value()) return do_give_up_on_source();

  if (let const *effects = actx.followed_source_effects_cache->find(
          canonical_path->text().view());
      effects != nullptr)
  {
    apply_followed_source_effects(actx, *effects, should_merge_parent_state,
                                  should_merge_parent_uncertainty);
    return should_merge_parent_state;
  }

  let contents = actx.source_provider != nullptr
                     ? actx.source_provider->read_source(*canonical_path)
                     : Maybe<String>{None};
  if (!contents.has_value()) contents = canonical_path->read_entire_file();
  if (!contents.has_value()) return do_give_up_on_source();
  contents->normalize_crlf_line_endings();

  if (!actx.followed_source_paths->add(canonical_path->text().view()))
    return false;

  let const arena_mark = AST_ARENA->mark();
  defer { AST_ARENA->release(arena_mark); };
  let *previous_function_arena = FUNCTION_ARENA;
  FUNCTION_ARENA = nullptr;
  defer { FUNCTION_ARENA = previous_function_arena; };

  let parser = Parser{
      Lexer{contents->view(), *AST_ARENA, false, canonical_path->text().view(),
            actx.eval_context->mood()}
  };
  parser.set_should_collect_analysis_scopes(true);

  let parse_errors = ArrayList<String>{heap_allocator()};
  let const child_diagnostic_start =
      actx.diagnostic_sink != nullptr ? actx.diagnostic_sink->count() : 0;
  let const ast = parser.construct_ast(parse_errors, actx.eval_context,
                                       actx.diagnostic_sink);
  if (!parse_errors.is_empty()) {
    if (actx.diagnostic_sink != nullptr) {
      for (usize index = child_diagnostic_start;
           index < actx.diagnostic_sink->count(); index++)
      {
        let &diagnostic = (*actx.diagnostic_sink)[index];
        if (diagnostic.source_name.is_empty())
          diagnostic.source_name = canonical_path->text();
      }
    }
    if (actx.diagnostic_sink == nullptr)
      for (let const &error : parse_errors)
        show_message(error);
    actx.has_fatal = true;
    followed_source_effects effects{};
    effects.has_fatal = true;
    actx.followed_source_effects_cache->set(canonical_path->text().view(),
                                            steal(effects));
    return true;
  }

  let const shellcheck_suppressions = parser.take_shellcheck_suppressions();
  let const scope_definitions = parser.take_analysis_scope_definitions();
  let const directive_spans = parser.take_shellcheck_directive_spans();
  let const heredoc_misses = parser.take_heredoc_terminator_misses();
  /* A child that skips its own nested sources records partial effects, wrong
     for a later visit that carries no uncertainty. */
  let const was_analyzed_under_uncertainty =
      actx.has_unknown_path || actx.has_unknown_working_directory;

  followed_source_effects effects{};
  let const analyzed = analyze_ast(
      ast, contents->view(), actx.defined_functions, actx.known_aliases,
      actx.eval_context, actx.warning_level,
      actx.should_silence_unresolved_commands, actx.is_default_mood,
      actx.should_emit_annoying_diagnostics, shellcheck_suppressions,
      scope_definitions, directive_spans, heredoc_misses, false,
      actx.should_report_optimizer_diagnostics, actx.followed_source_paths,
      actx.followed_source_effects_cache, &actx, nullptr,
      should_merge_parent_state, should_merge_parent_uncertainty, &effects,
      actx.diagnostic_sink, actx.source_provider);
  if (actx.diagnostic_sink != nullptr) {
    for (usize index = child_diagnostic_start;
         index < actx.diagnostic_sink->count(); index++)
    {
      let &diagnostic = (*actx.diagnostic_sink)[index];
      if (diagnostic.source_name.is_empty())
        diagnostic.source_name = canonical_path->text();
    }
  }
  if (!analyzed) actx.has_fatal = true;
  if (!was_analyzed_under_uncertainty) {
    actx.followed_source_effects_cache->set(canonical_path->text().view(),
                                            steal(effects));
  }

  return should_merge_parent_state;
}

fn command_resolves(
    StringView name, const SourceLocation &location,
    const AnalysisContext &actx,
    Maybe<utils::unavailable_path_source_component> &unavailable) throws -> bool
{
  if (name.is_empty()) return false;
  if (search_builtin(name).has_value()) return true;
  if (actx.are_koshkit_utilities_reachable &&
      koshkit::find_util(name).has_value())
  {
    return true;
  }
  if (os::has_directory_separator(name)) {
    if (let normalized = normalized_relative_executable_path(name);
        normalized.has_value() &&
        actx.generated_relative_executable_paths.contains(normalized->view()))
    {
      return true;
    }

    let const expanded = expanded_command_path(name, heap_allocator());
    let const typed_path = Path{expanded.view()};
    let const was_resolved =
        typed_path.has_trailing_separator()
            ? os::canonical_path(typed_path.to_absolute_without_normalizing())
                  .has_value()
            : Path::canonicalize(expanded.view()).has_value();
    if (was_resolved) return true;

    let const target = typed_path.to_absolute_without_normalizing();
    let raw_operand = name;
    if (let source_text = location.get_source_text(actx.source))
      raw_operand = *source_text;
    unavailable = utils::locate_first_unavailable_path_component(
        target, expanded.view(), raw_operand, location, heap_allocator());
    return false;
  }

  let resolver =
      ProgramResolver{actx.eval_context != nullptr
                          ? actx.eval_context->get_variable_value("PATH")
                          : os::get_environment_variable("PATH")};
  const bool was_resolved =
      resolver
          .search(name, ProgramResolver::SearchMode::First,
                  ProgramResolver::Requirement::Regular,
                  ProgramResolver::CachePolicy::Bypass)
          .count() != 0;
  LOG(Debug, "scanning PATH for '%.*s', the command was %s",
      static_cast<int>(name.length), name.data,
      was_resolved ? "found" : "not found");
  return was_resolved;
}

enum class bracket_scan_state : u8
{
  Outside,     /* no bracket expression is open */
  AfterOpen,   /* the byte after '[', where a '!' or '^' negates the class */
  InsideClass, /* the closing ']' has not been reached */
};

/* The scan mirrors the matcher in utils::glob_matches, where an active '[' with
   no closing ']' is a literal, so only a '[' that opens a class and never
   closes is malformed. A '[' as the last byte cannot open a class, which is why
   only InsideClass ends malformed. Returns true when malformed. */
pure fn word_has_malformed_glob_bracket(const Word &word) wontthrow -> bool
{
  let state = bracket_scan_state::Outside;

  for (let const &segment : word.segments) {
    /* Only an unquoted '[' or ']' is active, so a quoted "[" or an escaped \[
       stays literal and never opens a bracket expression. */
    let const is_glob_active = segment.has_live_glob_chars();

    for (usize i = 0; i < segment.text.count(); i++) {
      let const byte = segment.text[i];

      switch (state) {
      case bracket_scan_state::Outside:
        if (is_glob_active && byte == '[')
          state = bracket_scan_state::AfterOpen;
        break;

      case bracket_scan_state::AfterOpen:
        state = bracket_scan_state::InsideClass;
        if (byte == '!' || byte == '^') {
          break;
        }
        if (byte == ']') state = bracket_scan_state::Outside;
        break;

      case bracket_scan_state::InsideClass:
        if (byte == ']') state = bracket_scan_state::Outside;
        break;
      }
    }
  }

  return state == bracket_scan_state::InsideClass;
}

fn analyze_ast(const Expression *root, StringView source,
               const HashSet &known_functions, const HashSet &known_aliases,
               EvalContext *eval_context, u8 warning_level,
               bool silence_unresolved_commands, bool is_default_mood,
               bool should_emit_annoying_diagnostics,
               const ArrayList<shellcheck_suppression> &shellcheck_suppressions,
               const ArrayList<analysis_scope_definition> &scope_definitions,
               const ArrayList<shellcheck_directive_span> &directive_spans,
               const ArrayList<heredoc_terminator_miss> &heredoc_misses,
               bool is_named_script_file,
               bool should_report_optimizer_diagnostics,
               HashSet *followed_source_paths,
               StringMap<followed_source_effects> *source_effects_cache,
               AnalysisContext *parent_analysis_context,
               analysis_diagnostic_totals *deferred_diagnostic_totals,
               bool should_merge_parent_state,
               bool should_merge_parent_uncertainty,
               followed_source_effects *source_effects,
               ArrayList<source_diagnostic> *diagnostic_sink,
               AnalysisSourceProvider *source_provider,
               analysis_symbol_records *symbol_records,
               AnalysisUnitStream *unit_stream) throws -> bool
{
  ASSERT(root != nullptr || unit_stream != nullptr);

  AnalysisContext actx{source};
  actx.warning_level = warning_level;
  actx.is_default_mood = is_default_mood;
  actx.are_koshkit_utilities_reachable =
      eval_context != nullptr ? eval_context->koshkit_utilities_are_reachable()
                              : is_default_mood;
  actx.should_emit_annoying_diagnostics = should_emit_annoying_diagnostics;
  actx.shellcheck_suppressions = &shellcheck_suppressions;
  actx.should_silence_unresolved_commands = silence_unresolved_commands;
  actx.eval_context = eval_context;
  actx.should_report_optimizer_diagnostics =
      should_report_optimizer_diagnostics;
  actx.followed_source_paths = followed_source_paths;
  actx.followed_source_effects_cache = source_effects_cache;
  actx.diagnostic_sink = diagnostic_sink;
  actx.source_provider = source_provider;
  /* A followed source file is left out. Its byte positions index another
     source string. */
  actx.symbol_records = symbol_records;
  if (parent_analysis_context != nullptr) {
    actx.has_seen_runtime_definer =
        parent_analysis_context->has_seen_runtime_definer;
    actx.has_unknown_path = parent_analysis_context->has_unknown_path;
    actx.has_unknown_working_directory =
        parent_analysis_context->has_unknown_working_directory;
    parent_analysis_context->inherited_assigned_names.for_each(
        [&actx](StringView name) { actx.inherited_assigned_names.add(name); });
    parent_analysis_context->assigned_names_so_far.for_each(
        [&actx](StringView name, const SourceLocation &) {
          actx.inherited_assigned_names.add(name);
        });
    parent_analysis_context->inherited_global_assigned_names.for_each(
        [&actx](StringView name) {
          actx.inherited_global_assigned_names.add(name);
        });
    parent_analysis_context->global_assigned_names.for_each(
        [&actx](StringView name, const SourceLocation &) {
          actx.inherited_global_assigned_names.add(name);
        });
    parent_analysis_context->array_valued_names.for_each(
        [&actx](StringView name) { actx.array_valued_names.add(name); });
  }

  if (source.length >= 3 && static_cast<u8>(source[0]) == 0xef &&
      static_cast<u8>(source[1]) == 0xbb && static_cast<u8>(source[2]) == 0xbf)
    actx.report_diagnostic(diagnostic_id::sc1082, SourceLocation{0, 3});

  expressions::check_source_bytes(actx, source);

  if (parent_analysis_context != nullptr) {
    actx.is_posix_sh_shebang = parent_analysis_context->is_posix_sh_shebang;
  } else {
    expressions::check_shebang(actx, source, is_named_script_file);
  }

  expressions::check_shellcheck_directives(actx, source, directive_spans);

  expressions::check_heredoc_terminators(actx, source, heredoc_misses);

  LOG(Debug, "analyzing the ast, the posix sh shebang gate is %s",
      actx.is_posix_sh_shebang ? "armed" : "off");

  /* A function or alias defined by an earlier command resolves, so the already
     registered names seed the top-level scope. */
  known_functions.for_each(
      [&actx](StringView name) { actx.add_defined_function(name); });
  known_aliases.for_each(
      [&actx](StringView name) { actx.add_known_alias(name); });
  actx.current_source_effects = source_effects;
  actx.apply_scope_definitions(scope_definitions);

  if (unit_stream != nullptr) {
    /* Each unit is flushed before its arena span is handed back, so a warning
       never outlives the tree that produced it. */
    let sibling_carry = top_level_sibling_carry{};
    loop
    {
      let const *unit = unit_stream->next_unit();
      if (unit == nullptr) break;

      actx.stream_sibling_carry = &sibling_carry;
      unit->analyze(actx, true);
      actx.stream_sibling_carry = nullptr;

      actx.flush_warnings();
      unit_stream->release_unit();
    }
  } else {
    root->analyze(actx, true);
  }

  expressions::check_command_name_assignments(actx);
  expressions::check_unassigned_variable_reads(actx);
  expressions::check_function_argument_dataflow(actx);
  if (symbol_records != nullptr)
    resolve_function_occurrence_states(*symbol_records);

  actx.flush_warnings();

  if (parent_analysis_context != nullptr) {
    parent_analysis_context->reported_warning_count +=
        actx.reported_warning_count;
    parent_analysis_context->reported_error_count += actx.reported_error_count;
    ASSERT(source_effects != nullptr);
    source_effects->has_fatal = actx.has_fatal;
    apply_followed_source_effects(*parent_analysis_context, *source_effects,
                                  should_merge_parent_state,
                                  should_merge_parent_uncertainty);
  } else if (deferred_diagnostic_totals != nullptr) {
    deferred_diagnostic_totals->warning_count += actx.reported_warning_count;
    deferred_diagnostic_totals->error_count += actx.reported_error_count;
  } else if (diagnostic_sink == nullptr) {
    actx.print_diagnostic_summary();
  }

  actx.print_optimizer_summary();

  return !actx.has_fatal;
}

namespace expressions {

pure fn analysis_source_text(const AnalysisContext &actx,
                             const SourceLocation &location) wontthrow
    -> StringView
{
  if (location.position > actx.source.length ||
      location.length > actx.source.length - location.position)
    return {};
  return actx.source.substring_of_length(location.position, location.length);
}

pure fn classify_assignment_builtin(StringView name) wontthrow
    -> assignment_builtin
{
  static constexpr static_string_entry<assignment_builtin> ENTRIES[] = {
      {SSK("declare"),  assignment_builtin::Declare },
      {SSK("export"),   assignment_builtin::Export  },
      {SSK("local"),    assignment_builtin::Local   },
      {SSK("readonly"), assignment_builtin::Readonly},
      {SSK("typeset"),  assignment_builtin::Declare },
  };
  static constexpr StaticStringMap ASSIGNMENT_BUILTINS{ENTRIES};

  return ASSIGNMENT_BUILTINS.find(name).value_or(assignment_builtin::None);
}

/* A word segment spans the name and its modifiers, and the sigil and the braces
   around it belong to the expansion a reader sees. */
pure fn expansion_location_with_sigil(const AnalysisContext &actx,
                                      SourceLocation location) wontthrow
    -> SourceLocation
{
  if (location.length == 0) return location;
  if (location.position > actx.source.length ||
      location.length > actx.source.length - location.position)
  {
    return location;
  }

  usize start = location.position;
  usize length = location.length;

  if (start >= 2 && actx.source[start - 1] == '{' &&
      actx.source[start - 2] == '$')
  {
    start -= 2;
    length += 2;

    if (start + length < actx.source.length &&
        actx.source[start + length] == '}')
    {
      length++;
    }
  } else if (start >= 1 && actx.source[start - 1] == '$') {
    start--;
    length++;
  } else {
    return location;
  }

  return SourceLocation{start, length, location.source_name_index};
}

fn note_variable_reference(AnalysisContext &actx, const WordSegment &segment,
                           SourceLocation fallback_location) throws -> void
{
  let const segment_location =
      segment.get_source_location(fallback_location.source_name_index)
          .value_or(fallback_location);
  actx.note_variable_occurrence(
      segment.text.view(),
      expansion_location_with_sigil(actx, segment_location),
      variable_occurrence_kind::Reference);
}

fn merge_variable_occurrence_states(
    StringMap<variable_occurrence_state> &merged_states,
    const StringMap<variable_occurrence_state> &exit_states) throws -> void
{
  merged_states.for_each(
      [&](StringView name, variable_occurrence_state &state) {
        let const *exit_state = exit_states.find(name);
        if (exit_state == nullptr) {
          state.is_definitely_set = false;
          state.is_definitely_unset = false;
          state.has_unset_path = true;
          state.has_inherited_path = true;
          return;
        }
      });
  exit_states.for_each([&](StringView name,
                           const variable_occurrence_state &exit_state) {
    let *state = merged_states.find(name);
    if (state == nullptr) {
      let merged_state = exit_state;
      merged_state.is_definitely_set = false;
      merged_state.is_definitely_unset = false;
      merged_state.has_inherited_path = true;
      merged_states.set(name, steal(merged_state));
      return;
    }

    if (state->assignment_indices.is_empty()) {
      state->assignment_indices = exit_state.assignment_indices.clone();
    } else if (!exit_state.assignment_indices.is_empty()) {
      let merged_assignment_indices = ArrayList<usize>{heap_allocator()};
      merged_assignment_indices.reserve(state->assignment_indices.count() +
                                        exit_state.assignment_indices.count());
      usize state_index = 0;
      usize exit_index = 0;
      while (state_index < state->assignment_indices.count() ||
             exit_index < exit_state.assignment_indices.count())
      {
        let const has_state_index =
            state_index < state->assignment_indices.count();
        let const has_exit_index =
            exit_index < exit_state.assignment_indices.count();
        usize assignment_index;
        if (!has_exit_index ||
            (has_state_index && state->assignment_indices[state_index] <
                                    exit_state.assignment_indices[exit_index]))
        {
          assignment_index = state->assignment_indices[state_index++];
        } else if (!has_state_index ||
                   exit_state.assignment_indices[exit_index] <
                       state->assignment_indices[state_index])
        {
          assignment_index = exit_state.assignment_indices[exit_index++];
        } else {
          assignment_index = state->assignment_indices[state_index++];
          exit_index++;
        }
        if (merged_assignment_indices.is_empty() ||
            merged_assignment_indices.back() != assignment_index)
          merged_assignment_indices.push(assignment_index);
      }
      state->assignment_indices = steal(merged_assignment_indices);
    }
    state->is_definitely_set =
        state->is_definitely_set && exit_state.is_definitely_set;
    state->is_definitely_unset =
        state->is_definitely_unset && exit_state.is_definitely_unset;
    state->has_unset_path = state->has_unset_path || exit_state.has_unset_path;
    state->has_inherited_path =
        state->has_inherited_path || exit_state.has_inherited_path;
  });
}

pure fn location_spanning(SourceLocation first, SourceLocation last) wontthrow
    -> SourceLocation
{
  if (first.length == 0) return last;
  if (last.length == 0) return first;
  if (last.position < first.position) return first;

  return SourceLocation{first.position,
                        last.position + last.length - first.position,
                        first.source_name_index};
}

pure fn analysis_source_span(const AnalysisContext &actx,
                             const Expression &expression) wontthrow
    -> StringView
{
  let const start = expression.source_location().position;
  let const end = expression.source_end_position();
  if (start > end || end > actx.source.length) {
    return {};
  }
  return actx.source.substring_of_length(start, end - start);
}

pure fn arithmetic_reads_external_input(const AnalysisContext &actx,
                                        StringView expression) wontthrow -> bool
{
  for (usize position = 0; position < expression.length;) {
    if (!lexer::is_variable_name_start(expression[position])) {
      position++;
      continue;
    }
    let const start = position++;
    while (position < expression.length &&
           lexer::is_variable_name(expression[position]))
      position++;
    let const name = expression.substring_of_length(start, position - start);
    if (actx.external_input_names.contains(name)) return true;
  }
  return false;
}

IfStatement::IfStatement(SourceLocation location, const Expression *condition,
                         const Expression *then, const Expression *otherwise)
    : Expression(steal(location)), m_condition(condition), m_then(then),
      m_otherwise(otherwise)
{
  ASSERT(condition != nullptr);
  ASSERT(then != nullptr);
}

IfStatement::~IfStatement() = default;

hot fn IfStatement::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_then != nullptr);

  let const condition = m_condition->evaluate(cxt);
  if (cxt.has_pending_control_flow()) return condition;

  LOG(Debug, "the if condition yielded %lld, running the %s branch",
      static_cast<long long>(condition),
      condition ? "then" : (m_otherwise != nullptr ? "else" : "no"));

  if (condition)
    return m_then->evaluate(cxt);
  else if (m_otherwise != nullptr)
    return m_otherwise->evaluate(cxt);

  return 0;
}

cold fn IfStatement::to_string() const throws -> String { return "If"; }

cold fn IfStatement::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_then != nullptr);

  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[If]\n";
  s += pad + EXPRESSION_AST_INDENT + m_condition->to_ast_string(layer + 1) +
       "\n";
  s += pad + EXPRESSION_AST_INDENT + m_then->to_ast_string(layer + 1);

  if (m_otherwise != nullptr) {
    s += '\n';
    s += pad + pad + "[Else]\n";
    s += pad + EXPRESSION_AST_INDENT + m_otherwise->to_ast_string(layer + 1);
  }

  return s;
}

Command::Command(SourceLocation location) : Expression(steal(location)) {}

fn Command::make_async() wontthrow -> void { m_is_async = true; }

pure fn Command::is_async() const wontthrow -> bool { return m_is_async; }

fn Command::set_negated() wontthrow -> void { m_is_negated = true; }

pure fn Command::is_negated() const wontthrow -> bool { return m_is_negated; }

fn Command::set_timed(bool posix_format, SourceLocation location) wontthrow
    -> void
{
  m_is_timed = true;
  m_is_time_posix_format = posix_format;
  m_time_position = location.position;
}

pure fn Command::is_timed() const wontthrow -> bool { return m_is_timed; }

pure fn Command::time_location() const wontthrow -> SourceLocation
{
  constexpr usize TIME_KEYWORD_LENGTH = 4;

  return SourceLocation{m_time_position, TIME_KEYWORD_LENGTH,
                        source_location().source_name_index};
}

pure fn Command::time_uses_posix_format() const wontthrow -> bool
{
  return m_is_time_posix_format;
}

fn Command::set_local_vars(ArrayList<PrefixAssignment> &&vars) throws -> void
{
  m_local_vars.fill(steal(vars));
}

pure fn Command::local_vars() const wontthrow
    -> const SparseList<PrefixAssignment> &
{
  return m_local_vars;
}

fn Command::is_assignment() const wontthrow -> bool { return false; }

fn Command::is_compound_command() const wontthrow -> bool { return false; }

/* A plain command node carries no redirect target of its own, so the default
   reports that. A node that does take a target overrides this. */
fn Command::redirect_to(usize target_fd, String &filename,
                        bool duplicate) throws -> void
{
  unused(target_fd);
  unused(filename);
  unused(duplicate);
  throw ErrorWithLocation{source_location(), "Not implemented (Expressions)"};
}

fn Command::append_to(usize target_fd, String &filename, bool duplicate) throws
    -> void
{
  redirect_to(target_fd, filename, duplicate);
}

DummyExpression::DummyExpression(SourceLocation location)
    : Expression(steal(location))
{}

fn DummyExpression::is_dummy() const wontthrow -> bool { return true; }

fn DummyExpression::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

cold fn DummyExpression::to_string() const throws -> String { return "Dummy"; }

/* The special parameters carry their own splitting rules, so quoting advice
   never applies to them. */
} /* namespace expressions */

} /* namespace koshka */
