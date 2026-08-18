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

/* An option that carries a value swallows the operand behind it, so the name
   slot is not the first bare word. */
pure fn builtin_value_option_letters(command_name_id command_id) wontthrow
    -> StringView
{
  switch (command_id) {
  case command_name_id::Read: return StringView{"adinNptu"};

  case command_name_id::Mapfile:
  case command_name_id::Readarray: return StringView{"CcdnOsu"};

  default: return StringView{};
  }
}

pure fn builtin_target_binder(command_name_id command_id) wontthrow
    -> assignment_binder
{
  switch (command_id) {
  case command_name_id::Read: return assignment_binder::ReadInput;

  case command_name_id::Mapfile:
  case command_name_id::Readarray: return assignment_binder::MappedLines;

  case command_name_id::Getopts: return assignment_binder::ParsedOption;

  default: return assignment_binder::Assignment;
  }
}

fn note_variable_target_operands(AnalysisContext &actx,
                                 command_name_id command_id,
                                 const ArrayList<const Token *> &args,
                                 bool should_note_assignment,
                                 bool is_conditional) throws -> void
{
  let const value_options = builtin_value_option_letters(command_id);
  let const binder = builtin_target_binder(command_id);
  let const takes_one_named_operand = command_id == command_name_id::Getopts;

  let should_skip_option_value = false;
  let is_array_option_value = false;
  let bare_operand_count = usize{0};

  for (usize i = 1; i < args.count(); i++) {
    let const literal = args[i]->kind() == Token::Kind::Word
                            ? static_cast<const tokens::WordToken *>(args[i])
                                  ->word()
                                  .to_literal_string()
                            : args[i]->raw_string();
    let const view = literal.view();

    if (should_skip_option_value) {
      should_skip_option_value = false;
      if (!is_array_option_value) continue;
    } else if (!value_options.is_empty() && view.length >= 2 && view[0] == '-')
    {
      let const last_letter = view[view.length - 1];
      should_skip_option_value =
          value_options.find_character(last_letter).has_value();
      is_array_option_value =
          last_letter == 'a' && command_id == command_name_id::Read;
      continue;
    }

    bare_operand_count++;
    if (takes_one_named_operand && bare_operand_count != 2) continue;

    let const target = operand_target_name(view);
    if (target.is_empty()) continue;

    if (should_note_assignment)
      actx.note_variable_assignment(target, args[i]->source_location());

    /* The assignment builtin walk owns its operands and folds their values. */
    if (binder == assignment_binder::Assignment) continue;

    let const name_location =
        args[i]->source_location().length == view.length
            ? args[i]->source_location().subspan(0, target.length)
            : args[i]->source_location();

    actx.note_variable_binding_record(target, name_location, binder,
                                      is_conditional);
  }
}

fn SimpleCommand::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
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

    if (prefix_outlives_command) {
      actx.note_variable_assignment(var.name.view(), var.location);
      actx.note_variable_assignment_record(
          var.name.view(), &var.value, var.location,
          !is_unconditional || actx.has_seen_runtime_definer, var.is_append);
    }

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
    actx.note_variable_assignment_record(
        assignment.name.view(), nullptr, assignment.location,
        !is_unconditional || actx.has_seen_runtime_definer,
        assignment.is_append);
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
  let const lint_input =
      command_lint_input{m_args,
                         m_redirections,
                         m_local_vars,
                         source_location(),
                         command_literal,
                         command_info,
                         is_command_shadowed,
                         !is_unconditional || actx.has_seen_runtime_definer};

  /* A declare-family builtin writes its NAME=value operands, and those reach
     analysis as command arguments rather than as prefix assignments. */
  if (command_is_assignment_builtin && !is_command_shadowed &&
      actx.symbol_records != nullptr)
  {
    /* A -f, -F, or -p operand asks the builtin to report a name, so nothing on
       that command line is declared. */
    let is_reporting_form = false;
    for (usize i = 1; i < m_args.count(); i++) {
      let flag_storage = String{heap_allocator()};
      let const flag_text = borrowed_token_text(m_args[i], flag_storage);
      if (flag_text.length < 2 || flag_text[0] != '-') continue;

      if (flag_text == "--") break;

      if (flag_text.find_character('f').has_value() ||
          flag_text.find_character('F').has_value() ||
          flag_text.find_character('p').has_value())
      {
        is_reporting_form = true;
        break;
      }
    }

    for (usize i = 1; i < m_args.count(); i++) {
      let split = Maybe<word_assignment_split>{None};
      StringView recorded_name;
      const Word *recorded_value = nullptr;
      bool is_append = false;

      if (m_args[i]->kind() == Token::Kind::Assignment) {
        let const *assignment =
            static_cast<const tokens::Assignment *>(m_args[i]);
        recorded_name = assignment->key().view();
        recorded_value = &assignment->value_word();
        is_append = assignment->is_append();
      } else if (m_args[i]->kind() == Token::Kind::Word) {
        let const &word =
            static_cast<const tokens::WordToken *>(m_args[i])->word();

        split = word.get_assignment_split();
        if (!split.has_value()) split = word.get_quoted_assignment_split();

        if (!split.has_value()) {
          if (is_reporting_form) continue;

          let const declared = optimizer::literal_word_value(word);
          if (!declared.has_value()) continue;
          if (!optimizer::is_plain_variable_name(declared->view())) continue;

          actx.note_variable_binding_record(
              declared->view(), m_args[i]->source_location(),
              assignment_binder::Declaration,
              !is_unconditional || actx.has_seen_runtime_definer);

          continue;
        }

        recorded_name = split->name.view();
        recorded_value = &split->value;
        is_append = split->is_append;
      } else {
        continue;
      }

      /* An element assignment carries no scalar literal for the base name. */
      if (let const bracket = recorded_name.find_character('[');
          bracket.has_value())
      {
        recorded_name = recorded_name.substring_of_length(0, *bracket);
        recorded_value = nullptr;
      }

      actx.note_variable_assignment_record(
          recorded_name, recorded_value, m_args[i]->source_location(),
          !is_unconditional || actx.has_seen_runtime_definer, is_append);
    }
  }

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
        case WordSegment::Kind::ArithmeticExpansion: {
          quote_sandwich_state = 0;

          let const segment_location =
              segment.get_source_location(arg_location.filename)
                  .value_or(arg_location);
          let const base_position =
              segment_location.length == segment.text.count()
                  ? Maybe<usize>{segment_location.position}
                  : None;

          check_arithmetic_expression_lints(actx, segment.text.view(),
                                            segment_location, base_position,
                                            lint_input.is_conditional);

          if (is_operand && !has_external_arithmetic_read &&
              arithmetic_reads_external_input(actx, segment.text.view()))
          {
            has_external_arithmetic_read = true;
          }
          break;
        }

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
    let reported_diagnostic = resolution_diagnostic;
    let diagnostic_location = m_args[0]->source_location();
    let reported_name = *name;
    let is_missing_directory = false;
    if (unavailable.has_value() &&
        resolution_diagnostic == diagnostic_id::unresolved_command)
    {
      diagnostic_location = unavailable->location;
      reported_name = unavailable->reported_prefix.view();
      is_missing_directory = !unavailable->is_final_component;
      if (is_missing_directory)
        reported_diagnostic = diagnostic_id::unresolved_command_directory;
    }

    let suggestion = Maybe<String>{};
    if (!is_missing_directory) {
      let local_names = ArrayList<String>{heap_allocator()};
      actx.defined_functions.for_each(
          [&](StringView n) throws { local_names.push(String{n}); });
      actx.known_aliases.for_each([&](StringView n)
                                      throws { local_names.push(String{n}); });
      suggestion = utils::suggest_command(*name, local_names);
    }

    if (suggestion.has_value()) {
      actx.report_diagnostic(reported_diagnostic, diagnostic_location,
                             {reported_name, suggestion->view()});
    } else {
      actx.report_diagnostic(reported_diagnostic, diagnostic_location,
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
  if (!is_command_shadowed &&
      command_info.is_in_group(COMMAND_GROUP_VARIABLE_TARGET))
  {
    note_variable_target_operands(
        actx, command_id, m_args, is_top_level_unconditional,
        !is_unconditional || actx.has_seen_runtime_definer);
  }

  if (!is_top_level_unconditional || is_command_shadowed) return;
  if (command_info.is_in_group(COMMAND_GROUP_VARIABLE_TARGET) ||
      command_info.is_in_group(COMMAND_GROUP_VARIABLE_PROBE))
  {
    return;
  }

  for (usize i = 1; i < m_args.count(); i++) {
    if (m_args[i]->kind() != Token::Kind::Word) continue;

    let const &word = static_cast<const tokens::WordToken *>(m_args[i])->word();
    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;

      /* A read of a name this command also assigns as a prefix is reported by
         the prefix check, which names that shape exactly. */
      let is_read_of_own_prefix = false;
      for (let const &var : m_local_vars) {
        if (var.name.view() != segment.text.view()) continue;

        is_read_of_own_prefix = true;
        break;
      }

      if (is_read_of_own_prefix) continue;

      actx.note_variable_read(segment.text.view(), m_args[i]->source_location(),
                              is_top_level_unconditional);
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
} /* namespace expressions */

} /* namespace koshka */
