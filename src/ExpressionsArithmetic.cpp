/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements conditional and arithmetic expression nodes together
 * with C-style loops, subshells, function definitions, and redirected-command
 * wrappers. These node families keep their rendering, static analysis, folding,
 * and evaluation methods together, while list and branch execution lives in
 * ExpressionsCompound.cpp and ExpressionsControlFlow.cpp.
 */

#include "Builtin.hpp"
#include "CLI.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace expressions {

using namespace internal;

ConditionalCommand::ConditionalCommand(SourceLocation location,
                                       ArrayList<conditional_element> elements)
    : CompoundCommand(steal(location)), m_elements(steal(elements))
{}

ConditionalCommand::~ConditionalCommand() = default;

cold fn ConditionalCommand::to_string() const throws -> String
{
  let result = String{"ConditionalCommand"};
  append_ast_execution_flags(result);
  return result;
}

cold fn ConditionalCommand::to_ast_string(usize layer) const throws -> String
{
  return indent_for_layer(layer) + "[" + to_string() + "]";
}

cold static fn conditional_word_is_literal(const Token *token) wontthrow -> bool
{
  if (token == nullptr || token->kind() != Token::Kind::Word) {
    return false;
  }
  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  for (let const &segment : word.segments)
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::UnquotedText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText)
      return false;
  return true;
}

cold static fn conditional_word_has_glob(const Token *token) wontthrow -> bool
{
  if (token == nullptr || token->kind() != Token::Kind::Word) {
    return false;
  }
  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  for (let const &segment : word.segments)
    if (segment.has_live_glob_chars() && segment.has_glob_metacharacter())
      return true;
  return false;
}

cold static fn conditional_word_is_numeric_literal(const Token *token) throws
    -> bool
{
  if (!conditional_word_is_literal(token)) return false;
  let const literal = token->raw_string();
  let view = literal.view();
  if (!view.is_empty() && (view[0] == '-' || view[0] == '+'))
    view = view.substring(1);
  return !view.is_empty() && view.is_all_decimal_digits();
}

constexpr PackedStringKey CONDITIONAL_BINARY_OPERATOR_KEYS[] = {
    SSK("="),   SSK("=="),  SSK("!="),  SSK("=~"),  SSK("-eq"),
    SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"), SSK("-ge"),
    SSK("-ef"), SSK("-nt"), SSK("-ot"),
};
constexpr StaticStringSet CONDITIONAL_BINARY_OPERATORS{
    CONDITIONAL_BINARY_OPERATOR_KEYS};

cold static fn is_conditional_binary_operator(StringView op) wontthrow -> bool
{
  return CONDITIONAL_BINARY_OPERATORS.contains(op);
}

cold static fn
conditional_operator_view(const conditional_element &element) wontthrow
    -> Maybe<StringView>
{
  using Kind = conditional_element::Kind;
  switch (element.kind) {
  case Kind::Less: return StringView{"<"};
  case Kind::Greater: return StringView{">"};
  case Kind::Operand: break;
  default: return None;
  }

  if (!element.is_bare_unquoted || element.word == nullptr) return None;

  let view = element.word->raw_view();
  if (!view.has_value() || !is_conditional_binary_operator(*view)) {
    return None;
  }

  return view;
}

cold static fn conditional_inequality_left_operand(
    const ArrayList<conditional_element> &elements,
    usize operator_index) wontthrow -> Maybe<StringView>
{
  using Kind = conditional_element::Kind;
  if (operator_index == 0 || operator_index + 1 >= elements.count())
    return None;

  let const op_view = conditional_operator_view(elements[operator_index]);
  if (!op_view.has_value() || *op_view != StringView{"!="}) {
    return None;
  }

  let const &left = elements[operator_index - 1];
  if (left.kind != Kind::Operand || left.word == nullptr) {
    return None;
  }

  return left.word->raw_view();
}

enum class conditional_misuse_kind : u8
{
  AndOperator,
  OrOperator,
  EscapedParenthesis,
  BraceGroup,
};

constexpr static_string_entry<conditional_misuse_kind>
    CONDITIONAL_MISUSE_ENTRIES[] = {
        {SSK("-a"), conditional_misuse_kind::AndOperator       },
        {SSK("-o"), conditional_misuse_kind::OrOperator        },
        {SSK("("),  conditional_misuse_kind::EscapedParenthesis},
        {SSK(")"),  conditional_misuse_kind::EscapedParenthesis},
        {SSK("{"),  conditional_misuse_kind::BraceGroup        },
        {SSK("}"),  conditional_misuse_kind::BraceGroup        },
};
constexpr StaticStringMap CONDITIONAL_MISUSES{CONDITIONAL_MISUSE_ENTRIES};

constexpr PackedStringKey CONDITIONAL_PATH_TEST_KEYS[] = {
    SSK("-L"),
    SSK("-d"),
    SSK("-e"),
    SSK("-f"),
};
constexpr StaticStringSet CONDITIONAL_PATH_TESTS{CONDITIONAL_PATH_TEST_KEYS};

constexpr PackedStringKey CONDITIONAL_EQUALITY_OPERATOR_KEYS[] = {
    SSK("!="),
    SSK("="),
    SSK("=="),
};
constexpr StaticStringSet CONDITIONAL_EQUALITY_OPERATORS{
    CONDITIONAL_EQUALITY_OPERATOR_KEYS};

cold static fn is_conditional_pattern_operator(StringView op) wontthrow -> bool
{
  return op == StringView{"=~"} || CONDITIONAL_EQUALITY_OPERATORS.contains(op);
}

cold static fn
conditional_element_ends_operand(const conditional_element &element) wontthrow
    -> bool
{
  using Kind = conditional_element::Kind;
  if (element.kind == Kind::CloseParen) return true;
  if (element.kind != Kind::Operand || element.word == nullptr) {
    return false;
  }

  return !conditional_operator_view(element).has_value();
}

fn ConditionalCommand::analyze(AnalysisContext &actx,
                               bool is_unconditional) const throws -> void
{
  for (let const &element : m_elements)
    analyze_token_substitutions(actx, element.word, is_unconditional);

  using Kind = conditional_element::Kind;
  for (usize i = 0; i < m_elements.count(); i++) {
    let const &element = m_elements[i];
    if (element.kind == Kind::Less || element.kind == Kind::Greater) {
      let const op =
          element.kind == Kind::Less ? StringView{"<"} : StringView{">"};
      if ((i > 0 &&
           conditional_word_is_numeric_literal(m_elements[i - 1].word)) ||
          (i + 1 < m_elements.count() &&
           conditional_word_is_numeric_literal(m_elements[i + 1].word)))
        actx.report_diagnostic(diagnostic_id::sc2071, element.location, {op});
      if (i > 0 && i + 1 < m_elements.count() &&
          conditional_word_is_literal(m_elements[i - 1].word) &&
          conditional_word_is_literal(m_elements[i + 1].word))
      {
        actx.report_diagnostic(
            diagnostic_id::sc2050,
            location_spanning(m_elements[i - 1].word->source_location(),
                              m_elements[i + 1].word->source_location()),
            {op});
      }
      continue;
    }

    if (element.kind == Kind::Or && i >= 3) {
      let const before = conditional_inequality_left_operand(m_elements, i - 2);
      let const after = conditional_inequality_left_operand(m_elements, i + 2);
      if (before.has_value() && after.has_value() && *before == *after) {
        actx.report_diagnostic(diagnostic_id::sc2055,
                               m_elements[i + 1].word->source_location(),
                               {*before});
      }
    }

    if (element.kind != Kind::Operand || element.word == nullptr) {
      continue;
    }

    let const operand = element.word->raw_string();

    if (element.is_bare_unquoted &&
        is_test_unary_operator_word(operand.view()) &&
        i + 1 < m_elements.count())
    {
      let const next_operator = conditional_operator_view(m_elements[i + 1]);
      if (next_operator.has_value()) {
        actx.report_diagnostic(diagnostic_id::sc1019,
                               element.word->source_location(),
                               {operand.view(), *next_operator});
      }
    }

    let const misuse = CONDITIONAL_MISUSES.find(operand.view());
    if (misuse.has_value()) {
      let const written = element.word->source_location()
                              .get_source_text(actx.source)
                              .value_or(StringView{});
      let const was_written_bare = written == operand.view();
      let const follows_operand =
          i > 0 && conditional_element_ends_operand(m_elements[i - 1]);

      switch (*misuse) {
      case conditional_misuse_kind::AndOperator:
        if (was_written_bare && follows_operand) {
          actx.report_diagnostic(diagnostic_id::sc2108,
                                 element.word->source_location());
        }
        break;

      case conditional_misuse_kind::OrOperator:
        if (was_written_bare && follows_operand) {
          actx.report_diagnostic(diagnostic_id::sc2110,
                                 element.word->source_location());
        }
        break;

      case conditional_misuse_kind::EscapedParenthesis:
        if (written.starts_with(StringView{"\\"})) {
          actx.report_diagnostic(diagnostic_id::sc1029,
                                 element.word->source_location());
        }
        break;

      case conditional_misuse_kind::BraceGroup:
        if (was_written_bare) {
          actx.report_diagnostic(diagnostic_id::sc1026,
                                 element.word->source_location());
        }
        break;
      }
    }

    let const token_kind = element.word->kind();
    if (token_kind == Token::Kind::GreaterEquals ||
        token_kind == Token::Kind::LessEquals)
    {
      actx.report_diagnostic(diagnostic_id::sc2122,
                             element.word->source_location(), {operand.view()});
    }

    if (element.word->kind() == Token::Kind::Word) {
      let const &operand_word =
          static_cast<const tokens::WordToken *>(element.word)->word();
      let const shape = classify_test_operand(operand_word);

      check_posix_word_portability(actx, operand_word,
                                   element.word->source_location());

      for (let const &segment : operand_word.segments) {
        if (segment.kind != WordSegment::Kind::VariableReference) continue;

        note_variable_reference(actx, segment, element.word->source_location());
      }

      if (shape.has_array_spread || shape.has_brace_expansion ||
          shape.has_unquoted_glob)
      {
        let const written =
            analysis_source_text(actx, element.word->source_location());

        if (shape.has_array_spread) {
          actx.report_diagnostic(diagnostic_id::sc2199,
                                 element.word->source_location(), {written});
        }

        if (shape.has_brace_expansion) {
          actx.report_diagnostic(diagnostic_id::sc2201,
                                 element.word->source_location(), {written});
        }

        if (shape.has_unquoted_glob) {
          let const previous =
              i > 0 && m_elements[i - 1].kind == Kind::Operand &&
                      m_elements[i - 1].word != nullptr
                  ? m_elements[i - 1].word->raw_string()
                  : String{heap_allocator()};

          if (!is_conditional_pattern_operator(previous.view()) &&
              !CONDITIONAL_PATH_TESTS.contains(previous.view()))
          {
            actx.report_diagnostic(diagnostic_id::sc2203,
                                   element.word->source_location(), {written});
          }
        }
      }
    }

    let is_binary_operand = false;
    if (i > 0) {
      is_binary_operand =
          conditional_operator_view(m_elements[i - 1]).has_value();
    }
    if (!is_binary_operand && i + 1 < m_elements.count()) {
      is_binary_operand =
          conditional_operator_view(m_elements[i + 1]).has_value();
    }
    if (!is_binary_operand && !conditional_operator_view(element).has_value() &&
        element.word->kind() == Token::Kind::Word)
    {
      let const &word =
          static_cast<const tokens::WordToken *>(element.word)->word();
      for (let const &segment : word.segments)
        if (segment.kind == WordSegment::Kind::UnquotedText &&
            segment.text.view().find_character('=').has_value())
        {
          actx.report_diagnostic(diagnostic_id::sc2077,
                                 element.word->source_location());
          break;
        }
    }
    if (element.is_bare_unquoted &&
        (operand.view() == "-n" || operand.view() == "-z") &&
        i + 1 < m_elements.count() &&
        conditional_word_is_literal(m_elements[i + 1].word))
      actx.report_diagnostic(diagnostic_id::sc2157_string,
                             m_elements[i + 1].word->source_location());

    if (element.is_bare_unquoted &&
        CONDITIONAL_PATH_TESTS.contains(operand.view()) &&
        i + 1 < m_elements.count() &&
        conditional_word_has_glob(m_elements[i + 1].word))
      actx.report_diagnostic(diagnostic_id::sc2144,
                             m_elements[i + 1].word->source_location());

    if (!element.is_bare_unquoted ||
        !is_conditional_binary_operator(operand.view()) || i == 0 ||
        i + 1 >= m_elements.count())
      continue;

    let const left = m_elements[i - 1].word;
    let const right = m_elements[i + 1].word;
    let const should_prefer_string_comparison =
        operand.view() == "-eq" || operand.view() == "-ne";
    check_numeric_comparison_operand(actx, operand.view(), left,
                                     should_prefer_string_comparison);
    check_numeric_comparison_operand(actx, operand.view(), right,
                                     should_prefer_string_comparison);

    const bool is_pattern_operator =
        operand.view() == "=~" ||
        ((operand.view() == "=" || operand.view() == "==") &&
         conditional_word_has_glob(right));
    if (!is_pattern_operator && conditional_word_is_literal(left) &&
        conditional_word_is_literal(right))
    {
      actx.report_diagnostic(
          diagnostic_id::sc2050,
          location_spanning(left->source_location(), right->source_location()),
          {operand.view()});
    }

    if (right != nullptr && right->kind() == Token::Kind::Word &&
        CONDITIONAL_EQUALITY_OPERATORS.contains(operand.view()))
    {
      let const &right_word =
          static_cast<const tokens::WordToken *>(right)->word();
      if (right_word.segments.count() == 1 &&
          right_word.segments[0].kind == WordSegment::Kind::VariableReference &&
          !right_word.segments[0].is_in_double_quotes)
      {
        actx.report_diagnostic(
            diagnostic_id::sc2053, right->source_location(),
            {operand.view(),
             analysis_source_text(actx, right->source_location())});
      }
    }

    if (operand.view() == "=~" && right != nullptr) {
      let const source_text =
          analysis_source_text(actx, right->source_location());
      if (source_text.is_empty()) continue;

      if (source_text[0] == '\'' || source_text[0] == '"') {
        actx.report_diagnostic(diagnostic_id::sc2076, right->source_location());
      } else if (source_text[0] == '*' || source_text[0] == '?') {
        actx.report_diagnostic(diagnostic_id::sc2049, right->source_location(),
                               {source_text});
      }
    }
  }

  actx.constant_variables.clear();
}

static fn
conditional_command_text(const ArrayList<conditional_element> &elements) throws
    -> String
{
  let command_text = String{heap_allocator(), "[["};
  for (let const &element : elements) {
    command_text.push(' ');
    switch (element.kind) {
    case conditional_element::Kind::Operand:
      if (element.word != nullptr) command_text += element.word->raw_string();
      break;
    case conditional_element::Kind::And: command_text += "&&"; break;
    case conditional_element::Kind::Or: command_text += "||"; break;
    case conditional_element::Kind::Not: command_text += "!"; break;
    case conditional_element::Kind::OpenParen: command_text += "("; break;
    case conditional_element::Kind::CloseParen: command_text += ")"; break;
    case conditional_element::Kind::Less: command_text += "<"; break;
    case conditional_element::Kind::Greater: command_text += ">"; break;
    }
  }
  command_text += " ]]";
  return command_text;
}

fn ConditionalCommand::evaluate_impl(EvalContext &cxt, root_evaluation_mode)
    const throws -> status_result
{
  cxt.source_store().set_current_location(source_location());

  let const should_run_conditional =
      publish_command_and_run_debug_trap(cxt, [&] {
        return source_command_text(
            cxt, source_location(), source_end_position(),
            [&] { return conditional_command_text(m_elements); });
      });
  if (!should_run_conditional) return cxt.execution_store().last_exit_status();

  let const substitution_mark = cxt.mark_process_substitutions();
  defer { cxt.cleanup_process_substitutions(substitution_mark); };

  i64 status;
  try {
    let const scratch_mark = cxt.expansion_store().scratch_arena().mark();
    defer { cxt.expansion_store().scratch_arena().release(scratch_mark); };
    status = cxt.evaluate_conditional(m_elements) ? 0 : 1;
  } catch (const Error &e) {
    SourceLocation span = source_location();
    if (source_end_position() > span.position)
      span.length = static_cast<u32>(source_end_position() - span.position);
    relocate_if_unlocated(e, span);
  }
  LOG(Debug, "the [[ ]] conditional yielded status %lld",
      static_cast<long long>(status));
  cxt.publish_single_pipe_status(static_cast<i32>(status));
  SET_AND_RETURN_EXIT_STATUS(cxt, status);
}

ArithmeticCommand::ArithmeticCommand(SourceLocation location,
                                     StringView expression)
    : CompoundCommand(steal(location)), m_expression(expression)
{}

ArithmeticCommand::~ArithmeticCommand() = default;

fn ArithmeticCommand::can_evaluate_in_process_substitution(
    const EvalContext &cxt, HashSet &active_functions) const throws -> bool
{
  unused(cxt);
  unused(active_functions);
  return !is_async() && !is_timed();
}

cold fn ArithmeticCommand::to_string() const throws -> String
{
  return "ArithmeticCommand";
}

cold fn ArithmeticCommand::to_ast_string(usize layer) const throws -> String
{
  let label = to_string() + " \"" + m_expression + "\"";
  append_ast_execution_flags(label);
  return indent_for_layer(layer) + "[" + label + "]";
}

static pure fn is_blank_clause(StringView text) wontthrow -> bool
{
  for (usize i = 0; i < text.length; i++)
    if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n') {
      return false;
    }
  return true;
}

static pure fn clause_without_leading_blanks(StringView clause) wontthrow
    -> StringView
{
  usize start = 0;
  while (start < clause.length &&
         (clause[start] == ' ' || clause[start] == '\t'))
  {
    start++;
  }

  return clause.substring_of_length(start, clause.length - start);
}

static fn arithmetic_clause_command_text(StringView clause) throws -> String
{
  let command_text = String{heap_allocator()};
  command_text.reserve(clause.length + 4);
  command_text += "((";
  command_text.append(clause);
  command_text += "))";

  return command_text;
}

fn ArithmeticCommand::evaluate_impl(EvalContext &cxt, root_evaluation_mode)
    const throws -> status_result
{
  LOG(Debug, "evaluating the arithmetic command '%.*s'",
      static_cast<int>(m_expression.length), m_expression.data);

  cxt.source_store().set_current_location(source_location());

  let const should_run_clause = publish_command_and_run_debug_trap(
      cxt, [&] { return arithmetic_clause_command_text(m_expression); });
  if (!should_run_clause) return cxt.execution_store().last_exit_status();

  if (is_blank_clause(m_expression)) {
    cxt.publish_single_pipe_status(1);
    SET_AND_RETURN_EXIT_STATUS(cxt, 1);
  }

  bool is_nonzero;
  try {
    const SourceLocation body_base{source_location().position + 2, 0,
                                   source_location().source_name_index};
    is_nonzero = cxt.evaluate_arithmetic_nonzero(
        m_expression, &body_base, arithmetic_text_kind::ShellSource);
  } catch (const Error &e) {
    relocate_if_unlocated(e, source_location());
  }
  const i64 status = is_nonzero ? 0 : 1;
  cxt.publish_single_pipe_status(static_cast<i32>(status));
  SET_AND_RETURN_EXIT_STATUS(cxt, status);
}

fn ArithmeticCommand::analyze(AnalysisContext &actx,
                              bool is_unconditional) const throws -> void
{
  analyze_region_substitutions(
      actx, source_location(), source_location().position,
      source_end_position() - source_location().position, false,
      is_unconditional);

  if (arithmetic_reads_external_input(actx, m_expression))
    actx.report_diagnostic(diagnostic_id::external_arithmetic_input,
                           source_location());

  check_arithmetic_expression_lints(
      actx, m_expression, source_location(), source_location().position + 2,
      !is_unconditional || actx.effects.has_seen_runtime_definer);

  if (actx.is_posix_sh_shebang) {
    actx.report_diagnostic(diagnostic_id::sc3006, source_location());
    check_posix_arithmetic_operators(actx, m_expression, source_location());
  }

  actx.constant_variables.clear();
}

fn SelectLoop::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  analyze_token_list_substitutions(actx, m_words, is_unconditional);

  let loop_entry_occurrences = actx.occurrences.snapshot();

  for (let const t : m_words) {
    if (t->kind() != Token::Kind::Word) continue;

    let const &word = static_cast<const tokens::WordToken *>(t)->word();
    check_posix_word_portability(actx, word, t->source_location());

    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;

      note_variable_reference(actx, segment, t->source_location());
    }
  }

  let const is_conditional = !is_unconditional ||
                             actx.effects.has_seen_runtime_definer ||
                             (m_has_in_clause && m_words.is_empty());
  actx.note_variable_occurrence(m_variable_name, m_variable_location,
                                variable_occurrence_kind::Assignment,
                                is_conditional);
  actx.note_variable_binding_record(m_variable_name, m_variable_location,
                                    assignment_binder::SelectLoop,
                                    is_conditional);

  actx.constant_variables.clear();
  actx.loop_body_depth++;
  actx.conditional_branch_depth++;
  m_body->analyze(actx, false);
  actx.conditional_branch_depth--;
  actx.loop_body_depth--;

  loop_entry_occurrences.merge(actx.occurrences);
  actx.occurrences = steal(loop_entry_occurrences);
}

CStyleForLoop::CStyleForLoop(SourceLocation location, usize header_position,
                             StringView init, StringView condition,
                             StringView step, const Expression *body)
    : CompoundCommand(steal(location)), m_header_position(header_position),
      m_init(init), m_condition(condition), m_step(step), m_body(body)
{}

CStyleForLoop::~CStyleForLoop()
{
  for (arith_token_cache *cache : {m_condition_cache, m_step_cache}) {
    if (cache == nullptr) continue;
    cache->~arith_token_cache();
    heap_allocator().free_array(cache, 1);
  }
}

fn CStyleForLoop::get_clause_cache(arith_token_cache *&slot) throws
    -> arith_token_cache &
{
  if (slot == nullptr) {
    let const block = heap_allocator().alloc_array<arith_token_cache>(1);
    slot = new (block) arith_token_cache{};
  }

  return *slot;
}

cold fn CStyleForLoop::to_string() const throws -> String
{
  return "CStyleForLoop";
}

cold fn CStyleForLoop::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);
  let const pad = indent_for_layer(layer);
  let label =
      to_string() + " \"" + m_init + ";" + m_condition + ";" + m_step + "\"";
  append_ast_execution_flags(label);
  return pad + "[" + label + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

fn CStyleForLoop::evaluate_impl(EvalContext &cxt, root_evaluation_mode) const
    throws -> status_result
{
  ASSERT(m_body != nullptr);

  cxt.execution_store().terminal_exec_allowed() = false;

  let const should_skip_condition_commands = !folded_commands_are_observed(cxt);
  if (is_fully_eliminated() && should_skip_condition_commands) {
    LOG(Debug, "running the fully eliminated c-style for as a no-op");
    cxt.publish_single_pipe_status(0);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  cxt.source_store().set_current_location(source_location());

  LOG(Debug,
      "entering the c-style for loop with init '%.*s', condition '%.*s', step "
      "'%.*s'",
      static_cast<int>(m_init.length), m_init.data,
      static_cast<int>(m_condition.length), m_condition.data,
      static_cast<int>(m_step.length), m_step.data);

  let const do_publish_implied_clause = [&]() throws -> bool {
    cxt.source_store().set_current_location(source_location());

    return publish_command_and_run_debug_trap(
        cxt, [&] { return arithmetic_clause_command_text(StringView{"1"}); });
  };

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  if (is_blank_clause(m_init)) {
    if (!do_publish_implied_clause())
      return {cxt.execution_store().last_exit_status()};
  } else {
    let const should_run_init = publish_command_and_run_debug_trap(cxt, [&] {
      return arithmetic_clause_command_text(
          clause_without_leading_blanks(m_init));
    });
    if (!should_run_init) return {cxt.execution_store().last_exit_status()};

    cxt.evaluate_arithmetic_nonzero(m_init, nullptr,
                                    arithmetic_text_kind::ShellSource);
  }

  let const is_condition_blank = is_blank_clause(m_condition);
  let const is_condition_folded =
      m_folded_condition.has_value() && should_skip_condition_commands;
  let const is_step_blank = is_blank_clause(m_step);

  let const do_evaluate_condition = [&]() throws -> bool {
    cxt.source_store().set_current_location(source_location());

    let const should_run_condition =
        publish_command_and_run_debug_trap(cxt, [&] {
          return arithmetic_clause_command_text(
              clause_without_leading_blanks(m_condition));
        });
    if (!should_run_condition) return false;

    let &cache = get_clause_cache(m_condition_cache);

    return cxt.evaluate_arithmetic_cached_clause_nonzero(
        m_condition, cache.tokens, cache.is_tokenized, cache.is_simple);
  };

  let const do_test_condition = [&]() throws -> bool {
    if (is_condition_blank) return do_publish_implied_clause();

    if (is_condition_folded) {
      return cxt.runtime_state().is_extended_arithmetic_enabled()
                 ? m_is_exact_folded_condition_nonzero
                 : *m_folded_condition != 0;
    }

    return do_evaluate_condition();
  };

  status_result result{};
  while (do_test_condition()) {
    result = m_body->evaluate_status(cxt);
    if (cxt.runtime_state().no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
    if (is_step_blank) {
      if (!do_publish_implied_clause()) break;
    } else {
      cxt.source_store().set_current_location(source_location());

      let const should_run_step = publish_command_and_run_debug_trap(cxt, [&] {
        return arithmetic_clause_command_text(
            clause_without_leading_blanks(m_step));
      });
      if (!should_run_step) break;

      let &cache = get_clause_cache(m_step_cache);
      cxt.evaluate_arithmetic_cached_clause_nonzero(
          m_step, cache.tokens, cache.is_tokenized, cache.is_simple);
    }
  }

  if (cxt.control_flow_store().has_pending()) {
    result.status = cxt.execution_store().last_exit_status();
    return result;
  }

  cxt.execution_store().set_last_exit_status(result.status);
  return result;
}

fn CStyleForLoop::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  optimizer::optimize_node(this, actx);

  let const is_conditional =
      !is_unconditional || actx.effects.has_seen_runtime_definer;
  let const init_position = m_header_position;
  let const condition_position = init_position + m_init.length + 1;
  let const step_position = condition_position + m_condition.length + 1;
  let const location = source_location();

  analyze_region_substitutions(actx, location, init_position,
                               m_init.length + m_condition.length +
                                   m_step.length + 2,
                               false, is_unconditional);

  if (!m_init.is_empty()) {
    check_arithmetic_expression_lints(actx, m_init, location, init_position,
                                      is_conditional);
    if (actx.is_posix_sh_shebang)
      check_posix_arithmetic_operators(actx, m_init, location);
  }

  if (!m_condition.is_empty()) {
    check_arithmetic_expression_lints(actx, m_condition, location,
                                      condition_position, is_conditional);
    if (actx.is_posix_sh_shebang)
      check_posix_arithmetic_operators(actx, m_condition, location);
  }

  let condition_occurrences = actx.occurrences.snapshot();

  actx.constant_variables.clear();
  actx.loop_body_depth++;
  actx.conditional_branch_depth++;
  m_body->analyze(actx, false);
  actx.conditional_branch_depth--;
  actx.loop_body_depth--;

  if (!m_step.is_empty()) {
    check_arithmetic_expression_lints(actx, m_step, location, step_position,
                                      is_conditional);
    if (actx.is_posix_sh_shebang)
      check_posix_arithmetic_operators(actx, m_step, location);
  }

  condition_occurrences.merge(actx.occurrences);
  actx.occurrences = steal(condition_occurrences);
}

pure fn CStyleForLoop::condition_clause() const wontthrow -> StringView
{
  return m_condition;
}

pure fn CStyleForLoop::init_clause() const wontthrow -> StringView
{
  return m_init;
}

fn CStyleForLoop::set_folded_condition(i64 compatibility_value,
                                       bool is_exact_nonzero) const wontthrow
    -> void
{
  m_folded_condition = compatibility_value;
  m_is_exact_folded_condition_nonzero = is_exact_nonzero;
}

pure fn CStyleForLoop::has_folded_condition() const wontthrow -> bool
{
  return m_folded_condition.has_value();
}

fn CStyleForLoop::as_cstyle_for_loop() const wontthrow -> const CStyleForLoop *
{
  return this;
}

Subshell::Subshell(SourceLocation location, const Expression *body)
    : CompoundCommand(steal(location)), m_body(body)
{}

Subshell::~Subshell() = default;

fn Subshell::as_subshell() const wontthrow -> const Subshell * { return this; }

fn Subshell::error_report_location() const wontthrow -> SourceLocation
{
  let const location = source_location();
  if (source_end_position() <= location.position) return location;

  return SourceLocation{source_end_position() - 1, 1,
                        location.source_name_index};
}

fn Subshell::collapsed_body() const wontthrow -> const Expression *
{
  const Expression *current = m_body;

  while (current != nullptr) {
    let const *list = current->as_compound_list();
    if (list == nullptr) break;

    let const *sole = list->single_unconditional_command();
    if (sole == nullptr) break;

    let const *inner = sole->as_subshell();
    if (inner == nullptr) {
      let const *wrapper = sole->as_redirected_command();
      if (wrapper == nullptr) break;

      let const *wrapped = wrapper->child();
      if (wrapped == nullptr || wrapped->as_subshell() == nullptr) break;

      return sole;
    }

    current = inner->m_body;
  }

  return current;
}

cold fn Subshell::to_string() const throws -> String
{
  let result = String{"Subshell"};
  append_ast_execution_flags(result);
  return result;
}

cold fn Subshell::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

static fn evaluate_subshell_in_process(const Expression *body, EvalContext &cxt,
                                       bool should_allow_terminal_exec) throws
    -> i64
{
  ASSERT(body != nullptr);

  let const saved_loop_depth = cxt.execution_store().loop_depth();
  cxt.execution_store().loop_depth() = 0;
  defer { cxt.execution_store().loop_depth() = saved_loop_depth; };

  let const action_scope =
      TrapActionScope::leave_for_subshell(cxt.trap_store());

  LOG(Debug, "entering the snapshot subshell");

  let snapshot = cxt.snapshot_state();
  let const subshell_mark = cxt.expansion_store().scratch_arena().mark();
  defer { cxt.expansion_store().scratch_arena().release(subshell_mark); };
  bool did_enter_subshell = false;
  i64 ret = 0;
  try {
    cxt.execution_store().terminal_exec_allowed() = false;
    cxt.enter_subshell();
    did_enter_subshell = true;
    cxt.hide_coprocess_descriptors();
    cxt.job_table_store().inherit_parent_jobs(false);
    cxt.clear_inherited_exit_trap();
    cxt.reset_inherited_signal_traps();
    if (should_allow_terminal_exec)
      cxt.execution_store().allow_terminal_exec_at_current_depth();
    let const do_confine_error =
        [&](ErrorBase &error, Maybe<SourceLocation> location) throws -> void {
      if (!error.is_script_fatal() && !error.is_line_discarding()) {
        unused(cxt.run_subshell_exit_trap());
        throw;
      }
      LOG(Debug, "the subshell confined a script-fatal error: %s",
          error.message().c_str());
      if (!error.was_rendered()) {
        show_message(
            error.to_string(cxt.source_store().current_source_view(), &cxt));
        if (location.has_value() && error.is_line_discarding()) {
          cxt.print_source_backtrace(*location);
        }
        error.set_rendered();
      }
      ret =
          cxt.runtime_state().is_bash_compatible() ? error.command_status() : 2;
      cxt.execution_store().set_last_exit_status(static_cast<i32>(ret));
      cxt.control_flow_store().clear();
    };
    try {
      ret = body->evaluate(cxt);
    } catch (ErrorWithLocation &error) {
      do_confine_error(error, error.location());
    } catch (ErrorBase &error) {
      do_confine_error(error, None);
    }

    if (cxt.control_flow_store().has_pending()) {
      let const kind = cxt.control_flow_store().pending().kind;
      if (kind == control_flow::Kind::Exit ||
          kind == control_flow::Kind::Return)
      {
        ret = cxt.control_flow_store().pending().value;
        cxt.control_flow_store().clear();
      } else if (kind == control_flow::Kind::Break ||
                 kind == control_flow::Kind::Continue)
      {
        cxt.control_flow_store().clear();
      }
    }

    if (let const requested_status = cxt.run_subshell_exit_trap();
        requested_status.has_value())
    {
      ret = *requested_status;
    }
  } catch (...) {
    if (did_enter_subshell) cxt.leave_subshell();
    cxt.restore_state(steal(snapshot));
    cxt.note_subshell_child_exit();
    throw;
  }
  cxt.leave_subshell();
  cxt.restore_state(steal(snapshot));
  cxt.note_subshell_child_exit();
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

fn Subshell::evaluate_impl(EvalContext &cxt, root_evaluation_mode) const throws
    -> status_result
{
  ASSERT(m_body != nullptr);

  cxt.job_table_store().forget_waited_jobs();
  if (!cxt.job_table_store().jobs().is_empty())
    cxt.job_table_store().update_jobs();
  cxt.release_finished_coprocess();

  let const pending_end_position = [&] {
    let const end_position =
        cxt.execution_store().pending_subshell_end_position();
    cxt.execution_store().pending_subshell_end_position() = 0;
    return end_position;
  }();
  let const is_terminal_in_forked_child =
      cxt.in_subshell() && cxt.can_replace_process();
  let const should_elide_fork = [&] {
    let const should_elide =
        cxt.execution_store().should_elide_pending_subshell_fork();
    cxt.execution_store().should_elide_pending_subshell_fork() = false;
    return should_elide || is_terminal_in_forked_child;
  }();
  let const end_position = pending_end_position != 0
                               ? static_cast<usize>(pending_end_position)
                               : source_end_position();

  let const closing_location = error_report_location();
  let const *body = collapsed_body();

  let const *redirected_body = body->as_redirected_command();
  let const should_elide_body_fork =
      redirected_body != nullptr && redirected_body->child() != nullptr &&
      redirected_body->child()->as_subshell() != nullptr;

  let const do_run_body = [&](bool should_allow_terminal_exec) throws -> i64 {
    if (should_elide_body_fork)
      cxt.execution_store().should_elide_pending_subshell_fork() = true;

    let frame = SubstitutionFrame{cxt};
    frame.push_source_frame(source_location(), "a subshell");

    try {
      return evaluate_subshell_in_process(body, cxt,
                                          should_allow_terminal_exec);
    } catch (ErrorWithLocation &error) {
      if (error.is_script_fatal() || error.was_rendered()) throw;

      let const trace_location = error.location();
      show_message(
          error.to_string(cxt.source_store().current_source_view(), &cxt));
      cxt.print_source_backtrace(trace_location);
      error.set_rendered();
      throw;
    }
  };

  let const do_publish_subshell = [&]() throws -> void {
    cxt.source_store().set_current_location(closing_location);

    if (!command_text_is_observed(cxt)) return;

    let text =
        internal::subshell_command_text(cxt, source_location(), end_position);
    if (!text.is_empty())
      cxt.execution_store().set_current_command(steal(text));
  };

  if (!should_elide_fork) cxt.prepare_child_environment();

  koshka::flush();
  let const forked_child =
      should_elide_fork
          ? Maybe<os::process>{None}
          : os::try_fork_compound_stage(os::fork_compound_stage_options{});
  if (!forked_child.has_value()) {
    i32 status = 1;
    try {
      status = static_cast<i32>(do_run_body(is_terminal_in_forked_child));
    } catch (const ErrorBase &error) {
      if (!error.was_rendered()) {
        show_message(
            error.to_string(cxt.source_store().current_source_view(), &cxt));
      }
      if (cxt.runtime_state().is_posix_mode())
        status = static_cast<i32>(error.command_status());
    }

    do_publish_subshell();
    cxt.publish_single_pipe_status(status);
    SET_AND_RETURN_EXIT_STATUS(cxt, status);
  }

  let const child = *forked_child;
  if (os::process_id_of(child) == 0) {
    i32 status = 1;
    try {
      status = static_cast<i32>(do_run_body(true));
    } catch (const ErrorBase &error) {
      if (!error.was_rendered()) {
        show_message(
            error.to_string(cxt.source_store().current_source_view(), &cxt));
      }
      if (cxt.runtime_state().is_posix_mode())
        status = static_cast<i32>(error.command_status());
    } catch (...) {
      LOG(Debug, "the subshell child swallowed an unknown error");
    }
    koshka::flush();
    os::exit_process_immediately(status);
  }

  let was_stopped = false;
  let const status = os::wait_and_monitor_process(child, &was_stopped);
  unused(was_stopped);

  do_publish_subshell();
  cxt.publish_single_pipe_status(status);
  SET_AND_RETURN_EXIT_STATUS(cxt, status);
}

cold static fn
subshell_body_is_conditional_expression(StringView body) wontthrow -> bool
{
  if (body.length < 5) return false;

  let const closer = body.substring_of_length(body.length - 2, 2);
  if (body.starts_with(StringView{"[[ "})) return closer == StringView{"]]"};
  if (body.starts_with(StringView{"(( "})) return closer == StringView{"))"};

  return false;
}

cold static fn subshell_body_is_bracket_test(const Expression *body) throws
    -> bool
{
  let const compound_list = body->as_compound_list();
  if (compound_list == nullptr) return false;

  return compound_list->has_single_test_command();
}

fn Subshell::analyze(AnalysisContext &actx, bool is_unconditional) const throws
    -> void
{
  ASSERT(m_body != nullptr);

  let const was_analyzing_condition = actx.walk.is_analyzing_condition;
  actx.walk.is_analyzing_condition = false;

  let const end_position = source_end_position();
  if (end_position > source_location().position + 1 &&
      end_position <= actx.source.length)
  {
    let body = actx.source.substring_of_length(
        source_location().position + 1,
        end_position - source_location().position - 2);
    while (!body.is_empty() &&
           (body[0] == ' ' || body[0] == '\t' || body[0] == '\n'))
      body = body.substring(1);
    while (!body.is_empty() &&
           (body[body.length - 1] == ' ' || body[body.length - 1] == '\t' ||
            body[body.length - 1] == '\n'))
      body = body.substring_of_length(0, body.length - 1);

    let is_unary_test_prefix = false;
    if (body.length >= 3 && body[0] == '-') {
      switch (body[1]) {
      case 'd':
        is_unary_test_prefix = body.starts_with(StringView{"-d "});
        break;

      case 'e':
        is_unary_test_prefix = body.starts_with(StringView{"-e "});
        break;

      case 'f':
        is_unary_test_prefix = body.starts_with(StringView{"-f "});
        break;

      case 'n':
        is_unary_test_prefix = body.starts_with(StringView{"-n "});
        break;

      case 'z':
        is_unary_test_prefix = body.starts_with(StringView{"-z "});
        break;

      default: break;
      }
    }

    if (is_unary_test_prefix) {
      actx.report_diagnostic(was_analyzing_condition ? diagnostic_id::sc2205
                                                     : diagnostic_id::sc2204,
                             source_location());
    } else if (was_analyzing_condition) {
      if (subshell_body_is_conditional_expression(body)) {
        actx.report_diagnostic(diagnostic_id::sc2233, source_location());
      } else if (subshell_body_is_bracket_test(m_body)) {
        actx.report_diagnostic(diagnostic_id::sc2234, source_location());
      }
    }
  }

  {
    let scope = AnalysisScopeGuard{actx, analysis_scope_mode::Subshell};
    actx.apply_scope_definitions(m_analysis_scope_definitions);
    m_body->analyze(actx, is_unconditional);
  }

  actx.walk.is_analyzing_condition = was_analyzing_condition;
}

FunctionDefinition::FunctionDefinition(SourceLocation location, StringView name,
                                       FunctionBodyHandle body)
    : CompoundCommand(steal(location)), m_name(name),
      m_body_storage(steal(body)), m_body(m_body_storage.get_body())
{}

FunctionDefinition::~FunctionDefinition() = default;

pure fn FunctionDefinition::name() const wontthrow -> const String &
{
  return m_name;
}

pure fn FunctionDefinition::body() const wontthrow -> const Expression *
{
  return m_body;
}

cold fn FunctionDefinition::to_string() const throws -> String
{
  let result = String{"FunctionDefinition \""};
  result += StringView{m_name};
  result += "\"";
  append_ast_execution_flags(result);
  return result;
}

cold fn FunctionDefinition::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);

  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_body->to_ast_string(layer + 1);
}

fn FunctionDefinition::evaluate_impl(EvalContext &cxt, root_evaluation_mode)
    const throws -> status_result
{
  ASSERT(m_body != nullptr);

  let body_end_position = m_body->source_end_position();
  if (const RedirectedCommand *redirected = m_body->as_redirected_command();
      redirected != nullptr)
  {
    for (let const &redirection : redirected->redirections()) {
      if (redirection.heredoc == nullptr) continue;
      if (redirection.heredoc->source_end_position > body_end_position)
        body_end_position = redirection.heredoc->source_end_position;
    }
  }

  let definition_text = String{cxt.scratch_allocator()};
  if (const String *source = cxt.source_store().current_source();
      source != nullptr &&
      body_end_position > m_body->source_location().position &&
      body_end_position <= source->count())
  {
    definition_text.append(m_name.view());
    definition_text.append(StringView{" () \n"});
    definition_text.append(source->view().substring_of_length(
        m_body->source_location().position,
        body_end_position - m_body->source_location().position));
  }
  LOG(Info, "registering the function '%s'%s", m_name.c_str(),
      definition_text.is_empty() ? " without recorded definition text" : "");
  cxt.register_function(m_name, m_body_storage, definition_text.view(),
                        m_body->source_location().position, source_location());
  cxt.publish_single_pipe_status(0);
  SET_AND_RETURN_EXIT_STATUS(cxt, 0);
}

fn FunctionDefinition::analyze(AnalysisContext &actx,
                               bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  unused(is_unconditional);
  actx.add_defined_function(m_name);

  let const function_definition_index = actx.functions.records.count();
  {
    let scope = AnalysisScopeGuard{actx, analysis_scope_mode::Function};
    actx.apply_scope_definitions(m_analysis_scope_definitions);
    actx.active_function_definition_index = function_definition_index;
    actx.functions.records.push(function_definition_record{
        String{heap_allocator(), m_name.view()},
        0, 0,
        HashSet{heap_allocator(), SMALL_MAP_FIRST_CAPACITY},
        HashSet{heap_allocator(), SMALL_MAP_FIRST_CAPACITY},
        VariableOccurrenceStateMap{},
        String{heap_allocator()},
        Maybe<usize>{},
        0, 0, source_location(), SourceLocation{},
        SourceLocation{},
        false,
        false
    });
    actx.functions.records[function_definition_index].occurrence_start =
        actx.symbol_records != nullptr
            ? actx.symbol_records->variable_occurrences.count()
            : 0;
    actx.note_function_body_record(m_name.view(), source_location().position,
                                   m_body->source_location().position,
                                   m_body->source_end_position());
    m_body->analyze(actx, false);
    if (m_body->always_exits(actx))
      actx.always_exiting_function_names.add(m_name.view());
    else
      actx.always_exiting_function_names.remove(m_name.view());

    let &function_definition =
        actx.functions.records[function_definition_index];
    if (function_definition.recursive_call_count > 0) {
      let const diagnostic =
          function_definition.recursive_call_count >= 2 &&
                  function_definition.async_recursive_call_count >= 2
              ? diagnostic_id::fork_bomb
              : diagnostic_id::sc2264;
      actx.report_diagnostic(diagnostic,
                             function_definition.first_recursive_call_location,
                             {m_name.view()}, source_location());
    }
    function_definition.occurrence_end =
        actx.symbol_records != nullptr
            ? actx.symbol_records->variable_occurrences.count()
            : 0;
    function_definition.exit_states = actx.occurrences.assigned.snapshot();
    function_definition.is_analysis_complete = true;
    let const previous_definition_index =
        actx.functions.latest_indices.find(m_name.view());
    if (previous_definition_index.has_value()) {
      function_definition.previous_definition_index =
          *previous_definition_index.value();
    }
  }

  actx.functions.latest_indices.set(m_name.view(), function_definition_index);
}

RedirectedCommand::RedirectedCommand(SourceLocation location,
                                     const Command *child,
                                     ArrayList<Redirection> &&redirections)
    : Command(steal(location)), m_child(child)
{
  m_redirections.fill(steal(redirections));
}

RedirectedCommand::~RedirectedCommand() = default;

cold fn RedirectedCommand::to_string() const throws -> String
{
  let result = String{"RedirectedCommand"};
  append_ast_execution_flags(result);
  return result;
}

cold fn RedirectedCommand::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_child != nullptr);

  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + "]\n" + pad + EXPRESSION_AST_INDENT +
         m_child->to_ast_string(layer + 1);
}

fn RedirectedCommand::analyze(AnalysisContext &actx,
                              bool is_unconditional) const throws -> void
{
  ASSERT(m_child != nullptr);

  for (let const &redirection : m_redirections) {
    analyze_redirection_substitutions(actx, redirection, source_location(),
                                      is_unconditional);
  }
  m_child->analyze(actx, is_unconditional);

  if (m_redirections.is_empty()) return;

  let const no_args = ArrayList<const Token *>{heap_allocator()};
  let const no_prefix_assignments = SparseList<PrefixAssignment>{};
  let const lint_input = command_lint_input{no_args,
                                            m_redirections,
                                            no_prefix_assignments,
                                            source_location(),
                                            StringView{},
                                            analysis_command_info::unknown(),
                                            false,
                                            !is_unconditional};

  check_redirection_lints(actx, lint_input);
}

fn RedirectedCommand::as_redirected_command() const wontthrow
    -> const RedirectedCommand *
{
  return this;
}

fn RedirectedCommand::child() const wontthrow -> const Command *
{
  return m_child;
}

fn RedirectedCommand::redirections() const wontthrow
    -> const SparseList<Redirection> &
{
  return m_redirections;
}

fn RedirectedCommand::evaluate_impl(EvalContext &cxt, root_evaluation_mode)
    const throws -> status_result
{
  if (is_async()) return {static_cast<i32>(evaluate_async(cxt)), 0};

  try {
    return evaluate_redirected(cxt);
  } catch (const TrapAbandonedRedirection &) {
    return {cxt.execution_store().last_exit_status(), 0};
  }
}

fn RedirectedCommand::evaluate_async_body(EvalContext &cxt) const throws -> i64
{
  try {
    return evaluate_redirected(cxt).status;
  } catch (const TrapAbandonedRedirection &) {
    return cxt.execution_store().last_exit_status();
  }
}

fn RedirectedCommand::evaluate_redirected(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_child != nullptr);

  LOG(Debug, "applying %zu redirections around the compound command",
      m_redirections.count());

  let const should_elide_child_fork = [&] {
    let const should_elide =
        cxt.execution_store().should_elide_pending_subshell_fork();
    cxt.execution_store().should_elide_pending_subshell_fork() = false;
    return should_elide;
  }();

  cxt.source_store().set_current_location(source_location());

  let const own_redirection_substitution_mark =
      cxt.mark_process_substitutions();
  defer
  {
    cxt.cleanup_process_substitutions(own_redirection_substitution_mark);
  };

  cxt.execution_store().terminal_exec_allowed() = false;

  ArrayList<os::saved_descriptor> saved_descriptors{cxt.scratch_allocator()};
  defer
  {
    koshka::flush();
    for (usize i = saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(saved_descriptors[i - 1]);
  };

  koshka::flush();

  Maybe<eval_state_snapshot> redirection_snapshot;
  if (m_child->as_subshell() != nullptr &&
      cxt.runtime_state().is_bash_compatible() &&
      redirections_can_change_state(m_redirections))
  {
    redirection_snapshot = snapshot_child_redirection_state(cxt);
  }
  defer { discard_child_redirection_state(cxt, redirection_snapshot); };

  for (let const &redir : m_redirections) {
    let r = resolve_redirection(redir, cxt, source_location());
    r.target_fd =
        allocate_redirection_descriptor(redir, r, cxt, source_location());
    switch (r.kind) {
    case redirection_outcome::Heredoc:
    case redirection_outcome::OpenedFile: {
      if (os::descriptor_is_shell_fd(r.opened_fd, r.target_fd)) {
        saved_descriptors.push(
            os::saved_descriptor{.shell_fd = r.target_fd, .was_open = false});
        break;
      }
      saved_descriptors.push(
          os::save_and_replace_descriptor(r.target_fd, r.opened_fd));
      os::close_fd(r.opened_fd);
      break;
    }
    case redirection_outcome::BothStreams: {
      const os::saved_descriptor saved_out =
          os::save_and_replace_descriptor(1, r.opened_fd);
      saved_descriptors.push(saved_out);
      const os::saved_descriptor saved_err =
          os::save_and_replace_descriptor(2, r.opened_fd);
      saved_descriptors.push(saved_err);
      os::close_fd(r.opened_fd);
      if (!saved_out.is_dup2_ok || !saved_err.is_dup2_ok) {
        throw ErrorWithLocation{redir.target->source_location(),
                                "Bad file descriptor"};
      }
      break;
    }
    case redirection_outcome::Duplicate: {
      if (r.dup_from_fd == Redirection::DUP_FD_CLOSE) {
        saved_descriptors.push(os::save_and_replace_descriptor(
            r.target_fd, os::descriptor_for_shell_fd(r.target_fd)));
        os::close_fd(os::descriptor_for_shell_fd(r.target_fd));
        break;
      }

      let const source = os::descriptor_for_shell_fd(r.dup_from_fd);
      const os::saved_descriptor saved =
          os::save_and_replace_descriptor(r.target_fd, source);
      saved_descriptors.push(saved);
      if (!saved.is_dup2_ok) {
        let const location = redir.target != nullptr
                                 ? redir.target->source_location()
                                 : source_location();
        throw ErrorWithLocation{location,
                                String::from(r.dup_from_fd, heap_allocator()) +
                                    ": Bad file descriptor"};
      }
      break;
    }
    }
  }

  discard_child_redirection_state(cxt, redirection_snapshot);

  if (m_child->as_subshell() != nullptr) {
    cxt.execution_store().pending_subshell_end_position() =
        static_cast<u32>(source_end_position());
    if (should_elide_child_fork)
      cxt.execution_store().should_elide_pending_subshell_fork() = true;
  }

  try {
    return m_child->evaluate_status(cxt);
  } catch (ErrorWithLocationAndDetails &error) {
    if (!error.was_rendered()) {
      let const source_text = cxt.source_store().current_source_view();
      show_message(error.to_string(source_text, &cxt));
      show_message(error.details_to_string(source_text, &cxt));
      error.set_rendered();
    }
    throw;
  } catch (ErrorWithLocation &error) {
    if (!error.was_rendered()) {
      show_message(
          error.to_string(cxt.source_store().current_source_view(), &cxt));
      error.set_rendered();
    }
    throw;
  }
}

} /* namespace expressions */

} /* namespace koshka */
