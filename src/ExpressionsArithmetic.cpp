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

#include "Arena.hpp"
#include "Builtin.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
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
#include "Trace.hpp"
#include "Utils.hpp"

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

/* The operator an element spells when it is one, so a < or a > reaches the same
   comparison as a worded operator. */
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

/* The left operand of an X != Y triple whose operator sits at operator_index,
   absent when the elements there do not form one. The raw view is compared, so
   "$name" and $name stay distinct. */
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

/* The path tests whose glob operand is already reported as SC2144, so the
   general glob operand lint leaves the word after them alone. */
constexpr PackedStringKey CONDITIONAL_PATH_TEST_KEYS[] = {
    SSK("-L"),
    SSK("-d"),
    SSK("-e"),
    SSK("-f"),
};
constexpr StaticStringSet CONDITIONAL_PATH_TESTS{CONDITIONAL_PATH_TEST_KEYS};

/* The equality operators, whose right side is matched as a glob pattern while
   the regex operator takes a regular expression. */
constexpr PackedStringKey CONDITIONAL_EQUALITY_OPERATOR_KEYS[] = {
    SSK("!="),
    SSK("="),
    SSK("=="),
};
constexpr StaticStringSet CONDITIONAL_EQUALITY_OPERATORS{
    CONDITIONAL_EQUALITY_OPERATOR_KEYS};

/* Every operator that matches its right side against a pattern, where a glob
   there is the point of the comparison. */
cold static fn is_conditional_pattern_operator(StringView op) wontthrow -> bool
{
  return op == StringView{"=~"} || CONDITIONAL_EQUALITY_OPERATORS.contains(op);
}

/* A -a or a -o joins two parts only when a finished operand precedes it, so the
   unary file and option tests keep their leading position. */
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
  unused(is_unconditional);

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

    /* Two inequalities on the same operand hold for every value, shellcheck
       SC2055. */
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

    /* A unary operator followed by a binary operator lost its operand,
       shellcheck SC1019. */
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

    /* The word literal drops the quotes and the escapes, so the source text
       decides whether the operand was written as syntax or as data. */
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

    /* A double bracket expression takes its operand as one word and never
       expands it, so an array, a brace expansion, or a glob written there does
       not reach the values the author meant. The shape is read once from the
       segments the word already holds. */
    if (element.word->kind() == Token::Kind::Word) {
      let const &operand_word =
          static_cast<const tokens::WordToken *>(element.word)->word();
      let const shape = classify_test_operand(operand_word);

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
    /* A conditional prefers = over -eq for text, so an -eq or -ne against a
       non-integer literal is reported as SC2130 here. */
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

    /* The right side of an equality comparison is a glob pattern, so an
       unquoted variable there matches instead of comparing, shellcheck
       SC2053. */
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

fn ConditionalCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  cxt.set_current_location(source_location());
  i64 status;
  try {
    status = cxt.evaluate_conditional(m_elements) ? 0 : 1;
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (const Error &e) {
    SourceLocation span = source_location();
    if (source_end_position() > span.position)
      span.length = static_cast<u32>(source_end_position() - span.position);
    relocate_error(e, span);
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

fn ArithmeticCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  LOG(Debug, "evaluating the arithmetic command '%.*s'",
      static_cast<int>(m_expression.length), m_expression.data);

  cxt.set_current_location(source_location());

  if (is_blank_clause(m_expression)) {
    cxt.publish_single_pipe_status(1);
    SET_AND_RETURN_EXIT_STATUS(cxt, 1);
  }

  /* A non-zero value is success and zero is failure, the opposite of the
     value-to-status convention elsewhere. */
  bool is_nonzero;
  try {
    const SourceLocation body_base{source_location().position + 2, 0,
                                   source_location().source_name_index};
    is_nonzero = cxt.evaluate_arithmetic_nonzero(m_expression, &body_base);
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (const Error &e) {
    relocate_error(e, source_location());
  }
  const i64 status = is_nonzero ? 0 : 1;
  cxt.publish_single_pipe_status(static_cast<i32>(status));
  SET_AND_RETURN_EXIT_STATUS(cxt, status);
}

fn ArithmeticCommand::analyze(AnalysisContext &actx,
                              bool is_unconditional) const throws -> void
{
  if (arithmetic_reads_external_input(actx, m_expression))
    actx.report_diagnostic(diagnostic_id::external_arithmetic_input,
                           source_location());

  check_arithmetic_expression_lints(
      actx, m_expression, source_location(), source_location().position + 2,
      !is_unconditional || actx.has_seen_runtime_definer);

  if (actx.is_posix_sh_shebang) {
    actx.report_diagnostic(diagnostic_id::sc3006, source_location());
    check_posix_arithmetic_operators(actx, m_expression, source_location());
  }

  /* The prepass does not parse the expression, which may assign any name, so
     every recorded constant is forgotten. */
  actx.constant_variables.clear();
}

fn SelectLoop::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  let loop_entry_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let loop_entry_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();

  for (let const t : m_words) {
    if (t->kind() != Token::Kind::Word) continue;

    let const &word = static_cast<const tokens::WordToken *>(t)->word();
    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;

      note_variable_reference(actx, segment, t->source_location());
    }
  }

  let const is_conditional = !is_unconditional ||
                             actx.has_seen_runtime_definer ||
                             (m_has_in_clause && m_words.is_empty());
  actx.note_variable_occurrence(m_variable_name, m_variable_location,
                                variable_occurrence_kind::Assignment,
                                is_conditional);
  actx.note_variable_binding_record(m_variable_name, m_variable_location,
                                    assignment_binder::SelectLoop,
                                    is_conditional);

  actx.constant_variables.clear();
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;

  merge_variable_occurrence_states(loop_entry_occurrence_assignments,
                                   actx.variable_occurrence_assignments);
  merge_variable_occurrence_states(
      loop_entry_inherited_occurrence_assignments,
      actx.inherited_variable_occurrence_assignments);
  actx.variable_occurrence_assignments =
      steal(loop_entry_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(loop_entry_inherited_occurrence_assignments);
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

fn CStyleForLoop::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  return evaluate_status_impl(cxt).status;
}

fn CStyleForLoop::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_body != nullptr);

  cxt.set_terminal_exec_allowed(false);

  let const can_skip_condition_commands =
      !cxt.has_debug_trap() && !cxt.should_echo_expanded();
  if (is_fully_eliminated() && can_skip_condition_commands) {
    LOG(Debug, "running the fully eliminated c-style for as a no-op");
    cxt.publish_single_pipe_status(0);
    return {static_cast<i32>(set_and_return_exit_status(cxt, 0)), 0};
  }

  cxt.set_current_location(source_location());

  LOG(Debug,
      "entering the c-style for loop with init '%.*s', condition '%.*s', step "
      "'%.*s'",
      static_cast<int>(m_init.length), m_init.data,
      static_cast<int>(m_condition.length), m_condition.data,
      static_cast<int>(m_step.length), m_step.data);

  if (!is_blank_clause(m_init)) cxt.evaluate_arithmetic_nonzero(m_init);

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const condition_is_blank = is_blank_clause(m_condition);
  let const step_is_blank = is_blank_clause(m_step);

  let const do_evaluate_condition = [&]() throws -> bool {
    let &cache = get_clause_cache(m_condition_cache);

    return cxt.evaluate_arithmetic_cached_clause_nonzero(
        m_condition, cache.tokens, cache.is_tokenized, cache.is_simple);
  };

  status_result result{};
  /* An empty condition is always true, the way for ((;;)) loops forever. */
  while (condition_is_blank ||
         (m_folded_condition.has_value() && can_skip_condition_commands
              ? (cxt.is_extended_arithmetic_enabled()
                     ? m_is_exact_folded_condition_nonzero
                     : *m_folded_condition != 0)
              : do_evaluate_condition()))
  {
    result = m_body->evaluate_status(cxt);
    if (cxt.no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
    /* The step runs after the body on every iteration, including one ended by a
       continue. */
    if (!step_is_blank) {
      let &cache = get_clause_cache(m_step_cache);
      cxt.evaluate_arithmetic_cached_clause_nonzero(
          m_step, cache.tokens, cache.is_tokenized, cache.is_simple);
    }
  }
  cxt.set_last_exit_status(result.status);
  return result;
}

fn CStyleForLoop::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);

  optimizer::optimize_node(this, actx);

  let const is_conditional = !is_unconditional || actx.has_seen_runtime_definer;
  let const init_position = m_header_position;
  let const condition_position = init_position + m_init.length + 1;
  let const step_position = condition_position + m_condition.length + 1;
  let const location = source_location();

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

  let condition_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let condition_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();

  actx.constant_variables.clear();
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;

  if (!m_step.is_empty()) {
    check_arithmetic_expression_lints(actx, m_step, location, step_position,
                                      is_conditional);
    if (actx.is_posix_sh_shebang)
      check_posix_arithmetic_operators(actx, m_step, location);
  }

  merge_variable_occurrence_states(condition_occurrence_assignments,
                                   actx.variable_occurrence_assignments);
  merge_variable_occurrence_states(
      condition_inherited_occurrence_assignments,
      actx.inherited_variable_occurrence_assignments);
  actx.variable_occurrence_assignments =
      steal(condition_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(condition_inherited_occurrence_assignments);
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

static fn evaluate_subshell_in_process(const Expression *body,
                                       EvalContext &cxt) throws -> i64
{
  ASSERT(body != nullptr);

  /* This shell has no process-level subshell, so isolation is by snapshot. A
     loop in the parent is not the subshell's to break, so the body runs with a
     fresh loop count. */
  let const saved_loop_depth = cxt.loop_depth();
  cxt.set_loop_depth(0);
  defer { cxt.set_loop_depth(saved_loop_depth); };

  LOG(Debug, "entering the snapshot subshell");

  let snapshot = cxt.snapshot_state();
  let const subshell_mark = cxt.scratch_mark();
  defer { cxt.scratch_release(subshell_mark); };
  bool did_enter_subshell = false;
  i64 ret = 0;
  try {
    cxt.set_terminal_exec_allowed(false);
    cxt.enter_subshell();
    did_enter_subshell = true;
    /* The inherited EXIT action belongs to the parent and must not fire at the
       subshell's end. An EXIT action the body sets survives this clear. */
    cxt.clear_inherited_exit_trap();
    try {
      ret = body->evaluate(cxt);
    } catch (const ErrorBase &error) {
      /* A script-fatal error is confined to the subshell in every mood, status
         1 the way bash answers it and 2 the way dash does. */
      if (!error.is_script_fatal()) {
        cxt.run_subshell_exit_trap();
        throw;
      }
      LOG(Debug, "the subshell confined a script-fatal error: %s",
          error.message().c_str());
      const String *source = cxt.current_source();
      show_message(error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      ret = cxt.is_bash_compatible() ? 1 : 2;
      cxt.set_last_exit_status(static_cast<i32>(ret));
      cxt.clear_control_flow();
    }

    /* Exit and return end only the subshell. A break or continue is scoped to a
       loop inside it and is consumed here. */
    if (cxt.has_pending_control_flow()) {
      let const kind = cxt.pending_control_flow().kind;
      if (kind == control_flow::Kind::Exit ||
          kind == control_flow::Kind::Return)
      {
        ret = cxt.pending_control_flow().value;
        cxt.clear_control_flow();
      } else if (kind == control_flow::Kind::Break ||
                 kind == control_flow::Kind::Continue)
      {
        cxt.clear_control_flow();
      }
    }

    cxt.run_subshell_exit_trap();
  } catch (...) {
    if (did_enter_subshell) cxt.leave_subshell();
    cxt.restore_state(steal(snapshot));
    throw;
  }
  cxt.leave_subshell();
  cxt.restore_state(steal(snapshot));
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

fn Subshell::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  koshka::flush();
  let const forked_child = os::try_fork_compound_stage(None, None, None);
  if (!forked_child.has_value()) {
    i32 status = 1;
    try {
      status = static_cast<i32>(evaluate_subshell_in_process(m_body, cxt));
    } catch (const ErrorBase &error) {
      let const source = cxt.current_source();
      show_message(error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      if (cxt.is_posix_mode())
        status = static_cast<i32>(error.command_status());
    }
    cxt.publish_single_pipe_status(status);
    SET_AND_RETURN_EXIT_STATUS(cxt, status);
  }

  let const child = *forked_child;
  if (os::process_id_of(child) == 0) {
    i32 status = 1;
    try {
      status = static_cast<i32>(evaluate_subshell_in_process(m_body, cxt));
    } catch (const ErrorBase &error) {
      let const source = cxt.current_source();
      show_message(error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      if (cxt.is_posix_mode())
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

  let const was_analyzing_condition = actx.is_analyzing_condition;
  actx.is_analyzing_condition = false;

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

    if (body.starts_with(StringView{"-e "}) ||
        body.starts_with(StringView{"-f "}) ||
        body.starts_with(StringView{"-d "}) ||
        body.starts_with(StringView{"-n "}) ||
        body.starts_with(StringView{"-z "}))
    {
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

  /* An assignment in the body never changes a parent variable, so the body
     starts from an empty table and the outer constants are restored after. */
  let saved_constants = steal(actx.constant_variables);
  actx.constant_variables = StringMap<String>{heap_allocator()};
  let saved_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let saved_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();
  let saved_latest_function_definition_indices =
      actx.latest_function_definition_indices.clone();
  let const defined_function_insertion_count =
      actx.defined_function_insertions.count();
  let const known_alias_insertion_count = actx.known_alias_insertions.count();
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
  actx.current_source_effects = nullptr;
  let const was_inside_subshell_analysis = actx.is_inside_subshell_analysis;
  actx.is_inside_subshell_analysis = true;
  actx.apply_scope_definitions(m_analysis_scope_definitions);
  m_body->analyze(actx, is_unconditional);
  actx.current_source_effects = saved_source_effects;
  actx.is_inside_subshell_analysis = was_inside_subshell_analysis;
  actx.has_unknown_working_directory = saved_has_unknown_working_directory;
  actx.has_unknown_path = saved_has_unknown_path;
  actx.has_seen_runtime_definer = saved_has_seen_runtime_definer;
  actx.should_silence_unresolved_commands =
      saved_should_silence_unresolved_commands;
  actx.array_valued_names = steal(saved_array_valued_names);
  actx.inherited_global_assigned_names =
      steal(saved_inherited_global_assigned_names);
  actx.inherited_assigned_names = steal(saved_inherited_assigned_names);
  actx.constant_variables = steal(saved_constants);
  actx.variable_occurrence_assignments = steal(saved_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(saved_inherited_occurrence_assignments);
  actx.latest_function_definition_indices =
      steal(saved_latest_function_definition_indices);
  actx.rollback_defined_functions(defined_function_insertion_count);
  actx.rollback_known_aliases(known_alias_insertion_count);
  actx.is_analyzing_condition = was_analyzing_condition;
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

fn FunctionDefinition::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  /* The recorded definition is a "name () " line then the body's source span,
     the shape bash prints from declare -f. ble.sh clones a function by
     replacing the leading name and greps the "name ()" line, so both matter. */
  let definition_text = String{cxt.scratch_allocator()};
  if (const String *source = cxt.current_source();
      source != nullptr &&
      m_body->source_end_position() > m_body->source_location().position &&
      m_body->source_end_position() <= source->count())
  {
    definition_text.append(m_name.view());
    definition_text.append(StringView{" () \n"});
    definition_text.append(source->view().substring_of_length(
        m_body->source_location().position,
        m_body->source_end_position() - m_body->source_location().position));
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

  let body_source = analysis_source_span(actx, *m_body).trim_blanks();
  if (!body_source.is_empty() &&
      (body_source[0] == '{' || body_source[0] == '('))
  {
    body_source = body_source.substring(1).trim_blanks();
  }
  if (body_source.starts_with(m_name.view()) &&
      body_source.length > m_name.count() &&
      (body_source[m_name.count()] == ' ' ||
       body_source[m_name.count()] == '\t' ||
       body_source[m_name.count()] == '\n' ||
       body_source[m_name.count()] == ';'))
  {
    let const call_location = SourceLocation{
        static_cast<usize>(body_source.data - actx.source.data), m_name.count(),
        m_body->source_location().source_name_index};
    actx.report_diagnostic(diagnostic_id::sc2264, call_location,
                           {m_name.view()}, source_location());
  }

  /* The body runs later when the function is called, so it is analyzed from an
     empty constant table with the outer constants restored after. A called
     function edits the caller's own shell, and its search path, working
     directory, and runtime-definer effects outlive the body. */
  let saved_constants = steal(actx.constant_variables);
  actx.constant_variables = StringMap<String>{heap_allocator()};
  let saved_occurrence_assignments =
      actx.variable_occurrence_assignments.snapshot();
  let saved_inherited_occurrence_assignments =
      actx.inherited_variable_occurrence_assignments.snapshot();
  let saved_latest_function_definition_indices =
      actx.latest_function_definition_indices.clone();
  let const defined_function_insertion_count =
      actx.defined_function_insertions.count();
  let const known_alias_insertion_count = actx.known_alias_insertions.count();
  let saved_inherited_assigned_names = actx.inherited_assigned_names.clone();
  let saved_inherited_global_assigned_names =
      actx.inherited_global_assigned_names.clone();
  let saved_array_valued_names = actx.array_valued_names.clone();
  let *saved_source_effects = actx.current_source_effects;
  actx.current_source_effects = nullptr;
  let saved_locals = steal(actx.function_local_names);
  actx.function_local_names = StringMap<SourceLocation>{heap_allocator()};
  actx.inherited_variable_occurrence_assignments = VariableOccurrenceStateMap{};
  actx.variable_occurrence_assignments = VariableOccurrenceStateMap{};
  actx.apply_scope_definitions(m_analysis_scope_definitions);
  let const saved_loop_body_depth = actx.loop_body_depth;
  actx.loop_body_depth = 0;
  let const saved_active_function = actx.active_function_definition_index;
  actx.active_function_definition_index = actx.function_definitions.count();
  actx.function_definitions.push(function_definition_record{
      String{heap_allocator(), m_name.view()},
      source_location(), 0, 0,
      HashSet{heap_allocator()},
      HashSet{heap_allocator()},
      VariableOccurrenceStateMap{},
      String{heap_allocator()},
      SourceLocation{},
      false, false
  });
  let const function_definition_index = actx.active_function_definition_index;
  actx.function_definitions[function_definition_index].occurrence_start =
      actx.symbol_records != nullptr
          ? actx.symbol_records->variable_occurrences.count()
          : 0;
  actx.note_function_body_record(m_name.view(), source_location().position,
                                 m_body->source_location().position,
                                 m_body->source_end_position());
  actx.function_scope_depth++;
  m_body->analyze(actx, false);
  let &function_definition =
      actx.function_definitions[function_definition_index];
  function_definition.occurrence_end =
      actx.symbol_records != nullptr
          ? actx.symbol_records->variable_occurrences.count()
          : 0;
  function_definition.exit_states =
      actx.variable_occurrence_assignments.snapshot();
  function_definition.is_analysis_complete = true;
  actx.latest_function_definition_indices.set(m_name.view(),
                                              function_definition_index);
  actx.current_source_effects = saved_source_effects;
  actx.function_scope_depth--;
  actx.active_function_definition_index = saved_active_function;
  actx.loop_body_depth = saved_loop_body_depth;
  actx.array_valued_names = steal(saved_array_valued_names);
  actx.inherited_global_assigned_names =
      steal(saved_inherited_global_assigned_names);
  actx.inherited_assigned_names = steal(saved_inherited_assigned_names);
  actx.function_local_names = steal(saved_locals);
  actx.constant_variables = steal(saved_constants);
  actx.variable_occurrence_assignments = steal(saved_occurrence_assignments);
  actx.inherited_variable_occurrence_assignments =
      steal(saved_inherited_occurrence_assignments);
  actx.latest_function_definition_indices =
      steal(saved_latest_function_definition_indices);
  actx.latest_function_definition_indices.set(m_name.view(),
                                              function_definition_index);
  actx.rollback_defined_functions(defined_function_insertion_count);
  actx.rollback_known_aliases(known_alias_insertion_count);
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

  m_child->analyze(actx, is_unconditional);

  if (m_redirections.is_empty()) return;

  /* The lint input borrows its lists. This node has no command word and no
     prefix assignment, and an empty list allocates nothing. */
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

fn RedirectedCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  return evaluate_status_impl(cxt).status;
}

fn RedirectedCommand::evaluate_status_impl(EvalContext &cxt) const throws
    -> status_result
{
  ASSERT(m_child != nullptr);

  LOG(Debug, "applying %zu redirections around the compound command",
      m_redirections.count());

  cxt.set_current_location(source_location());

  /* The mark is taken before the expansion below so this command reaps only the
     process substitution its own redirection opens. Registered first so it runs
     last, after the descriptor backups restore. */
  let const substitution_mark = cxt.mark_process_substitutions();
  defer { cxt.cleanup_process_substitutions(substitution_mark); };

  cxt.set_terminal_exec_allowed(false);

  /* The backups restore in reverse on every exit path, a normal return, a
     thrown diagnostic, or a pending break, continue, return, or exit. */
  ArrayList<os::saved_descriptor> saved_descriptors{cxt.scratch_allocator()};
  defer
  {
    koshka::flush();
    for (usize i = saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(saved_descriptors[i - 1]);
  };

  koshka::flush();

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

  return m_child->evaluate_status(cxt);
}

UnaryExpression::UnaryExpression(SourceLocation location, const Expression *rhs)
    : Expression(steal(location)), m_rhs(rhs)
{}

UnaryExpression::~UnaryExpression() = default;

cold fn UnaryExpression::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_rhs != nullptr);

  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[Unary " + to_string() + "]\n";
  s += pad + EXPRESSION_AST_INDENT + m_rhs->to_ast_string(layer + 1);
  return s;
}

BinaryExpression::BinaryExpression(SourceLocation location,
                                   const Expression *lhs, const Expression *rhs)
    : Expression(steal(location)), m_lhs(lhs), m_rhs(rhs)
{}

BinaryExpression::~BinaryExpression() = default;

cold fn BinaryExpression::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_lhs != nullptr);
  ASSERT(m_rhs != nullptr);

  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[Binary " + to_string() + "]\n";
  s += pad + EXPRESSION_AST_INDENT + m_lhs->to_ast_string(layer + 1) + "\n";
  s += pad + EXPRESSION_AST_INDENT + m_rhs->to_ast_string(layer + 1);

  return s;
}

ConstantNumber::ConstantNumber(SourceLocation location, i64 value)
    : Expression(steal(location)), m_value(value)
{}

ConstantNumber::~ConstantNumber() = default;

fn ConstantNumber::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  unused(cxt);
  return m_value;
}

cold fn ConstantNumber::to_ast_string(usize layer) const throws -> String
{
  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[Number " + to_string() + "]";
  return s;
}

cold fn ConstantNumber::to_string() const throws -> String
{
  return String::from(m_value, heap_allocator());
}

#define UNARY_EXPRESSION_DECLS(e, expr)                                        \
  e::e(SourceLocation location, const Expression *rhs)                         \
      : UnaryExpression(steal(location), rhs)                                  \
  {}                                                                           \
  String e::to_string() const throws { return #expr; }                         \
  i64 e::evaluate_impl(EvalContext &cxt) const throws                          \
  {                                                                            \
    return expr m_rhs->evaluate(cxt);                                          \
  }

UNARY_EXPRESSION_DECLS(Negate, -);
UNARY_EXPRESSION_DECLS(Unnegate, +);
UNARY_EXPRESSION_DECLS(LogicalNot, !);
UNARY_EXPRESSION_DECLS(BinaryComplement, ~);

BinaryDummyExpression::BinaryDummyExpression(SourceLocation location,
                                             const Expression *lhs,
                                             const Expression *rhs)
    : BinaryExpression(steal(location), lhs, rhs)
{}

cold fn BinaryDummyExpression::to_string() const throws -> String
{
  return "BinaryDummyExpression";
}

fn BinaryDummyExpression::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  unused(cxt);
  return 0;
}

Divide::Divide(SourceLocation location, const Expression *lhs,
               const Expression *rhs)
    : BinaryExpression(steal(location), lhs, rhs)
{}

cold fn Divide::to_string() const throws -> String { return "/"; }

fn Divide::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_lhs != nullptr);
  ASSERT(m_rhs != nullptr);

  let const denom = m_rhs->evaluate(cxt);
  if (denom == 0)
    throw ErrorWithLocation{m_rhs->source_location(), "Division by zero"};

  return m_lhs->evaluate(cxt) / denom;
}

#define BINARY_EXPRESSION_DECLS(e, expr)                                       \
  e::e(SourceLocation location, const Expression *lhs, const Expression *rhs)  \
      : BinaryExpression(steal(location), lhs, rhs)                            \
  {}                                                                           \
  String e::to_string() const throws { return #expr; }                         \
  i64 e::evaluate_impl(EvalContext &cxt) const throws                          \
  {                                                                            \
    return m_lhs->evaluate(cxt) expr m_rhs->evaluate(cxt);                     \
  }

BINARY_EXPRESSION_DECLS(Add, +);
BINARY_EXPRESSION_DECLS(Subtract, -);
BINARY_EXPRESSION_DECLS(Multiply, *);
BINARY_EXPRESSION_DECLS(Module, %);
BINARY_EXPRESSION_DECLS(BinaryAnd, &);
BINARY_EXPRESSION_DECLS(LogicalAnd, &&);
BINARY_EXPRESSION_DECLS(GreaterThan, >);
BINARY_EXPRESSION_DECLS(GreaterOrEqual, >=);
BINARY_EXPRESSION_DECLS(RightShift, >>);
BINARY_EXPRESSION_DECLS(LessThan, <);
BINARY_EXPRESSION_DECLS(LessOrEqual, <=);
BINARY_EXPRESSION_DECLS(LeftShift, <<);
BINARY_EXPRESSION_DECLS(BinaryOr, |);
BINARY_EXPRESSION_DECLS(LogicalOr, ||);
BINARY_EXPRESSION_DECLS(Xor, ^);
BINARY_EXPRESSION_DECLS(Equal, ==);
BINARY_EXPRESSION_DECLS(NotEqual, !=);

} /* namespace expressions */

} /* namespace koshka */
