#include "DiagnosticsChecksInternal.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

fn check_posix_arithmetic_operators(AnalysisContext &actx,
                                    StringView expression,
                                    const SourceLocation &location) throws
    -> void
{
  let has_increment = false;
  let has_decrement = false;
  let has_exponent = false;

  for (usize position = 0; position + 1 < expression.length; position++) {
    let const byte = expression[position];
    if (expression[position + 1] != byte) continue;

    switch (byte) {
    case '+': has_increment = true; break;
    case '-': has_decrement = true; break;
    case '*': has_exponent = true; break;
    default: continue;
    }

    position++;
  }

  if (has_increment)
    actx.report_diagnostic(diagnostic_id::sc3018, location, {"++"});
  if (has_decrement)
    actx.report_diagnostic(diagnostic_id::sc3018, location, {"--"});
  if (has_exponent) actx.report_diagnostic(diagnostic_id::sc3019, location);
}

/* An expansion that stands left of an arithmetic assignment names the variable
   the assignment writes, so the dollar sign carries the indirection and cannot
   be dropped. */
static pure fn arithmetic_expansion_assigns(StringView expression,
                                            usize expansion_end) wontthrow
    -> bool
{
  usize at = expansion_end;

  while (at < expression.length && lexer::is_whitespace(expression[at]))
    at++;

  if (at >= expression.length) return false;

  switch (expression[at]) {
  case '<':
  case '>':
    if (at + 1 >= expression.length || expression[at + 1] != expression[at])
      return false;
    at += 2;
    break;

  case '+':
  case '-':
  case '*':
  case '/':
  case '%':
  case '&':
  case '|':
  case '^': at++; break;

  default: break;
  }

  if (at >= expression.length || expression[at] != '=') return false;

  return at + 1 >= expression.length || expression[at + 1] != '=';
}

fn note_arithmetic_target_record(AnalysisContext &actx, StringView expression,
                                 StringView target,
                                 const SourceLocation &location,
                                 Maybe<usize> expression_base_position,
                                 bool is_conditional, bool is_append) throws
    -> void
{
  if (!expression_base_position.has_value()) return;

  let const target_offset = static_cast<usize>(target.data - expression.data);
  const SourceLocation name_location{*expression_base_position + target_offset,
                                     target.length, location.source_name_index};
  if (analysis_source_text(actx, name_location) != target) return;

  actx.note_variable_occurrence(target, name_location,
                                variable_occurrence_kind::Assignment,
                                is_conditional, is_append);
  actx.note_variable_binding_record(
      target, name_location, assignment_binder::Arithmetic, is_conditional);
}

fn check_arithmetic_expression_lints(AnalysisContext &actx,
                                     StringView expression,
                                     const SourceLocation &location,
                                     Maybe<usize> expression_base_position,
                                     bool is_conditional) throws -> void
{
  let has_reported_test_operator = false;
  let has_reported_decimal = false;
  let has_reported_octal = false;
  let has_reported_precision_loss = false;
  let has_redundant_dollar = false;
  /* A division truncates its result, so a multiplication that follows it in the
     same term multiplies the truncated value, shellcheck SC2017. Any operator
     that ends the term clears the flag. */
  let has_pending_division = false;

  struct arithmetic_write
  {
    StringView name;
    usize end_position;
    usize parenthesis_depth;
    usize bracket_depth;
    usize ternary_depth;
    bool is_append;
    bool has_fixed_end;
  };
  let pending_writes = ArrayList<arithmetic_write>{heap_allocator()};
  usize parenthesis_depth = 0;
  usize bracket_depth = 0;
  usize ternary_depth = 0;
  let const do_apply_write = [&]() throws -> void {
    let const write = pending_writes.back();
    pending_writes.pop_back();
    actx.note_variable_assignment(write.name, location, !is_conditional);
    note_arithmetic_target_record(actx, expression, write.name, location,
                                  expression_base_position, is_conditional,
                                  write.is_append);
  };
  let const do_apply_fixed_writes = [&](usize end_position) throws -> void {
    while (!pending_writes.is_empty() && pending_writes.back().has_fixed_end &&
           pending_writes.back().end_position <= end_position)
    {
      do_apply_write();
    }
  };
  let const do_apply_boundary_writes = [&]() throws -> void {
    while (!pending_writes.is_empty()) {
      let const &write = pending_writes.back();
      if (write.has_fixed_end || write.parenthesis_depth != parenthesis_depth ||
          write.bracket_depth != bracket_depth ||
          write.ternary_depth != ternary_depth)
      {
        break;
      }
      do_apply_write();
    }
  };

  for (usize position = 0; position < expression.length; position++) {
    do_apply_fixed_writes(position);

    switch (expression[position]) {
    case '/':
      if (position + 1 < expression.length &&
          (expression[position + 1] == '/' || expression[position + 1] == '='))
      {
        position++;
        has_pending_division = false;
        break;
      }
      has_pending_division = true;
      break;

    case '*':
      if (position + 1 < expression.length && expression[position + 1] == '*') {
        position++;
        has_pending_division = false;
        break;
      }
      if (has_pending_division && !has_reported_precision_loss) {
        actx.report_diagnostic(diagnostic_id::sc2017, location);
        has_reported_precision_loss = true;
      }
      has_pending_division = false;
      break;

    case '-': {
      has_pending_division = false;
      if (has_reported_test_operator) break;
      if (position + 3 > expression.length) break;
      if (position > 0 && !lexer::is_whitespace(expression[position - 1]))
        break;

      let const after = position + 3;
      if (after < expression.length && !lexer::is_whitespace(expression[after]))
        break;

      let const candidate = expression.substring_of_length(position, 3);
      if (is_test_numeric_operator_word(candidate)) {
        actx.report_diagnostic(diagnostic_id::sc1106, location, {candidate});
        has_reported_test_operator = true;
      }
      break;
    }

    case '(':
      parenthesis_depth++;
      has_pending_division = false;
      break;

    case ')':
      do_apply_boundary_writes();
      if (parenthesis_depth > 0) parenthesis_depth--;
      has_pending_division = false;
      break;

    case '[': bracket_depth++; break;

    case ']':
      do_apply_boundary_writes();
      if (bracket_depth > 0) bracket_depth--;
      break;

    case '?':
      ternary_depth++;
      has_pending_division = false;
      break;

    case ':':
      do_apply_boundary_writes();
      if (ternary_depth > 0) ternary_depth--;
      has_pending_division = false;
      break;

    case ',':
      do_apply_boundary_writes();
      has_pending_division = false;
      break;

    case '+':
    case ';':
    case '|':
    case '&':
    case '^':
    case '<':
    case '>':
    case '%': has_pending_division = false; break;

    case '=': has_pending_division = false; break;

    /* An arithmetic expression is never quoted, so a $ here introduces a real
       expansion and a positional name behind it is a positional read. */
    case '$': {
      has_pending_division = false;

      let const dollar_position = position;
      usize name_start = position + 1;
      let const is_braced =
          name_start < expression.length && expression[name_start] == '{';
      if (is_braced) name_start++;

      let const has_plain_name =
          name_start < expression.length &&
          lexer::is_variable_name_start(expression[name_start]);

      usize name_end = name_start;
      while (name_end < expression.length &&
             lexer::is_variable_name(expression[name_end]))
        name_end++;

      if (name_end == name_start && name_end < expression.length) {
        switch (expression[name_end]) {
        case '@':
        case '*':
        case '#': name_end++; break;

        default: break;
        }
      }

      position = name_end - 1;

      if (is_braced &&
          (name_end >= expression.length || expression[name_end] != '}'))
      {
        break;
      }

      /* A name character on either side means the expansion builds a longer
         name, and dropping the sign would merge the parts into one
         identifier. */
      let const expansion_end = is_braced ? name_end + 1 : name_end;
      let const is_name_part =
          (dollar_position > 0 &&
           lexer::is_variable_name(expression[dollar_position - 1])) ||
          (expansion_end < expression.length &&
           lexer::is_variable_name(expression[expansion_end]));
      if (has_plain_name && !is_name_part &&
          !arithmetic_expansion_assigns(expression, expansion_end))
      {
        has_redundant_dollar = true;
      }

      let const referenced =
          expression.substring_of_length(name_start, name_end - name_start);
      if (!referenced.is_empty() && expression_base_position.has_value()) {
        let const occurrence_start = dollar_position;
        let const occurrence_length = expansion_end - occurrence_start;
        let const occurrence_location =
            SourceLocation{*expression_base_position + occurrence_start,
                           occurrence_length, location.source_name_index};
        actx.note_variable_occurrence(referenced, occurrence_location,
                                      variable_occurrence_kind::Reference);
        actx.note_positional_reference(referenced, occurrence_location);
      }
      break;
    }

    default: {
      if (!lexer::is_variable_name(expression[position])) break;

      /* The whole name or number is consumed so the scan never restarts inside
         one and reads a suffix as a fresh literal. */
      let const start = position;
      while (position + 1 < expression.length &&
             (lexer::is_variable_name(expression[position + 1]) ||
              expression[position + 1] == '.'))
      {
        position++;
      }

      let const word =
          expression.substring_of_length(start, position + 1 - start);
      if (!lexer::is_number(word[0])) {
        usize after_name = position + 1;
        while (after_name < expression.length &&
               lexer::is_whitespace(expression[after_name]))
        {
          after_name++;
        }

        let is_assignment_target = false;
        let is_compound_assignment = false;
        let is_step_target = false;
        let is_prefix_step_target = false;
        usize operator_end = after_name;
        if (start >= 2 &&
            (expression[start - 1] == '+' || expression[start - 1] == '-') &&
            expression[start - 2] == expression[start - 1])
        {
          is_assignment_target = true;
          is_compound_assignment = true;
          is_prefix_step_target = true;
        } else if (after_name < expression.length) {
          switch (expression[after_name]) {
          case '=':
            is_assignment_target =
                arithmetic_assignment_target(expression, after_name) == word;
            break;

          case '+':
          case '-':
          case '*':
          case '/':
          case '%':
          case '&':
          case '|':
          case '^':
            if (after_name + 1 < expression.length &&
                expression[after_name + 1] == '=')
            {
              is_assignment_target = arithmetic_assignment_target(
                                         expression, after_name + 1) == word;
              is_compound_assignment = is_assignment_target;
            } else if (after_name + 1 < expression.length &&
                       (expression[after_name] == '+' ||
                        expression[after_name] == '-') &&
                       expression[after_name + 1] == expression[after_name])
            {
              is_assignment_target = true;
              is_compound_assignment = true;
              is_step_target = true;
            }
            break;

          case '<':
          case '>':
            if (after_name + 2 < expression.length &&
                expression[after_name + 1] == expression[after_name] &&
                expression[after_name + 2] == '=')
            {
              is_assignment_target = arithmetic_assignment_target(
                                         expression, after_name + 2) == word;
              is_compound_assignment = is_assignment_target;
            }
            break;

          default: break;
          }
        }

        if (expression_base_position.has_value()) {
          let const name_location =
              SourceLocation{*expression_base_position + start, word.length,
                             location.source_name_index};
          if (!is_assignment_target || is_compound_assignment) {
            actx.note_variable_occurrence(word, name_location,
                                          variable_occurrence_kind::Reference);
          }
        }

        if (is_assignment_target) {
          if (is_prefix_step_target) {
            operator_end = after_name;
          } else {
            operator_end = after_name + 1;
            if (is_compound_assignment) {
              operator_end++;
              if (!is_step_target &&
                  (expression[after_name] == '<' ||
                   expression[after_name] == '>') &&
                  after_name + 1 < expression.length &&
                  expression[after_name + 1] == expression[after_name])
              {
                operator_end++;
              }
            }
          }
          pending_writes.push(arithmetic_write{
              word, operator_end, parenthesis_depth, bracket_depth,
              ternary_depth, is_compound_assignment,
              is_step_target || is_prefix_step_target});
        }
        break;
      }

      let const dot = word.find_character('.');
      if (!actx.is_default_mood && dot.has_value() && *dot + 1 < word.length &&
          lexer::is_number(word[*dot + 1]) && !has_reported_decimal)
      {
        actx.report_diagnostic(diagnostic_id::sc2079, location, {word});
        has_reported_decimal = true;
        break;
      }

      if (word[0] == '0' && word.length > 1 && word.is_all_decimal_digits() &&
          !has_reported_octal)
      {
        actx.report_diagnostic(diagnostic_id::sc2080, location, {word});
        has_reported_octal = true;
      }
      break;
    }
    }
  }

  while (!pending_writes.is_empty())
    do_apply_write();

  if (has_redundant_dollar)
    actx.report_diagnostic(diagnostic_id::sc2004, location);
}

fn check_numeric_comparison_operand(AnalysisContext &actx,
                                    StringView operator_view,
                                    const Token *operand_token,
                                    bool should_prefer_string_comparison) throws
    -> void
{
  if (operand_token == nullptr || operand_token->kind() != Token::Kind::Word) {
    return;
  }

  if (!is_test_numeric_operator_word(operator_view)) return;

  let const location = operand_token->source_location();
  let const &word =
      static_cast<const tokens::WordToken *>(operand_token)->word();
  if (!word_is_fully_literal(word)) return;

  let const literal = word.to_literal_string();
  if (view_is_integer_literal(literal.view())) return;

  if (view_has_arithmetic_operator(literal.view())) {
    actx.report_diagnostic(diagnostic_id::sc2255, location, {literal.view()});
  } else if (view_has_decimal_fraction(literal.view())) {
    actx.report_diagnostic(diagnostic_id::sc2072, location,
                           {operator_view, literal.view()});
  } else if (should_prefer_string_comparison) {
    actx.report_diagnostic(diagnostic_id::sc2130, location, {literal.view()});
  } else {
    actx.report_diagnostic(diagnostic_id::sc2170, location,
                           {operator_view, literal.view()});
  }
}

fn check_posix_word_portability(AnalysisContext &actx,
                                const WordSegment &segment,
                                SourceLocation fallback_location) throws -> void
{
  let const text = segment.text.view();
  let const do_get_location = [&]() -> SourceLocation {
    return expansion_location_with_sigil(
        actx, segment.get_source_location(fallback_location.source_name_index)
                  .value_or(fallback_location));
  };

  switch (segment.kind) {
  case WordSegment::Kind::ProcessSubstitution:
    actx.report_diagnostic(diagnostic_id::sc3001, do_get_location());
    break;

  case WordSegment::Kind::CommandSubstitution: {
    usize position = 0;
    while (position < text.length &&
           (text[position] == ' ' || text[position] == '\t'))
      position++;

    if (position >= text.length || text[position] != '<') {
      break;
    }

    let const location = do_get_location();
    let const source_text = analysis_source_text(actx, location);
    let const spelling = !source_text.is_empty() && source_text[0] == '`'
                             ? diagnostic_id::sc3035
                             : diagnostic_id::sc3034;
    actx.report_diagnostic(spelling, location);
    break;
  }

  case WordSegment::Kind::VariableReference:
    check_posix_parameter_expansion(actx, segment, text, fallback_location);
    break;

  case WordSegment::Kind::ArithmeticExpansion:
    check_posix_arithmetic_operators(actx, text, do_get_location());
    break;

  case WordSegment::Kind::LiteralText:
    if (segment.was_ansi_c_quoted)
      actx.report_diagnostic(diagnostic_id::sc3003, do_get_location());
    break;

  case WordSegment::Kind::UnquotedText: {
    let has_extended_glob = false;
    let has_caret_bracket = false;
    for (usize position = 0; position + 1 < text.length; position++) {
      let const following = text[position + 1];
      if (following != '(' && following != '^') {
        continue;
      }

      if (following == '(') {
        has_extended_glob |= lexer::is_extglob_operator(text[position]);
      } else {
        has_caret_bracket |= text[position] == '[';
      }

      if (has_extended_glob && has_caret_bracket) {
        break;
      }
    }

    if (!has_extended_glob && !has_caret_bracket) {
      break;
    }

    let const location = do_get_location();
    if (has_extended_glob)
      actx.report_diagnostic(diagnostic_id::sc3002, location, {text});
    if (has_caret_bracket)
      actx.report_diagnostic(diagnostic_id::sc3026, location, {text});
    break;
  }

  default: break;
  }
}

fn check_operand_lints_before_scan(AnalysisContext &actx,
                                   const command_lint_input &input) throws
    -> void
{
  if (input.is_command_shadowed) return;

  let const &args = input.args;

  if (input.is_in_group(COMMAND_GROUP_TEST)) {
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      if (literal.view() == "=~")
        actx.report_diagnostic(diagnostic_id::sc2074,
                               args[i]->source_location());
    }

    if (args.count() >= 4) {
      let const first_operand = args[1]->raw_string();
      if (first_operand.view().starts_with(StringView{"x$"}) ||
          first_operand.view().starts_with(StringView{"x\"$"}))
      {
        actx.report_diagnostic(diagnostic_id::sc2268,
                               args[1]->source_location());
      }
    }

    return;
  }

  switch (input.command_id()) {
  case command_name_id::Find: {
    bool has_exec = false;
    bool has_exec_terminator = false;
    bool has_or = false;
    bool has_group = false;
    bool has_action = false;
    bool has_path_operand = false;
    bool is_before_path_operand = true;
    bool is_inside_exec_action = false;
    Maybe<SourceLocation> exec_location{};
    Maybe<SourceLocation> or_location{};
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();

      if (is_before_path_operand && !is_find_leading_option(literal.view())) {
        is_before_path_operand = false;
        has_path_operand = literal.view() == "-f" ||
                           (!literal.is_empty() && literal[0] != '-' &&
                            literal.view() != "(" && literal.view() != "!");
      }

      if (literal.view() == "-exec" || literal.view() == "-execdir") {
        has_exec = true;
        is_inside_exec_action = true;
        exec_location = args[i]->source_location();
      } else if (has_exec && (literal.view() == ";" || literal.view() == "+")) {
        has_exec_terminator = true;
        is_inside_exec_action = false;
      } else if (is_inside_exec_action &&
                 token_has_command_substitution(args[i]))
      {
        actx.report_diagnostic(diagnostic_id::sc2014,
                               args[i]->source_location());
      } else if (literal.view() == "-o") {
        has_or = true;
        or_location = args[i]->source_location();
      } else if (literal.view() == "(" || literal.view() == ")") {
        has_group = true;
      }
      if (is_find_action(literal.view())) has_action = true;
    }
    if (has_exec && !has_exec_terminator && exec_location.has_value())
      actx.report_diagnostic(diagnostic_id::sc2067, *exec_location);
    if (has_or && has_action && !has_group && or_location.has_value())
      actx.report_diagnostic(diagnostic_id::sc2146, *or_location);
    if (!has_path_operand)
      actx.report_diagnostic(diagnostic_id::sc2185, input.command_location());
    break;
  }

  case command_name_id::Alias:
    for (usize i = 1; i < args.count(); i++) {
      let const raw = args[i]->raw_string();
      if (view_contains(raw.view(), StringView{"$1"}) ||
          view_contains(raw.view(), StringView{"$@"}) ||
          view_contains(raw.view(), StringView{"$*"}))
      {
        actx.report_diagnostic(diagnostic_id::sc2142,
                               args[i]->source_location());
      }
    }
    break;

  case command_name_id::Tr:
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      let const set_view = literal.view();

      if (i <= 2 && literal.length() >= 5 && literal[0] == '[' &&
          literal[literal.length() - 1] == ']' &&
          set_view.find_character('-').has_value())
      {
        actx.report_diagnostic(diagnostic_id::sc2021,
                               args[i]->source_location());
      }

      if (set_view == "a-z") {
        actx.report_diagnostic(diagnostic_id::sc2018,
                               args[i]->source_location());
      } else if (set_view == "A-Z") {
        actx.report_diagnostic(diagnostic_id::sc2019,
                               args[i]->source_location());
      } else if (!set_view.is_empty() && set_view[0] != '-' &&
                 set_view[0] != '[' && view_repeats_a_letter(set_view))
      {
        actx.report_diagnostic(diagnostic_id::sc2020,
                               args[i]->source_location(), {set_view});
      }
    }
    break;

  case command_name_id::Echo: {
    if (args.count() == 2 && args[1]->kind() == Token::Kind::Word) {
      let const &word = static_cast<const tokens::WordToken *>(args[1])->word();
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::CommandSubstitution)
      {
        actx.report_diagnostic(diagnostic_id::sc2005,
                               args[0]->source_location());
      }
    }

    StringView escape_sequence{};
    Maybe<SourceLocation> escape_location{};
    for (usize i = 1; i < args.count(); i++) {
      let const operand_text =
          analysis_source_text(actx, args[i]->source_location());

      if (view_settles_echo_escapes(operand_text)) {
        escape_location = None;
        break;
      }

      if (escape_location.has_value()) continue;
      if (token_has_ansi_c_quote(args[i])) continue;

      escape_sequence = find_echo_escape_sequence(operand_text);
      if (!escape_sequence.is_empty())
        escape_location = args[i]->source_location();
    }

    if (escape_location.has_value())
      actx.report_diagnostic(diagnostic_id::sc2028, *escape_location,
                             {escape_sequence});
    break;
  }

  case command_name_id::Sed:
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      let const script_view = literal.view();

      if (script_view.starts_with(StringView{"-f"})) break;

      if (!script_view.is_empty() && script_view[0] == '-') {
        continue;
      }

      if (view_is_plain_substitution_script(script_view)) {
        actx.report_diagnostic(diagnostic_id::sc2001,
                               args[i]->source_location(), {script_view});
      }

      break;
    }
    break;

  default: break;
  }
}

/* The parser refuses a malformed assignment and hands the word on as a command
   name, so the leading byte of the source text decides which of the shellcheck
   assignment shapes was written. */
fn check_equals_bearing_command_name(AnalysisContext &actx,
                                     StringView command_literal,
                                     usize equals_position,
                                     const SourceLocation &location) throws
    -> bool
{
  switch (command_literal[0]) {
  case '$': {
    if (command_literal[1] == '0' && equals_position == 2) {
      let const id = actx.is_posix_sh_shebang ? diagnostic_id::sc2279
                                              : diagnostic_id::sc2277;

      return actx.report_diagnostic(id, location, {command_literal});
    }

    if (command_literal[1] >= '0' && command_literal[1] <= '9') {
      return actx.report_diagnostic(diagnostic_id::sc2270, location,
                                    {command_literal});
    }

    if (lexer::is_variable_name_start(command_literal[1]))
      return actx.report_diagnostic(diagnostic_id::sc2281, location);

    return false;
  }

  case '=': {
    bool is_all_equals = true;
    for (usize i = 0; i < command_literal.length; i += 1) {
      if (command_literal[i] != '=') {
        is_all_equals = false;
        break;
      }
    }

    if (is_all_equals && command_literal.length >= 3) {
      return actx.report_diagnostic(diagnostic_id::sc2273, location,
                                    {command_literal});
    }

    let const id = command_literal.starts_with(StringView{"==="})
                       ? diagnostic_id::sc2274
                       : diagnostic_id::sc2275;

    return actx.report_diagnostic(id, location, {command_literal});
  }

  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    return actx.report_diagnostic(diagnostic_id::sc2282, location,
                                  {command_literal});

  default: break;
  }

  if (equals_position + 1 < command_literal.length &&
      command_literal[equals_position + 1] == '=')
  {
    return actx.report_diagnostic(diagnostic_id::sc2272, location,
                                  {command_literal});
  }

  for (usize i = 0; i < equals_position; i += 1) {
    if (command_literal[i] != '$') continue;

    return actx.report_diagnostic(diagnostic_id::sc2271, location,
                                  {command_literal});
  }

  return false;
}

/* A name like [ holds a glob metacharacter that static_command_name rejects,
   so the literal text is taken separately for the test recognition. */
fn check_command_word_shape(AnalysisContext &actx,
                            const command_lint_input &input) throws -> bool
{
  let const &args = input.args;
  let const command_literal = input.command_literal;
  let const location = input.command_location();
  let has_explained_resolution_failure = false;

  if (!command_literal.is_empty() && command_literal[0] == '-')
    actx.report_diagnostic(diagnostic_id::sc2215, location);

  let const equals_position = command_literal.find_character('=');

  if (equals_position.has_value() && command_literal.length > 1 &&
      !command_literal.starts_with(StringView{"[["}))
  {
    /* The word literal drops the quotes, so the source text tells a quoted
       assignment name apart from a bare one, shellcheck SC2276. */
    let const command_source = location.get_source_text(actx.source);
    if (command_source.has_value() && !command_source->is_empty() &&
        ((*command_source)[0] == '"' || (*command_source)[0] == '\''))
    {
      has_explained_resolution_failure = actx.report_diagnostic(
          diagnostic_id::sc2276, location, {*command_source});
    } else {
      has_explained_resolution_failure = check_equals_bearing_command_name(
          actx, command_literal, *equals_position, location);
    }
  }

  if ((command_literal.starts_with(StringView{"["}) &&
       command_literal != "[") ||
      (command_literal.starts_with(StringView{"[["}) &&
       command_literal != "[["))
    actx.report_diagnostic(diagnostic_id::sc1035, location);

  if (command_literal.starts_with(StringView{"[["}) &&
      command_literal.find_character('=').has_value())
    actx.report_diagnostic(diagnostic_id::sc2077, location);

  if (command_literal.starts_with(StringView{"["}) && command_literal != "[" &&
      command_literal != "[[" && args.count() > 1)
  {
    String last_storage{heap_allocator()};
    let const last_raw = borrowed_token_text(args.back(), last_storage);
    if (!last_raw.is_empty() && last_raw[last_raw.length - 1] == ']')
      actx.report_diagnostic(diagnostic_id::sc1014, location);
  }

  /* The word literal drops the quotes, so the source text decides whether the
     bracket was written as syntax or as data. */
  if (args.count() >= 2 && command_literal != "[" && command_literal != "[[" &&
      args.back()->source_location().get_source_text(actx.source) ==
          StringView{"]"})
  {
    actx.report_diagnostic(diagnostic_id::sc2171,
                           args.back()->source_location());
  }

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"="}) {
    has_explained_resolution_failure |= actx.report_diagnostic(
        diagnostic_id::sc2283, args[1]->source_location());
  }

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"=="}) {
    has_explained_resolution_failure |= actx.report_diagnostic(
        diagnostic_id::sc2284, args[1]->source_location(), {command_literal});
  }

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"+="}) {
    has_explained_resolution_failure |= actx.report_diagnostic(
        diagnostic_id::sc2285, args[1]->source_location(), {command_literal});
  }

  return has_explained_resolution_failure;
}

fn note_formatted_target(AnalysisContext &actx, const command_lint_input &input,
                         StringView name, const SourceLocation &location) throws
    -> void
{
  if (name.is_empty() || input.is_command_shadowed) return;

  if (!input.is_conditional && actx.function_scope_depth == 0)
    actx.note_variable_assignment(name, location, true);

  actx.note_variable_occurrence(name, location,
                                variable_occurrence_kind::Assignment,
                                input.is_conditional);
  actx.note_variable_binding_record(
      name, location, assignment_binder::FormattedText, input.is_conditional);
}

fn check_operand_lints_after_scan(AnalysisContext &actx,
                                  const command_lint_input &input) throws
    -> void
{
  if (input.is_command_shadowed) return;

  let const &args = input.args;

  switch (input.command_id()) {
  case command_name_id::Read: {
    let should_skip_option_operand = false;
    let should_take_array_target = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      if (should_skip_option_operand) {
        should_skip_option_operand = false;
        if (should_take_array_target) {
          should_take_array_target = false;
          let const target = operand_target_name(literal.view());
          if (!target.is_empty()) {
            actx.add_array_valued_name(target);
            actx.external_input_names.add(target);
          }
        }

        continue;
      }
      if (literal.view().length == 2 && literal.view()[0] == '-') {
        switch (literal.view()[1]) {
        case 'a':
          should_skip_option_operand = true;
          should_take_array_target = true;
          continue;

        case 'd':
        case 'i':
        case 'n':
        case 'N':
        case 'p':
        case 't':
        case 'u': should_skip_option_operand = true; continue;

        default: break;
        }
      }
      if (literal.view().starts_with("-")) continue;
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::VariableReference)
        actx.report_diagnostic(diagnostic_id::sc2229,
                               args[i]->source_location());

      if (actx.is_direct_pipeline_stage && !literal.view().starts_with("-")) {
        let const target = operand_target_name(literal.view());
        if (!target.is_empty()) {
          actx.report_diagnostic(diagnostic_id::sc2030_read,
                                 args[i]->source_location());
          actx.pipeline_lost_names.add(target);
        }
      }
      if (!literal.view().starts_with("-")) {
        let const target = operand_target_name(literal.view());
        if (!target.is_empty()) actx.external_input_names.add(target);
      }
    }
    break;
  }

  /* The client expands an ssh operand before the remote shell ever sees it,
     shellcheck SC2029. The host is the first plain operand, so the command that
     follows it is the part that runs remotely. */
  case command_name_id::Ssh: {
    let has_seen_host = false;
    let should_skip_option_value = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (should_skip_option_value) {
        should_skip_option_value = false;
        continue;
      }
      if (!view.is_empty() && view[0] == '-') {
        if (view.length >= 2 && ssh_option_takes_value(view[view.length - 1]))
          should_skip_option_value = true;
        continue;
      }
      if (!has_seen_host) {
        has_seen_host = true;
        continue;
      }

      for (let const &segment : word.segments)
        if (segment.kind == WordSegment::Kind::VariableReference ||
            segment.kind == WordSegment::Kind::CommandSubstitution)
        {
          actx.report_diagnostic(diagnostic_id::sc2029,
                                 args[i]->source_location());
          break;
        }
    }
    break;
  }

  /* su starts a fresh shell, so a function name never resolves there,
     shellcheck SC2032. A bare command operand without -c is SC2117. */
  case command_name_id::Su: {
    let is_command_value_next = false;
    let has_seen_user = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const literal = static_cast<const tokens::WordToken *>(args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      if (is_command_value_next) {
        let const command_word = leading_command_word(view);
        if (actx.defined_functions.contains(command_word)) {
          actx.report_diagnostic(diagnostic_id::sc2032,
                                 args[i]->source_location(), {command_word});
        }
        break;
      }
      if (!view.is_empty() && view[0] == '-') {
        if (view == "-c" || view == "--command") {
          is_command_value_next = true;
        }
        continue;
      }
      if (!has_seen_user) {
        has_seen_user = true;
        continue;
      }

      if (actx.defined_functions.contains(view)) {
        actx.report_diagnostic(diagnostic_id::sc2032,
                               args[i]->source_location(), {view});
      } else {
        actx.report_diagnostic(diagnostic_id::sc2117,
                               args[i]->source_location(), {view});
      }
      break;
    }
    break;
  }

  case command_name_id::Export: {
    let const should_check_cdpath = !args_have_short_flag(args, 'n');
    for (usize i = 1; i < args.count(); i++) {
      let const raw = args[i]->raw_string();
      if (should_check_cdpath &&
          (raw.view().starts_with(StringView{"CDPATH="}) ||
           raw.view() == "CDPATH"))
      {
        actx.report_diagnostic(diagnostic_id::exported_cdpath,
                               args[i]->source_location());
      }

      if (args[i]->kind() != Token::Kind::Word) continue;

      /* export $name exports whatever the value spells, shellcheck SC2163. A
         modifier or a brace form is left alone, since the name no longer spans
         the whole segment. */
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word.segments.count() != 1 ||
          word.segments[0].kind != WordSegment::Kind::VariableReference)
        continue;

      let const text = word.segments[0].text.view();
      let const name = operand_target_name(text);
      if (!name.is_empty() && name.length == text.length)
        actx.report_diagnostic(diagnostic_id::sc2163,
                               args[i]->source_location(), {name});
    }
    break;
  }

  case command_name_id::Unset: {
    let is_function_mode = false;
    let has_ended_options = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const raw = args[i]->raw_string();
      let const raw_view = raw.view();
      if (!has_ended_options && raw_view == "--") {
        has_ended_options = true;
      } else if (!has_ended_options && raw_view.length > 1 &&
                 raw_view[0] == '-' && raw_view.find_character('f').has_value())
      {
        is_function_mode = true;
      }
    }

    has_ended_options = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const raw = args[i]->raw_string();
      let const raw_view = raw.view();
      if (!has_ended_options && raw_view == "--") {
        has_ended_options = true;
        continue;
      }
      if (!has_ended_options && raw_view.length > 1 && raw_view[0] == '-') {
        continue;
      }

      let const target = operand_target_name(raw_view);
      if (!is_function_mode && !target.is_empty() &&
          !raw_view.find_character('[').has_value() &&
          !actx.readonly_assigned_names.contains(target))
      {
        let const name_location =
            raw.count() == target.length
                ? args[i]->source_location().subspan(0, target.length)
                : args[i]->source_location();
        actx.note_variable_occurrence(target, name_location,
                                      variable_occurrence_kind::Unset);
      }

      let const source_text =
          analysis_source_text(actx, args[i]->source_location());
      if (raw.view().find_character('[').has_value() &&
          raw.view().find_character(']').has_value() &&
          (source_text.is_empty() ||
           (source_text[0] != '\'' && source_text[0] != '"')))
        actx.report_diagnostic(diagnostic_id::sc2184,
                               args[i]->source_location());
    }
    break;
  }

  case command_name_id::Find:
    for (usize i = 1; i + 1 < args.count(); i++) {
      let const predicate = args[i]->raw_string();
      if (predicate.view() == "-name" || predicate.view() == "-iname" ||
          predicate.view() == "-path" || predicate.view() == "-ipath" ||
          predicate.view() == "-regex")
      {
        if (args[i + 1]->kind() == Token::Kind::Word &&
            word_is_bare_glob(
                static_cast<const tokens::WordToken *>(args[i + 1])->word()))
          actx.report_diagnostic(diagnostic_id::sc2061,
                                 args[i + 1]->source_location());
      }

      if (predicate.view() == "-exec" || predicate.view() == "-execdir") {
        /* find launches the action itself, so a shell function is never found,
           shellcheck SC2033. */
        let const action = args[i + 1]->raw_string();
        if (actx.defined_functions.contains(action.view())) {
          actx.report_diagnostic(diagnostic_id::sc2033,
                                 args[i + 1]->source_location(),
                                 {action.view()});
        }

        if (i + 3 < args.count()) {
          let const shell_flag = args[i + 2]->raw_string();
          let const script = args[i + 3]->raw_string();
          if ((action.view() == "sh" || action.view() == "bash") &&
              shell_flag.view() == "-c" &&
              view_contains(script.view(), StringView{"{}"}))
            actx.report_diagnostic(diagnostic_id::sc2156,
                                   args[i + 3]->source_location());
        }
      }
    }
    break;

  case command_name_id::Tr:
    for (usize i = 1; i < args.count() && i <= 2; i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word_is_bare_glob(word))
        actx.report_diagnostic(diagnostic_id::sc2060,
                               args[i]->source_location());
    }
    break;

  case command_name_id::Let:
    for (usize i = 1; i < args.count(); i++) {
      let const expression = args[i]->raw_string();
      if (arithmetic_reads_external_input(actx, expression.view()))
        actx.report_diagnostic(diagnostic_id::external_arithmetic_input,
                               args[i]->source_location());

      let const operand_location = args[i]->source_location();
      let const base_position = operand_location.length == expression.count()
                                    ? Maybe<usize>{operand_location.position}
                                    : None;

      check_arithmetic_expression_lints(actx, expression.view(),
                                        operand_location, base_position,
                                        input.is_conditional);

      if (actx.is_posix_sh_shebang) {
        check_posix_arithmetic_operators(actx, expression.view(),
                                         args[i]->source_location());
      }
    }
    break;

  case command_name_id::Printf: {
    usize format_index = 1;
    if (format_index < args.count()) {
      let const leading = args[format_index]->raw_string();
      let const view = leading.view();

      if (view == "-v") {
        format_index += 2;
        if (format_index - 1 < args.count()) {
          let const name = args[format_index - 1]->raw_string();
          note_formatted_target(actx, input, operand_target_name(name.view()),
                                args[format_index - 1]->source_location());
        }
      } else if (view.length > 2 && view[0] == '-' && view[1] == 'v') {
        let const name = operand_target_name(view.substring(2));
        let const option_location = args[format_index]->source_location();
        let const is_span_verbatim = option_location.length == view.length;
        note_formatted_target(actx, input, name,
                              is_span_verbatim
                                  ? option_location.subspan(2, name.length)
                                  : option_location);
        format_index++;
      } else if (view == "--") {
        format_index++;
      }
    }

    if (format_index < args.count() &&
        args[format_index]->kind() == Token::Kind::Word)
    {
      let const &format_word =
          static_cast<const tokens::WordToken *>(args[format_index])->word();
      if (word_is_fully_literal(format_word)) {
        let const format = format_word.to_literal_string();
        let has_quote_conversion = false;
        let const consumed =
            printf_consumed_argument_count(format.view(), has_quote_conversion);
        let const available = args.count() - format_index - 1;
        if (consumed > available) {
          actx.report_diagnostic(diagnostic_id::sc2183,
                                 args[format_index]->source_location());
        } else if (consumed == 0 && available > 0) {
          actx.report_diagnostic(diagnostic_id::sc2182,
                                 args[format_index]->source_location());
        }

        if (has_quote_conversion && actx.is_posix_sh_shebang) {
          actx.report_diagnostic(diagnostic_id::sc3050,
                                 args[format_index]->source_location());
        }
      }
    }
    break;
  }

  case command_name_id::Sudo: {
    for (let const &redirection : input.redirections)
      if (redirection.target != nullptr)
        actx.report_diagnostic(diagnostic_id::sc2024_redirection,
                               redirection.target->source_location());

    let has_seen_command_word = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word_is_bare_glob(word)) {
        actx.report_diagnostic(diagnostic_id::sc2024_glob,
                               args[i]->source_location());
      }

      if (has_seen_command_word) continue;

      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (view.is_empty() || view[0] == '-') {
        continue;
      }

      has_seen_command_word = true;

      /* sudo starts an external program, so a builtin with no program of the
         same name is never reached, shellcheck SC2232. */
      if (is_shell_only_builtin(view))
        actx.report_diagnostic(diagnostic_id::sc2232,
                               args[i]->source_location(), {view});
    }
    break;
  }

  default: break;
  }
}

} /* namespace expressions */

} /* namespace koshka */
