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
    : m_location(location),
      m_source_end_position(location.position + location.length)
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
  m_source_end_position = position;
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

fn AnalysisContext::warn(SourceLocation location, StringView message,
                         StringView suggestion, diagnostic_tier tier,
                         Maybe<SourceLocation> related_location,
                         StringView related_message) throws -> void
{
  if (tier == diagnostic_tier::Annoying && !should_emit_annoying_diagnostics) {
    return;
  }

  u8 required_level = 0;
  switch (tier) {
  case diagnostic_tier::Strict: required_level = 1; break;
  case diagnostic_tier::Lenient: required_level = 2; break;
  case diagnostic_tier::Annoying: required_level = 3; break;
  }
  if (!is_default_mood && warning_level < required_level) {
    return;
  }

  reported_warning_count++;

  pending_warnings.push(
      pending_analysis_warning{location, String{message}, String{suggestion},
                               related_location, String{related_message}});
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
  let const tier = get_diagnostic_definition(id).tier;

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
    diagnostic_id id, SourceLocation location,
    std::initializer_list<StringView> arguments,
    Maybe<SourceLocation> related_location) throws -> void
{
  if (!should_report(id)) return;
  if (is_diagnostic_suppressed(id, location)) return;

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
    fail(location, message.view(), suggestion.view(), definition.tier,
         related_location, related_message.view());
    break;
  case diagnostic_delivery::Warning:
    warn(location, message.view(), suggestion.view(), definition.tier,
         related_location, related_message.view());
    break;
  }
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
    SourceLocation location) const throws -> void
{
  if (eval_context != nullptr) eval_context->print_source_backtrace(location);
}

fn AnalysisContext::fail(SourceLocation location, StringView message,
                         StringView suggestion, diagnostic_tier tier,
                         Maybe<SourceLocation> related_location,
                         StringView related_message) throws -> void
{
  if (tier == diagnostic_tier::Annoying && !should_emit_annoying_diagnostics) {
    return;
  }

  u8 required_level = 0;
  switch (tier) {
  case diagnostic_tier::Strict: required_level = 1; break;
  case diagnostic_tier::Lenient: required_level = 2; break;
  case diagnostic_tier::Annoying: required_level = 3; break;
  }
  if (!is_default_mood) {
    if (warning_level >= required_level)
      warn(location, message, suggestion, tier, related_location,
           related_message);
    return;
  }

  if (tier == diagnostic_tier::Annoying) {
    warn(location, message, suggestion, tier, related_location,
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
    warn(location, message, suggestion, tier, related_location,
         related_message);
    return;
  }

  flush_warnings();
  reported_error_count++;

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
    diagnostic_id id, SourceLocation location) const wontthrow -> bool
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

fn AnalysisContext::note_variable_assignment(StringView name,
                                             SourceLocation location) throws
    -> void
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

/* The name an assign form ${name=value} or ${name:=value} writes back, or an
   empty view for every other expansion. */
pure fn assign_form_target_name(StringView expansion_text) wontthrow
    -> StringView
{
  let const name = expressions::operand_target_name(expansion_text);
  if (!optimizer::is_plain_variable_name(name)) return StringView{};

  let remainder = expansion_text.substring(name.length);
  if (!remainder.is_empty() && remainder[0] == ':')
    remainder = remainder.substring(1);

  if (remainder.is_empty() || remainder[0] != '=') {
    return StringView{};
  }

  return name;
}

fn AnalysisContext::note_variable_read(StringView name, SourceLocation location,
                                       bool is_top_level_unconditional) throws
    -> void
{
  if (!is_top_level_unconditional) return;
  if (has_seen_runtime_definer) return;

  if (!optimizer::is_plain_variable_name(name)) {
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
  rebased.position = resolved.to_render_position(rebased.position);
  rebased.filename = resolved.filename_or_none();
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

namespace {

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
  if (wrapper_id == command_name_id::Builtin) return 1;
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

  usize path_index = command_index + 1;
  let const option = static_command_name(args[path_index]);
  if (option.has_value() && *option == "--help") return true;
  if (option.has_value() && *option == "--") {
    path_index++;
  }
  if (path_index >= args.count()) return true;

  let const literal_path = static_command_name(args[path_index]);
  if (!literal_path.has_value()) {
    actx.mark_path_unknown(false);
    actx.mark_working_directory_unknown();
    return false;
  }

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
  if (!resolved_path.has_value()) return true;

  let canonical_path = os::canonical_path(*resolved_path);
  if (!canonical_path.has_value()) return true;
  if (let const *effects = actx.followed_source_effects_cache->find(
          canonical_path->text().view());
      effects != nullptr)
  {
    apply_followed_source_effects(actx, *effects, should_merge_parent_state,
                                  should_merge_parent_uncertainty);
    return should_merge_parent_state;
  }
  if (!actx.followed_source_paths->add(canonical_path->text().view())) {
    return false;
  }

  let contents = canonical_path->read_entire_file();
  if (!contents.has_value()) return true;
  contents->normalize_crlf_line_endings();

  let const arena_mark = AST_ARENA->mark();
  defer { AST_ARENA->release(arena_mark); };
  let *previous_function_arena = FUNCTION_ARENA;
  FUNCTION_ARENA = nullptr;
  defer { FUNCTION_ARENA = previous_function_arena; };

  let parser = Parser{
      Lexer{String{contents->view()}, *AST_ARENA, false,
            canonical_path->text().view(), actx.eval_context->mood()}
  };
  parser.set_should_collect_analysis_scopes(true);

  let parse_errors = ArrayList<String>{heap_allocator()};
  let const ast = parser.construct_ast(parse_errors, actx.eval_context);
  if (!parse_errors.is_empty()) {
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
  followed_source_effects effects{};
  let const analyzed = analyze_ast(
      ast, contents->view(), actx.defined_functions, actx.known_aliases,
      actx.eval_context, actx.warning_level,
      actx.should_silence_unresolved_commands, actx.is_default_mood,
      actx.should_emit_annoying_diagnostics, shellcheck_suppressions,
      scope_definitions, directive_spans, heredoc_misses, false,
      actx.should_report_optimizer_diagnostics, actx.followed_source_paths,
      actx.followed_source_effects_cache, &actx, nullptr,
      should_merge_parent_state, should_merge_parent_uncertainty, &effects);
  if (!analyzed) actx.has_fatal = true;
  actx.followed_source_effects_cache->set(canonical_path->text().view(),
                                          steal(effects));

  return should_merge_parent_state;
}

fn command_resolves(
    StringView name, SourceLocation location, const AnalysisContext &actx,
    Maybe<utils::unavailable_path_source_component> &unavailable) throws -> bool
{
  if (name.is_empty()) return false;
  if (search_builtin(name).has_value()) return true;
  /* The prepass runs only in the default mood, where a coreutil falls back to
     its koshkit implementation, so a koshkit name resolves without a PATH
     binary. */
  if (koshkit::find_util(name).has_value()) return true;
  if (os::has_directory_separator(name)) {
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

enum class bracket_scan_state
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

} /* namespace */

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
               followed_source_effects *source_effects) throws -> bool
{
  ASSERT(root != nullptr);

  AnalysisContext actx{source};
  actx.warning_level = warning_level;
  actx.is_default_mood = is_default_mood;
  actx.should_emit_annoying_diagnostics = should_emit_annoying_diagnostics;
  actx.shellcheck_suppressions = &shellcheck_suppressions;
  actx.should_silence_unresolved_commands = silence_unresolved_commands;
  actx.eval_context = eval_context;
  actx.should_report_optimizer_diagnostics =
      should_report_optimizer_diagnostics;
  actx.followed_source_paths = followed_source_paths;
  actx.followed_source_effects_cache = source_effects_cache;
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

  root->analyze(actx, true);

  expressions::check_command_name_assignments(actx);
  expressions::check_unassigned_variable_reads(actx);
  expressions::check_function_argument_dataflow(actx);

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
  } else {
    actx.print_diagnostic_summary();
  }

  actx.print_optimizer_summary();

  return !actx.has_fatal;
}

namespace expressions {

pure fn analysis_source_text(const AnalysisContext &actx,
                             SourceLocation location) wontthrow -> StringView
{
  if (location.position > actx.source.length ||
      location.length > actx.source.length - location.position)
    return {};
  return actx.source.substring_of_length(location.position, location.length);
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

  return SourceLocation{start, length, location.filename};
}

pure fn location_spanning(SourceLocation first, SourceLocation last) wontthrow
    -> SourceLocation
{
  if (first.length == 0) return last;
  if (last.length == 0) return first;
  if (last.position < first.position) return first;

  return SourceLocation{first.position,
                        last.position + last.length - first.position,
                        first.filename};
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
    : Expression(location), m_condition(condition), m_then(then),
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

Command::Command(SourceLocation location) : Expression(location) {}

fn Command::make_async() wontthrow -> void { m_is_async = true; }

pure fn Command::is_async() const wontthrow -> bool { return m_is_async; }

fn Command::set_negated() wontthrow -> void { m_is_negated = true; }

pure fn Command::is_negated() const wontthrow -> bool { return m_is_negated; }

fn Command::set_timed(bool posix_format, SourceLocation location) wontthrow
    -> void
{
  m_is_timed = true;
  m_is_time_posix_format = posix_format;
  m_time_location = location;
}

pure fn Command::is_timed() const wontthrow -> bool { return m_is_timed; }

pure fn Command::time_location() const wontthrow -> SourceLocation
{
  return m_time_location;
}

pure fn Command::time_uses_posix_format() const wontthrow -> bool
{
  return m_is_time_posix_format;
}

fn Command::set_local_vars(ArrayList<prefix_assignment> &&vars) throws -> void
{
  m_local_vars = steal(vars);
}

pure fn Command::local_vars() const wontthrow
    -> const ArrayList<prefix_assignment> &
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

DummyExpression::DummyExpression(SourceLocation location) : Expression(location)
{}

fn DummyExpression::is_dummy() const wontthrow -> bool { return true; }

fn DummyExpression::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

cold fn DummyExpression::to_string() const throws -> String { return "Dummy"; }

/* The special parameters carry their own splitting rules, so quoting advice
   never applies to them. */
pure fn is_split_exempt_variable_name(StringView name) wontthrow -> bool
{
  if (name.length != 1) return false;

  switch (name[0]) {
  case '@':
  case '*':
  case '?':
  case '#':
  case '$':
  case '!':
  case '-': return true;
  default: return false;
  }
}

/* A spread, a positional list, and a length read carry their own diagnostics,
   so only a plain scalar name splits into several array elements. */
pure fn reference_is_plain_scalar_name(StringView name) wontthrow -> bool
{
  if (name.is_empty()) return false;

  switch (name[0]) {
  case '#':
  case '*':
  case '@': return false;

  default: break;
  }

  return !name.find_character('[').has_value();
}

cold fn word_is_bare_glob(const Word &word) wontthrow -> bool
{
  return word.segments.count() == 1 &&
         word.segments[0].kind == WordSegment::Kind::UnquotedText &&
         word.segments[0].has_glob_metacharacter();
}

/* A brace expansion needs a comma or a range inside its braces, so a lone brace
   in a path is left alone. */
cold static fn view_has_brace_expansion(StringView text) wontthrow -> bool
{
  let const open = text.find_character('{');
  if (!open.has_value()) return false;

  for (usize position = *open + 1; position < text.length; position++) {
    switch (text[position]) {
    case '}': return false;
    case ',': return true;
    case '.':
      if (position + 1 < text.length && text[position + 1] == '.') {
        return true;
      }
      break;
    default: break;
    }
  }

  return false;
}

cold fn classify_test_operand(const Word &word) wontthrow -> test_operand_shape
{
  test_operand_shape shape;

  for (let const &segment : word.segments) {
    switch (segment.kind) {
    case WordSegment::Kind::VariableReference: {
      if (!segment.is_in_double_quotes) shape.has_unquoted_expansion = true;

      let const name = segment.text.view();
      if (name == "@" || (name.length >= 3 &&
                          name.substring(name.length - 3) == StringView{"[@]"}))
      {
        shape.has_array_spread = true;
      }
      if (reference_names_positional(name))
        shape.has_positional_reference = true;
      break;
    }

    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::FunctionSubstitution:
      if (!segment.is_in_double_quotes) shape.has_unquoted_expansion = true;
      break;

    case WordSegment::Kind::UnquotedText:
      if (segment.has_glob_metacharacter()) shape.has_unquoted_glob = true;
      if (view_has_brace_expansion(segment.text.view()))
        shape.has_brace_expansion = true;
      break;

    default: break;
    }
  }

  return shape;
}

cold fn bare_glob_can_start_with_dash(const Word &word) wontthrow -> bool
{
  if (!word_is_bare_glob(word)) return false;
  let const text = word.segments[0].text.view();
  if (text.is_empty()) return false;
  if (text[0] == '*' || text[0] == '?') {
    return true;
  }
  if (text[0] != '[') return false;

  let const close = text.find_character(']');
  if (!close.has_value()) return false;
  if (*close > 1 && (text[1] == '!' || text[1] == '^')) {
    return true;
  }
  for (usize position = 1; position < *close; position++)
    if (text[position] == '-' &&
        (position == 1 || position + 1 == *close || text[position - 1] == '\\'))
      return true;
  return false;
}

cold pure fn view_contains(StringView view, StringView needle) wontthrow -> bool
{
  if (needle.length == 0 || needle.length > view.length) {
    return false;
  }

  let const last_start = view.length - needle.length;
  for (usize start = 0; start <= last_start; start++) {
    if (view[start] != needle[0]) continue;

    usize matched = 1;
    while (matched < needle.length && view[start + matched] == needle[matched])
      matched++;

    if (matched == needle.length) return true;
  }

  return false;
}

cold pure fn substitution_body_is_bare_echo(StringView body) wontthrow -> bool
{
  usize start = 0;
  while (start < body.length && (body[start] == ' ' || body[start] == '\t'))
    start++;

  let const trimmed = body.substring(start);
  if (!trimmed.starts_with(StringView{"echo "}) && trimmed != "echo")
    return false;

  for (usize i = 0; i < trimmed.length; i++)
    if (trimmed[i] == '|' || trimmed[i] == ';' || trimmed[i] == '&' ||
        trimmed[i] == '<' || trimmed[i] == '>' || trimmed[i] == '`')
    {
      return false;
    }

  return true;
}

cold fn args_have_stdin_operand(const ArrayList<const Token *> &args) throws
    -> bool
{
  let storage = String{heap_allocator()};
  for (usize i = 1; i < args.count(); i++) {
    let const raw = borrowed_token_text(args[i], storage);
    if (raw == "-" || raw == "/dev/stdin") {
      return true;
    }
  }
  return false;
}

fn operand_target_name(StringView text) wontthrow -> StringView
{
  if (text.is_empty() || text[0] == '-') {
    return StringView{};
  }
  usize end = 0;
  while (end < text.length && lexer::is_variable_name(text[end]))
    end++;
  return text.substring_of_length(0, end);
}

fn SimpleCommand::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
  unused(is_unconditional);

  optimizer::optimize_node(this, actx);

  let const is_command_prefix = !m_args.is_empty();
  let const leading_command_word =
      is_command_prefix ? m_args[0]->raw_view() : Maybe<StringView>{};

  /* A prefix reaches only the environment of the command it leads, and a prefix
     on a POSIX special builtin persists after the command in the default
     mood. */
  let const prefix_outlives_command =
      !is_command_prefix || !leading_command_word.has_value() ||
      is_special_builtin_name(*leading_command_word);

  for (let const &var : m_local_vars) {
    /* A PATH=... prefix leaves the runtime search path unknown to the prepass,
       so the not-found check for the prefixed command and everything after it
       stays quiet. */
    if (var.name.view() == "PATH") actx.mark_path_unknown(true);
    if (is_source_location_variable(var.name.view()))
      actx.mark_working_directory_unknown();

    if (prefix_outlives_command)
      actx.note_variable_assignment(var.name.view(), var.location);

    if (actx.is_posix_sh_shebang && var.is_append) {
      actx.report_diagnostic(diagnostic_id::sc3024, var.location,
                             {var.name.view()});
    }

    let const shape = scan_assignment_value(actx, var.value, var.location);
    check_assignment_value_shape(
        actx, assignment_lint_input{
                  var.name.view(), analysis_source_text(actx, var.location),
                  var.location, var.is_append, is_command_prefix, shape});
  }

  for (let const &assignment : m_array_args) {
    actx.note_variable_assignment(assignment.name.view(), assignment.location);
    actx.add_array_valued_name(assignment.name.view());
    actx.constant_variables.erase(assignment.name.view());

    for (let const element : assignment.elements) {
      if (element->kind() != Token::Kind::Word) continue;

      let const &word = static_cast<const tokens::WordToken *>(element)->word();
      let has_unquoted_substitution = false;
      let has_unquoted_reference = false;
      for (let const &segment : word.segments) {
        if (segment.is_in_double_quotes) continue;

        switch (segment.kind) {
        case WordSegment::Kind::CommandSubstitution:
          has_unquoted_substitution = true;
          break;

        case WordSegment::Kind::VariableReference:
          if (reference_is_plain_scalar_name(segment.text.view()))
            has_unquoted_reference = true;
          break;

        default: break;
        }
      }

      if (has_unquoted_substitution) {
        actx.report_diagnostic(diagnostic_id::sc2207,
                               element->source_location());
      } else if (has_unquoted_reference) {
        actx.report_diagnostic(diagnostic_id::sc2206,
                               element->source_location());
      }
    }
  }

  if (m_args.is_empty()) {
    /* A bare redirection opens its target and nothing reads or writes it,
       shellcheck SC2188 and SC2189. An assignment-only command is the SC2036
       shape and is reported by the pipeline. */
    if (!m_redirections.is_empty() && m_local_vars.is_empty() &&
        m_array_args.is_empty())
    {
      let const id = actx.is_direct_pipeline_stage ? diagnostic_id::sc2189
                                                   : diagnostic_id::sc2188;
      let const target = m_redirections[0].target;
      actx.report_diagnostic(id, target != nullptr ? target->source_location()
                                                   : source_location());
    }

    for (let const &assignment : m_array_args) {
      if (assignment.elements.count() < 2) continue;

      let const first = assignment.elements[0];
      let const last = assignment.elements.back();
      if (first->kind() != Token::Kind::LeftParen ||
          last->kind() != Token::Kind::RightParen)
      {
        continue;
      }

      let const outer_open_position =
          assignment.location.position + assignment.location.length;
      let const expression_start_position =
          first->source_location().position + first->source_location().length;
      let const expression_end_position = last->source_location().position;
      let const outer_close_position =
          last->source_location().position + last->source_location().length;
      if (outer_open_position >= actx.source.length ||
          outer_close_position >= actx.source.length ||
          actx.source[outer_open_position] != '(' ||
          first->source_location().position != outer_open_position + 1 ||
          actx.source[outer_close_position] != ')')
      {
        continue;
      }

      usize parenthesis_depth = 0;
      let has_single_wrapping_pair = true;
      for (usize i = 0; i < assignment.elements.count(); i++) {
        let const kind = assignment.elements[i]->kind();
        if (kind == Token::Kind::LeftParen) {
          parenthesis_depth++;
        } else if (kind == Token::Kind::RightParen) {
          if (parenthesis_depth == 0) {
            has_single_wrapping_pair = false;
            break;
          }
          parenthesis_depth--;
          if (parenthesis_depth == 0 && i + 1 != assignment.elements.count()) {
            has_single_wrapping_pair = false;
            break;
          }
        }
      }
      if (!has_single_wrapping_pair || parenthesis_depth != 0) {
        continue;
      }

      let expression = actx.source.substring_of_length(
          expression_start_position,
          expression_end_position - expression_start_position);
      while (!expression.is_empty() &&
             (expression[0] == ' ' || expression[0] == '\t'))
        expression = expression.substring(1);
      while (!expression.is_empty() &&
             (expression[expression.length - 1] == ' ' ||
              expression[expression.length - 1] == '\t'))
        expression = expression.substring_of_length(0, expression.length - 1);
      if (expression.is_empty()) continue;

      let location = assignment.location;
      location.length = outer_close_position + 1 - location.position;
      actx.report_diagnostic(diagnostic_id::arith_assign, location,
                             {assignment.name.view(), expression});
    }

    return;
  }

  ASSERT(m_args[0] != nullptr);
  let const name = static_command_name(m_args[0]);
  let const borrowed_command_literal = m_args[0]->raw_view();
  let const command_literal_storage = borrowed_command_literal.has_value()
                                          ? String{heap_allocator()}
                                          : m_args[0]->raw_string();
  let const command_literal = borrowed_command_literal.has_value()
                                  ? *borrowed_command_literal
                                  : command_literal_storage.view();
  let const command_is_defined_function =
      actx.defined_functions.contains(command_literal);
  let const is_command_shadowed = command_is_defined_function ||
                                  actx.known_aliases.contains(command_literal);

  /* A literal command word borrows from the syntax tree, which outlives the
     sweep. A word the analysis had to build lives in a local and is left out.
   */
  if (command_is_defined_function && borrowed_command_literal.has_value()) {
    actx.function_calls.push(function_call_record{
        command_literal, m_args[0]->source_location(), m_args.count() > 1,
        actx.function_scope_depth != 0});
  }

  let const command_info = get_analysis_command_info(command_literal);
  let const command_id = command_info.id;
  if (!is_command_shadowed &&
      (command_id == command_name_id::Cd || command_literal == "pushd" ||
       command_literal == "popd"))
  {
    actx.mark_working_directory_unknown();
  }
  let const command_is_assignment_builtin =
      command_info.is_in_group(COMMAND_GROUP_ASSIGNMENT_BUILTIN);
  let const lint_input = command_lint_input{
      m_args,          m_redirections, m_local_vars,       source_location(),
      command_literal, command_info,   is_command_shadowed};

  usize source_command_index = m_args.count();
  if (!is_command_shadowed && (command_id == command_name_id::Dot ||
                               command_id == command_name_id::Source))
  {
    source_command_index = 0;
  } else if (!is_command_shadowed) {
    let const wrapped_index = wrapped_command_index(command_id, m_args);
    if (wrapped_index.has_value()) {
      let command_storage = String{heap_allocator()};
      let const wrapped_command =
          borrowed_token_text(m_args[*wrapped_index], command_storage);
      let const wrapped_command_id =
          get_analysis_command_info(wrapped_command).id;
      if (wrapped_command_id == command_name_id::Dot ||
          wrapped_command_id == command_name_id::Source)
      {
        source_command_index = *wrapped_index;
      }
    }
  }

  bool did_analyze_source = false;
  if (source_command_index < m_args.count()) {
    let const should_merge_source_state = is_unconditional;
    let const should_merge_source_uncertainty =
        actx.function_scope_depth == 0 && !actx.is_direct_pipeline_stage &&
        !actx.is_inside_subshell_analysis;
    did_analyze_source = analyze_followed_source(
        actx, m_args, source_command_index, should_merge_source_state,
        should_merge_source_uncertainty);
  }

  if (!is_command_shadowed && actx.is_inside_loop_condition &&
      command_id == command_name_id::Read)
  {
    actx.has_input_reading_loop_condition = true;
  }

  append_presence_tested_command_names(actx, actx.tested_command_names, true);

  if (!is_command_shadowed && actx.is_inside_read_loop &&
      command_id == command_name_id::Ssh)
  {
    actx.report_diagnostic(diagnostic_id::sc2095, m_args[0]->source_location());
  }

  check_operand_lints_before_scan(actx, lint_input);

  bool has_command_substitution_argument = false;
  let const should_scan_for_useless_echo =
      actx.should_report(diagnostic_id::sc2116);
  let const should_scan_for_malformed_glob =
      actx.should_report(diagnostic_id::malformed_glob);
  let const should_check_unquoted_expansion =
      !is_command_shadowed && !command_info.is_in_group(COMMAND_GROUP_TEST) &&
      !command_info.is_in_group(COMMAND_GROUP_VARIABLE_TARGET);
  let const should_check_dash_glob =
      !is_command_shadowed && !command_info.is_in_group(COMMAND_GROUP_TEST) &&
      command_id != command_name_id::Echo &&
      command_id != command_name_id::Printf;
  let const should_check_array_reads = actx.array_valued_names.count() != 0;
  let const should_check_quoted_values =
      actx.quoted_literal_assignments.count() != 0;
  bool has_end_of_options = false;

  for (usize i = 0; i < m_args.count(); i++) {
    let const arg = m_args[i];
    let const arg_location = arg->source_location();
    let const source_text = analysis_source_text(actx, arg_location);
    let const is_operand = i >= 1;
    const Word *const word =
        arg->kind() == Token::Kind::Word
            ? &static_cast<const tokens::WordToken *>(arg)->word()
            : nullptr;

    bool has_dollar_bracket = false;
    bool has_multi_digit_positional = false;
    bool has_double_dot = false;
    bool has_open_brace = false;
    bool has_dollar_byte = false;
    bool has_quote_break_for_dollar = false;
    bool has_literal_dollar_in_quotes = false;

    for (usize position = 0; position < source_text.length; position++) {
      let const byte = source_text[position];

      if (byte == '{') {
        has_open_brace = true;
      } else if (byte == '.') {
        if (position + 1 < source_text.length &&
            source_text[position + 1] == '.')
          has_double_dot = true;
      } else if (byte == '$') {
        has_dollar_byte = true;
        if (position + 1 < source_text.length &&
            source_text[position + 1] == '[')
        {
          has_dollar_bracket = true;
        }
        if (position + 2 < source_text.length &&
            source_text[position + 1] >= '1' &&
            source_text[position + 1] <= '9' &&
            source_text[position + 2] >= '0' &&
            source_text[position + 2] <= '9')
        {
          has_multi_digit_positional = true;
        }
        /* The quote that follows leaves the dollar sign literal. The word is
           read no further only when the quote also closes the word, and a
           dollar sign that carries text before it is ordinary prose. */
        if (position + 1 < source_text.length &&
            source_text[position + 1] == '"')
        {
          if (position + 2 < source_text.length) {
            has_quote_break_for_dollar = true;
          } else if (position > 0 && source_text[position - 1] == '"') {
            has_literal_dollar_in_quotes = true;
          }
        }
      }
    }

    if (source_text.length >= 2 && source_text[0] == '`' &&
        source_text[source_text.length - 1] == '`')
    {
      actx.report_diagnostic(diagnostic_id::sc2006, arg_location);
    }

    if (has_dollar_bracket)
      actx.report_diagnostic(diagnostic_id::sc2007, arg_location);

    bool has_dollar_in_arithmetic = false;
    bool has_external_arithmetic_read = false;
    bool has_unquoted_command_substitution = false;
    bool has_bare_star_reference = false;
    bool has_spread_alone_unquoted = false;
    bool has_spread_in_longer_word = false;
    bool has_split_eligible_variable = false;
    bool has_bracket_byte = false;
    bool has_quote_sandwich = false;
    u8 quote_sandwich_state = 0;
    StringView quote_sandwich_word{};
    usize echo_only_substitution_count = 0;
    StringView lost_pipeline_name{};
    StringView bare_array_name{};
    StringView quoted_value_name{};
    const SourceLocation *quoted_value_assignment = nullptr;
    SourceLocation split_eligible_location = arg_location;

    if (word != nullptr) {
      for (let const &segment : word->segments) {
        if (actx.is_posix_sh_shebang)
          check_posix_word_portability(actx, segment, arg_location);

        if (should_scan_for_malformed_glob && !has_bracket_byte &&
            segment.has_live_glob_chars() &&
            segment.text.view().find_character('[').has_value())
        {
          has_bracket_byte = true;
        }

        switch (segment.kind) {
        case WordSegment::Kind::ArithmeticExpansion:
          quote_sandwich_state = 0;
          check_arithmetic_expression_lints(
              actx, segment.text.view(),
              segment.get_source_location(arg_location.filename)
                  .value_or(arg_location));
          if (!has_dollar_in_arithmetic &&
              segment.text.view().find_character('$').has_value())
          {
            has_dollar_in_arithmetic = true;
          }
          if (is_operand && !has_external_arithmetic_read &&
              arithmetic_reads_external_input(actx, segment.text.view()))
          {
            has_external_arithmetic_read = true;
          }
          break;

        case WordSegment::Kind::CommandSubstitution:
          quote_sandwich_state = 0;
          has_command_substitution_argument = true;
          if (!segment.is_in_double_quotes)
            has_unquoted_command_substitution = true;
          if (should_scan_for_useless_echo &&
              substitution_body_is_bare_echo(segment.text.view()))
          {
            echo_only_substitution_count++;
          }
          break;

        case WordSegment::Kind::FunctionSubstitution:
          quote_sandwich_state = 0;
          actx.mark_runtime_definer_seen();
          break;

        case WordSegment::Kind::DoubleQuotedText:
          if (quote_sandwich_state == 2) has_quote_sandwich = true;
          quote_sandwich_state = 1;
          break;

        case WordSegment::Kind::UnquotedText:
          if (quote_sandwich_state == 1) {
            quote_sandwich_state = 2;
            quote_sandwich_word = segment.text.view();
          } else {
            quote_sandwich_state = 0;
          }
          break;

        case WordSegment::Kind::VariableReference: {
          quote_sandwich_state = 0;
          let const referenced = segment.text.view();
          actx.note_positional_reference(referenced);
          if (!is_operand) break;

          /* An unquoted default assignment is split and globbed before it is
             stored, shellcheck SC2223. */
          if (segment.is_split_eligible()) {
            let const colon = referenced.find_character(':');
            if (colon.has_value() && *colon + 1 < referenced.length &&
                referenced[*colon + 1] == '=')
            {
              actx.report_diagnostic(
                  diagnostic_id::sc2223, arg_location,
                  {referenced.substring_of_length(0, *colon)});
            }
          }

          if (word->segments.count() == 1 && referenced == "*" &&
              !segment.is_in_double_quotes)
          {
            has_bare_star_reference = true;
          }
          if (referenced == "@" && !has_spread_alone_unquoted &&
              !has_spread_in_longer_word)
          {
            has_spread_alone_unquoted =
                word->segments.count() == 1 && !segment.is_in_double_quotes;
            has_spread_in_longer_word = word->segments.count() > 1;
          }
          if (lost_pipeline_name.is_empty() &&
              actx.pipeline_lost_names.contains(referenced))
          {
            lost_pipeline_name = referenced;
          }
          if (!has_split_eligible_variable && segment.is_split_eligible() &&
              !is_split_exempt_variable_name(referenced))
          {
            has_split_eligible_variable = true;
            split_eligible_location = expansion_location_with_sigil(
                actx, segment.get_source_location(arg_location.filename)
                          .value_or(arg_location));
          }
          if (should_check_array_reads && bare_array_name.is_empty() &&
              actx.array_valued_names.contains(referenced))
          {
            bare_array_name = referenced;
          }
          if (should_check_quoted_values && quoted_value_name.is_empty() &&
              segment.is_split_eligible())
          {
            quoted_value_assignment =
                actx.quoted_literal_assignments.find(referenced);
            if (quoted_value_assignment != nullptr)
              quoted_value_name = referenced;
          }
          break;
        }

        default: quote_sandwich_state = 0; break;
        }
      }
    }

    /* The command word expands to a number and the shell then looks that number
       up as a program, shellcheck SC2084. */
    if (!is_operand && word != nullptr && word->segments.count() == 1 &&
        word->segments[0].kind == WordSegment::Kind::ArithmeticExpansion)
    {
      actx.report_diagnostic(diagnostic_id::sc2084, arg_location);
    }

    /* The command word is one expansion, so whatever the name holds is meant to
       run and an assignment of a bare command name to it was deliberate. */
    if (!is_operand && word != nullptr && word->segments.count() == 1 &&
        word->segments[0].kind == WordSegment::Kind::VariableReference &&
        actx.command_name_assignments.count() != 0)
    {
      actx.command_position_names.add(word->segments[0].text.view());
    }

    if (has_quote_sandwich) {
      actx.report_diagnostic(diagnostic_id::sc2140, arg_location,
                             {quote_sandwich_word});
    }

    if (has_quote_break_for_dollar)
      actx.report_diagnostic(diagnostic_id::sc1135, arg_location);

    if (has_literal_dollar_in_quotes)
      actx.report_diagnostic(diagnostic_id::sc1000, arg_location);

    if (has_dollar_in_arithmetic)
      actx.report_diagnostic(diagnostic_id::sc2004, arg_location);

    if (has_multi_digit_positional)
      actx.report_diagnostic(diagnostic_id::sc1037, arg_location);

    if (is_operand && has_double_dot && has_open_brace && has_dollar_byte)
      actx.report_diagnostic(diagnostic_id::sc2051, arg_location);

    if (word != nullptr && is_operand) {
      let const is_quoted_home = source_text == "\"~\"" ||
                                 source_text == "'~'" ||
                                 source_text.starts_with(StringView{"\"~/"}) ||
                                 source_text.starts_with(StringView{"'~/"});
      if (is_quoted_home)
        actx.report_diagnostic(diagnostic_id::sc2088, arg_location);

      if (!is_command_shadowed && command_id == command_name_id::Echo &&
          source_text.length >= 4 && source_text[0] == '\'' &&
          source_text[1] == '$' && source_text[source_text.length - 1] == '\'')
      {
        let const referenced =
            source_text.substring_of_length(2, source_text.length - 3);
        bool is_simple_name = !referenced.is_empty() &&
                              lexer::is_variable_name_start(referenced[0]);
        for (usize position = 1; position < referenced.length && is_simple_name;
             position++)
          is_simple_name = lexer::is_variable_name(referenced[position]);

        if (is_simple_name)
          actx.report_diagnostic(diagnostic_id::sc2016, arg_location);
      }

      if (has_bare_star_reference)
        actx.report_diagnostic(diagnostic_id::sc2048, arg_location);

      if (!bare_array_name.is_empty()) {
        actx.report_diagnostic(diagnostic_id::sc2128, arg_location,
                               {bare_array_name});
      }

      /* The pair is reported once for each assignment, so the name is dropped
         after the first split-eligible read reaches it. */
      if (!quoted_value_name.is_empty()) {
        let const assignment_location = *quoted_value_assignment;
        actx.report_diagnostic(diagnostic_id::sc2089, assignment_location,
                               {quoted_value_name});
        actx.report_diagnostic(diagnostic_id::sc2090, arg_location,
                               {quoted_value_name}, assignment_location);
        actx.quoted_literal_assignments.erase(quoted_value_name);
      }

      if (!lost_pipeline_name.is_empty()) {
        actx.report_diagnostic(diagnostic_id::sc2031, arg_location);
        actx.pipeline_lost_names.remove(lost_pipeline_name);
      }

      if (has_external_arithmetic_read) {
        actx.report_diagnostic(diagnostic_id::external_arithmetic_input,
                               arg_location);
      }

      if (should_check_unquoted_expansion && has_split_eligible_variable &&
          !(command_is_assignment_builtin &&
            word->get_assignment_split().has_value()))
      {
        actx.report_diagnostic(diagnostic_id::sc2086_expansion,
                               split_eligible_location);
      }

      if (should_check_dash_glob && !has_end_of_options) {
        if (arg->raw_view() == StringView{"--"}) {
          has_end_of_options = true;
        } else if (bare_glob_can_start_with_dash(*word)) {
          actx.report_diagnostic(diagnostic_id::sc2035, arg_location);
        }
      }
    }

    if (word != nullptr && has_bracket_byte &&
        word_has_malformed_glob_bracket(*word))
    {
      actx.report_diagnostic(diagnostic_id::malformed_glob, arg_location);
    }

    if (is_operand && word != nullptr && has_unquoted_command_substitution &&
        !(command_is_assignment_builtin &&
          word->get_assignment_split().has_value()))
    {
      actx.report_diagnostic(diagnostic_id::sc2046, arg_location);
    }

    if (is_operand && command_id != command_name_id::DoubleBracket) {
      if (has_spread_alone_unquoted) {
        actx.report_diagnostic(diagnostic_id::sc2068, arg_location);
      } else if (has_spread_in_longer_word) {
        actx.report_diagnostic(diagnostic_id::sc2145, arg_location);
      }
    }

    for (usize report = 0; report < echo_only_substitution_count; report++)
      actx.report_diagnostic(diagnostic_id::sc2116, arg_location);
  }

  if (m_args[0]->kind() == Token::Kind::Word) {
    let const &command_word =
        static_cast<const tokens::WordToken *>(m_args[0])->word();
    if (command_word.segments.count() == 1 &&
        command_word.segments[0].kind == WordSegment::Kind::CommandSubstitution)
    {
      actx.report_diagnostic(diagnostic_id::sc2091,
                             m_args[0]->source_location());
    }
  }

  /* local, declare, and typeset name variables that stay inside the function,
     so their names are recorded and the leak warning stays quiet for a later
     assignment. */
  if (actx.function_scope_depth > 0 && name.has_value() &&
      command_info.is_in_group(COMMAND_GROUP_DECLARATION_BUILTIN))
  {
    for (usize i = 1; i < m_args.count(); i++) {
      let const word = m_args[i]->kind() == Token::Kind::Word
                           ? static_cast<const tokens::WordToken *>(m_args[i])
                                 ->word()
                                 .to_literal_string()
                           : m_args[i]->raw_string();
      let const target_name = operand_target_name(word.view());
      if (!target_name.is_empty()) {
        actx.function_local_names.set(target_name,
                                      m_args[i]->source_location());
      }
    }
  }

  let has_explained_resolution_failure =
      check_command_word_shape(actx, lint_input);

  /* An assignment builtin that sets PATH also leaves the runtime search path
     unknown to the prepass. */
  if (name.has_value() &&
      (command_info.is_in_group(COMMAND_GROUP_ASSIGNMENT_BUILTIN) ||
       command_id == command_name_id::Unset))
  {
    for (usize i = 1; i < m_args.count(); i++) {
      let const word = m_args[i]->kind() == Token::Kind::Word
                           ? static_cast<const tokens::WordToken *>(m_args[i])
                                 ->word()
                                 .to_literal_string()
                           : m_args[i]->raw_string();
      let const target_name = operand_target_name(word.view());
      if (target_name == "PATH") actx.mark_path_unknown(true);
      if (is_source_location_variable(target_name))
        actx.mark_working_directory_unknown();
    }
  }

  check_operand_lints_after_scan(actx, lint_input);

  /* A dot, source, eval, or alias runs or defines code the prepass cannot see,
     so any later unresolved command must not be a hard failure. */
  let runtime_definer_name = String{command_literal};
  let runtime_definer_id = command_id;
  bool is_runtime_definer =
      command_info.is_in_group(COMMAND_GROUP_RUNTIME_DEFINER);

  if (let const wrapped_index = wrapped_command_index(command_id, m_args);
      wrapped_index.has_value())
  {
    if (*wrapped_index < m_args.count()) {
      runtime_definer_name = m_args[*wrapped_index]->raw_string();
      let const runtime_definer_info =
          get_analysis_command_info(runtime_definer_name.view());
      runtime_definer_id = runtime_definer_info.id;
      is_runtime_definer =
          runtime_definer_info.is_in_group(COMMAND_GROUP_RUNTIME_DEFINER);
    }
  }

  if (did_analyze_source && (runtime_definer_id == command_name_id::Dot ||
                             runtime_definer_id == command_name_id::Source))
  {
    is_runtime_definer = false;
  }

  if (runtime_definer_id == command_name_id::Eval) {
    actx.mark_path_unknown(false);
    actx.mark_working_directory_unknown();
  }

  if (is_runtime_definer) {
    LOG(Debug,
        "'%s' may define commands at run time, later resolution failures "
        "degrade to warnings",
        runtime_definer_name.c_str());
    actx.mark_runtime_definer_seen();
  }

  check_command_name_lints(actx, lint_input);
  check_command_value_lints(actx, lint_input);
  check_redirection_lints(actx, lint_input);
  check_test_operand_lints(actx, lint_input);
  has_explained_resolution_failure |=
      check_prefix_assignment_reads(actx, lint_input);

  /* No search path holds a program named after an entity, so the finding needs
     no resolution scan and survives an unknown path. */
  let const command_is_html_entity_tail =
      name.has_value() && !is_command_shadowed &&
      command_info.is_in_group(COMMAND_GROUP_HTML_ENTITY_TAIL) &&
      !actx.tested_command_names.contains(*name);

  if (command_is_html_entity_tail) {
    actx.report_diagnostic(diagnostic_id::sc1109, m_args[0]->source_location(),
                           {*name});
  }

  let const resolution_diagnostic =
      actx.has_seen_runtime_definer
          ? diagnostic_id::unresolved_command_uncertain
          : diagnostic_id::unresolved_command;
  /* The resolution scan reads PATH and the filesystem, so it is skipped when
     its only diagnostic cannot reach the output. */
  let const should_check_command_resolution =
      name.has_value() && !actx.should_silence_unresolved_commands &&
      !is_command_shadowed && !command_is_html_entity_tail &&
      !has_explained_resolution_failure &&
      actx.should_report(resolution_diagnostic);

  let unavailable = Maybe<utils::unavailable_path_source_component>{};
  let command_was_resolved = false;
  if (should_check_command_resolution) {
    command_was_resolved = command_resolves(*name, m_args[0]->source_location(),
                                            actx, unavailable);
  }
  if (should_check_command_resolution && !command_was_resolved &&
      !actx.tested_command_names.contains(*name))
  {
    let diagnostic_location = m_args[0]->source_location();
    let reported_name = *name;
    if (unavailable.has_value() &&
        resolution_diagnostic == diagnostic_id::unresolved_command)
    {
      diagnostic_location = unavailable->location;
      reported_name = unavailable->reported_prefix.view();
    }

    let local_names = ArrayList<String>{heap_allocator()};
    actx.defined_functions.for_each(
        [&](StringView n) throws { local_names.push(String{n}); });
    actx.known_aliases.for_each([&](StringView n)
                                    throws { local_names.push(String{n}); });
    let const suggestion = utils::suggest_command(*name, local_names);
    if (suggestion.has_value()) {
      actx.report_diagnostic(resolution_diagnostic, diagnostic_location,
                             {reported_name, suggestion->view()});
    } else {
      actx.report_diagnostic(resolution_diagnostic, diagnostic_location,
                             {reported_name});
    }
  }

  /* A recorded constant survives only across an environment-neutral command
     that writes no variable and runs no unseen code. Every other command
     forgets the whole table. */
  let should_clear_constants =
      !command_info.is_in_group(COMMAND_GROUP_ENVIRONMENT_NEUTRAL) ||
      has_command_substitution_argument;

  /* A neutral builtin shadowed by a function or alias is really a call into
     user code, so it forgets the table too. */
  if (!should_clear_constants &&
      (actx.defined_functions.contains(command_literal) ||
       actx.known_aliases.contains(command_literal)))
  {
    should_clear_constants = true;
  }

  if (should_clear_constants) {
    LOG(Debug,
        "the command '%.*s' may write variables, forgetting the recorded "
        "constants",
        static_cast<int>(command_literal.length), command_literal.data);
    actx.constant_variables.clear();
  }

  let const is_top_level_unconditional =
      actx.function_scope_depth == 0 && is_unconditional;
  if (is_top_level_unconditional && !is_command_shadowed) {
    if (command_info.is_in_group(COMMAND_GROUP_VARIABLE_TARGET)) {
      for (usize i = 1; i < m_args.count(); i++) {
        let const word = m_args[i]->kind() == Token::Kind::Word
                             ? static_cast<const tokens::WordToken *>(m_args[i])
                                   ->word()
                                   .to_literal_string()
                             : m_args[i]->raw_string();
        actx.note_variable_assignment(operand_target_name(word.view()),
                                      m_args[i]->source_location());
      }
    } else if (!command_info.is_in_group(COMMAND_GROUP_VARIABLE_PROBE)) {
      for (usize i = 1; i < m_args.count(); i++) {
        if (m_args[i]->kind() != Token::Kind::Word) continue;

        let const &word =
            static_cast<const tokens::WordToken *>(m_args[i])->word();
        for (let const &segment : word.segments) {
          if (segment.kind != WordSegment::Kind::VariableReference) continue;

          /* A read of a name this command also assigns as a prefix is reported
             by the prefix check, which names that shape exactly. */
          let is_read_of_own_prefix = false;
          for (let const &var : m_local_vars) {
            if (var.name.view() != segment.text.view()) continue;

            is_read_of_own_prefix = true;
            break;
          }

          if (is_read_of_own_prefix) continue;

          actx.note_variable_read(segment.text.view(),
                                  m_args[i]->source_location(),
                                  is_top_level_unconditional);
        }
      }
    }
  }
}

fn SimpleCommand::append_presence_tested_command_names(
    const AnalysisContext &actx, HashSet &names,
    bool status_is_success) const throws -> void
{
  if (status_is_success == is_negated() || m_args.is_empty()) {
    return;
  }

  let const name = static_command_name(m_args[0]);
  if (!name.has_value()) return;
  if (actx.defined_functions.contains(*name) ||
      actx.known_aliases.contains(*name))
  {
    return;
  }

  let const is_command_test = *name == "command";
  let const is_type_or_hash = *name == "type" || *name == "hash";
  if (!is_command_test && !is_type_or_hash) {
    return;
  }

  bool has_presence_flag = is_type_or_hash;
  for (usize i = 1; i < m_args.count(); i++) {
    let const arg = static_command_name(m_args[i]);
    if (!arg.has_value()) break;
    if (is_command_test && (*arg == "-v" || *arg == "-V")) {
      has_presence_flag = true;
      continue;
    }
    if (arg->starts_with("-")) continue;
    if (has_presence_flag) names.add(*arg);
  }
}

cold fn SimpleCommand::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  /* A redirection, an async or negated command, or a prefix assignment is not
     constant, so the fold declines it. The guards read this node's private
     members. */
  if (!m_redirections.is_empty()) return koshka::None;
  if (is_async() || is_negated()) {
    return koshka::None;
  }
  if (m_local_vars.count() > 0) return koshka::None;

  return optimizer::simple_command_static_verdict(m_args, actx);
}

/* The redirection that takes the descriptor away from the pipe, or null when
   the stage leaves it alone. */
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
  let const stage_is_unconditional =
      is_unconditional && m_commands.count() == 1;
  for (let const command : m_commands) {
    ASSERT(command != nullptr);
    let const was_direct_pipeline_stage = actx.is_direct_pipeline_stage;
    let const saved_has_seen_runtime_definer = actx.has_seen_runtime_definer;
    let const saved_has_unknown_path = actx.has_unknown_path;
    let const saved_has_unknown_working_directory =
        actx.has_unknown_working_directory;
    let const saved_should_silence_unresolved_commands =
        actx.should_silence_unresolved_commands;
    let saved_inherited_assigned_names = actx.inherited_assigned_names.clone();
    let saved_inherited_global_assigned_names =
        actx.inherited_global_assigned_names.clone();
    let saved_array_valued_names = actx.array_valued_names.clone();
    let *saved_source_effects = actx.current_source_effects;
    if (m_commands.count() > 1) actx.current_source_effects = nullptr;
    actx.is_direct_pipeline_stage =
        m_commands.count() > 1 && (command->as_simple_command() != nullptr ||
                                   command->as_assign_command() != nullptr);
    command->analyze(actx, stage_is_unconditional);
    actx.current_source_effects = saved_source_effects;
    actx.is_direct_pipeline_stage = was_direct_pipeline_stage;
    if (m_commands.count() > 1) {
      actx.has_unknown_working_directory = saved_has_unknown_working_directory;
      actx.has_unknown_path = saved_has_unknown_path;
      actx.has_seen_runtime_definer = saved_has_seen_runtime_definer;
      actx.should_silence_unresolved_commands =
          saved_should_silence_unresolved_commands;
      actx.array_valued_names = steal(saved_array_valued_names);
      actx.inherited_global_assigned_names =
          steal(saved_inherited_global_assigned_names);
      actx.inherited_assigned_names = steal(saved_inherited_assigned_names);
    }
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
                             first_stage->local_vars()[0].location,
                             {first_stage->local_vars()[0].name.view()});
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
  const Token *first_directory_change = nullptr;

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
            first_directory_change = simple->args()[0];
          } else if (first_directory_change != nullptr) {
            actx.report_diagnostic(diagnostic_id::sc2103,
                                   first_directory_change->source_location(),
                                   {}, simple->args()[0]->source_location());
            first_directory_change = nullptr;
          }
        }
        if (*name == "exec" && simple->args().count() > 1 &&
            i + 1 < m_nodes.count())
        {
          let const next_command = m_nodes[i + 1]->command();
          ASSERT(next_command != nullptr);
          actx.report_diagnostic(diagnostic_id::sc2093,
                                 simple->args()[0]->source_location(), {},
                                 next_command->source_location());
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

  usize repeated_append_count = 0;
  String repeated_append_target{heap_allocator()};
  SourceLocation repeated_append_location{};
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
        i + 1 < m_nodes.count() && node->command() != nullptr)
    {
      actx.report_diagnostic(diagnostic_id::sc2251,
                             node->command()->source_location());
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

  /* The condition always runs to decide the branch. The branches do not. */
  m_condition->analyze(actx, is_unconditional);
  m_then->analyze(actx, false);
  if (m_otherwise != nullptr) m_otherwise->analyze(actx, false);

  /* A branch may have reassigned a name, so a value recorded before this if is
     no longer proven in the block after it. */
  actx.constant_variables.clear();
}

} /* namespace expressions */

} /* namespace koshka */
