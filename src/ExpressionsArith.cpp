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

ConditionalCommand::ConditionalCommand(SourceLocation location,
                                       ArrayList<conditional_element> elements)
    : CompoundCommand(location), m_elements(steal(elements))
{}

ConditionalCommand::~ConditionalCommand() = default;

cold fn ConditionalCommand::to_string() const throws -> String
{
  return "ConditionalCommand";
}

cold fn ConditionalCommand::to_ast_string(usize layer) const throws -> String
{
  return indent_for_layer(layer) + "[" + to_string() + "]";
}

cold static fn conditional_word_is_literal(const Token *token) wontthrow -> bool
{
  if (token == nullptr || token->kind() != Token::Kind::Word) return false;
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
  if (token == nullptr || token->kind() != Token::Kind::Word) return false;
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

  if (element.word == nullptr) return None;

  let const view = element.word->raw_view();
  if (!view.has_value() || !is_conditional_binary_operator(*view)) return None;

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

  let const &op = elements[operator_index];
  if (op.kind != Kind::Operand || op.word == nullptr) return None;

  let const op_view = op.word->raw_view();
  if (!op_view.has_value() || *op_view != StringView{"!="}) return None;

  let const &left = elements[operator_index - 1];
  if (left.kind != Kind::Operand || left.word == nullptr) return None;

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
  if (element.kind != Kind::Operand || element.word == nullptr) return false;

  let const view = element.word->raw_view();

  return !view.has_value() || !is_conditional_binary_operator(*view);
}

fn ConditionalCommand::analyze(AnalysisContext &actx,
                               bool is_unconditional) const throws -> void
{
  unused(is_unconditional);

  let conditional_location = source_location();
  if (source_end_position() > conditional_location.position) {
    conditional_location.length =
        source_end_position() - conditional_location.position;
  }

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
        actx.report_diagnostic(diagnostic_id::sc2071, source_location(), {op});
      if (i > 0 && i + 1 < m_elements.count() &&
          conditional_word_is_literal(m_elements[i - 1].word) &&
          conditional_word_is_literal(m_elements[i + 1].word))
        actx.report_diagnostic(diagnostic_id::sc2050, conditional_location,
                               {op});
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

    if (element.kind != Kind::Operand || element.word == nullptr) continue;

    let const operand = element.word->raw_string();

    /* A unary operator followed by a binary operator lost its operand,
       shellcheck SC1019. */
    if (is_test_unary_operator_word(operand.view()) &&
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

    if (operand.view() == ">=" || operand.view() == "<=") {
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
      let const &previous = m_elements[i - 1];
      is_binary_operand =
          previous.kind == Kind::Less || previous.kind == Kind::Greater ||
          (previous.kind == Kind::Operand && previous.word != nullptr &&
           is_conditional_binary_operator(previous.word->raw_string().view()));
    }
    if (!is_binary_operand && i + 1 < m_elements.count()) {
      let const &next = m_elements[i + 1];
      is_binary_operand =
          next.kind == Kind::Less || next.kind == Kind::Greater ||
          (next.kind == Kind::Operand && next.word != nullptr &&
           is_conditional_binary_operator(next.word->raw_string().view()));
    }
    if (!is_binary_operand && !is_conditional_binary_operator(operand.view()) &&
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
    if ((operand.view() == "-n" || operand.view() == "-z") &&
        i + 1 < m_elements.count() &&
        conditional_word_is_literal(m_elements[i + 1].word))
      actx.report_diagnostic(diagnostic_id::sc2157_string,
                             m_elements[i + 1].word->source_location());

    if (CONDITIONAL_PATH_TESTS.contains(operand.view()) &&
        i + 1 < m_elements.count() &&
        conditional_word_has_glob(m_elements[i + 1].word))
      actx.report_diagnostic(diagnostic_id::sc2144,
                             m_elements[i + 1].word->source_location());

    if (!is_conditional_binary_operator(operand.view()) || i == 0 ||
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
      actx.report_diagnostic(diagnostic_id::sc2050, conditional_location,
                             {operand.view()});

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
      span.length = source_end_position() - span.position;
    relocate_error(e, span);
  }
  LOG(Debug, "the [[ ]] conditional yielded status %lld",
      static_cast<long long>(status));
  cxt.publish_single_pipe_status(static_cast<i32>(status));
  SET_AND_RETURN_EXIT_STATUS(cxt, status);
}

ArithmeticCommand::ArithmeticCommand(SourceLocation location, String expression)
    : CompoundCommand(location), m_expression(steal(expression))
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
  return indent_for_layer(layer) + "[" + to_string() + " \"" +
         m_expression.view() + "\"]";
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
  LOG(Debug, "evaluating the arithmetic command '%s'", m_expression.c_str());

  cxt.set_current_location(source_location());

  if (is_blank_clause(m_expression.view())) {
    cxt.publish_single_pipe_status(1);
    SET_AND_RETURN_EXIT_STATUS(cxt, 1);
  }

  /* A non-zero value is success and zero is failure, the opposite of the
     value-to-status convention elsewhere. */
  i64 value;
  try {
    const SourceLocation body_base{source_location().position + 2, 0,
                                   source_location().filename};
    value = cxt.evaluate_arithmetic(m_expression.view(), &body_base);
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (const Error &e) {
    relocate_error(e, source_location());
  }
  const i64 status = value != 0 ? 0 : 1;
  cxt.publish_single_pipe_status(static_cast<i32>(status));
  SET_AND_RETURN_EXIT_STATUS(cxt, status);
}

fn ArithmeticCommand::analyze(AnalysisContext &actx,
                              bool is_unconditional) const throws -> void
{
  unused(is_unconditional);
  if (arithmetic_reads_external_input(actx, m_expression.view()))
    actx.report_diagnostic(diagnostic_id::external_arithmetic_input,
                           source_location());

  check_arithmetic_expression_lints(actx, m_expression.view(),
                                    source_location());

  if (actx.shebang_is_posix_sh) {
    actx.report_diagnostic(diagnostic_id::sc3006, source_location());
    check_posix_arithmetic_operators(actx, m_expression.view(),
                                     source_location());
  }

  /* The prepass does not parse the expression, which may assign any name, so
     every recorded constant is forgotten. */
  actx.constant_variables.clear();
}

fn SelectLoop::analyze(AnalysisContext &actx,
                       bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);
  unused(is_unconditional);
  actx.constant_variables.clear();
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;
}

CStyleForLoop::CStyleForLoop(SourceLocation location, String init,
                             String condition, String step,
                             const Expression *body)
    : CompoundCommand(location), m_init(steal(init)),
      m_condition(steal(condition)), m_step(steal(step)), m_body(body)
{}

CStyleForLoop::~CStyleForLoop() = default;

cold fn CStyleForLoop::to_string() const throws -> String
{
  return "CStyleForLoop";
}

cold fn CStyleForLoop::to_ast_string(usize layer) const throws -> String
{
  ASSERT(m_body != nullptr);
  let const pad = indent_for_layer(layer);
  return pad + "[" + to_string() + " \"" + m_init.view() + ";" +
         m_condition.view() + ";" + m_step.view() + "\"]\n" + pad +
         EXPRESSION_AST_INDENT + m_body->to_ast_string(layer + 1);
}

ArrayAssignCommand::ArrayAssignCommand(SourceLocation location, StringView name,
                                       ArrayList<const Token *> elements,
                                       bool is_append)
    : Command(location), m_name(name), m_elements(steal(elements)),
      m_is_append(is_append)
{}

ArrayAssignCommand::~ArrayAssignCommand() = default;

cold fn ArrayAssignCommand::to_string() const throws -> String
{
  return "ArrayAssignCommand";
}

cold fn ArrayAssignCommand::to_ast_string(usize layer) const throws -> String
{
  return indent_for_layer(layer) + "[" + to_string() + " " + m_name.view() +
         (m_is_append ? "+=(...)" : "=(...)") + "]";
}

fn ArrayAssignCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  cxt.set_current_location(source_location());

  ArrayList<String> values =
      cxt.process_args(m_elements, argument_lifetime::Persistent,
                       argument_context::ArrayLiteral);
  LOG(Debug, "assigning %zu elements to the array '%s'", values.count(),
      m_name.c_str());
  cxt.assign_indexed_array_elements(m_name.view(), steal(values), m_is_append);
  let ran_substitution = false;
  for (let const element : m_elements) {
    if (element->kind() != Token::Kind::Word) continue;
    if (static_cast<const tokens::WordToken *>(element)
            ->word()
            .runs_substitution())
    {
      ran_substitution = true;
      break;
    }
  }
  if (!ran_substitution) cxt.set_last_exit_status(0);
  cxt.publish_single_pipe_status(cxt.last_exit_status());
  return cxt.last_exit_status();
}

fn ArrayAssignCommand::analyze(AnalysisContext &actx,
                               bool is_unconditional) const throws -> void
{
  unused(is_unconditional);

  /* The name is no longer a scalar literal, so the constant table forgets it.
   */
  actx.array_valued_names.add(m_name.view());
  actx.constant_variables.erase(m_name.view());
}

fn CStyleForLoop::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  ASSERT(m_body != nullptr);

  cxt.set_terminal_exec_allowed(false);

  let const can_skip_condition_commands =
      !cxt.has_debug_trap() && !cxt.should_echo_expanded();
  if (m_is_fully_eliminated && can_skip_condition_commands) {
    LOG(Debug, "running the fully eliminated c-style for as a no-op");
    cxt.publish_single_pipe_status(0);
    SET_AND_RETURN_EXIT_STATUS(cxt, 0);
  }

  cxt.set_current_location(source_location());

  LOG(Debug,
      "entering the c-style for loop with init '%s', condition '%s', step "
      "'%s'",
      m_init.c_str(), m_condition.c_str(), m_step.c_str());

  if (!is_blank_clause(m_init.view())) cxt.evaluate_arithmetic(m_init.view());

  cxt.enter_loop();
  defer { cxt.leave_loop(); };

  let const condition_is_blank = is_blank_clause(m_condition.view());
  let const step_is_blank = is_blank_clause(m_step.view());

  i64 ret = 0;
  /* An empty condition is always true, the way for ((;;)) loops forever. */
  while (condition_is_blank ||
         (m_folded_condition.has_value() && can_skip_condition_commands
              ? (*m_folded_condition != 0)
              : cxt.evaluate_arithmetic_cached_clause(
                    m_condition.view(), m_condition_tokens,
                    m_condition_tokenized, m_condition_simple) != 0))
  {
    ret = m_body->evaluate(cxt);
    if (cxt.no_exec()) break;
    if (resolve_loop_control(cxt) == loop_disposition::StopLoop) break;
    /* The step runs after the body on every iteration, including one ended by a
       continue. */
    if (!step_is_blank) {
      cxt.evaluate_arithmetic_cached_clause(m_step.view(), m_step_tokens,
                                            m_step_tokenized, m_step_simple);
    }
  }
  SET_AND_RETURN_EXIT_STATUS(cxt, ret);
}

fn CStyleForLoop::analyze(AnalysisContext &actx,
                          bool is_unconditional) const throws -> void
{
  ASSERT(m_body != nullptr);
  unused(is_unconditional);

  /* The folding rule reads the three clauses while unchanged, so the optimizer
     runs before the constant table is cleared for the body. */
  optimizer::optimize_node(this, actx);

  for (let const clause : {m_init.view(), m_condition.view(), m_step.view()}) {
    if (clause.is_empty()) continue;

    check_arithmetic_expression_lints(actx, clause, source_location());

    if (actx.shebang_is_posix_sh)
      check_posix_arithmetic_operators(actx, clause, source_location());
  }

  actx.constant_variables.clear();
  actx.loop_body_depth++;
  m_body->analyze(actx, false);
  actx.loop_body_depth--;
}

pure fn CStyleForLoop::condition_clause() const wontthrow -> StringView
{
  return m_condition.view();
}

pure fn CStyleForLoop::init_clause() const wontthrow -> StringView
{
  return m_init.view();
}

fn CStyleForLoop::set_folded_condition(i64 value) const wontthrow -> void
{
  m_folded_condition = value;
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
    : CompoundCommand(location), m_body(body)
{}

Subshell::~Subshell() = default;

fn Subshell::as_subshell() const wontthrow -> const Subshell * { return this; }

cold fn Subshell::to_string() const throws -> String { return "Subshell"; }

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

  cxt.set_terminal_exec_allowed(false);

  /* This shell has no process-level subshell, so isolation is by snapshot. A
     loop in the parent is not the subshell's to break, so the body runs with a
     fresh loop count. */
  let const saved_loop_depth = cxt.loop_depth();
  cxt.set_loop_depth(0);
  defer { cxt.set_loop_depth(saved_loop_depth); };

  LOG(Debug, "entering the snapshot subshell");

  let snapshot = cxt.snapshot_state();

  /* The defer runs after restore_state on both the normal and the thrown
     path. */
  let const subshell_mark = cxt.scratch_mark();
  defer { cxt.scratch_release(subshell_mark); };
  cxt.enter_subshell();
  /* The inherited EXIT action belongs to the parent and must not fire at the
     subshell's end. An EXIT action the body sets survives this clear. */
  cxt.clear_inherited_exit_trap();
  i64 ret = 0;
  try {
    ret = body->evaluate(cxt);
  } catch (const ErrorBase &error) {
    /* A script-fatal error is confined to the subshell in every mood, status 1
       the way bash answers it and 2 the way dash does. */
    if (error.is_script_fatal()) {
      LOG(Debug, "the subshell confined a script-fatal error: %s",
          error.message().c_str());
      const String *source = cxt.current_source();
      show_message(error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      ret = cxt.is_bash_compatible() ? 1 : 2;
      cxt.set_last_exit_status(static_cast<i32>(ret));
      cxt.clear_control_flow();
    } else {
      cxt.run_subshell_exit_trap();
      cxt.leave_subshell();
      cxt.restore_state(steal(snapshot));
      throw;
    }
  } catch (...) {
    cxt.run_subshell_exit_trap();
    cxt.leave_subshell();
    cxt.restore_state(steal(snapshot));
    throw;
  }

  /* Exit and return end only the subshell. A break or continue is scoped to a
     loop inside it and is consumed here. */
  if (cxt.has_pending_control_flow()) {
    const control_flow::Kind kind = cxt.pending_control_flow().kind;
    if (kind == control_flow::Kind::Exit || kind == control_flow::Kind::Return)
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

  const os::process child = *forked_child;
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

cold static fn subshell_body_is_bracket_test(StringView body) wontthrow -> bool
{
  if (body.starts_with(StringView{"test "})) return true;

  return body.length >= 3 && body.starts_with(StringView{"[ "}) &&
         body[body.length - 1] == ']';
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
      } else if (subshell_body_is_bracket_test(body)) {
        actx.report_diagnostic(diagnostic_id::sc2234, source_location());
      }
    }
  }

  /* An assignment in the body never changes a parent variable, so the body
     starts from an empty table and the outer constants are restored after. */
  let saved_constants = steal(actx.constant_variables);
  actx.constant_variables = StringMap<String>{heap_allocator()};
  let const defined_function_insertion_count =
      actx.defined_function_insertions.count();
  let const known_alias_insertion_count = actx.known_alias_insertions.count();
  actx.apply_scope_definitions(m_analysis_scope_definitions);
  m_body->analyze(actx, is_unconditional);
  actx.constant_variables = steal(saved_constants);
  actx.rollback_defined_functions(defined_function_insertion_count);
  actx.rollback_known_aliases(known_alias_insertion_count);
  actx.is_analyzing_condition = was_analyzing_condition;
}

FunctionDefinition::FunctionDefinition(SourceLocation location, StringView name,
                                       const Expression *body)
    : CompoundCommand(location), m_name(name), m_body(body)
{}

/* The body is owned by the function table, not this node. */
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
  cxt.register_function(m_name, m_body, definition_text.view(),
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
    let const call_location =
        SourceLocation{static_cast<usize>(body_source.data - actx.source.data),
                       m_name.count(), m_body->source_location().filename};
    actx.report_diagnostic(diagnostic_id::sc2264, call_location,
                           {m_name.view()}, source_location());
  }

  /* The body runs later when the function is called, so it is analyzed from an
     empty constant table with the outer constants restored after. */
  let saved_constants = steal(actx.constant_variables);
  actx.constant_variables = StringMap<String>{heap_allocator()};
  let const defined_function_insertion_count =
      actx.defined_function_insertions.count();
  let const known_alias_insertion_count = actx.known_alias_insertions.count();
  let saved_locals = steal(actx.function_local_names);
  actx.function_local_names = HashSet{heap_allocator()};
  actx.apply_scope_definitions(m_analysis_scope_definitions);
  let const saved_loop_body_depth = actx.loop_body_depth;
  actx.loop_body_depth = 0;
  actx.function_scope_depth++;
  m_body->analyze(actx, false);
  actx.function_scope_depth--;
  actx.loop_body_depth = saved_loop_body_depth;
  actx.function_local_names = steal(saved_locals);
  actx.constant_variables = steal(saved_constants);
  actx.rollback_defined_functions(defined_function_insertion_count);
  actx.rollback_known_aliases(known_alias_insertion_count);
}

RedirectedCommand::RedirectedCommand(SourceLocation location,
                                     const Command *child,
                                     ArrayList<Redirection> &&redirections)
    : Command(location), m_child(child)
{
  m_redirections = steal(redirections);
}

RedirectedCommand::~RedirectedCommand() = default;

cold fn RedirectedCommand::to_string() const throws -> String
{
  return "RedirectedCommand";
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
}

fn RedirectedCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
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

      const os::descriptor source = os::descriptor_for_shell_fd(r.dup_from_fd);
      const os::saved_descriptor saved =
          os::save_and_replace_descriptor(r.target_fd, source);
      saved_descriptors.push(saved);
      if (!saved.is_dup2_ok) {
        const SourceLocation location = redir.target != nullptr
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

  const i64 result = m_child->evaluate(cxt);
  return result;
}

UnaryExpression::UnaryExpression(SourceLocation location, const Expression *rhs)
    : Expression(location), m_rhs(rhs)
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
    : Expression(location), m_lhs(lhs), m_rhs(rhs)
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
    : Expression(location), m_value(value)
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

ConstantString::ConstantString(SourceLocation location, StringView value)
    : Expression(location), m_value(value)
{}

ConstantString::~ConstantString() = default;

fn ConstantString::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  unused(cxt);
  unreachable();
}

cold fn ConstantString::to_ast_string(usize layer) const throws -> String
{
  let s = String{heap_allocator()};
  let const pad = indent_for_layer(layer);

  s += pad + "[String \"" + to_string() + "\"]";
  return s;
}

cold fn ConstantString::to_string() const throws -> String { return m_value; }

#define UNARY_EXPRESSION_DECLS(e, expr)                                        \
  e::e(SourceLocation location, const Expression *rhs)                         \
      : UnaryExpression(location, rhs)                                         \
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
    : BinaryExpression(location, lhs, rhs)
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
    : BinaryExpression(location, lhs, rhs)
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
      : BinaryExpression(location, lhs, rhs)                                   \
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

} // namespace expressions

} // namespace koshka
