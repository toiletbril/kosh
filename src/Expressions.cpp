/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements common expression and command bases, analysis
 * diagnostics, variable dataflow, assignment indexes, source-following
 * analysis, substitution body analysis, and shared syntax helpers. It also
 * provides analyze_ast and the common command execution flags. The split keeps
 * behavior shared by every syntax node outside the specialized expression
 * sources.
 */

#include "Expressions.hpp"

#include "Builtin.hpp"
#include "CLI.hpp"
#include "CLIColors.hpp"
#include "Completion.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"
#include "ParserFormats.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/StaticStringMap.hpp"
#include "base/Trace.hpp"

namespace koshka {

using namespace expressions::internal;

static constexpr StringView BINDER_DESCRIPTIONS[] = {
    "The value is assigned here.",
    "The loop assigns each word to the variable.",
    "The select menu assigns the chosen entry to the variable.",
    "The arithmetic expression sets the variable.",
    "read assigns an input field to the variable.",
    "mapfile assigns input lines to the variable.",
    "getopts assigns the current option letter to the variable.",
    "printf assigns formatted text to the variable.",
    "This declaration does not assign a value.",
};
static_assert(countof(BINDER_DESCRIPTIONS) ==
              static_cast<usize>(assignment_binder::Declaration) + 1);

pure fn binder_description(assignment_binder binder) wontthrow -> StringView
{
  let const index = static_cast<usize>(binder);
  if (index >= countof(BINDER_DESCRIPTIONS)) return StringView{};

  return BINDER_DESCRIPTIONS[index];
}

struct assignment_index_storage
{
  usize reference_count{1};
  usize element_count{0};
  usize allocation_count{0};
};

static fn assignment_index_storage_length(usize count) throws -> usize
{
  if (count > (SIZE_MAX - sizeof(assignment_index_storage)) / sizeof(usize))
    throw std::bad_alloc{};

  return sizeof(assignment_index_storage) + count * sizeof(usize);
}

static pure fn
assignment_index_data(assignment_index_storage *storage) wontthrow -> usize *
{
  return reinterpret_cast<usize *>(storage + 1);
}

static fn allocate_assignment_index_storage(usize count) throws
    -> assignment_index_storage *
{
  let const allocation_length = assignment_index_storage_length(count);
  let *storage =
      static_cast<assignment_index_storage *>(heap_allocator().raw_alloc(
          allocation_length, alignof(assignment_index_storage)));
  new (storage) assignment_index_storage{};
  storage->allocation_count = count;
  return storage;
}

AssignmentIndexSet::AssignmentIndexSet(
    const AssignmentIndexSet &other) wontthrow : m_storage(other.m_storage)
{
  retain();
}

AssignmentIndexSet::AssignmentIndexSet(AssignmentIndexSet &&other) noexcept
    : m_storage(other.m_storage)
{
  other.m_storage = nullptr;
}

AssignmentIndexSet::~AssignmentIndexSet() { release(); }

fn AssignmentIndexSet::operator=(const AssignmentIndexSet &other) wontthrow
    -> AssignmentIndexSet &
{
  if (this == &other) return *this;

  release();
  m_storage = other.m_storage;
  retain();
  return *this;
}

fn AssignmentIndexSet::operator=(AssignmentIndexSet &&other) noexcept
    -> AssignmentIndexSet &
{
  if (this == &other) return *this;

  release();
  m_storage = other.m_storage;
  other.m_storage = nullptr;
  return *this;
}

fn AssignmentIndexSet::singleton(usize index) throws -> AssignmentIndexSet
{
  let *storage = allocate_assignment_index_storage(1);
  storage->element_count = 1;
  assignment_index_data(storage)[0] = index;
  return AssignmentIndexSet{storage};
}

fn AssignmentIndexSet::merge(const AssignmentIndexSet &other) throws -> void
{
  if (other.is_empty() || m_storage == other.m_storage) return;
  if (is_empty()) {
    *this = other;
    return;
  }

  if (count() > SIZE_MAX - other.count()) throw std::bad_alloc{};

  let *merged_storage =
      allocate_assignment_index_storage(count() + other.count());
  let *merged_indices = assignment_index_data(merged_storage);
  usize left_index = 0;
  usize right_index = 0;
  usize merged_count = 0;

  while (left_index < count() || right_index < other.count()) {
    usize assignment_index;
    if (right_index >= other.count() ||
        (left_index < count() && (*this)[left_index] < other[right_index]))
    {
      assignment_index = (*this)[left_index++];
    } else if (left_index >= count() ||
               other[right_index] < (*this)[left_index])
    {
      assignment_index = other[right_index++];
    } else {
      assignment_index = (*this)[left_index++];
      right_index++;
    }

    if (merged_count == 0 ||
        merged_indices[merged_count - 1] != assignment_index)
      merged_indices[merged_count++] = assignment_index;
  }

  merged_storage->element_count = merged_count;
  release();
  m_storage = merged_storage;
}

pure fn AssignmentIndexSet::count() const wontthrow -> usize
{
  return m_storage != nullptr ? m_storage->element_count : 0;
}

pure fn AssignmentIndexSet::is_empty() const wontthrow -> bool
{
  return m_storage == nullptr || m_storage->element_count == 0;
}

pure fn AssignmentIndexSet::operator[](usize index) const wontthrow -> usize
{
  ASSERT(index < count());
  return assignment_index_data(m_storage)[index];
}

pure fn AssignmentIndexSet::begin() const wontthrow -> const usize *
{
  return m_storage != nullptr ? assignment_index_data(m_storage) : nullptr;
}

pure fn AssignmentIndexSet::end() const wontthrow -> const usize *
{
  return m_storage != nullptr ? assignment_index_data(m_storage) + count()
                              : nullptr;
}

fn AssignmentIndexSet::retain() wontthrow -> void
{
  if (m_storage != nullptr) m_storage->reference_count++;
}

fn AssignmentIndexSet::release() wontthrow -> void
{
  if (m_storage == nullptr) return;

  ASSERT(m_storage->reference_count > 0);
  m_storage->reference_count--;
  if (m_storage->reference_count == 0) {
    let const allocation_length = sizeof(assignment_index_storage) +
                                  m_storage->allocation_count * sizeof(usize);
    m_storage->~assignment_index_storage();
    heap_allocator().raw_free(m_storage, allocation_length,
                              alignof(assignment_index_storage));
  }
  m_storage = nullptr;
}

struct variable_occurrence_map_storage
{
  usize reference_count{1};
  usize depth{0};
  variable_occurrence_map_storage *parent{nullptr};
  StringMap<variable_occurrence_map_entry> entries{heap_allocator(),
                                                   SMALL_MAP_FIRST_CAPACITY};
};

static fn create_variable_occurrence_map_storage(
    variable_occurrence_map_storage *parent) throws
    -> variable_occurrence_map_storage *
{
  let *storage =
      heap_allocator().alloc_array<variable_occurrence_map_storage>(1);
  new (storage) variable_occurrence_map_storage{};
  storage->parent = parent;
  storage->depth = parent != nullptr ? parent->depth + 1 : 0;

  return storage;
}

static fn retain_variable_occurrence_map_storage(
    variable_occurrence_map_storage *storage) wontthrow -> void
{
  if (storage == nullptr) return;

  ASSERT(storage->reference_count < SIZE_MAX);
  storage->reference_count++;
}

static fn release_variable_occurrence_map_storage(
    variable_occurrence_map_storage *storage) wontthrow -> void
{
  while (storage != nullptr) {
    ASSERT(storage->reference_count > 0);
    storage->reference_count--;
    if (storage->reference_count != 0) return;

    let *const parent = storage->parent;
    storage->~variable_occurrence_map_storage();
    heap_allocator().free_array(storage, 1);
    storage = parent;
  }
}

static fn find_common_variable_occurrence_storage(
    const variable_occurrence_map_storage *left,
    const variable_occurrence_map_storage *right) wontthrow
    -> const variable_occurrence_map_storage *
{
  while (left != nullptr && right != nullptr && left != right) {
    if (left->depth >= right->depth) {
      left = left->parent;
    } else {
      right = right->parent;
    }
  }

  return left == right ? left : nullptr;
}

VariableOccurrenceStateMap::VariableOccurrenceStateMap(
    const VariableOccurrenceStateMap &other)
    : m_head(other.m_head)
{
  retain_variable_occurrence_map_storage(m_head);
}

VariableOccurrenceStateMap::VariableOccurrenceStateMap(
    VariableOccurrenceStateMap &&other) noexcept
    : m_head(other.m_head)
{
  other.m_head = nullptr;
}

VariableOccurrenceStateMap::~VariableOccurrenceStateMap()
{
  release_variable_occurrence_map_storage(m_head);
}

fn VariableOccurrenceStateMap::operator=(
    const VariableOccurrenceStateMap &other) throws
    -> VariableOccurrenceStateMap &
{
  if (this == &other) return *this;

  let copy = VariableOccurrenceStateMap{other};
  *this = steal(copy);
  return *this;
}

fn VariableOccurrenceStateMap::operator=(
    VariableOccurrenceStateMap &&other) noexcept -> VariableOccurrenceStateMap &
{
  if (this == &other) return *this;

  release_variable_occurrence_map_storage(m_head);
  m_head = other.m_head;
  other.m_head = nullptr;
  return *this;
}

fn VariableOccurrenceStateMap::snapshot() throws -> VariableOccurrenceStateMap
{
  collapse_exclusive_layers();

  return VariableOccurrenceStateMap{*this};
}

pure fn VariableOccurrenceStateMap::find(StringView name) const wontthrow
    -> const variable_occurrence_state *
{
  let const hash = hash_bytes(name);
  for (let const *storage = m_head; storage != nullptr;
       storage = storage->parent)
  {
    let const entry = storage->entries.find_hashed(name, hash);
    if (entry.has_value()) return entry->is_present ? &entry->state : nullptr;
  }

  return nullptr;
}

fn VariableOccurrenceStateMap::set(StringView name,
                                   variable_occurrence_state state) throws
    -> void
{
  get_private_head()->entries.set(
      name, variable_occurrence_map_entry{steal(state), true});
}

fn VariableOccurrenceStateMap::erase(StringView name) throws -> void
{
  if (find(name) == nullptr) return;
  get_private_head()->entries.set(name, variable_occurrence_map_entry{});
}

fn VariableOccurrenceStateMap::clear() wontthrow -> void
{
  release_variable_occurrence_map_storage(m_head);
  m_head = nullptr;
}

fn VariableOccurrenceStateMap::get_private_head() throws
    -> variable_occurrence_map_storage *
{
  if (m_head != nullptr && m_head->reference_count == 1) {
    collapse_exclusive_layers();

    return m_head;
  }

  m_head = create_variable_occurrence_map_storage(m_head);

  return m_head;
}

fn VariableOccurrenceStateMap::collapse_exclusive_layers() throws -> void
{
  while (m_head != nullptr && m_head->reference_count == 1 &&
         m_head->parent != nullptr && m_head->parent->reference_count == 1)
  {
    let *const upper = m_head;
    let *const lower = upper->parent;
    if (upper->entries.count() <= lower->entries.count()) {
      upper->entries.for_each(
          [&](StringView name, const variable_occurrence_map_entry &entry) {
            lower->entries.set(name, entry);
          });
      upper->parent = nullptr;
      release_variable_occurrence_map_storage(upper);
      m_head = lower;
      continue;
    }

    lower->entries.for_each(
        [&](StringView name, const variable_occurrence_map_entry &entry) {
          upper->entries.insert(name, entry);
        });
    upper->parent = lower->parent;
    upper->depth = lower->depth;
    lower->parent = nullptr;
    release_variable_occurrence_map_storage(lower);
  }
}

fn VariableOccurrenceStateMap::merge(
    const VariableOccurrenceStateMap &other) throws -> void
{
  if (this == &other) return;

  let do_merge_name = [&](VariableOccurrenceStateMap &result, StringView name) {
    let const *left_state = find(name);
    let const *right_state = other.find(name);
    if (left_state == nullptr && right_state == nullptr) {
      result.erase(name);
      return;
    }

    let merged_state = left_state != nullptr ? *left_state : *right_state;
    if (left_state == nullptr) {
      merged_state.is_definitely_set = false;
      merged_state.is_definitely_unset = false;
      merged_state.has_inherited_path = true;
    } else if (right_state == nullptr) {
      merged_state.is_definitely_set = false;
      merged_state.is_definitely_unset = false;
      merged_state.has_unset_path = true;
      merged_state.has_inherited_path = true;
    } else {
      merged_state.assignment_indices.merge(right_state->assignment_indices);
      merged_state.is_definitely_set =
          merged_state.is_definitely_set && right_state->is_definitely_set;
      merged_state.is_definitely_unset =
          merged_state.is_definitely_unset && right_state->is_definitely_unset;
      merged_state.has_unset_path =
          merged_state.has_unset_path || right_state->has_unset_path;
      merged_state.has_inherited_path =
          merged_state.has_inherited_path || right_state->has_inherited_path;
    }
    result.set(name, steal(merged_state));
  };

  let const *const common_storage =
      find_common_variable_occurrence_storage(m_head, other.m_head);
  let result = VariableOccurrenceStateMap{*this};
  let do_merge_layers = [&](const variable_occurrence_map_storage *storage) {
    for (; storage != common_storage; storage = storage->parent) {
      storage->entries.for_each(
          [&](StringView name, const variable_occurrence_map_entry &) {
            do_merge_name(result, name);
          });
    }
  };

  do_merge_layers(m_head);
  do_merge_layers(other.m_head);
  *this = steal(result);
}

fn expressions::internal::indent_for_layer(usize layer) throws -> String
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

hot flatten fn Expression::evaluate_root_status(
    EvalContext &cxt, root_evaluation_mode mode) const throws -> status_result
{
  if (os::INTERRUPT_REQUESTED) {
    os::INTERRUPT_REQUESTED = 0;
    throw InterruptErrorWithLocation{source_location()};
  }

  if (os::SIGNAL_PENDING) {
    let const was_control_flow_pending = cxt.control_flow_store().has_pending();
    cxt.run_pending_traps();

    if (!was_control_flow_pending && cxt.control_flow_store().has_pending())
      return cxt.execution_store().last_exit_status();
  }

  cxt.evaluation_metrics_store().add_evaluated_expression(
      cxt.runtime_state().stats_enabled());
  if (is_compound_command()) {
    let const command = static_cast<const CompoundCommand *>(this);
    if (command->is_async()) return command->evaluate_async(cxt);
  }
  try {
    return evaluate_impl(cxt, mode);
  } catch (InterruptErrorWithLocation &error) {
    let const location = error.location();
    if (location.position == 0 && location.length == 0 &&
        location.source_name_index == 0)
    {
      error.set_location(source_location());
    }
    throw;
  }
}

fn Expression::operator delete(opaque *pointer) wontthrow -> void
{
  if (is_arena_pointer(pointer)) return;
  ::operator delete(pointer);
}

fn analysis_reporter::warn(diagnostic_id id, const SourceLocation &location,
                           StringView message, StringView suggestion,
                           diagnostic_tier tier,
                           const Maybe<SourceLocation> &related_location,
                           StringView related_message) throws -> void
{
  if (!should_report(tier)) return;

  totals.warning_count++;

  pending.push(pending_analysis_warning{id, location, String{message},
                                        String{suggestion}, related_location,
                                        String{related_message}});
}

fn analysis_reporter::flush(const analysis_report_site &site) throws -> void
{
  if (pending.is_empty()) return;

  let const source = site.source;
  let *const eval_context = site.eval_context;

  if (eval_context != nullptr && colors::stderr_wants_color()) {
    let collected_positions = ArrayList<usize>{heap_allocator()};
    collected_positions.reserve(pending.count() * 2);
    for (let const &warning : pending) {
      if (warning.location.position <= source.length)
        collected_positions.push(warning.location.position);
      if (warning.related_location.has_value() &&
          warning.related_location->position <= source.length)
      {
        collected_positions.push(warning.related_location->position);
      }
    }
    let const positions =
        steal(collected_positions).make_sorted(sort_order::ascending);

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

  let report_order = ArrayList<usize>{heap_allocator()};
  report_order.reserve(pending.count());
  for (usize index = 0; index < pending.count(); index++)
    report_order.push(index);
  report_order.sort([this](const usize &left, const usize &right) {
    let const &left_location = pending[left].location;
    let const &right_location = pending[right].location;
    if (left_location.source_name_index != right_location.source_name_index)
      return left_location.source_name_index < right_location.source_name_index;
    if (left_location.position != right_location.position)
      return left_location.position < right_location.position;
    return left < right;
  });

  for (let const report_index : report_order) {
    let const &warning = pending[report_index];
    if (sink != nullptr) {
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
      sink->push(source_diagnostic{
          warning.id, error_severity::warning, warning.location,
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
    if (eval_context != nullptr)
      eval_context->print_source_backtrace(warning.location);
  }
  pending.clear();
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
  print_analysis_diagnostic_summary(reporter.totals);
}

cold fn AnalysisContext::print_optimizer_summary() const throws -> void
{
  if (!options.should_report_optimizer_diagnostics) return;

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

pure fn analysis_reporter::should_report(diagnostic_id id) const wontthrow
    -> bool
{
  return should_report(get_diagnostic_definition(id).tier);
}

pure fn analysis_reporter::should_report(diagnostic_tier tier) const wontthrow
    -> bool
{
  if (tier == diagnostic_tier::Annoying && !options.should_emit_annoying) {
    return false;
  }
  if (options.is_default_mood) return true;

  u8 required_level = 0;
  switch (tier) {
  case diagnostic_tier::Strict: required_level = 1; break;
  case diagnostic_tier::Lenient: required_level = 2; break;
  case diagnostic_tier::Annoying: required_level = 3; break;
  }

  return options.warning_level >= required_level;
}

pure fn AnalysisContext::should_silence_unresolved_command_at(
    usize position) const wontthrow -> bool
{
  if (effects.should_silence_unresolved_commands) return true;
  if (format_document == nullptr) return false;
  let const fragment_index =
      parser_format_fragment_at(*format_document, position);
  if (!fragment_index.has_value()) return false;

  return format_document->fragments[*fragment_index]
      .should_silence_unresolved_commands;
}

fn analysis_reporter::report(
    const analysis_report_site &site, diagnostic_id id,
    const SourceLocation &location, std::initializer_list<StringView> arguments,
    const Maybe<SourceLocation> &related_location) throws -> bool
{
  if (!should_report(id)) return false;
  if (is_suppressed(site, id, location)) return false;

  let named_location = location;
  if (named_location.source_name_index == 0)
    named_location.source_name_index = source_name_index;

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
    fail(site, id, named_location, message.view(), suggestion.view(),
         definition.tier, related_location, related_message.view());
    break;
  case diagnostic_delivery::Warning:
    warn(id, named_location, message.view(), suggestion.view(), definition.tier,
         related_location, related_message.view());
    break;
  }

  return true;
}

cold fn AnalysisContext::trace_optimizer_line(StringView message) const throws
    -> void
{
  if (!options.should_report_optimizer_diagnostics) return;
  print_error("[optimizer] ");
  print_error(message);
  print_error("\n");
}

fn analysis_reporter::fail(const analysis_report_site &site, diagnostic_id id,
                           const SourceLocation &location, StringView message,
                           StringView suggestion, diagnostic_tier tier,
                           const Maybe<SourceLocation> &related_location,
                           StringView related_message) throws -> void
{
  let const source = site.source;
  let *const eval_context = site.eval_context;

  if (!options.is_default_mood) {
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

  if (options.warning_level >= demote_at_level) {
    warn(id, location, message, suggestion, tier, related_location,
         related_message);
    return;
  }

  flush(site);
  totals.error_count++;

  if (sink != nullptr) {
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
    sink->push(source_diagnostic{
        id, error_severity::error, location, steal(source_name),
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
  if (eval_context != nullptr) eval_context->print_source_backtrace(location);
  has_fatal = true;
}

pure fn analysis_reporter::is_suppressed(
    const analysis_report_site &site, diagnostic_id id,
    const SourceLocation &location) const wontthrow -> bool
{
  if (suppressions == nullptr) return false;

  for (let const &suppression : *suppressions) {
    if (location.position < suppression.start_position ||
        location.position >= suppression.end_position)
    {
      continue;
    }

    for (let const &selector : suppression.selectors) {
      if (shellcheck_selector_disables(selector, site.source, id)) return true;
    }
  }

  return false;
}

fn AnalysisContext::note_variable_assignment(
    StringView name, const SourceLocation &location,
    bool is_proven_unconditional) throws -> void
{
  if (name.is_empty()) return;

  assigned_names_so_far.set(name, location);
  let const name_location = location.length >= name.length
                                ? location.subspan(0, name.length)
                                : location;
  diagnostic_assignment_traces.set(
      name, diagnostic_assignment_trace{name_location,
                                        assignment_binder::Assignment});
  if (current_source_effects != nullptr)
    current_source_effects->assigned_names.add(name);
  if (!is_proven_unconditional) return;

  if (let const read_location = reads_before_assignment.find(name);
      read_location.has_value())
  {
    report_diagnostic(diagnostic_id::use_before_assign, *read_location.value(),
                      {name}, location);
    reads_before_assignment.erase(name);
  }
}

static constexpr usize RECORDED_LITERAL_LENGTH_LIMIT = 256;

fn AnalysisContext::note_variable_assignment_record(
    StringView name, const Word *value_word, const SourceLocation &location,
    bool is_conditional, assignment_update_mode update_mode) throws -> void
{
  if (name.is_empty()) return;

  let const name_location = location.length >= name.length
                                ? location.subspan(0, name.length)
                                : location;
  diagnostic_assignment_traces.set(
      name, diagnostic_assignment_trace{name_location,
                                        assignment_binder::Assignment});
  if (symbol_records == nullptr) return;

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
      String{name}, steal(literal_value), location.position, location.length,
      assignment_binder::Assignment, is_conditional, update_mode,
      value_word == nullptr});
}

fn AnalysisContext::note_variable_binding_record(StringView name,
                                                 const SourceLocation &location,
                                                 assignment_binder binder,
                                                 bool is_conditional) throws
    -> void
{
  if (name.is_empty()) return;

  diagnostic_assignment_traces.set(
      name, diagnostic_assignment_trace{location, binder});
  if (symbol_records == nullptr) return;

  symbol_records->assignments.push(variable_assignment_record{
      String{name}, None, location.position, location.length, binder,
      is_conditional, assignment_update_mode::Replace, false});
}

fn AnalysisContext::note_variable_occurrence(
    StringView name, const SourceLocation &location,
    variable_occurrence_kind kind, bool is_unresolved,
    assignment_update_mode update_mode) throws -> void
{
  if (name.is_empty() || location.length == 0) return;

  if (name.length > 1 && name[0] == '#') name = name.substring(1);
  if (!lexer::word_is_variable_name(name) && !reference_names_positional(name))
    name = expressions::internal::operand_target_name(name);
  if (name.is_empty()) return;

  note_variable_scope(name);

  if (kind == variable_occurrence_kind::Assignment &&
      active_function_definition_index == NO_ACTIVE_FUNCTION_DEFINITION)
  {
    top_level_assigned_names.add(name);
  }

  let const function_definition_index = active_function_definition_index;
  let const *current_state = kind == variable_occurrence_kind::Reference
                                 ? occurrences.assigned.find(name)
                                 : nullptr;
  let const has_inherited_function_path =
      function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION &&
      kind == variable_occurrence_kind::Reference &&
      (current_state == nullptr || current_state->has_inherited_path);
  let occurrence_is_unresolved = is_unresolved;
  let occurrence_is_unused = false;
  let const occurrence_index =
      symbol_records != nullptr ? symbol_records->variable_occurrences.count()
                                : usize{0};

  if (kind == variable_occurrence_kind::Assignment) {
    if (update_mode == assignment_update_mode::Append &&
        symbol_records != nullptr)
    {
      let const *prior_state = occurrences.find(name);
      if (prior_state != nullptr) {
        for (let const assignment_index : prior_state->assignment_indices)
          symbol_records->variable_occurrences[assignment_index].is_unused =
              false;
      }
    }

    let state = variable_occurrence_state{};
    if (symbol_records != nullptr)
      state.assignment_indices =
          AssignmentIndexSet::singleton(occurrence_index);
    state.is_definitely_set = true;
    state.is_definitely_unset = false;
    state.has_inherited_path = false;
    occurrences.assigned.set(name, steal(state));
    occurrence_is_unused = true;
    if (function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION)
      functions.records[function_definition_index].affected_names.add(name);
  } else if (kind == variable_occurrence_kind::Reference) {
    let const *state = occurrences.find(name);
    if (state != nullptr && symbol_records != nullptr) {
      for (let const assignment_index : state->assignment_indices)
        symbol_records->variable_occurrences[assignment_index].is_unused =
            false;
    }

    occurrence_is_unresolved =
        is_unresolved || (state != nullptr && !state->is_definitely_set) ||
        (state == nullptr &&
         !expressions::internal::is_shell_maintained_variable(name) &&
         !(eval_context != nullptr && eval_context->has_variable_name(name)) &&
         !os::get_environment_variable(name).has_value());
  } else {
    let const *state = occurrences.find(name);
    occurrence_is_unresolved = state == nullptr || !state->is_definitely_set;

    let unset_state = variable_occurrence_state{};
    unset_state.is_definitely_unset = true;
    unset_state.has_unset_path = true;
    occurrences.replace(name, steal(unset_state));
    if (function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION)
      functions.records[function_definition_index].affected_names.add(name);
  }

  if (symbol_records == nullptr) return;

  symbol_records->variable_occurrences.push(variable_occurrence_record{
      String{name}, location.position, location.length,
      function_definition_index, kind, occurrence_is_unresolved,
      occurrence_is_unused, false, false, has_inherited_function_path});
}

fn AnalysisContext::apply_called_function(
    StringView name, const SourceLocation &call_location) throws -> void
{
  if (active_function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION &&
      functions.records[active_function_definition_index].name.view() == name)
  {
    return;
  }

  let const selected_definition_index = functions.latest_indices.find(name);
  if (!selected_definition_index.has_value()) return;

  let const &selected_definition =
      functions.records[*selected_definition_index.value()];
  if (!selected_definition.is_analysis_complete ||
      selected_definition.location.position > call_location.position)
  {
    return;
  }

  let &definition = functions.records[*selected_definition_index.value()];
  definition.has_been_called = true;
  if (active_function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION) {
    let &active_definition =
        functions.records[active_function_definition_index];
    definition.affected_names.for_each([&](StringView affected_name) {
      if (!definition.local_names.contains(affected_name))
        active_definition.affected_names.add(affected_name);
    });
  }

  if (symbol_records != nullptr) {
    for (usize occurrence_index = definition.occurrence_start;
         occurrence_index < definition.occurrence_end; occurrence_index++)
    {
      let &occurrence = symbol_records->variable_occurrences[occurrence_index];
      if (occurrence.kind != variable_occurrence_kind::Reference ||
          occurrence.function_definition_index !=
              *selected_definition_index.value() ||
          !occurrence.has_inherited_function_path)
      {
        continue;
      }
      if (active_function_definition_index != NO_ACTIVE_FUNCTION_DEFINITION) {
        occurrence.function_definition_index = active_function_definition_index;
        continue;
      }

      let const *state = occurrences.find(occurrence.name.view());
      if (state != nullptr && state->is_definitely_set) {
        occurrence.has_resolved_function_path = true;
        for (let const assignment_index : state->assignment_indices)
          symbol_records->variable_occurrences[assignment_index].is_unused =
              false;
      } else {
        occurrence.has_unresolved_function_path = true;
      }
    }
  }

  definition.affected_names.for_each([&](StringView affected_name) {
    if (definition.local_names.contains(affected_name)) return;

    let const *exit_state = definition.exit_states.find(affected_name);
    if (exit_state == nullptr) return;

    if (exit_state->is_definitely_set || exit_state->is_definitely_unset) {
      occurrences.replace(affected_name, *exit_state);
      return;
    }

    let const *caller_state = occurrences.find(affected_name);
    if (caller_state == nullptr) {
      occurrences.replace(affected_name, *exit_state);
      return;
    }

    let merged_state = *caller_state;
    merged_state.assignment_indices.merge(exit_state->assignment_indices);
    merged_state.is_definitely_set =
        merged_state.is_definitely_set && exit_state->is_definitely_set;
    merged_state.is_definitely_unset = false;
    merged_state.has_unset_path =
        merged_state.has_unset_path || exit_state->has_unset_path;
    merged_state.has_inherited_path =
        merged_state.has_inherited_path || exit_state->has_inherited_path;
    occurrences.replace(affected_name, steal(merged_state));
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

static pure fn assign_form_target_name(StringView expansion_text) wontthrow
    -> StringView
{
  let const name = expressions::internal::operand_target_name(expansion_text);
  if (!lexer::word_is_variable_name(name)) return StringView{};

  let remainder = expansion_text.substring(name.length);
  if (!remainder.is_empty() && remainder[0] == ':')
    remainder = remainder.substring(1);

  if (remainder.is_empty() || remainder[0] != '=') {
    return StringView{};
  }

  return name;
}

fn AnalysisContext::note_variable_scope(StringView name) throws -> void
{
  let const scope =
      active_function_definition_index == NO_ACTIVE_FUNCTION_DEFINITION
          ? usize{0}
          : active_function_definition_index + 1;
  let const first_scope = variable_first_scopes.find(name);
  if (!first_scope.has_value()) {
    variable_first_scopes.set(name, scope);
    return;
  }

  if (**first_scope != scope) shared_scope_variable_names.add(name);
}

pure fn AnalysisContext::is_posix_mood() const wontthrow -> bool
{
  return is_posix_sh_shebang ||
         (eval_context != nullptr &&
          eval_context->runtime_state().get_mood() == mimic_mood::Posix);
}

fn AnalysisContext::note_variable_read(StringView name,
                                       const SourceLocation &location,
                                       bool is_top_level_unconditional) throws
    -> void
{
  if (!is_top_level_unconditional) return;
  if (effects.has_seen_runtime_definer) return;

  if (!lexer::word_is_variable_name(name)) {
    let const assigned = assign_form_target_name(name);
    if (!assigned.is_empty()) {
      let state = variable_occurrence_state{};
      state.is_definitely_set = true;
      occurrences.replace(assigned, steal(state));
      note_variable_assignment(assigned, location, true);
    }

    return;
  }

  let const *assignment_state = occurrences.find(name);
  if (assignment_state != nullptr && assignment_state->is_definitely_set)
    return;
  if (inherited_assigned_names.contains(name)) return;
  if (function_local_names.find(name).has_value()) return;
  if (global_assigned_names.find(name).has_value()) return;
  if (reads_before_assignment.find(name).has_value()) return;
  if (expressions::internal::is_shell_maintained_variable(name)) return;

  if (eval_context != nullptr &&
      (eval_context->is_exported(name) ||
       eval_context->variable_store().shell_variables().find(name).has_value()))
  {
    return;
  }

  reads_before_assignment.set(name, location);
}

cold fn expressions::internal::report_command_resolution_error(
    EvalContext &cxt, CommandResolutionErrorWithLocation &e) throws -> void
{
  let const trace_location = e.location();
  let const windowed = window_function_body_error(cxt, e);
  show_message(e.to_string(windowed.has_value()
                               ? *windowed
                               : cxt.source_store().current_source_view(),
                           &cxt));
  cxt.print_source_backtrace(trace_location);
}

fn expressions::internal::window_function_body_error(
    EvalContext &cxt, ErrorWithLocation &error) wontthrow -> Maybe<StringView>
{
  let const resolved = cxt.resolve_render_source(error.location());
  if (!resolved.is_windowed || resolved.text == nullptr) {
    return None;
  }

  let const rebased = resolved.rebase(error.location());
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

fn Expression::is_compound_command() const wontthrow -> bool { return false; }

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

fn Expression::as_compound_list() const wontthrow
    -> const expressions::CompoundList *
{
  return nullptr;
}

fn Expression::always_exits(const AnalysisContext &) const wontthrow -> bool
{
  return false;
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

fn Expression::as_redirected_command() const wontthrow
    -> const expressions::RedirectedCommand *
{
  return nullptr;
}

fn Expression::error_report_location() const wontthrow -> SourceLocation
{
  return source_location();
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

fn expressions::internal::static_command_name(const Token *token) throws
    -> Maybe<StringView>
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return koshka::None;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();

  for (let const &segment : word.segments) {
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

fn expressions::internal::normalized_relative_executable_path(
    StringView path) throws -> Maybe<String>
{
  if (path.is_empty()) return None;

  let const typed_path = Path{path};
  if (!typed_path.is_relative()) return None;

  let normalized = typed_path.normalized().text();
  if (normalized == ".") return None;

  return normalized;
}

fn expressions::internal::borrowed_token_text(const Token *token,
                                              String &storage) throws
    -> StringView
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

fn expressions::internal::wrapped_command_index(
    command_name_id wrapper_id, const ArrayList<const Token *> &args) throws
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

fn expressions::internal::apply_followed_source_effects(
    AnalysisContext &actx, const followed_source_effects &followed,
    bool should_merge_parent_state, bool should_merge_parent_uncertainty) throws
    -> void
{
  if (should_merge_parent_state) {
    followed.defined_functions.for_each(
        [&actx](StringView name) { actx.add_defined_function(name); });
    followed.known_aliases.for_each(
        [&actx](StringView name) { actx.add_known_alias(name); });
    followed.assigned_names.for_each([&actx](StringView name) {
      actx.add_scoped_name(actx.inherited_assigned_names, name);
      if (actx.current_source_effects != nullptr)
        actx.current_source_effects->assigned_names.add(name);
    });
    followed.global_assigned_names.for_each([&actx](StringView name) {
      actx.add_scoped_name(actx.inherited_global_assigned_names, name);
      if (actx.current_source_effects != nullptr)
        actx.current_source_effects->global_assigned_names.add(name);
    });
    followed.array_valued_names.for_each(
        [&actx](StringView name) { actx.add_array_valued_name(name); });
  }

  if (should_merge_parent_uncertainty) {
    if (followed.effects.has_seen_runtime_definer)
      actx.mark_runtime_definer_seen();
    if (followed.effects.has_unknown_path) {
      actx.mark_path_unknown(
          followed.effects.should_silence_unresolved_commands);
    }
    if (followed.effects.has_unknown_working_directory)
      actx.mark_working_directory_unknown();
  }
  actx.reporter.has_fatal = actx.reporter.has_fatal || followed.has_fatal;
}

fn expressions::internal::analyze_followed_source(
    AnalysisContext &actx, const ArrayList<const Token *> &args,
    usize command_index, bool should_merge_parent_state,
    bool should_merge_parent_uncertainty) throws -> bool
{
  if (actx.followed_source_paths == nullptr ||
      actx.followed_source_effects_cache == nullptr ||
      actx.eval_context == nullptr ||
      actx.eval_context->arena_store().parse_arena() == nullptr ||
      command_index + 1 >= args.count())
  {
    return true;
  }

  let path_index = command_index + 1;
  let const option = static_command_name(args[path_index]);
  if (option.has_value() && *option == "--help") return true;
  if (option.has_value() && *option == "--") path_index++;
  if (path_index >= args.count()) return true;

  let const do_give_up_on_source = [&actx]() throws -> bool {
    actx.mark_path_unknown(false);
    actx.mark_working_directory_unknown();
    return false;
  };

  let const literal_path = static_command_name(args[path_index]);
  if (!literal_path.has_value()) return do_give_up_on_source();

  let tilde_expansion = source_tilde_expansion::Disabled;
  if (args[path_index]->kind() == Token::Kind::Word) {
    let const &word =
        static_cast<const tokens::WordToken *>(args[path_index])->word();
    if (!word.segments.is_empty() &&
        word.segments.front().is_tilde_candidate() &&
        !word.segments.front().text.is_empty() &&
        word.segments.front().text.first_character() == '~')
    {
      tilde_expansion = source_tilde_expansion::Enabled;
    }
  }
  if (tilde_expansion == source_tilde_expansion::Enabled &&
      actx.effects.has_unknown_working_directory)
  {
    return false;
  }
  let const source_path = Path{*literal_path};
  if (tilde_expansion == source_tilde_expansion::Disabled &&
      !source_path.is_absolute())
  {
    if (os::has_directory_separator(*literal_path)) {
      if (actx.effects.has_unknown_working_directory) return false;
    } else if (actx.effects.has_unknown_path ||
               actx.effects.has_unknown_working_directory)
    {
      return false;
    }
  }
  let resolved_path =
      actx.eval_context->resolve_source_path(*literal_path, tilde_expansion);
  if (!resolved_path.has_value()) return do_give_up_on_source();
  if (!resolved_path->is_absolute() &&
      !actx.options.source_base_directory.is_empty())
  {
    let based_path = Path{actx.options.source_base_directory};
    based_path.append(resolved_path->view());
    resolved_path = steal(based_path);
  }

  let canonical_path = os::canonical_path(*resolved_path);
  if (!canonical_path.has_value()) return do_give_up_on_source();

  if (let const effects = actx.followed_source_effects_cache->find(
          canonical_path->text().view());
      effects.has_value())
  {
    apply_followed_source_effects(actx, *effects.value(),
                                  should_merge_parent_state,
                                  should_merge_parent_uncertainty);
    return should_merge_parent_state;
  }

  let contents = actx.source_provider != nullptr
                     ? actx.source_provider->read_source(*canonical_path)
                     : Maybe<String>{None};
  static constexpr u64 MAX_FOLLOWED_SOURCE_BYTES = 16 * 1024 * 1024;
  if (!contents.has_value()) {
    let const size = canonical_path->file_size();
    if (!canonical_path->is_regular_file() || !size.has_value() ||
        *size > MAX_FOLLOWED_SOURCE_BYTES)
    {
      return do_give_up_on_source();
    }
    contents = canonical_path->read_entire_file();
  }
  if (!contents.has_value()) return do_give_up_on_source();
  contents->normalize_crlf_line_endings();

  if (!actx.followed_source_paths->add(canonical_path->text().view()))
    return false;

  let const arena_mark = actx.eval_context->arena_store().parse_arena()->mark();
  defer
  {
    actx.eval_context->arena_store().parse_arena()->release(arena_mark);
  };
  let parser = Parser{
      Lexer{contents->view(), *actx.eval_context->arena_store().parse_arena(),
            canonical_path->text().view(),
            actx.eval_context->runtime_state().get_mood()}
  };
  parser.set_analysis_metadata_collection_mode(
      analysis_metadata_collection_mode::Enabled);
  parser.set_substitution_validation_mode(
      substitution_validation_mode::Enabled);

  let parse_errors = ArrayList<String>{heap_allocator()};
  let const child_diagnostic_start =
      actx.reporter.sink != nullptr ? actx.reporter.sink->count() : 0;
  let const ast =
      parser.construct_ast(parse_errors, actx.eval_context, actx.reporter.sink);
  if (!parse_errors.is_empty()) {
    if (actx.reporter.sink != nullptr) {
      for (usize index = child_diagnostic_start;
           index < actx.reporter.sink->count(); index++)
      {
        let &diagnostic = (*actx.reporter.sink)[index];
        if (diagnostic.source_name.is_empty())
          diagnostic.source_name = canonical_path->text();
      }
    }
    if (actx.reporter.sink == nullptr)
      for (let const &error : parse_errors)
        show_message(error);
    actx.reporter.has_fatal = true;
    followed_source_effects effects{};
    effects.has_fatal = true;
    actx.followed_source_effects_cache->set(canonical_path->text().view(),
                                            steal(effects));
    return true;
  }

  let const directives = parser.take_analysis_directives();
  let const was_analyzed_under_uncertainty =
      actx.effects.has_unknown_path ||
      actx.effects.has_unknown_working_directory;

  followed_source_effects effects{};
  let child_options = actx.options;
  child_options.should_silence_unresolved_commands =
      actx.effects.should_silence_unresolved_commands;
  let const analyzed = analyze_ast(
      ast, contents->view(), actx.functions.defined, actx.functions.aliases,
      actx.eval_context, child_options, directives,
      {actx.followed_source_paths, actx.followed_source_effects_cache},
      {&actx, should_merge_parent_state, should_merge_parent_uncertainty,
       &effects},
      {nullptr, actx.reporter.sink, nullptr}, actx.source_provider);
  if (actx.reporter.sink != nullptr) {
    for (usize index = child_diagnostic_start;
         index < actx.reporter.sink->count(); index++)
    {
      let &diagnostic = (*actx.reporter.sink)[index];
      if (diagnostic.source_name.is_empty())
        diagnostic.source_name = canonical_path->text();
    }
  }
  if (!analyzed) actx.reporter.has_fatal = true;
  if (!was_analyzed_under_uncertainty) {
    actx.followed_source_effects_cache->set(canonical_path->text().view(),
                                            steal(effects));
  }

  return should_merge_parent_state;
}

fn expressions::internal::command_resolves(
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

  let environment_resolver = Maybe<ProgramResolver>{};
  ProgramResolver *resolver = nullptr;
  if (actx.eval_context != nullptr) {
    resolver = &actx.eval_context->program_resolver();
  } else {
    environment_resolver =
        ProgramResolver{os::get_environment_variable("PATH")};
    resolver = &*environment_resolver;
  }
  const bool was_resolved =
      resolver
          ->search(name, ProgramResolver::SearchMode::First,
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
  Outside,
  AfterOpen,
  InsideClass,
};

pure fn expressions::internal::word_has_malformed_glob_bracket(
    const Word &word) wontthrow -> bool
{
  let state = bracket_scan_state::Outside;

  for (let const &segment : word.segments) {
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
               EvalContext *eval_context, const analysis_options &options,
               const analysis_directives &directives,
               const analysis_followed_sources &followed_sources,
               const analysis_parent_link &parent,
               const analysis_outputs &outputs,
               AnalysisSourceProvider *source_provider,
               AnalysisUnitStream *unit_stream,
               const parsed_format_document *format_document) throws -> bool
{
  ASSERT(root != nullptr || unit_stream != nullptr);

  let const parent_analysis_context = parent.context;
  let const symbol_records = outputs.symbol_records;
  let const source_effects = parent.source_effects;

  AnalysisContext actx{source, options};
  actx.are_koshkit_utilities_reachable =
      eval_context != nullptr
          ? eval_context->runtime_state().koshkit_utilities_are_reachable()
          : options.is_default_mood;
  actx.reporter.suppressions = &directives.shellcheck_suppressions;
  actx.reporter.source_name_index = directives.source_name_index;
  actx.format_document = format_document;
  actx.eval_context = eval_context;
  actx.followed_source_paths = followed_sources.paths;
  actx.followed_source_effects_cache = followed_sources.effects_cache;
  actx.reporter.sink = outputs.diagnostic_sink;
  actx.source_provider = source_provider;
  actx.symbol_records = symbol_records;
  if (parent_analysis_context != nullptr) {
    actx.effects.has_seen_runtime_definer =
        parent_analysis_context->effects.has_seen_runtime_definer;
    actx.effects.has_unknown_path =
        parent_analysis_context->effects.has_unknown_path;
    actx.effects.has_unknown_working_directory =
        parent_analysis_context->effects.has_unknown_working_directory;
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

  expressions::internal::check_source_bytes(actx, source);

  if (parent_analysis_context != nullptr) {
    actx.is_posix_sh_shebang = parent_analysis_context->is_posix_sh_shebang;
  } else {
    expressions::internal::check_shebang(actx, source, options.shebang_policy);
  }

  expressions::internal::check_shellcheck_directives(
      actx, source, directives.directive_spans);

  expressions::internal::check_heredoc_terminators(actx, source,
                                                   directives.heredoc_misses);

  LOG(Debug, "analyzing the ast, the posix sh shebang gate is %s",
      actx.is_posix_sh_shebang ? "armed" : "off");

  known_functions.for_each(
      [&actx](StringView name) { actx.add_defined_function(name); });
  known_aliases.for_each(
      [&actx](StringView name) { actx.add_known_alias(name); });
  actx.current_source_effects = source_effects;
  actx.apply_scope_definitions(directives.scope_definitions);

  if (unit_stream != nullptr) {
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

  expressions::internal::check_command_name_assignments(actx);
  expressions::internal::check_unassigned_variable_reads(actx);
  expressions::internal::check_function_argument_dataflow(actx);

  for (let const &assignment : actx.function_global_assignments) {
    if (actx.shared_scope_variable_names.contains(assignment.name.view())) {
      actx.report_diagnostic(diagnostic_id::function_global_assignment,
                             assignment.location, {assignment.name.view()});
    }
  }

  if (symbol_records != nullptr)
    resolve_function_occurrence_states(*symbol_records);

  actx.flush_warnings();

  if (parent_analysis_context != nullptr) {
    parent_analysis_context->reporter.totals.warning_count +=
        actx.reporter.totals.warning_count;
    parent_analysis_context->reporter.totals.error_count +=
        actx.reporter.totals.error_count;
    ASSERT(source_effects != nullptr);
    source_effects->has_fatal = actx.reporter.has_fatal;
    apply_followed_source_effects(*parent_analysis_context, *source_effects,
                                  parent.should_merge_state,
                                  parent.should_merge_uncertainty);
  } else if (outputs.deferred_totals != nullptr) {
    outputs.deferred_totals->warning_count +=
        actx.reporter.totals.warning_count;
    outputs.deferred_totals->error_count += actx.reporter.totals.error_count;
  } else if (outputs.diagnostic_sink == nullptr) {
    actx.print_diagnostic_summary();
  }

  actx.print_optimizer_summary();

  return !actx.reporter.has_fatal;
}

namespace expressions {

using namespace internal;

pure fn internal::analysis_source_text(const AnalysisContext &actx,
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

pure fn internal::expansion_location_with_sigil(
    const AnalysisContext &actx, SourceLocation location) wontthrow
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

fn internal::note_variable_reference(AnalysisContext &actx,
                                     const WordSegment &segment,
                                     SourceLocation fallback_location) throws
    -> void
{
  let const segment_location =
      segment.get_source_location(fallback_location.source_name_index)
          .value_or(fallback_location);
  let const expansion_location =
      expansion_location_with_sigil(actx, segment_location);
  actx.note_variable_occurrence(segment.text.view(), expansion_location,
                                variable_occurrence_kind::Reference);
  actx.note_positional_reference(segment.text.view(), expansion_location);
}

internal::AnalysisScopeGuard::AnalysisScopeGuard(AnalysisContext &actx,
                                                 analysis_scope_mode mode)
    : m_actx{actx}, m_mode{mode},
      m_occurrences{mode == analysis_scope_mode::Substitution
                        ? variable_occurrence_pair{}
                        : actx.occurrences.snapshot()},
      m_function_mark{actx.functions.get_mark()},
      m_scoped_name_mark{actx.get_scoped_name_mark()},
      m_source_effects{actx.current_source_effects}, m_effects{actx.effects},
      m_was_inside_subshell_analysis{actx.walk.is_inside_subshell_analysis}
{
  if (mode == analysis_scope_mode::Substitution) {
    m_constants = steal(actx.constant_variables);
    actx.constant_variables = StringMap<String>{heap_allocator()};
    actx.walk.is_inside_subshell_analysis = true;

    return;
  }

  actx.current_source_effects = nullptr;

  switch (mode) {
  case analysis_scope_mode::Pipeline:
  case analysis_scope_mode::Substitution: break;

  case analysis_scope_mode::Subshell:
    m_constants = steal(actx.constant_variables);
    actx.walk.is_inside_subshell_analysis = true;
    break;

  case analysis_scope_mode::Function:
    m_constants = steal(actx.constant_variables);
    m_function_local_names = steal(actx.function_local_names);
    actx.occurrences = variable_occurrence_pair{};
    m_loop_body_depth = actx.loop_body_depth;
    actx.loop_body_depth = 0;
    m_conditional_branch_depth = actx.conditional_branch_depth;
    actx.conditional_branch_depth = 0;
    m_active_function_definition_index = actx.active_function_definition_index;
    actx.function_scope_depth++;
    break;
  }
}

internal::AnalysisScopeGuard::~AnalysisScopeGuard() { leave(); }

fn internal::AnalysisScopeGuard::leave() throws -> void
{
  if (m_mode == analysis_scope_mode::Substitution) {
    m_actx.walk.is_inside_subshell_analysis = m_was_inside_subshell_analysis;
    m_actx.constant_variables = steal(m_constants);

    return;
  }

  m_actx.current_source_effects = m_source_effects;
  m_actx.rollback_scoped_names(m_scoped_name_mark);
  m_actx.occurrences = steal(m_occurrences);
  m_actx.functions.rollback(m_function_mark);

  switch (m_mode) {
  case analysis_scope_mode::Pipeline:
  case analysis_scope_mode::Substitution: m_actx.effects = m_effects; break;

  case analysis_scope_mode::Subshell:
    m_actx.effects = m_effects;
    m_actx.walk.is_inside_subshell_analysis = m_was_inside_subshell_analysis;
    m_actx.constant_variables = steal(m_constants);
    break;

  case analysis_scope_mode::Function:
    m_actx.function_scope_depth--;
    m_actx.active_function_definition_index =
        m_active_function_definition_index;
    m_actx.loop_body_depth = m_loop_body_depth;
    m_actx.conditional_branch_depth = m_conditional_branch_depth;
    m_actx.function_local_names = steal(m_function_local_names);
    m_actx.constant_variables = steal(m_constants);
    break;
  }
}

static fn body_is_bare_file_read(const Expression *ast) wontthrow -> bool
{
  let const *list = ast->as_compound_list();
  let const *command =
      list != nullptr ? list->single_unconditional_command() : nullptr;
  let const *simple =
      command != nullptr ? command->as_simple_command() : nullptr;
  if (simple == nullptr || !simple->args().is_empty() ||
      !simple->local_vars().is_empty() || simple->redirections().count() != 1)
  {
    return false;
  }

  return simple->redirections()[0].kind == Redirection::Kind::ReadInput;
}

static fn is_plain_substitution_word(const Token *token) throws -> bool
{
  if (token == nullptr || token->kind() != Token::Kind::Word) return false;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  for (let const &segment : word.segments) {
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::CommandSubstitution: break;

    case WordSegment::Kind::VariableReference:
      if (!lexer::word_is_variable_name(segment.text.view())) return false;
      break;

    default: return false;
    }
  }

  return true;
}

static fn is_trivial_substitution_body(const AnalysisContext &actx,
                                       const Expression *ast) throws -> bool
{
  let const *list = ast->as_compound_list();
  let const *command =
      list != nullptr ? list->single_unconditional_command() : nullptr;
  let const *simple =
      command != nullptr ? command->as_simple_command() : nullptr;
  if (simple == nullptr || !simple->local_vars().is_empty()) return false;

  let const &args = simple->args();
  if (args.is_empty()) return false;

  let const name = static_command_name(args[0]);
  if (!name.has_value()) return false;

  if (actx.functions.defined.contains(*name) ||
      actx.functions.aliases.contains(*name))
  {
    return false;
  }

  let const info = get_analysis_command_info(*name);
  switch (info.id) {
  case command_name_id::Cd:
  case command_name_id::Unset:
  case command_name_id::Eval:
  case command_name_id::Dot:
  case command_name_id::Source:
  case command_name_id::Alias:
  case command_name_id::Command:
  case command_name_id::Builtin: return false;

  default: break;
  }

  constexpr u32 STATE_GROUPS =
      COMMAND_GROUP_RUNTIME_DEFINER | COMMAND_GROUP_ASSIGNMENT_BUILTIN |
      COMMAND_GROUP_DECLARATION_BUILTIN | COMMAND_GROUP_VARIABLE_TARGET;
  if (info.is_in_group(STATE_GROUPS)) return false;

  if (search_builtin(*name).has_value()) {
    let const is_neutral = info.is_in_group(COMMAND_GROUP_ENVIRONMENT_NEUTRAL);
    if (!is_neutral && info.id != command_name_id::Printf) return false;
  } else if (args.count() == 1) {
    return false;
  }

  for (usize index = 0; index < args.count(); index++) {
    if (!is_plain_substitution_word(args[index])) return false;

    if (info.id == command_name_id::Printf && index > 0) {
      let const operand = static_command_name(args[index]);
      if (!operand.has_value() || operand->starts_with("-v")) return false;
    }
  }

  for (let const &redirection : simple->redirections()) {
    if (redirection.fd_allocation_name_token != nullptr ||
        redirection.kind == Redirection::Kind::Heredoc ||
        redirection.kind == Redirection::Kind::HereString ||
        (redirection.target != nullptr &&
         !is_plain_substitution_word(redirection.target)))
    {
      return false;
    }
  }

  return true;
}

static fn analyze_substitution_body(AnalysisContext &actx,
                                    const lexer::nested_substitution &body,
                                    const SourceLocation &location,
                                    bool is_subshell,
                                    bool is_unconditional) throws -> void
{
  if (!body.is_exact ||
      actx.substitution_analysis_depth >= lexer::MAX_SUBSTITUTION_NESTING_DEPTH)
  {
    return;
  }

  let &arena = actx.substitution_arena;
  let const arena_mark = arena.mark();
  defer { arena.release(arena_mark); };
  actx.substitution_analysis_depth++;
  defer { actx.substitution_analysis_depth--; };

  let const mood = actx.eval_context != nullptr
                       ? actx.eval_context->runtime_state().get_mood()
                       : mimic_mood::Default;
  let nested_lexer = Lexer{
      actx.source.substring_of_length(0, body.body_position + body.body_length),
      arena, source_name_at(location.source_name_index), mood};
  nested_lexer.set_start_position(body.body_position);
  let nested_parser = Parser{steal(nested_lexer)};
  let parse_errors = ArrayList<String>{heap_allocator()};
  let const *ast = nested_parser.construct_ast(parse_errors, nullptr, nullptr);
  if (!parse_errors.is_empty()) return;

  let const was_direct_pipeline_stage = actx.walk.is_direct_pipeline_stage;
  let const was_analyzing_condition = actx.walk.is_analyzing_condition;
  let const was_bare_read_substitution = actx.walk.is_bare_read_substitution;
  let const was_inside_substitution_subshell =
      actx.walk.is_inside_substitution_subshell;
  let const saved_getopts = actx.active_getopts;
  top_level_sibling_carry *const saved_carry = actx.stream_sibling_carry;
  actx.walk.is_direct_pipeline_stage = false;
  actx.walk.is_analyzing_condition = false;
  actx.stream_sibling_carry = nullptr;
  actx.walk.is_bare_read_substitution = body_is_bare_file_read(ast);
  actx.walk.is_inside_substitution_subshell =
      is_subshell || was_inside_substitution_subshell;

  if (is_subshell) {
    let const mode = is_trivial_substitution_body(actx, ast)
                         ? analysis_scope_mode::Substitution
                         : analysis_scope_mode::Subshell;
    let scope = AnalysisScopeGuard{actx, mode};
    ast->analyze(actx, is_unconditional);
  } else {
    ast->analyze(actx, is_unconditional);
  }

  actx.walk.is_inside_substitution_subshell = was_inside_substitution_subshell;
  actx.walk.is_bare_read_substitution = was_bare_read_substitution;
  actx.stream_sibling_carry = saved_carry;
  actx.active_getopts = saved_getopts;
  actx.walk.is_analyzing_condition = was_analyzing_condition;
  actx.walk.is_direct_pipeline_stage = was_direct_pipeline_stage;
}

fn internal::analyze_region_substitutions(AnalysisContext &actx,
                                          const SourceLocation &location,
                                          usize region_position,
                                          usize region_length, bool is_heredoc,
                                          bool is_unconditional) throws -> void
{
  if (region_position > actx.source.length ||
      region_length > actx.source.length - region_position)
  {
    return;
  }

  let const found = lexer::find_nested_substitutions(
      actx.source, region_position, region_length, is_heredoc, false);
  for (let const &body : found)
    analyze_substitution_body(actx, body, location, true, is_unconditional);
}

fn internal::analyze_word_substitutions(AnalysisContext &actx, const Word &word,
                                        const SourceLocation &location,
                                        bool is_unconditional) throws -> void
{
  for (let const &segment : word.segments) {
    switch (segment.kind) {
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::ProcessSubstitution:
    case WordSegment::Kind::FunctionSubstitution: {
      let const body = lexer::find_segment_substitution(actx.source, segment);
      if (!body.has_value()) break;

      analyze_substitution_body(actx, *body, location,
                                segment.kind !=
                                    WordSegment::Kind::FunctionSubstitution,
                                is_unconditional);
      break;
    }

    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::ArithmeticExpansion: {
      let const text = segment.text.view();
      if (!text.find_character('$').has_value() &&
          !text.find_character('`').has_value())
      {
        break;
      }

      let const position = static_cast<usize>(segment.source_position);
      if (text.length > actx.source.length ||
          position > actx.source.length - text.length ||
          actx.source.substring_of_length(position, text.length) != text)
      {
        break;
      }

      let const found = lexer::find_nested_substitutions(
          actx.source, position, text.length, false,
          segment.is_in_double_quotes != 0);
      for (let const &body : found)
        analyze_substitution_body(actx, body, location, true, is_unconditional);
      break;
    }

    default: break;
    }
  }
}

fn internal::analyze_token_substitutions(AnalysisContext &actx,
                                         const Token *token,
                                         bool is_unconditional) throws -> void
{
  if (token == nullptr) return;

  if (token->kind() == Token::Kind::Assignment) {
    analyze_word_substitutions(
        actx, static_cast<const tokens::Assignment *>(token)->value_word(),
        token->source_location(), is_unconditional);
    return;
  }

  if (token->kind() != Token::Kind::Word) return;

  analyze_word_substitutions(
      actx, static_cast<const tokens::WordToken *>(token)->word(),
      token->source_location(), is_unconditional);
}

fn internal::analyze_token_list_substitutions(
    AnalysisContext &actx, const ArrayList<const Token *> &tokens,
    bool is_unconditional) throws -> void
{
  for (let const *token : tokens)
    analyze_token_substitutions(actx, token, is_unconditional);
}

fn internal::analyze_redirection_substitutions(
    AnalysisContext &actx, const Redirection &redirection,
    const SourceLocation &node_location, bool is_unconditional) throws -> void
{
  analyze_token_substitutions(actx, redirection.target, is_unconditional);
  if (redirection.fd_allocation_name_token != nullptr) {
    let const allocation_target = static_cast<const tokens::WordToken *>(
                                      redirection.fd_allocation_name_token)
                                      ->word()
                                      .get_fd_allocation_target();
    if (allocation_target.has_value()) {
      let const allocation_location =
          redirection.fd_allocation_name_token->source_location();
      let const name_location =
          allocation_location.length > allocation_target->name.length
              ? allocation_location.subspan(1, allocation_target->name.length)
              : allocation_location;
      let const is_definite =
          is_unconditional && !actx.effects.has_seen_runtime_definer;
      actx.note_variable_occurrence(allocation_target->name, name_location,
                                    variable_occurrence_kind::Assignment,
                                    !is_definite);
      actx.note_variable_assignment(allocation_target->name, name_location,
                                    is_definite);
    }
  }

  if (redirection.heredoc == nullptr || !redirection.should_expand_heredoc ||
      redirection.heredoc->source_end_position <=
          redirection.heredoc->source_position)
  {
    return;
  }

  analyze_region_substitutions(actx, node_location,
                               redirection.heredoc->source_position,
                               redirection.heredoc->source_end_position -
                                   redirection.heredoc->source_position,
                               true, is_unconditional);
}

pure fn internal::location_spanning(SourceLocation first,
                                    SourceLocation last) wontthrow
    -> SourceLocation
{
  if (first.length == 0) return last;
  if (last.length == 0) return first;
  if (last.position < first.position) return first;

  return SourceLocation{first.position,
                        last.position + last.length - first.position,
                        first.source_name_index};
}

pure fn internal::arithmetic_reads_external_input(
    const AnalysisContext &actx, StringView expression) wontthrow -> bool
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

hot fn IfStatement::evaluate_impl(EvalContext &cxt, root_evaluation_mode) const
    throws -> status_result
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_then != nullptr);

  let const condition = m_condition->evaluate(cxt);
  if (cxt.control_flow_store().has_pending()) return condition;

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

fn Command::make_async() wontthrow -> void
{
  set_execution_flag(ExecutionFlag::Async);
}

pure fn Command::is_async() const wontthrow -> bool
{
  return has_execution_flag(ExecutionFlag::Async);
}

fn Command::append_ast_execution_flags(String &label) const throws -> void
{
  if (is_async()) label += ", Async";
}

fn Command::set_negated() wontthrow -> void
{
  set_execution_flag(ExecutionFlag::Negated);
}

pure fn Command::is_negated() const wontthrow -> bool
{
  return has_execution_flag(ExecutionFlag::Negated);
}

fn Command::set_timed(SourceLocation location, time_format_mode format,
                      time_rss_mode rss) wontthrow -> void
{
  set_execution_flag(ExecutionFlag::Timed);
  set_execution_flag(ExecutionFlag::TimePosixFormat,
                     format == time_format_mode::Posix);
  set_execution_flag(ExecutionFlag::TimeReportRss,
                     rss == time_rss_mode::Include);
  m_time_position = location.position;
}

pure fn Command::is_timed() const wontthrow -> bool
{
  return has_execution_flag(ExecutionFlag::Timed);
}

pure fn Command::time_location() const wontthrow -> SourceLocation
{
  constexpr usize TIME_KEYWORD_LENGTH = 4;

  return SourceLocation{m_time_position, TIME_KEYWORD_LENGTH,
                        source_location().source_name_index};
}

pure fn Command::get_time_format_mode() const wontthrow -> time_format_mode
{
  return has_execution_flag(ExecutionFlag::TimePosixFormat)
             ? time_format_mode::Posix
             : time_format_mode::Default;
}

pure fn Command::get_time_rss_mode() const wontthrow -> time_rss_mode
{
  return has_execution_flag(ExecutionFlag::TimeReportRss)
             ? time_rss_mode::Include
             : time_rss_mode::Omit;
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

DummyExpression::DummyExpression(SourceLocation location)
    : Expression(steal(location))
{}

fn DummyExpression::is_dummy() const wontthrow -> bool { return true; }

fn DummyExpression::evaluate_impl(EvalContext &cxt, root_evaluation_mode) const
    throws -> status_result
{
  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

cold fn DummyExpression::to_string() const throws -> String { return "Dummy"; }

} /* namespace expressions */

} /* namespace koshka */
