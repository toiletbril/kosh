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
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Shitbox.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace shit {

namespace {

pure fn parse_shellcheck_code(StringView text) wontthrow -> Maybe<u16>
{
  if (text.starts_with(StringView{"SC"})) text = text.substring(2);
  if (!text.is_all_decimal_digits()) return None;

  u32 value = 0;
  for (usize i = 0; i < text.length; i++) {
    value = value * 10 + static_cast<u32>(text[i] - '0');
    if (value > UINT16_MAX) return None;
  }

  return static_cast<u16>(value);
}

pure fn shellcheck_value_disables(StringView value,
                                  u16 diagnostic_code) wontthrow -> bool
{
  if (value == StringView{"all"}) return true;

  usize component_start = 0;
  while (component_start <= value.length) {
    usize component_end = component_start;
    while (component_end < value.length && value[component_end] != ',') {
      component_end++;
    }
    let const component = value.substring_of_length(
        component_start, component_end - component_start);
    let const dash = component.find_character('-');
    if (dash.has_value()) {
      let const first =
          parse_shellcheck_code(component.substring_of_length(0, *dash));
      let const end = parse_shellcheck_code(component.substring(*dash + 1));
      if (first.has_value() && end.has_value() && diagnostic_code >= *first &&
          diagnostic_code < *end)
      {
        return true;
      }
    } else {
      let const code = parse_shellcheck_code(component);
      if (code.has_value() && *code == diagnostic_code) {
        return true;
      }
    }
    if (component_end == value.length) break;
    component_start = component_end + 1;
  }

  return false;
}

pure fn shellcheck_directive_disables(StringView comment,
                                      u16 diagnostic_code) wontthrow -> bool
{
  usize position = 1;
  while (position < comment.length &&
         (comment[position] == ' ' || comment[position] == '\t'))
  {
    position++;
  }
  let const directive_text = comment.substring(position);
  if (!directive_text.starts_with(StringView{"shellcheck"}) ||
      (directive_text.length > 10 && directive_text[10] != ' ' &&
       directive_text[10] != '\t'))
  {
    return false;
  }
  position += 10;

  while (position < comment.length) {
    while (position < comment.length &&
           (comment[position] == ' ' || comment[position] == '\t'))
    {
      position++;
    }
    if (position >= comment.length || comment[position] == '#') {
      break;
    }
    if (!comment.substring(position).starts_with(StringView{"disable="})) {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
      continue;
    }
    position += 8;

    char quote = '\0';
    if (position < comment.length &&
        (comment[position] == '\'' || comment[position] == '"'))
    {
      quote = comment[position++];
    }
    let const value_start = position;
    if (quote != '\0') {
      while (position < comment.length && comment[position] != quote) {
        position++;
      }
      if (position == comment.length) return false;
    } else {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
    }
    let const value =
        comment.substring_of_length(value_start, position - value_start);
    if (shellcheck_value_disables(value, diagnostic_code)) return true;
    if (quote != '\0' && position < comment.length) {
      position++;
    }
  }

  return false;
}

pure fn shellcheck_tier(u16 diagnostic_code) wontthrow -> diagnostic_tier
{
  for (let const &check : SHELLCHECK_CHECKS) {
    let const code = parse_shellcheck_code(check.code);
    if (code.has_value() && *code == diagnostic_code) {
      return check.tier;
    }
  }
  return diagnostic_tier::Strict;
}

} /* namespace */

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

  if (!should_trace_optimizer) {
    pending_warnings.push(
        pending_analysis_warning{location, String{message}, String{suggestion},
                                 related_location, String{related_message}});
    return;
  }

  if (related_location.has_value()) {
    let const located = WarningWithLocation{location, message};
    show_message(located.to_string(source, eval_context));

    let const related = ErrorWithLocationAndDetails{
        location, {}, *related_location, related_message, suggestion};
    show_message(related.details_to_string(source, eval_context));
  } else {
    let const located =
        WarningWithLocationAndDetails{location, message, suggestion};
    show_message(located.to_string(source, eval_context));
  }
  print_script_backtrace_if_rooted(location);
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

fn AnalysisContext::warn_shellcheck(u16 diagnostic_code,
                                    SourceLocation location, StringView message,
                                    StringView suggestion,
                                    Maybe<SourceLocation> related_location,
                                    StringView related_message) throws -> void
{
  if (is_shellcheck_suppressed(diagnostic_code, location)) return;

  fail(location, message, suggestion, shellcheck_tier(diagnostic_code),
       related_location, related_message);
}

cold fn AnalysisContext::trace_optimizer_line(StringView message) const throws
    -> void
{
  if (!should_trace_optimizer) return;
  print_error("[optimizer] ");
  print_error(message);
  print_error("\n");
}

cold fn AnalysisContext::trace_eliminated_node(SourceLocation location,
                                               StringView message) const throws
    -> void
{
  if (!should_print_optimizer_state) return;
  const WarningWithLocation located{location, message};
  print_error("[optimizer-state] ");
  print_error(located.to_string(source, eval_context));
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

  u8 demote_at_level = 0;
  switch (tier) {
  case diagnostic_tier::Annoying: demote_at_level = 1; break;
  case diagnostic_tier::Lenient: demote_at_level = 2; break;
  case diagnostic_tier::Strict: demote_at_level = 3; break;
  }

  if (warning_level >= demote_at_level) {
    warn(location, message, suggestion, tier, related_location,
         related_message);
    return;
  }

  flush_warnings();

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

fn AnalysisContext::fail_shellcheck(u16 diagnostic_code,
                                    SourceLocation location, StringView message,
                                    StringView suggestion,
                                    Maybe<SourceLocation> related_location,
                                    StringView related_message) throws -> void
{
  if (is_shellcheck_suppressed(diagnostic_code, location)) return;

  fail(location, message, suggestion, shellcheck_tier(diagnostic_code),
       related_location, related_message);
}

pure fn AnalysisContext::is_shellcheck_suppressed(
    u16 diagnostic_code, SourceLocation location) const wontthrow -> bool
{
  if (shellcheck_suppressions == nullptr) return false;

  for (let const &suppression : *shellcheck_suppressions) {
    if (location.position < suppression.start_position ||
        location.position >= suppression.end_position)
    {
      continue;
    }

    for (let const &directive : suppression.directives) {
      if (directive.position + directive.length > source.length) continue;
      let const comment =
          source.substring_of_length(directive.position, directive.length);
      if (shellcheck_directive_disables(comment, diagnostic_code)) return true;
    }
  }

  return false;
}

fn AnalysisContext::note_variable_assignment(StringView name) throws -> void
{
  if (name.is_empty()) return;

  assigned_names_so_far.add(name);

  if (const SourceLocation *read_location = reads_before_assignment.find(name);
      read_location != nullptr)
  {
    fail(*read_location,
         StringView{"The variable '"} + name +
             "' is read before it is assigned",
         StringView{}, diagnostic_tier::Lenient);
    reads_before_assignment.erase(name);
  }
}

fn AnalysisContext::note_variable_read(StringView name, SourceLocation location,
                                       bool is_top_level_unconditional) throws
    -> void
{
  if (!is_top_level_unconditional) return;
  if (has_seen_runtime_definer) return;
  if (!optimizer::is_plain_variable_name(name)) return;

  if (assigned_names_so_far.contains(name)) return;
  if (function_local_names.contains(name)) return;
  if (global_assigned_names.contains(name)) return;
  if (reads_before_assignment.find(name) != nullptr) return;

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
  if (!resolved.is_windowed || resolved.text == nullptr) return None;

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

cold fn Expression::register_defined_functions(
    AnalysisContext &actx) const throws -> void
{
  unused(actx);
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

fn Expression::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  unused(actx);
  return shit::None;
}

fn Expression::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  unused(cxt);
  unused(active_functions);
  return false;
}

fn static_command_name(const Token *token) throws -> Maybe<String>
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return shit::None;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();

  let name = String{heap_allocator()};
  for (let const &segment : word.segments) {
    /* Any expansion segment makes the name a runtime value, so its raw bytes
       must not pass for the program text. */
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText &&
        segment.kind != WordSegment::Kind::UnquotedText)
    {
      return shit::None;
    }
    if (segment.kind == WordSegment::Kind::UnquotedText) {
      for (usize i = 0; i < segment.text.count(); i++) {
        if (lexer::is_expandable_char(segment.text[i])) return shit::None;
      }
    }
    name.append(segment.text.view());
  }
  return name;
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

fn command_resolves(
    const String &name, SourceLocation location, const AnalysisContext &actx,
    Maybe<utils::unavailable_path_source_component> &unavailable) throws -> bool
{
  if (name.is_empty()) return false;
  if (search_builtin(name.view()).has_value()) return true;
  /* The prepass runs only in the default mood, where a coreutil falls back to
     its shitbox implementation, so a shitbox name resolves without a PATH
     binary. */
  if (shitbox::find_util(name.view()).has_value()) return true;
  if (os::has_directory_separator(name.view())) {
    let const expanded = expanded_command_path(name.view(), heap_allocator());
    let const typed_path = Path{expanded.view()};
    let const was_resolved =
        typed_path.has_trailing_separator()
            ? os::canonical_path(typed_path.to_absolute_without_normalizing())
                  .has_value()
            : Path::canonicalize(expanded.view()).has_value();
    if (was_resolved) return true;

    let const target = typed_path.to_absolute_without_normalizing();
    let raw_operand = name.view();
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
          .search(name.view(), ProgramResolver::SearchMode::First,
                  ProgramResolver::Requirement::Regular,
                  ProgramResolver::CachePolicy::Bypass)
          .count() != 0;
  LOG(Debug, "scanning PATH for '%s', the command was %s", name.c_str(),
      was_resolved ? "found" : "not found");
  return was_resolved;
}

/* Only an unquoted '[' or ']' is active, so a quoted "[" or an escaped \[ stays
   literal and never opens a bracket expression. */
struct glob_scan_byte
{
  char ch;
  bool is_glob_active;
};

fn collect_glob_scan_bytes(const Word &word) throws -> ArrayList<glob_scan_byte>
{
  ArrayList<glob_scan_byte> bytes{heap_allocator()};
  for (let const &segment : word.segments) {
    const bool is_active = segment.has_live_glob_chars();
    for (usize i = 0; i < segment.text.count(); i++) {
      bytes.push(glob_scan_byte{segment.text[i], is_active});
    }
  }
  return bytes;
}

/* The scan mirrors the matcher in utils::glob_matches, where an active '[' with
   no closing ']' is a literal, so only a '[' that opens a class and never
   closes is malformed. Returns true when malformed. */
fn word_has_malformed_glob_bracket(const Word &word) throws -> bool
{
  const ArrayList<glob_scan_byte> bytes = collect_glob_scan_bytes(word);

  usize position = 0;
  while (position < bytes.count()) {
    if (!(bytes[position].is_glob_active && bytes[position].ch == '[')) {
      position++;
      continue;
    }

    /* A '[' as the last byte cannot open a class and stays literal. */
    usize scan = position + 1;
    if (scan >= bytes.count()) {
      position++;
      continue;
    }

    /* A leading '!' or '^' negates the class, mirroring the matcher which skips
       either one before scanning for the closing ']'. */
    if (bytes[scan].ch == '!' || bytes[scan].ch == '^') {
      scan++;
    }

    /* A leading ']' stays in view so [^] and [!] both open and close the
       degenerate class the way the matcher accepts them. */
    for (; scan < bytes.count(); scan++)
      if (bytes[scan].ch == ']') break;

    if (scan == bytes.count()) return true;

    position = scan + 1;
  }

  return false;
}

} /* namespace */

fn analyze_ast(const Expression *root, StringView source,
               const HashSet &known_functions, const HashSet &known_aliases,
               EvalContext *eval_context, u8 warning_level,
               bool silence_unresolved_commands, bool is_default_mood,
               bool should_emit_annoying_diagnostics,
               const ArrayList<shellcheck_suppression> &shellcheck_suppressions,
               bool show_optimizer_state) throws -> bool
{
  ASSERT(root != nullptr);

  AnalysisContext actx{source};
  actx.warning_level = warning_level;
  actx.is_default_mood = is_default_mood;
  actx.should_emit_annoying_diagnostics = should_emit_annoying_diagnostics;
  actx.shellcheck_suppressions = &shellcheck_suppressions;
  actx.should_silence_unresolved_commands = silence_unresolved_commands;
  actx.eval_context = eval_context;
  actx.should_print_optimizer_state = show_optimizer_state;
  actx.should_trace_optimizer = show_optimizer_state;

  if (source.length >= 3 && static_cast<u8>(source[0]) == 0xef &&
      static_cast<u8>(source[1]) == 0xbb && static_cast<u8>(source[2]) == 0xbf)
    actx.fail(SourceLocation{0, 3},
              "A UTF-8 byte-order mark precedes the script text",
              "Save the script as UTF-8 without a byte-order mark");

  /* A leading shebang that names a POSIX shell gates the bashism lints. The
     first line is scanned for a contained 'dash', or for an 'sh' interpreter
     name without 'bash'. */
  if (source.length >= 2 && source[0] == '#' && source[1] == '!') {
    usize line_end = 0;
    while (line_end < source.length && source[line_end] != '\n')
      line_end++;
    let const first_line = source.substring_of_length(0, line_end);
    let contains_dash = false;
    let contains_bash = false;
    let interpreter_is_sh = false;
    for (usize i = 0; i + 4 <= first_line.length; i++) {
      if (first_line.substring(i).starts_with(StringView{"dash"}))
        contains_dash = true;
      if (first_line.substring(i).starts_with(StringView{"bash"}))
        contains_bash = true;
    }
    /* A trailing 'sh' at the line end is the sh program name. */
    if (first_line.length >= 2 &&
        first_line.substring(first_line.length - 2) == StringView{"sh"})
    {
      interpreter_is_sh = true;
    }
    if (contains_dash || (interpreter_is_sh && !contains_bash)) {
      actx.shebang_is_posix_sh = true;
    }
  }

  LOG(Debug, "analyzing the ast, the posix sh shebang gate is %s",
      actx.shebang_is_posix_sh ? "armed" : "off");

  /* A function or alias defined by an earlier command resolves, so the already
     registered names seed the prepass. */
  known_functions.for_each(
      [&actx](StringView name) { actx.add_defined_function(name); });
  known_aliases.for_each(
      [&actx](StringView name) { actx.add_known_alias(name); });

  root->analyze(actx, true);

  actx.flush_warnings();

  if (actx.should_trace_optimizer) {
    let summary = String{"summary: "};
    summary.append(
        String::from(actx.optimizer_folded_arithmetic, heap_allocator()));
    summary.append(" arithmetic folded, ");
    summary.append(
        String::from(actx.optimizer_recorded_constants, heap_allocator()));
    summary.append(" constants recorded, ");
    summary.append(
        String::from(actx.optimizer_folded_branches, heap_allocator()));
    summary.append(" branches folded, ");
    summary.append(String::from(actx.optimizer_folded_loops, heap_allocator()));
    summary.append(" loops folded, ");
    summary.append(
        String::from(actx.optimizer_eliminated_compounds, heap_allocator()));
    summary.append(" compounds eliminated");
    actx.trace_optimizer_line(summary.view());
  }

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

  const i64 condition = m_condition->evaluate(cxt);
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

fn Command::set_timed(bool posix_format) wontthrow -> void
{
  m_is_timed = true;
  m_is_time_posix_format = posix_format;
}

pure fn Command::is_timed() const wontthrow -> bool { return m_is_timed; }

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

cold fn SimpleCommand::register_defined_functions(
    AnalysisContext &actx) const throws -> void
{
  if (m_args.is_empty() || m_args[0]->raw_string() != "alias") return;

  /* An alias defined anywhere in the input resolves a later use of its name, so
     each alias name is recorded before the resolution check. The name is taken
     from the raw token text up to the '='. */
  for (usize i = 1; i < m_args.count(); i++) {
    let const text = m_args[i]->raw_string();
    let const equals_position = text.find_character('=');
    if (equals_position.has_value() && *equals_position > 0)
      actx.add_known_alias(StringView{text.data(), *equals_position});
  }
}

/* The direct test operator a leading ! collapses into, for the SC2335 lint.
   None for an operator with no negated shortcut. */
constexpr static_string_entry<StringView> NEGATED_TEST_OPERATOR_ENTRIES[] = {
    {SSK("-eq"), StringView{"-ne", 3}},
    {SSK("-ne"), StringView{"-eq", 3}},
    {SSK("-lt"), StringView{"-ge", 3}},
    {SSK("-ge"), StringView{"-lt", 3}},
    {SSK("-gt"), StringView{"-le", 3}},
    {SSK("-le"), StringView{"-gt", 3}},
    {SSK("="),   StringView{"!=", 2} },
    {SSK("!="),  StringView{"=", 1}  },
};
constexpr StaticStringMap NEGATED_TEST_OPERATORS{NEGATED_TEST_OPERATOR_ENTRIES};

cold fn negated_test_operator(StringView op) wontthrow -> Maybe<StringView>
{
  return NEGATED_TEST_OPERATORS.find(op);
}

/* The binary operators of test, used to tell a == in the operator slot from a
   literal == operand, so the SC3014 lint does not flag [ x = == ]. */
constexpr PackedStringKey TEST_BINARY_OPERATOR_KEYS[] = {
    SSK("="),   SSK("=="),  SSK("!="),  SSK("<"),   SSK(">"),
    SSK("-eq"), SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"),
    SSK("-ge"), SSK("-ef"), SSK("-nt"), SSK("-ot"),
};
constexpr StaticStringSet TEST_BINARY_OPERATORS{TEST_BINARY_OPERATOR_KEYS};

cold fn is_test_binary_operator_word(StringView op) wontthrow -> bool
{
  return TEST_BINARY_OPERATORS.contains(op);
}

/* The numeric comparison operators of test, for the SC2170 lint. */
constexpr PackedStringKey TEST_NUMERIC_OPERATOR_KEYS[] = {
    SSK("-eq"), SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"), SSK("-ge"),
};
constexpr StaticStringSet TEST_NUMERIC_OPERATORS{TEST_NUMERIC_OPERATOR_KEYS};

cold fn is_test_numeric_operator_word(StringView op) wontthrow -> bool
{
  return TEST_NUMERIC_OPERATORS.contains(op);
}

cold fn word_is_fully_literal(const Word &word) wontthrow -> bool
{
  for (let const &segment : word.segments)
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::UnquotedText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText)
    {
      return false;
    }
  return true;
}

cold fn word_has_split_eligible_variable(const Word &word) wontthrow -> bool
{
  for (let const &segment : word.segments)
    if (segment.kind == WordSegment::Kind::VariableReference &&
        segment.is_split_eligible() && segment.text.view() != "@" &&
        segment.text.view() != "*" && segment.text.view() != "?" &&
        segment.text.view() != "#" && segment.text.view() != "$" &&
        segment.text.view() != "!" && segment.text.view() != "-")
      return true;

  return false;
}

cold fn word_is_bare_glob(const Word &word) wontthrow -> bool
{
  return word.segments.count() == 1 &&
         word.segments[0].kind == WordSegment::Kind::UnquotedText &&
         word.segments[0].has_glob_metacharacter();
}

cold fn bare_glob_can_start_with_dash(const Word &word) wontthrow -> bool
{
  if (!word_is_bare_glob(word)) return false;
  let const text = word.segments[0].text.view();
  if (text.is_empty()) return false;
  if (text[0] == '*' || text[0] == '?') return true;
  if (text[0] != '[') return false;

  let const close = text.find_character(']');
  if (!close.has_value()) return false;
  if (*close > 1 && (text[1] == '!' || text[1] == '^')) return true;
  for (usize position = 1; position < *close; position++)
    if (text[position] == '-' &&
        (position == 1 || position + 1 == *close || text[position - 1] == '\\'))
      return true;
  return false;
}

cold fn printf_consumed_argument_count(StringView format) wontthrow -> usize
{
  usize count = 0;
  for (usize i = 0; i < format.length; i++) {
    if (format[i] != '%' || i + 1 >= format.length) continue;
    i++;
    if (format[i] == '%') continue;

    while (i < format.length &&
           (format[i] == '-' || format[i] == '+' || format[i] == ' ' ||
            format[i] == '#' || format[i] == '0'))
      i++;
    if (i >= format.length) break;
    if (format[i] == '*') {
      count++;
      i++;
    } else
      while (i < format.length && (format[i] >= '0' && format[i] <= '9'))
        i++;
    if (i < format.length && format[i] == '.') {
      i++;
      if (i < format.length && format[i] == '*') {
        count++;
        i++;
      } else
        while (i < format.length && (format[i] >= '0' && format[i] <= '9'))
          i++;
    }
    while (i < format.length &&
           (format[i] == 'h' || format[i] == 'l' || format[i] == 'L' ||
            format[i] == 'j' || format[i] == 'z' || format[i] == 't'))
      i++;
    if (i < format.length && format[i] == '(') {
      while (i < format.length && format[i] != ')')
        i++;
      if (i + 1 < format.length) i++;
    }
    count++;
  }

  return count;
}

cold pure fn view_is_integer_literal(StringView view) wontthrow -> bool
{
  usize start = view.length >= 1 && view[0] == '-' ? 1 : 0;
  return start < view.length && view.substring(start).is_all_decimal_digits();
}

cold pure fn view_contains(StringView view, StringView needle) wontthrow -> bool
{
  if (needle.length == 0 || needle.length > view.length) return false;
  for (usize i = 0; i + needle.length <= view.length; i++)
    if (view.substring(i).starts_with(needle)) return true;
  return false;
}

cold fn args_have_stdin_operand(const ArrayList<const Token *> &args) throws
    -> bool
{
  for (usize i = 1; i < args.count(); i++) {
    let const raw = args[i]->raw_string();
    if (raw.view() == "-" || raw.view() == "/dev/stdin") return true;
  }
  return false;
}

cold fn args_have_short_flag(const ArrayList<const Token *> &args,
                             char letter) throws -> bool
{
  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const literal = static_cast<const tokens::WordToken *>(args[i])
                            ->word()
                            .to_literal_string();
    let const view = literal.view();
    if (view.length >= 2 && view[0] == '-' && view[1] != '-' &&
        view.find_character(letter).has_value())
    {
      return true;
    }
  }
  return false;
}

/* The commands that never read stdin, so a pipe or input redirect into one
   silently discards the upstream data, shellcheck SC2216 and SC2217. */
constexpr PackedStringKey NON_STDIN_READER_KEYS[] = {
    SSK("rm"),      SSK("echo"),  SSK("printf"), SSK("true"),  SSK("false"),
    SSK("mkdir"),   SSK("rmdir"), SSK("touch"),  SSK("chmod"), SSK("chown"),
    SSK("cp"),      SSK("mv"),    SSK("ln"),     SSK("kill"),  SSK("basename"),
    SSK("dirname"), SSK("sleep"), SSK("unlink"),
};
constexpr StaticStringSet NON_STDIN_READERS{NON_STDIN_READER_KEYS};

/* The top-level system directories rm -r must never aim at, the SC2114
   table. */
constexpr PackedStringKey SYSTEM_DIRECTORY_KEYS[] = {
    SSK("/"),     SSK("/bin"), SSK("/boot"), SSK("/dev"),  SSK("/etc"),
    SSK("/home"), SSK("/lib"), SSK("/proc"), SSK("/root"), SSK("/sbin"),
    SSK("/sys"),  SSK("/usr"), SSK("/var"),
};
constexpr StaticStringSet SYSTEM_DIRECTORIES{SYSTEM_DIRECTORY_KEYS};

constexpr PackedStringKey TEST_COMMAND_KEYS[] = {SSK("["), SSK("test"),
                                                 SSK("[[")};
constexpr StaticStringSet TEST_COMMANDS{TEST_COMMAND_KEYS};

constexpr PackedStringKey DECLARATION_BUILTIN_KEYS[] = {
    SSK("local"), SSK("declare"), SSK("typeset")};
constexpr StaticStringSet DECLARATION_BUILTINS{DECLARATION_BUILTIN_KEYS};

constexpr PackedStringKey RUNTIME_DEFINER_KEYS[] = {SSK("."), SSK("source"),
                                                    SSK("eval"), SSK("alias")};
constexpr StaticStringSet RUNTIME_DEFINER_COMMANDS{RUNTIME_DEFINER_KEYS};

constexpr PackedStringKey FIND_ACTION_KEYS[] = {
    SSK("-delete"), SSK("-exec"),    SSK("-execdir"), SSK("-fls"),
    SSK("-fprint"), SSK("-fprint0"), SSK("-fprintf"), SSK("-ls"),
    SSK("-ok"),     SSK("-okdir"),   SSK("-print"),   SSK("-print0"),
    SSK("-printf"), SSK("-prune"),   SSK("-quit"),    SSK("-used")};
constexpr StaticStringSet FIND_ACTIONS{FIND_ACTION_KEYS};

constexpr PackedStringKey ASSIGNMENT_BUILTIN_KEYS[] = {
    SSK("export"), SSK("readonly"), SSK("local"), SSK("declare"),
    SSK("typeset")};
constexpr StaticStringSet ASSIGNMENT_BUILTINS{ASSIGNMENT_BUILTIN_KEYS};

constexpr PackedStringKey VARIABLE_PROBE_COMMAND_KEYS[] = {
    SSK("["), SSK("test"), SSK("[["), SSK("unset"), SSK("let"), SSK("eval"),
};
constexpr StaticStringSet VARIABLE_PROBE_COMMANDS{VARIABLE_PROBE_COMMAND_KEYS};

constexpr PackedStringKey VARIABLE_TARGET_COMMAND_KEYS[] = {
    SSK("read"),    SSK("mapfile"),  SSK("readarray"),
    SSK("getopts"), SSK("declare"),  SSK("typeset"),
    SSK("export"),  SSK("readonly"), SSK("local"),
};
constexpr StaticStringSet VARIABLE_TARGET_COMMANDS{
    VARIABLE_TARGET_COMMAND_KEYS};

fn operand_target_name(StringView text) wontthrow -> StringView
{
  if (text.is_empty() || text[0] == '-') return StringView{};
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

  /* A PATH=... prefix leaves the runtime search path unknown to the prepass, so
     the not-found check for the prefixed command and everything after it stays
     quiet. */
  for (let const &var : m_local_vars)
    if (var.name.view() == "PATH")
      actx.should_silence_unresolved_commands = true;

  if (m_args.is_empty()) {
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
      if (!has_single_wrapping_pair || parenthesis_depth != 0) continue;

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
      actx.fail(location,
                StringView{"The assignment of '"} + assignment.name +
                    "' uses array syntax instead of arithmetic",
                StringView{"Use `let '"} + assignment.name + "=" + expression +
                    "'` to evaluate and assign it");
    }

    return;
  }

  ASSERT(m_args[0] != nullptr);
  let const name = static_command_name(m_args[0]);
  let const command_literal =
      m_args[0]->kind() == Token::Kind::Word
          ? static_cast<const tokens::WordToken *>(m_args[0])
                ->word()
                .to_literal_string()
          : m_args[0]->raw_string();
  let const command_is_shadowed =
      actx.defined_functions.contains(command_literal.view()) ||
      actx.known_aliases.contains(command_literal.view());

  if (!command_is_shadowed && actx.is_inside_loop_condition &&
      command_literal == "read")
  {
    actx.loop_condition_reads_input = true;
  }

  append_presence_tested_command_names(actx, actx.tested_command_names, true);

  if (!command_is_shadowed && actx.is_inside_read_loop &&
      command_literal == "ssh")
  {
    actx.fail_shellcheck(
        2095, m_args[0]->source_location(),
        "An ssh command in a while-read loop can consume the loop input",
        "Redirect ssh input from /dev/null or pass -n");
  }

  if (!command_is_shadowed && TEST_COMMANDS.contains(command_literal.view())) {
    for (usize i = 1; i < m_args.count(); i++) {
      let const literal = m_args[i]->raw_string();
      if (literal.view() == "=~")
        actx.fail_shellcheck(2074, m_args[i]->source_location(),
                             "The test builtin does not support the =~ regular "
                             "expression operator",
                             "Use [[ value =~ expression ]]");
    }
  }

  if (!command_is_shadowed && command_literal == "find") {
    bool has_exec = false;
    bool has_exec_terminator = false;
    bool has_or = false;
    bool has_group = false;
    bool has_action = false;
    Maybe<SourceLocation> exec_location{};
    Maybe<SourceLocation> or_location{};
    for (usize i = 1; i < m_args.count(); i++) {
      let const literal = m_args[i]->raw_string();
      if (literal.view() == "-exec" || literal.view() == "-execdir") {
        has_exec = true;
        exec_location = m_args[i]->source_location();
      } else if (has_exec && (literal.view() == ";" || literal.view() == "+")) {
        has_exec_terminator = true;
      } else if (literal.view() == "-o") {
        has_or = true;
        or_location = m_args[i]->source_location();
      } else if (literal.view() == "(" || literal.view() == ")") {
        has_group = true;
      }
      if (FIND_ACTIONS.contains(literal.view())) has_action = true;
    }
    if (has_exec && !has_exec_terminator && exec_location.has_value()) {
      actx.fail_shellcheck(
          2067, *exec_location,
          "The find -exec action has no terminating ';' or '+'",
          "Terminate the action with an escaped semicolon or plus");
    }
    if (has_or && has_action && !has_group && or_location.has_value()) {
      actx.fail_shellcheck(
          2146, *or_location,
          "The find expression uses -o without grouping its actions",
          "Group each side with escaped parentheses");
    }
  }

  if (!command_is_shadowed && command_literal == "alias") {
    for (usize i = 1; i < m_args.count(); i++) {
      let const raw = m_args[i]->raw_string();
      if (view_contains(raw.view(), StringView{"$1"}) ||
          view_contains(raw.view(), StringView{"$@"}) ||
          view_contains(raw.view(), StringView{"$*"}))
      {
        actx.fail_shellcheck(
            2142, m_args[i]->source_location(),
            "An alias body cannot receive positional arguments",
            "Use a function when the wrapper needs arguments");
      }
    }
  }

  if (!command_is_shadowed && TEST_COMMANDS.contains(command_literal.view()) &&
      m_args.count() >= 4)
  {
    let const first_operand = m_args[1]->raw_string();
    if (first_operand.view().starts_with(StringView{"x$"}) ||
        first_operand.view().starts_with(StringView{"x\"$"}))
    {
      actx.warn_shellcheck(2268, m_args[1]->source_location(),
                           "The x-prefix test workaround is obsolete",
                           "Quote the variable directly");
    }
  }

  if (!command_is_shadowed && command_literal == "tr") {
    for (usize i = 1; i < m_args.count() && i <= 2; i++) {
      let const literal = m_args[i]->raw_string();
      if (literal.length() >= 5 && literal[0] == '[' &&
          literal[literal.length() - 1] == ']' &&
          literal.view().find_character('-').has_value())
      {
        actx.fail_shellcheck(
            2021, m_args[i]->source_location(),
            "Brackets around a tr range add literal bracket bytes",
            "Use a quoted range without brackets");
      }
    }
  }

  for (usize i = 0; i < m_args.count(); i++) {
    let const source_text =
        analysis_source_text(actx, m_args[i]->source_location());
    if (source_text.length >= 2 && source_text[0] == '`' &&
        source_text[source_text.length - 1] == '`')
    {
      actx.warn_shellcheck(
          2006, m_args[i]->source_location(),
          "Backticks are harder to nest than command substitutions",
          "Use $(...) for command substitution");
    }
    if (view_contains(source_text, StringView{"$["}))
      actx.warn_shellcheck(
          2007, m_args[i]->source_location(),
          "$[...] is the obsolete arithmetic expansion spelling",
          "Use $((...)) for arithmetic expansion");
    if (m_args[i]->kind() == Token::Kind::Word) {
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      for (let const &segment : word.segments) {
        if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
            segment.text.view().find_character('$').has_value())
        {
          actx.warn_shellcheck(
              2004, m_args[i]->source_location(),
              "Arithmetic variables do not need a dollar sign",
              "Use the variable name directly inside arithmetic");
          break;
        }
      }
    }
    for (usize position = 0; position + 2 < source_text.length; position++) {
      if (source_text[position] != '$' || source_text[position + 1] < '1' ||
          source_text[position + 1] > '9' || source_text[position + 2] < '0' ||
          source_text[position + 2] > '9')
      {
        continue;
      }
      actx.fail_shellcheck(1037, m_args[i]->source_location(),
                           "A positional parameter above nine needs braces",
                           "Write ${10} to select positional parameter 10");
      break;
    }
  }

  if (m_args[0]->kind() == Token::Kind::Word) {
    let const &command_word =
        static_cast<const tokens::WordToken *>(m_args[0])->word();
    if (command_word.segments.count() == 1 &&
        command_word.segments[0].kind == WordSegment::Kind::CommandSubstitution)
    {
      actx.fail_shellcheck(
          2091, m_args[0]->source_location(),
          "A command substitution in command position executes its output",
          "Run the command inside the substitution directly");
    }
  }

  /* local, declare, and typeset name variables that stay inside the function,
     so their names are recorded and the leak warning stays quiet for a later
     assignment. */
  if (actx.function_scope_depth > 0 && name.has_value() &&
      DECLARATION_BUILTINS.contains(name->view()))
  {
    for (usize i = 1; i < m_args.count(); i++) {
      let const word = m_args[i]->kind() == Token::Kind::Word
                           ? static_cast<const tokens::WordToken *>(m_args[i])
                                 ->word()
                                 .to_literal_string()
                           : m_args[i]->raw_string();
      let const target_name = operand_target_name(word.view());
      if (!target_name.is_empty()) actx.function_local_names.add(target_name);
    }
  }

  /* A name like [ holds a glob metacharacter that static_command_name rejects,
     so the literal text is taken separately for the test recognition. */
  let const command_raw = m_args[0]->raw_string();
  if (!command_raw.is_empty() && command_raw[0] == '-')
    actx.warn_shellcheck(
        2215, m_args[0]->source_location(),
        "An option-shaped word in command position is not a command",
        "Place the option after its command");
  if (command_raw.length() > 1 && command_raw[0] == '$' &&
      lexer::is_variable_name_start(command_raw[1]) &&
      command_raw.view().find_character('=').has_value())
    actx.warn_shellcheck(2281, m_args[0]->source_location(),
                         "An assignment name must not start with a dollar sign",
                         "Remove the dollar sign from the assignment name");
  if ((command_raw.view().starts_with(StringView{"["}) &&
       command_raw.view() != "[") ||
      (command_raw.view().starts_with(StringView{"[["}) &&
       command_raw.view() != "[["))
    actx.warn_shellcheck(
        1035, m_args[0]->source_location(),
        "Test brackets and operands require separating spaces",
        "Add spaces after the opening bracket and before the close");
  if (command_raw.view().starts_with(StringView{"[["}) &&
      command_raw.view().find_character('=').has_value())
    actx.warn_shellcheck(2077, m_args[0]->source_location(),
                         "A conditional operator needs surrounding spaces",
                         "Place spaces around the comparison operator");
  if (command_raw.view().starts_with(StringView{"["}) &&
      command_raw.view() != "[" && command_raw.view() != "[[" &&
      m_args.count() > 1)
  {
    let const last_raw = m_args.back()->raw_string();
    if (!last_raw.is_empty() && last_raw[last_raw.length() - 1] == ']')
      actx.warn_shellcheck(
          1014, m_args[0]->source_location(),
          "Test brackets do not run the command written inside them",
          "Run the command directly as the if condition");
  }
  if (m_args.count() >= 2 && m_args[1]->raw_string().view() == "=")
    actx.warn_shellcheck(2283, m_args[1]->source_location(),
                         "An assignment cannot contain spaces around equals",
                         "Write NAME=value without spaces");

  for (usize i = 1; i < m_args.count(); i++) {
    let const raw = m_args[i]->raw_string();
    if (view_contains(raw.view(), StringView{".."}) &&
        raw.view().find_character('{').has_value() &&
        raw.view().find_character('$').has_value())
      actx.warn_shellcheck(2051, m_args[i]->source_location(),
                           "Brace ranges are expanded before variables",
                           "Use an arithmetic loop for a variable limit");
  }

  /* An assignment builtin that sets PATH also leaves the runtime search path
     unknown to the prepass. */
  if (name.has_value() &&
      (ASSIGNMENT_BUILTINS.contains(name->view()) || *name == "unset"))
  {
    for (usize i = 1; i < m_args.count(); i++) {
      let const word = m_args[i]->kind() == Token::Kind::Word
                           ? static_cast<const tokens::WordToken *>(m_args[i])
                                 ->word()
                                 .to_literal_string()
                           : m_args[i]->raw_string();
      if (operand_target_name(word.view()) == "PATH")
        actx.should_silence_unresolved_commands = true;
    }
  }

  /* A user-defined function or alias of a builtin name runs that user code, so
     a lint that keys on the builtin name must stay quiet here. */
  for (usize i = 1; i < m_args.count(); i++) {
    if (m_args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(m_args[i])->word();
    let const source_text =
        analysis_source_text(actx, m_args[i]->source_location());

    const bool is_quoted_home = source_text == "\"~\"" ||
                                source_text == "'~'" ||
                                source_text.starts_with(StringView{"\"~/"}) ||
                                source_text.starts_with(StringView{"'~/"});
    if (is_quoted_home)
      actx.warn_shellcheck(
          2088, m_args[i]->source_location(),
          "A quoted tilde stays literal instead of expanding to the "
          "home directory",
          "Leave the tilde unquoted or use a quoted $HOME expansion");

    if (!command_is_shadowed && command_literal == "echo" &&
        source_text.length >= 4 && source_text[0] == '\'' &&
        source_text[1] == '$' && source_text[source_text.length - 1] == '\'')
    {
      let const name =
          source_text.substring_of_length(2, source_text.length - 3);
      bool is_simple_name =
          !name.is_empty() && lexer::is_variable_name_start(name[0]);
      for (usize position = 1; position < name.length && is_simple_name;
           position++)
        is_simple_name = lexer::is_variable_name(name[position]);
      if (is_simple_name)
        actx.warn_shellcheck(
            2016, m_args[i]->source_location(),
            "Single quotes prevent the expansion written inside them",
            "Use double quotes if the value should expand");
    }

    if (word.segments.count() == 1 &&
        word.segments[0].kind == WordSegment::Kind::VariableReference &&
        word.segments[0].text.view() == "*" &&
        !word.segments[0].is_in_double_quotes)
      actx.warn_shellcheck(
          2048, m_args[i]->source_location(),
          "$* splits positional parameters and loses their boundaries",
          "Use quoted \"$@\" to preserve each argument");

    for (let const &segment : word.segments)
      if (segment.kind == WordSegment::Kind::VariableReference &&
          actx.pipeline_lost_names.contains(segment.text.view()))
      {
        actx.warn_shellcheck(
            2031, m_args[i]->source_location(),
            "This read sees the value from before the pipeline",
            "Move the assignment outside the pipeline or avoid the "
            "pipeline subshell");
        actx.pipeline_lost_names.remove(segment.text.view());
        break;
      }

    for (let const &segment : word.segments)
      if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
          arithmetic_reads_external_input(actx, segment.text.view()))
      {
        actx.fail(m_args[i]->source_location(),
                  "External input is evaluated as arithmetic code",
                  "Validate the value as decimal digits before arithmetic");
        break;
      }
  }

  if (!command_is_shadowed && command_literal == "read") {
    let should_skip_option_operand = false;
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      let const literal = word.to_literal_string();
      if (should_skip_option_operand) {
        should_skip_option_operand = false;
        continue;
      }
      if (literal.view() == "-p" || literal.view() == "-t" ||
          literal.view() == "-n" || literal.view() == "-N" ||
          literal.view() == "-d" || literal.view() == "-u" ||
          literal.view() == "-i")
      {
        should_skip_option_operand = true;
        continue;
      }
      if (literal.view().starts_with("-")) continue;
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::VariableReference)
        actx.warn_shellcheck(
            2229, m_args[i]->source_location(),
            "A read operand is a variable name, not a variable value",
            "Drop the dollar sign from the variable name");

      if (actx.is_direct_pipeline_stage && !literal.view().starts_with("-")) {
        let const target = operand_target_name(literal.view());
        if (!target.is_empty()) {
          actx.warn_shellcheck(
              2030, m_args[i]->source_location(),
              "This pipeline read assignment is lost when the stage "
              "exits",
              "Feed the loop with a redirection or process substitution");
          actx.pipeline_lost_names.add(target);
        }
      }
      if (!literal.view().starts_with("-")) {
        let const target = operand_target_name(literal.view());
        if (!target.is_empty()) actx.external_input_names.add(target);
      }
    }
  }

  if (!command_is_shadowed && command_literal == "export" &&
      !args_have_short_flag(m_args, 'n'))
  {
    for (usize i = 1; i < m_args.count(); i++) {
      let const raw = m_args[i]->raw_string();
      if (raw.view().starts_with(StringView{"CDPATH="}) ||
          raw.view() == "CDPATH")
        actx.warn_shellcheck(
            2184, m_args[i]->source_location(),
            "An exported CDPATH can redirect cd commands in child "
            "scripts",
            "Keep CDPATH unexported or clear it before running scripts");
    }
  }

  if (!command_is_shadowed && command_literal == "unset") {
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const raw = m_args[i]->raw_string();
      let const source_text =
          analysis_source_text(actx, m_args[i]->source_location());
      if (raw.view().find_character('[').has_value() &&
          raw.view().find_character(']').has_value() &&
          (source_text.is_empty() ||
           (source_text[0] != '\'' && source_text[0] != '"')))
        actx.warn_shellcheck(
            2184, m_args[i]->source_location(),
            "An unquoted unset array index can expand as a filename "
            "glob",
            "Quote the complete array element name");
    }
  }

  if (!command_is_shadowed && command_literal == "find") {
    for (usize i = 1; i + 1 < m_args.count(); i++) {
      let const predicate = m_args[i]->raw_string();
      if (predicate.view() == "-name" || predicate.view() == "-iname" ||
          predicate.view() == "-path" || predicate.view() == "-ipath" ||
          predicate.view() == "-regex")
      {
        if (m_args[i + 1]->kind() == Token::Kind::Word &&
            word_is_bare_glob(
                static_cast<const tokens::WordToken *>(m_args[i + 1])->word()))
          actx.warn_shellcheck(
              2061, m_args[i + 1]->source_location(),
              "The unquoted find pattern expands before find sees it",
              "Quote the pattern so find performs the match");
      }

      if (predicate.view() == "-exec" && i + 3 < m_args.count()) {
        let const shell_name = m_args[i + 1]->raw_string();
        let const shell_flag = m_args[i + 2]->raw_string();
        let const script = m_args[i + 3]->raw_string();
        if ((shell_name.view() == "sh" || shell_name.view() == "bash") &&
            shell_flag.view() == "-c" &&
            view_contains(script.view(), StringView{"{}"}))
          actx.warn_shellcheck(
              2156, m_args[i + 3]->source_location(),
              "The find result is inserted into shell source and can "
              "execute filename text",
              "Pass the result as an argument and reference it as $1");
      }
    }
  }

  if (!command_is_shadowed && command_literal == "tr") {
    for (usize i = 1; i < m_args.count() && i <= 2; i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      if (word_is_bare_glob(word))
        actx.warn_shellcheck(
            2060, m_args[i]->source_location(),
            "The unquoted tr range can expand as a filename glob",
            "Quote the range");
    }
  }

  if (!command_is_shadowed && command_literal == "let") {
    for (usize i = 1; i < m_args.count(); i++) {
      let const expression = m_args[i]->raw_string();
      if (arithmetic_reads_external_input(actx, expression.view()))
        actx.fail(m_args[i]->source_location(),
                  "External input is evaluated as arithmetic code",
                  "Validate the value as decimal digits before arithmetic");
    }
  }

  if (!command_is_shadowed && command_literal == "printf") {
    usize format_index = 1;
    if (format_index < m_args.count() &&
        m_args[format_index]->raw_string().view() == "-v")
      format_index += 2;
    if (format_index < m_args.count() &&
        m_args[format_index]->kind() == Token::Kind::Word)
    {
      let const &format_word =
          static_cast<const tokens::WordToken *>(m_args[format_index])->word();
      if (word_is_fully_literal(format_word)) {
        let const format = format_word.to_literal_string();
        let const consumed = printf_consumed_argument_count(format.view());
        let const available = m_args.count() - format_index - 1;
        if (consumed > available)
          actx.warn_shellcheck(
              2183, m_args[format_index]->source_location(),
              "The printf format consumes more arguments than the "
              "command supplies",
              "Add the missing arguments or remove format directives");
      }
    }
  }

  if (!command_is_shadowed && command_literal == "sudo") {
    for (let const &redirection : m_redirections)
      if (redirection.target != nullptr)
        actx.warn_shellcheck(
            2024, redirection.target->source_location(),
            "The shell opens this redirection before sudo changes "
            "privileges",
            "Run a shell under sudo or pipe through sudo tee");
    for (usize i = 1; i < m_args.count(); i++)
      if (m_args[i]->kind() == Token::Kind::Word &&
          word_is_bare_glob(
              static_cast<const tokens::WordToken *>(m_args[i])->word()))
        actx.warn_shellcheck(
            2024, m_args[i]->source_location(),
            "The shell expands this glob before sudo changes "
            "privileges",
            "Run the glob expansion inside a shell under sudo");
  }

  if (!command_is_shadowed && !TEST_COMMANDS.contains(command_literal.view()) &&
      !VARIABLE_TARGET_COMMANDS.contains(command_literal.view()))
  {
    let const command_is_assignment_builtin =
        ASSIGNMENT_BUILTINS.contains(command_literal.view());
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      if (!word_has_split_eligible_variable(word)) continue;
      if (command_is_assignment_builtin &&
          word.get_assignment_split().has_value())
        continue;
      actx.warn_shellcheck(
          2086, m_args[i]->source_location(),
          "An unquoted variable can split into words and expand globs",
          "Quote the expansion to keep one argument");
    }
  }

  if (!command_is_shadowed && !TEST_COMMANDS.contains(command_literal.view()) &&
      command_literal != "echo" && command_literal != "printf")
  {
    bool has_end_of_options = false;
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      let const literal = word.to_literal_string();
      if (literal.view() == "--") {
        has_end_of_options = true;
        continue;
      }
      if (has_end_of_options) continue;
      if (bare_glob_can_start_with_dash(word))
        actx.fail_shellcheck(
            2035, m_args[i]->source_location(),
            "A bare glob can expand to a filename that begins with '-'",
            "Prefix the glob with a directory or place -- before it");
    }
  }

  /* A dot, source, eval, or alias runs or defines code the prepass cannot see,
     so any later unresolved command must not be a hard failure. */
  if (RUNTIME_DEFINER_COMMANDS.contains(command_literal.view())) {
    LOG(Debug,
        "'%s' may define commands at run time, later resolution failures "
        "degrade to warnings",
        command_literal.c_str());
    actx.has_seen_runtime_definer = true;
  }

  /* A funsub argument, ${ ...; }, runs its body in the current shell, so a
     function it defines persists where the prepass cannot see it. */
  for (let const t : m_args) {
    if (t->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(t)->word();
    for (let const &segment : word.segments) {
      if (segment.kind == WordSegment::Kind::FunctionSubstitution) {
        actx.has_seen_runtime_definer = true;
        break;
      }
    }
  }

  /* An unterminated bracket expression would throw at run time, and the fault
     is visible from the word bytes alone. */
  for (let const t : m_args) {
    if (t->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(t)->word();
    if (word_has_malformed_glob_bracket(word)) {
      actx.fail(t->source_location(),
                "Malformed glob pattern, unterminated '['");
    }
  }

  /* An unquoted variable inside a test silently breaks when it is empty or
     splits. This stays a warning even at the strict default, since the split
     may be intended. */
  if (TEST_COMMANDS.contains(command_literal.view())) {
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      for (let const &segment : word.segments) {
        if (segment.kind == WordSegment::Kind::VariableReference &&
            segment.is_split_eligible())
        {
          actx.warn_shellcheck(2086, m_args[i]->source_location(),
                               "A test reads an unquoted variable",
                               "Quote it to avoid an empty or split argument");
          break;
        }
      }
    }
  }

  /* read without -r lets a backslash escape the next byte, mangling a line,
     shellcheck SC2162. */
  if (command_literal == "read" && !command_is_shadowed &&
      !args_have_short_flag(m_args, 'r'))
  {
    actx.warn_shellcheck(2162, source_location(),
                         "A read without -r mangles a backslash in the input",
                         "Add -r to read the line literally");
  }

  /* The bashism lints, each fired only under a POSIX shebang. echo -e/-n/-E is
     SC3037, declare and typeset are SC3044, source is SC3046. */
  if (actx.shebang_is_posix_sh && !command_is_shadowed) {
    if (command_literal == "echo" && m_args.count() >= 2 &&
        m_args[1]->kind() == Token::Kind::Word)
    {
      let const flag = static_cast<const tokens::WordToken *>(m_args[1])
                           ->word()
                           .to_literal_string();
      let const view = flag.view();
      if (view == "-e" || view == "-n" || view == "-E" || view == "-ne" ||
          view == "-en")
      {
        actx.warn_shellcheck(
            3037, m_args[1]->source_location(),
            "An echo " + view +
                " relies on a bash builtin, the POSIX echo prints the "
                "flag as text",
            "Use printf instead under a sh shebang");
      }
    }
    if (command_literal == "declare" || command_literal == "typeset")
      actx.warn_shellcheck(
          3044, m_args[0]->source_location(),
          StringView{"The "} + command_literal.view() +
              " builtin is not in POSIX",
          "Assign the variable plainly under a sh shebang, or switch the "
          "shebang to bash");
    if (command_literal == "source")
      actx.warn_shellcheck(
          3046, m_args[0]->source_location(),
          "The name source is the bash spelling, the POSIX dot command "
          "is '.'",
          "Use '.' under a sh shebang");
    if (command_literal == "local")
      actx.warn_shellcheck(
          3043, m_args[0]->source_location(),
          "The local builtin is not in POSIX sh, the value stays global",
          "rework the function or switch the shebang to bash");
    if (command_literal == "printf" && m_args.count() >= 2 &&
        m_args[1]->kind() == Token::Kind::Word &&
        static_cast<const tokens::WordToken *>(m_args[1])
                ->word()
                .to_literal_string()
                .view() == "-v")
    {
      actx.warn_shellcheck(
          3045, m_args[1]->source_location(),
          "The printf -v form is a bash extension, the POSIX printf "
          "has no -v",
          "capture the output with a command substitution under a sh "
          "shebang");
    }
    /* mapfile and its readarray alias are bash array builtins, shellcheck
       SC3030. */
    if (command_literal == "mapfile" || command_literal == "readarray")
      actx.warn_shellcheck(
          3030, m_args[0]->source_location(),
          command_literal.view() +
              " is a bash array builtin absent from POSIX sh",
          "read the input with a while read loop or switch the shebang "
          "to bash");
  }

  if (!command_is_shadowed) {
    if (command_literal == "egrep")
      actx.warn_shellcheck(
          2196, m_args[0]->source_location(), "The egrep command is deprecated",
          "Use grep -E for the extended regular expression match");
    else if (command_literal == "fgrep")
      actx.warn_shellcheck(2197, m_args[0]->source_location(),
                           "The fgrep command is deprecated",
                           "Use grep -F for the fixed string match");
    else if (command_literal == "expr")
      actx.warn_shellcheck(
          2003, m_args[0]->source_location(),
          "An expr forks for arithmetic the shell does natively",
          "Use $((...)) for the calculation");
    else if (command_literal == "local" && actx.function_scope_depth == 0)
      actx.warn_shellcheck(
          2168, m_args[0]->source_location(),
          "A local outside a function has no scope to bind",
          "Declare the variable plainly or move it into a function");
    else if (command_literal == "typeset" && !actx.shebang_is_posix_sh)
      actx.fail(m_args[0]->source_location(),
                "The typeset builtin is the ksh spelling of declare",
                "Write declare for the clearer bash name",
                diagnostic_tier::Annoying);
  }

  if (command_literal == "echo" && !command_is_shadowed &&
      m_args.count() == 2 && m_args[1]->kind() == Token::Kind::Word)
  {
    let const &word = static_cast<const tokens::WordToken *>(m_args[1])->word();
    if (word.segments.count() == 1 &&
        word.segments[0].kind == WordSegment::Kind::CommandSubstitution)
    {
      actx.warn_shellcheck(
          2005, m_args[0]->source_location(),
          "An echo of a command substitution prints what the command already "
          "prints",
          "Run the command on its own instead");
    }
  }

  /* A double-quoted trap action expands at set time, not when it fires,
     shellcheck SC2064. The action is the first operand. */
  if (command_literal == "trap" && !command_is_shadowed &&
      m_args.count() >= 2 && m_args[1]->kind() == Token::Kind::Word)
  {
    let const &action =
        static_cast<const tokens::WordToken *>(m_args[1])->word();
    let action_expands_now = false;
    for (let const &segment : action.segments)
      if (segment.is_in_double_quotes &&
          (segment.kind == WordSegment::Kind::VariableReference ||
           segment.kind == WordSegment::Kind::CommandSubstitution))
      {
        action_expands_now = true;
        break;
      }
    if (action_expands_now)
      actx.warn_shellcheck(
          2064, m_args[1]->source_location(),
          "The double-quoted trap action expands now, when the trap is "
          "set, not when it fires",
          "Single-quote it so it expands as the signal arrives");
  }

  /* A variable or command substitution in the printf format lets the data
     control the directives, shellcheck SC2059. The format is the first
     non-option word, and a -- forces the next word as the format. */
  if (command_literal == "printf" && !command_is_shadowed) {
    usize format_index = 0;
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) {
        format_index = i;
        break;
      }
      let const literal = static_cast<const tokens::WordToken *>(m_args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      if (view == "--") {
        if (i + 1 < m_args.count()) format_index = i + 1;
        break;
      }
      if (!(view.length >= 1 && view[0] == '-')) {
        format_index = i;
        break;
      }
    }

    if (format_index != 0 && m_args[format_index]->kind() == Token::Kind::Word)
    {
      let const &format =
          static_cast<const tokens::WordToken *>(m_args[format_index])->word();
      bool format_has_expansion = false;
      for (let const &segment : format.segments) {
        if (segment.kind == WordSegment::Kind::VariableReference ||
            segment.kind == WordSegment::Kind::CommandSubstitution)
        {
          format_has_expansion = true;
          break;
        }
      }
      if (format_has_expansion)
        actx.warn_shellcheck(
            2059, m_args[format_index]->source_location(),
            "The printf format comes from a variable, the data can "
            "inject format directives",
            "Use printf '%s' to print it");
    }
  }

  /* An unquoted command substitution splits on IFS and globs each field,
     shellcheck SC2046. An assignment-builtin operand such as export FOO=$(cmd)
     does not split in assignment context. */
  let const command_is_assignment_builtin =
      ASSIGNMENT_BUILTINS.contains(command_literal.view());

  /* A declaration builtin that assigns from a command substitution, such as
     local x=$(cmd), reports its own success rather than the command's status,
     shellcheck SC2155. The value rides an Assignment token. */
  if (command_is_assignment_builtin && !command_is_shadowed)
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Assignment) continue;
      let const &value =
          static_cast<const tokens::Assignment *>(m_args[i])->value_word();
      let value_has_substitution = false;
      for (let const &segment : value.segments)
        if (segment.kind == WordSegment::Kind::CommandSubstitution) {
          value_has_substitution = true;
          break;
        }
      if (!value_has_substitution) continue;
      actx.warn_shellcheck(
          2155, m_args[i]->source_location(),
          "Declaring and assigning from a command substitution in one "
          "command masks the command's exit status",
          "Split the declaration and the assignment so a failure is "
          "seen");
      break;
    }

  for (usize i = 1; i < m_args.count(); i++) {
    if (m_args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(m_args[i])->word();
    bool word_has_unquoted_command_substitution = false;
    for (let const &segment : word.segments) {
      if (segment.kind == WordSegment::Kind::CommandSubstitution &&
          !segment.is_in_double_quotes)
      {
        word_has_unquoted_command_substitution = true;
        break;
      }
    }
    if (!word_has_unquoted_command_substitution) continue;
    /* An assignment-builtin operand does not split in assignment context. This
       split check allocates, so it runs only for a word carrying an unquoted
       substitution. */
    if (command_is_assignment_builtin &&
        word.get_assignment_split().has_value())
    {
      continue;
    }
    actx.warn_shellcheck(2046, m_args[i]->source_location(),
                         "An unquoted command substitution splits its output",
                         "Quote it to keep one argument");
  }

  /* rm -r with a "$var/" operand deletes / when the variable is empty,
     shellcheck SC2115. A literal top-level system directory is SC2114. */
  if (command_literal == "rm" && !command_is_shadowed &&
      args_have_short_flag(m_args, 'r'))
  {
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      if (word.segments.count() >= 2 &&
          word.segments[0].kind == WordSegment::Kind::VariableReference &&
          !word.segments[0].text.view().find_character(':').has_value() &&
          !word.segments[1].text.is_empty() && word.segments[1].text[0] == '/')
      {
        actx.warn_shellcheck(
            2115, m_args[i]->source_location(),
            "A rm -r on \"$" + word.segments[0].text.view() +
                "/\" deletes '/' when the variable is empty",
            StringView{"write ${"} + word.segments[0].text.view() +
                ":?} so an empty value aborts the command instead");
      }
      if (word_is_fully_literal(word)) {
        let const literal = word.to_literal_string();
        if (SYSTEM_DIRECTORIES.contains(literal.view()))
          actx.warn_shellcheck(2114, m_args[i]->source_location(),
                               "A rm -r targets the system directory '" +
                                   literal.view() + "'",
                               "double-check the path before running this");
      }
    }
  }

  /* The grep pattern lints. An unquoted pattern with a glob metacharacter is
     SC2062, a pattern with a leading * that has nothing to repeat is SC2063.
     The pattern is the first word past the options. */
  if ((command_literal == "grep" || command_literal == "egrep" ||
       command_literal == "fgrep") &&
      !command_is_shadowed)
  {
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (view.length >= 1 && view[0] == '-') continue;
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::UnquotedText &&
          word.segments[0].has_glob_metacharacter())
      {
        actx.warn_shellcheck(
            2062, m_args[i]->source_location(),
            "The unquoted grep pattern can glob against the local files "
            "before grep sees it",
            "Quote the pattern");
      } else if (!view.is_empty() && view[0] == '*') {
        actx.warn_shellcheck(
            2063, m_args[i]->source_location(),
            "A grep reads a regular expression, where a leading * has "
            "nothing to repeat, this pattern looks like a glob");
      }
      break;
    }
  }

  /* mkdir -pm applies the mode only to the deepest directory, shellcheck
     SC2174. */
  if (command_literal == "mkdir" && !command_is_shadowed &&
      args_have_short_flag(m_args, 'p') && args_have_short_flag(m_args, 'm'))
  {
    actx.warn_shellcheck(
        2174, m_args[0]->source_location(),
        "A mkdir -pm applies the mode only to the deepest directory, "
        "the "
        "created parents keep the umask default");
  }

  /* An exit or return code outside the literal 0-255 shape errors or wraps
     modulo 256, shellcheck SC2242. */
  if ((command_literal == "exit" || command_literal == "return") &&
      !command_is_shadowed && m_args.count() >= 2 &&
      m_args[1]->kind() == Token::Kind::Word)
  {
    let const &operand =
        static_cast<const tokens::WordToken *>(m_args[1])->word();
    if (word_is_fully_literal(operand)) {
      let const literal = operand.to_literal_string();
      let const view = literal.view();
      let is_in_range = view_is_integer_literal(view) && view[0] != '-';
      if (is_in_range) {
        let const parsed_code = view.to<i64>();
        is_in_range = !parsed_code.is_error() && parsed_code.value() <= 255;
      }
      if (!is_in_range)
        actx.warn_shellcheck(2242, m_args[1]->source_location(),
                             "The code '" + view +
                                 "' is not a number from 0 to 255, " +
                                 command_literal.view() +
                                 " either rejects it or wraps it modulo 256");
    }
  }

  /* The $@ word lints. A bare unquoted $@ is SC2068, a $@ mixed into a longer
     word is SC2145. The [[ form gets SC2199 below. */
  for (usize i = command_literal == "[[" ? m_args.count() : 1;
       i < m_args.count(); i++)
  {
    if (m_args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(m_args[i])->word();
    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference ||
          segment.text.view() != "@")
      {
        continue;
      }
      if (word.segments.count() == 1 && !segment.is_in_double_quotes) {
        actx.warn_shellcheck(
            2068, m_args[i]->source_location(),
            "An unquoted $@ word-splits and globs each argument",
            "Quote it as \"$@\" to pass the arguments through unchanged");
      } else if (word.segments.count() > 1) {
        actx.warn_shellcheck(
            2145, m_args[i]->source_location(),
            "$@ inside a longer word concatenates the surrounding text "
            "onto the first and last argument",
            "Use $* for one joined string or a separate \"$@\" word");
      }
      break;
    }
  }

  /* A command substitution that only echoes runs a subshell for text the caller
     already has, shellcheck SC2116. A body carrying an operator runs more than
     the echo. */
  for (usize i = 0; i < m_args.count(); i++) {
    if (m_args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(m_args[i])->word();
    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::CommandSubstitution) continue;
      let const body = segment.text.view();
      usize start = 0;
      while (start < body.length && (body[start] == ' ' || body[start] == '\t'))
        start++;
      let const trimmed = body.substring(start);
      if (!trimmed.starts_with(StringView{"echo "}) && trimmed != "echo")
        continue;
      let body_runs_more_than_echo = false;
      for (usize b = 0; b < trimmed.length; b++)
        if (trimmed[b] == '|' || trimmed[b] == ';' || trimmed[b] == '&' ||
            trimmed[b] == '<' || trimmed[b] == '>' || trimmed[b] == '`')
        {
          body_runs_more_than_echo = true;
          break;
        }
      if (!body_runs_more_than_echo)
        actx.warn_shellcheck(
            2116, m_args[i]->source_location(),
            "A command substitution wraps a useless echo",
            "The text can be used directly without the subshell");
    }
  }

  /* The redirection lints. 2>&1 before the stdout file redirect is SC2069,
     reading and truncating the same file is SC2094, an input redirect into a
     non-stdin command is SC2217. */
  {
    let saw_stderr_to_stdout = false;
    /* An owned String, since the view of a to_literal_string() temporary would
       dangle past the statement. */
    String read_target{heap_allocator()};
    const Token *read_token = nullptr;
    for (let const &redirection : m_redirections) {
      if (redirection.kind == Redirection::Kind::DuplicateOutput &&
          redirection.fd == 2 && redirection.dup_fd == 1)
      {
        saw_stderr_to_stdout = true;
        continue;
      }
      let const is_file_output =
          redirection.kind == Redirection::Kind::TruncateOutput ||
          redirection.kind == Redirection::Kind::TruncateOutputOverride;
      if (is_file_output && redirection.fd == 1 && saw_stderr_to_stdout &&
          redirection.target != nullptr)
      {
        actx.warn_shellcheck(
            2069, redirection.target->source_location(),
            "2>&1 before the file redirect duplicates the terminal, so "
            "stderr stays on the terminal",
            "Put the file redirect first as in '>file 2>&1'");
      }
      if (redirection.target != nullptr &&
          redirection.target->kind() == Token::Kind::Word)
      {
        let const &target_word =
            static_cast<const tokens::WordToken *>(redirection.target)->word();
        for (let const &segment : target_word.segments)
          if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
              (view_contains(segment.text.view(), StringView{"++"}) ||
               view_contains(segment.text.view(), StringView{"--"}) ||
               segment.text.view().find_character('=').has_value()))
          {
            actx.warn_shellcheck(
                2257, redirection.target->source_location(),
                "A redirection expansion can run in a child and lose "
                "its mutation",
                "Update the variable before forming the redirect path");
            break;
          }
      }
      if (redirection.kind == Redirection::Kind::ReadInput &&
          redirection.target != nullptr &&
          redirection.target->kind() == Token::Kind::Word)
      {
        read_target = static_cast<const tokens::WordToken *>(redirection.target)
                          ->word()
                          .to_literal_string();
        read_token = redirection.target;
      }
      if (is_file_output && redirection.target != nullptr &&
          redirection.target->kind() == Token::Kind::Word &&
          read_token != nullptr)
      {
        let const write_target =
            static_cast<const tokens::WordToken *>(redirection.target)
                ->word()
                .to_literal_string();
        if (!read_target.is_empty() &&
            write_target.view() == read_target.view())
        {
          actx.warn_shellcheck(
              2094, redirection.target->source_location(),
              "The command reads and truncates '" + read_target.view() +
                  "' at once, the truncation empties the input before "
                  "it is read",
              "Write to a temporary and move it over",
              read_token->source_location(),
              "this redirect reads the file that is later truncated");
        }
      }
    }

    if (!m_redirections.is_empty() && !command_is_shadowed &&
        NON_STDIN_READERS.contains(command_literal.view()))
    {
      if (!args_have_stdin_operand(m_args))
        for (let const &redirection : m_redirections)
          if (redirection.kind == Redirection::Kind::ReadInput ||
              redirection.kind == Redirection::Kind::Heredoc ||
              redirection.kind == Redirection::Kind::HereString)
          {
            actx.warn_shellcheck(
                2217, m_args[0]->source_location(),
                "The input redirect feeds '" + command_literal.view() +
                    "', which never reads stdin, so the data is "
                    "discarded");
            break;
          }
    }
  }

  /* Obsolescent or redundant test forms. -a or -o joining two conditions is
     SC2166, warned only past the first operand and not after a !. A negated -z
     or -n is SC2236 and SC2237. */
  if (TEST_COMMANDS.contains(command_literal.view()) && !command_is_shadowed) {
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const literal = static_cast<const tokens::WordToken *>(m_args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      /* The literal of the previous word, empty for a non-word predecessor. */
      let const previous_literal =
          m_args[i - 1]->kind() == Token::Kind::Word
              ? static_cast<const tokens::WordToken *>(m_args[i - 1])
                    ->word()
                    .to_literal_string()
              : String{heap_allocator()};
      /* == is a bashism in test, shellcheck SC3014, warned only when == sits in
         the operator slot so [ x = == ] comparing the literal == is left
         alone. */
      if (view == "==" && i >= 2 &&
          !is_test_binary_operator_word(previous_literal.view()))
      {
        actx.warn_shellcheck(3014, m_args[i]->source_location(),
                             "== is undefined in POSIX test",
                             "Use = for string equality");
      }
      let const previous_is_bang = previous_literal.view() == "!";
      if (i >= 2 && !previous_is_bang && (view == "-a" || view == "-o")) {
        actx.warn_shellcheck(2166, m_args[i]->source_location(),
                             "A test with -a or -o is obsolescent",
                             "Join two tests with && or || instead");
      } else if (view == "!" && i + 1 < m_args.count() &&
                 m_args[i + 1]->kind() == Token::Kind::Word)
      {
        let const next = static_cast<const tokens::WordToken *>(m_args[i + 1])
                             ->word()
                             .to_literal_string();
        if (next.view() == "-z") {
          actx.warn_shellcheck(2236, m_args[i]->source_location(),
                               "A negated -z is just -n",
                               "Test with -n instead");
        } else if (next.view() == "-n") {
          actx.warn_shellcheck(2237, m_args[i]->source_location(),
                               "A negated -n is just -z",
                               "Test with -z instead");
        } else if (i + 2 < m_args.count() &&
                   m_args[i + 2]->kind() == Token::Kind::Word)
        {
          /* The ! X OP Y shape where OP has a direct negated form, shellcheck
             SC2335. */
          let const op = static_cast<const tokens::WordToken *>(m_args[i + 2])
                             ->word()
                             .to_literal_string();
          let const inverse = negated_test_operator(op.view());
          if (inverse.has_value()) {
            actx.warn_shellcheck(
                2335, m_args[i]->source_location(),
                StringView{"A negated "} + op + " is just " + inverse.value(),
                StringView{"Drop the ! and use "} + inverse.value());
          }
        }
      }
    }
  }

  /* A single-operand test with no operator is the nonempty-string test,
     shellcheck SC2244. A flag-shaped operand is left alone so [ -n ] is not
     told to use -n. */
  if (TEST_COMMANDS.contains(command_literal.view()) && !command_is_shadowed) {
    usize operand_end = m_args.count();
    bool bracket_form_is_closed = true;
    if (command_literal == "[" || command_literal == "[[") {
      bracket_form_is_closed =
          m_args.count() >= 2 &&
          m_args[m_args.count() - 1]->kind() == Token::Kind::Word &&
          static_cast<const tokens::WordToken *>(m_args[m_args.count() - 1])
                  ->word()
                  .to_literal_string()
                  .view() == (command_literal == "[" ? "]" : "]]");
      if (bracket_form_is_closed) operand_end = m_args.count() - 1;
    }
    if (bracket_form_is_closed && operand_end == 2 &&
        m_args[1]->kind() == Token::Kind::Word)
    {
      let const operand = static_cast<const tokens::WordToken *>(m_args[1])
                              ->word()
                              .to_literal_string();
      if (!(operand.view().length >= 1 && operand.view()[0] == '-')) {
        actx.warn_shellcheck(2244, m_args[1]->source_location(),
                             "A one-operand test is the nonempty-string test",
                             "Write it with -n to read clearer");
      }
    }

    /* The operand-shape lints over the closed operand range. A -z or -n on a
       literal operand is SC2157, a numeric comparison against a non-numeric
       literal is SC2170, and a = or == against a glob literal is SC2081. */
    for (usize i = 1; i < operand_end; i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      let const literal = word.to_literal_string();
      let const view = literal.view();

      if ((view == "-z" || view == "-n") && i + 1 < operand_end &&
          m_args[i + 1]->kind() == Token::Kind::Word)
      {
        let const &next =
            static_cast<const tokens::WordToken *>(m_args[i + 1])->word();
        if (word_is_fully_literal(next))
          actx.warn_shellcheck(2157, m_args[i + 1]->source_location(),
                               "The operand is a literal, so this " + view +
                                   " test is constant",
                               "Test a variable or drop the check");
      }

      if (is_test_numeric_operator_word(view)) {
        for (usize side = i - 1; side <= i + 1; side += 2) {
          /* Index zero is the command word, never an operand. */
          if (side == 0 || side >= operand_end ||
              m_args[side]->kind() != Token::Kind::Word)
            continue;
          let const &operand =
              static_cast<const tokens::WordToken *>(m_args[side])->word();
          if (!word_is_fully_literal(operand)) continue;
          let const operand_literal = operand.to_literal_string();
          if (!view_is_integer_literal(operand_literal.view()))
            actx.warn_shellcheck(
                2170, m_args[side]->source_location(),
                "The numeric comparison " + view + " reads '" +
                    operand_literal.view() +
                    "', which is not a number, so the test errors at run time");
        }
      }

      if (command_literal != "[[" && (view == "=" || view == "==") &&
          i + 1 < operand_end && m_args[i + 1]->kind() == Token::Kind::Word)
      {
        let const &right =
            static_cast<const tokens::WordToken *>(m_args[i + 1])->word();
        if (word_is_fully_literal(right)) {
          let const right_literal = right.to_literal_string();
          if (right_literal.view().find_character('*').has_value() ||
              right_literal.view().find_character('?').has_value())
          {
            actx.warn_shellcheck(
                2081, m_args[i + 1]->source_location(),
                "[ and test compare strings byte for byte and never glob-match",
                "Use a case or the [[ ]] form for the pattern");
          }
        }
      }

      /* A test against $? checks the exit status indirectly, shellcheck
         SC2181. */
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::VariableReference &&
          word.segments[0].text.view() == "?")
      {
        actx.warn_shellcheck(
            2181, m_args[i]->source_location(),
            "Testing $? checks the exit status indirectly",
            "Test the command directly with if or && so an intervening command "
            "cannot clobber the status");
      }
    }
  }

  /* A prefix assignment does not affect the expansion on the same command, so a
     reference to one of its names reads the old value. */
  if (m_local_vars.count() > 0) {
    for (usize i = 1; i < m_args.count(); i++) {
      if (m_args[i]->kind() != Token::Kind::Word) continue;
      let const &word =
          static_cast<const tokens::WordToken *>(m_args[i])->word();
      for (let const &segment : word.segments) {
        if (segment.kind != WordSegment::Kind::VariableReference) continue;
        const StringView referenced{segment.text.data(), segment.text.count()};
        bool does_name_a_prefix = false;
        for (let const &var : m_local_vars) {
          if (var.name.view() == referenced) {
            does_name_a_prefix = true;
            break;
          }
        }
        if (does_name_a_prefix) {
          let const message =
              StringView{"The assignment prefix does not affect this "
                         "command, '"} +
              segment.text + StringView{"' is read before it is set"};
          actx.fail(m_args[i]->source_location(), message, {},
                    diagnostic_tier::Lenient);
          break;
        }
      }
    }
  }

  let unavailable = Maybe<utils::unavailable_path_source_component>{};
  let command_was_resolved = false;
  if (name.has_value() && !actx.should_silence_unresolved_commands &&
      !command_is_shadowed)
  {
    command_was_resolved = command_resolves(*name, m_args[0]->source_location(),
                                            actx, unavailable);
  }
  if (name.has_value() && !actx.should_silence_unresolved_commands &&
      !command_is_shadowed && !command_was_resolved &&
      !actx.tested_command_names.contains(*name))
  {
    let diagnostic_location = m_args[0]->source_location();
    let reported_name = name->view();
    if (unavailable.has_value()) {
      diagnostic_location = unavailable->location;
      reported_name = unavailable->reported_prefix.view();
    }
    let const message =
        StringView{"Command '"} + reported_name + StringView{"' was not found"};
    /* A close name is offered as a did-you-mean hint on a trailing note. */
    let local_names = ArrayList<String>{heap_allocator()};
    actx.defined_functions.for_each(
        [&](StringView n) throws { local_names.push(String{n}); });
    actx.known_aliases.for_each([&](StringView n)
                                    throws { local_names.push(String{n}); });
    let suggestion_note = String{heap_allocator()};
    if (Maybe<String> suggestion =
            utils::suggest_command(StringView{*name}, local_names))
    {
      suggestion_note = "Did you mean '" + *suggestion + "'?";
    }
    /* A missing command is a fatal analysis error. After a dot, source, or eval
       the command may be defined by code the prepass cannot see, so it is only
       a warning there. */
    if (actx.has_seen_runtime_definer)
      actx.warn(diagnostic_location, message, suggestion_note.view());
    else
      actx.fail(diagnostic_location, message, suggestion_note.view(),
                diagnostic_tier::Lenient);
  }

  /* A recorded constant survives only across an environment-neutral command
     that writes no variable and runs no unseen code. Every other command
     forgets the whole table. */
  let should_clear_constants =
      !optimizer::command_is_environment_neutral(command_literal.view());
  if (!should_clear_constants) {
    /* A command substitution runs arbitrary code, so even a neutral builtin
       carrying one forgets the table. */
    for (let const t : m_args) {
      if (t->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(t)->word();
      for (let const &segment : word.segments) {
        if (segment.kind == WordSegment::Kind::CommandSubstitution) {
          should_clear_constants = true;
          break;
        }
      }
      if (should_clear_constants) break;
    }
  }

  /* A neutral builtin shadowed by a function or alias is really a call into
     user code, so it forgets the table too. */
  if (!should_clear_constants &&
      (actx.defined_functions.contains(command_literal.view()) ||
       actx.known_aliases.contains(command_literal.view())))
  {
    should_clear_constants = true;
  }

  if (should_clear_constants) {
    LOG(Debug,
        "the command '%s' may write variables, forgetting the recorded "
        "constants",
        command_literal.c_str());
    actx.constant_variables.clear();
  }

  let const is_top_level_unconditional =
      actx.function_scope_depth == 0 && is_unconditional;
  if (is_top_level_unconditional && !command_is_shadowed) {
    if (VARIABLE_TARGET_COMMANDS.contains(command_literal.view())) {
      for (usize i = 1; i < m_args.count(); i++) {
        let const word = m_args[i]->kind() == Token::Kind::Word
                             ? static_cast<const tokens::WordToken *>(m_args[i])
                                   ->word()
                                   .to_literal_string()
                             : m_args[i]->raw_string();
        actx.note_variable_assignment(operand_target_name(word.view()));
      }
    } else if (!VARIABLE_PROBE_COMMANDS.contains(command_literal.view())) {
      for (usize i = 1; i < m_args.count(); i++) {
        if (m_args[i]->kind() != Token::Kind::Word) continue;

        let const &word =
            static_cast<const tokens::WordToken *>(m_args[i])->word();
        for (let const &segment : word.segments) {
          if (segment.kind != WordSegment::Kind::VariableReference) continue;

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
  if (status_is_success == is_negated() || m_args.is_empty()) return;

  let const name = static_command_name(m_args[0]);
  if (!name.has_value()) return;
  if (actx.defined_functions.contains(name->view()) ||
      actx.known_aliases.contains(name->view()))
  {
    return;
  }

  let const is_command_test = name->view() == "command";
  let const is_type_or_hash = name->view() == "type" || name->view() == "hash";
  if (!is_command_test && !is_type_or_hash) return;

  bool has_presence_flag = is_type_or_hash;
  for (usize i = 1; i < m_args.count(); i++) {
    let const arg = static_command_name(m_args[i]);
    if (!arg.has_value()) break;
    if (is_command_test && (arg->view() == "-v" || arg->view() == "-V")) {
      has_presence_flag = true;
      continue;
    }
    if (arg->view().starts_with("-")) continue;
    if (has_presence_flag) names.add(arg->view());
  }
}

cold fn SimpleCommand::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  /* A redirection, an async or negated command, or a prefix assignment is not
     constant, so the fold declines it. The guards read this node's private
     members. */
  if (!m_redirections.is_empty()) return shit::None;
  if (is_async() || is_negated()) return shit::None;
  if (m_local_vars.count() > 0) return shit::None;

  return optimizer::simple_command_static_verdict(m_args, actx);
}

fn Pipeline::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  /* A multi-stage pipeline runs each stage in a forked child, so a stage
     assignment must not be recorded as a straight-line constant. A single
     command keeps the caller's unconditional context. */
  let const stage_is_unconditional =
      is_unconditional && m_commands.count() == 1;
  for (let const command : m_commands) {
    ASSERT(command != nullptr);
    const bool was_direct_pipeline_stage = actx.is_direct_pipeline_stage;
    actx.is_direct_pipeline_stage =
        m_commands.count() > 1 && command->as_simple_command() != nullptr;
    command->analyze(actx, stage_is_unconditional);
    actx.is_direct_pipeline_stage = was_direct_pipeline_stage;
  }

  /* cat feeding a single named file into the next stage runs an extra process,
     shellcheck SC2002. The first stage must be cat with one plain file operand
     and a later stage must follow. */
  if (m_commands.count() > 1) {
    ASSERT(m_commands[0] != nullptr);
    const SimpleCommand *first_stage = m_commands[0]->as_simple_command();
    if (first_stage != nullptr) {
      let const &cat_args = first_stage->args();
      if (cat_args.count() == 2) {
        let const name = static_command_name(cat_args[0]);
        let const raw_operand = cat_args[1]->raw_string();
        let const file_is_plain_operand =
            cat_args[1]->kind() == Token::Kind::Word &&
            !raw_operand.is_empty() && raw_operand[0] != '-';
        if (name.has_value() && name->view() == "cat" &&
            !actx.defined_functions.contains(name->view()) &&
            !actx.known_aliases.contains(name->view()) && file_is_plain_operand)
        {
          actx.warn_shellcheck(
              2002, cat_args[0]->source_location(), "A useless cat",
              "Give the file to the next command directly instead of piping "
              "cat");
        }
      }
    }
  }

  /* The stage-pair lints. find piped into xargs is SC2038, a pipe into a
     non-stdin command is SC2216. */
  for (usize i = 0; i + 1 < m_commands.count(); i++) {
    const SimpleCommand *stage = m_commands[i]->as_simple_command();
    const SimpleCommand *next = m_commands[i + 1]->as_simple_command();
    if (stage == nullptr || next == nullptr) continue;
    if (stage->args().is_empty() || next->args().is_empty()) continue;
    let const stage_name = static_command_name(stage->args()[0]);
    let const next_name = static_command_name(next->args()[0]);
    if (!stage_name.has_value() || !next_name.has_value()) continue;
    let const next_is_user =
        actx.defined_functions.contains(next_name->view()) ||
        actx.known_aliases.contains(next_name->view());

    if (stage_name->view() == "find" && next_name->view() == "xargs" &&
        !next_is_user && !actx.defined_functions.contains(stage_name->view()) &&
        !actx.known_aliases.contains(stage_name->view()))
    {
      let has_null_flag = false;
      for (usize a = 1; a < stage->args().count() && !has_null_flag; a++)
        if (stage->args()[a]->raw_string().view() == "-print0")
          has_null_flag = true;
      for (usize a = 1; a < next->args().count() && !has_null_flag; a++) {
        let const raw = next->args()[a]->raw_string();
        if (raw.view() == "-0" || raw.view() == "--null") has_null_flag = true;
      }
      if (!has_null_flag)
        actx.warn_shellcheck(
            2038, next->args()[0]->source_location(),
            "An xargs splits the find output on whitespace and quotes",
            "Pair find -print0 with xargs -0 or use find -exec");
    }

    if (!next_is_user && NON_STDIN_READERS.contains(next_name->view())) {
      if (!args_have_stdin_operand(next->args()))
        actx.warn_shellcheck(2216, next->args()[0]->source_location(),
                             "The pipe feeds '" + next_name->view() +
                                 "', which never reads stdin, so the upstream "
                                 "output is discarded");
    }

    let const stage_is_user =
        actx.defined_functions.contains(stage_name->view()) ||
        actx.known_aliases.contains(stage_name->view());
    let const next_is_grep = next_name->view() == "grep" ||
                             next_name->view() == "egrep" ||
                             next_name->view() == "fgrep";

    /* ps piped into grep races the process table and matches the grep itself,
       shellcheck SC2009. */
    if (stage_name->view() == "ps" && !stage_is_user && next_is_grep)
      actx.warn_shellcheck(
          2009, next->args()[0]->source_location(),
          "Grepping the ps output races the process table and matches the grep "
          "itself",
          "Use pgrep to match a process by name");

    /* ls piped into grep mangles a name with a space or newline, shellcheck
       SC2010. */
    if (stage_name->view() == "ls" && !stage_is_user && next_is_grep)
      actx.warn_shellcheck(
          2010, next->args()[0]->source_location(),
          "Grepping the ls listing mangles a name with a space or a newline",
          "Match the names with a glob or with find instead");

    /* grep feeding wc -l counts matches with a second process, shellcheck
       SC2126. */
    if (stage_name->view() == "grep" && !stage_is_user &&
        next_name->view() == "wc" && !next_is_user &&
        next->args().count() == 2 &&
        next->args()[1]->raw_string().view() == "-l")
    {
      actx.warn_shellcheck(
          2126, stage->args()[0]->source_location(),
          "Counting grep output with wc -l runs an extra process",
          "Use grep -c to count the matching lines directly");
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
  if (m_commands.count() != 1 || m_commands[0] == nullptr) return;
  m_commands[0]->append_presence_tested_command_names(
      actx, names, status_is_success != is_negated());
}

fn Pipeline::as_simple_command() const wontthrow -> const SimpleCommand *
{
  if (m_commands.count() != 1 || m_commands[0] == nullptr) return nullptr;
  return m_commands[0]->as_simple_command();
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

cold fn CompoundListCondition::register_defined_functions(
    AnalysisContext &actx) const throws -> void
{
  ASSERT(m_cmd != nullptr);

  m_cmd->register_defined_functions(actx);
}

cold fn CompoundListCondition::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  ASSERT(m_cmd != nullptr);

  /* An && or || node depends on the command before it, so only a plain sequence
     node carries its own verdict. */
  if (m_kind != Kind::None) return shit::None;
  return m_cmd->try_static_condition_verdict(actx);
}

fn CompoundList::analyze(AnalysisContext &actx,
                         bool is_unconditional) const throws -> void
{
  let const owns_definition_registration =
      !actx.has_registered_definitions_in_scope;
  if (owns_definition_registration) {
    actx.has_registered_definitions_in_scope = true;
    for (let const node : m_nodes) {
      ASSERT(node != nullptr);
      node->register_defined_functions(actx);
    }
  }

  for (usize i = 0; i < m_nodes.count(); i++) {
    ASSERT(m_nodes[i] != nullptr);
    let const command = m_nodes[i]->command();
    let const simple =
        command != nullptr ? command->as_simple_command() : nullptr;
    if (simple != nullptr && !simple->args().is_empty()) {
      let const name = static_command_name(simple->args()[0]);
      if (name.has_value() && !actx.defined_functions.contains(name->view()) &&
          !actx.known_aliases.contains(name->view()))
      {
        if (name->view() == "cd" && i + 1 < m_nodes.count() &&
            m_nodes[i + 1]->kind() == CompoundListCondition::Kind::None)
        {
          actx.fail_shellcheck(2164, simple->args()[0]->source_location(),
                               "This cd is unchecked, so later commands can "
                               "run in the wrong directory",
                               "Stop or return when cd fails");
        }
        if (name->view() == "exec" && simple->args().count() > 1 &&
            i + 1 < m_nodes.count())
        {
          let const next_command = m_nodes[i + 1]->command();
          ASSERT(next_command != nullptr);
          actx.warn_shellcheck(
              2093, simple->args()[0]->source_location(),
              "Commands after exec do not run when exec succeeds",
              "Remove exec or remove the unreachable commands",
              next_command->source_location(),
              "this is the first command skipped after exec");
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
        is_test_command = middle_name.has_value() &&
                          TEST_COMMANDS.contains(middle_name->view());
      }
      if (!is_test_command)
        actx.warn_shellcheck(2015, m_nodes[i]->source_location(),
                             "A && B || C also runs C when B fails",
                             "Use an if statement when C is the else branch");
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
      actx.warn_shellcheck(
          2129, repeated_append_location,
          "Several commands append to the same file separately",
          "Apply one append redirection to a grouped command", current_location,
          "this later append belongs under the same redirection");
  }

  let saved_tested_command_names = actx.tested_command_names.clone();
  const CompoundListCondition *previous_node = nullptr;
  for (let const node : m_nodes) {
    ASSERT(node != nullptr);

    if (node->kind() != CompoundListCondition::Kind::And) {
      actx.tested_command_names = saved_tested_command_names.clone();
      if (node->kind() == CompoundListCondition::Kind::Or &&
          previous_node != nullptr)
      {
        previous_node->append_presence_tested_command_names(
            actx, actx.tested_command_names, false);
      }
    }

    /* A semicolon or newline node runs whenever the list runs, an && or || node
       is conditional. */
    let const node_unconditional =
        is_unconditional && node->kind() == CompoundListCondition::Kind::None;
    node->analyze(actx, node_unconditional);
    previous_node = node;
  }
  if (!actx.should_retain_tested_command_names)
    actx.tested_command_names = steal(saved_tested_command_names);
  if (owns_definition_registration)
    actx.has_registered_definitions_in_scope = false;
}

fn CompoundList::append_presence_tested_command_names(
    const AnalysisContext &actx, HashSet &names,
    bool status_is_success) const throws -> void
{
  if (m_nodes.count() != 1 || m_nodes[0] == nullptr) return;
  m_nodes[0]->append_presence_tested_command_names(actx, names,
                                                   status_is_success);
}

cold fn CompoundList::register_defined_functions(
    AnalysisContext &actx) const throws -> void
{
  for (let const node : m_nodes) {
    ASSERT(node != nullptr);
    node->register_defined_functions(actx);
  }
}

cold fn CompoundList::try_static_condition_verdict(
    const AnalysisContext &actx) const wontthrow -> Maybe<bool>
{
  /* Only a condition list of exactly one command has a verdict the whole
     condition takes. */
  if (m_nodes.count() != 1) return shit::None;
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

cold fn IfStatement::register_defined_functions(
    AnalysisContext &actx) const throws -> void
{
  ASSERT(m_condition != nullptr);
  ASSERT(m_then != nullptr);

  m_condition->register_defined_functions(actx);
  m_then->register_defined_functions(actx);
  if (m_otherwise != nullptr) m_otherwise->register_defined_functions(actx);
}

} /* namespace expressions */

} /* namespace shit */
