/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements syntax-tree optimization. It folds constants and
 * removes proven dead work without changing observable shell behavior.
 */

#include "Optimizer.hpp"

#include "Builtin.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Tokens.hpp"
#include "base/Common.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace optimizer {

namespace {

pure fn is_constant_arithmetic_text(StringView text) wontthrow -> bool
{
  static constexpr let CONSTANT_BYTES = [] {
    constexpr char MEMBERS[] = "0123456789 \t\n\r()+-*/%&|^~!<>=?:";
    struct
    {
      bool is_member[256]{};
    } table;
    for (usize i = 0; i + 1 < sizeof(MEMBERS); i++)
      table.is_member[static_cast<u8>(MEMBERS[i])] = true;
    return table;
  }();

  static constexpr u64 MAXIMUM_FOLDED_GROWTH_OPERAND = 1024;
  usize growing_operator_count = 0;
  for (usize i = 0; i < text.length; i++) {
    if (!CONSTANT_BYTES.is_member[static_cast<u8>(text[i])]) return false;

    let const is_growing_operator =
        i + 1 < text.length &&
        ((text[i] == '*' && text[i + 1] == '*') ||
         (text[i] == '<' && text[i + 1] == '<'));
    if (!is_growing_operator) continue;

    growing_operator_count++;
    if (growing_operator_count > 1) return false;

    usize operand_position = i + 2;
    while (operand_position < text.length &&
           (text[operand_position] == ' ' || text[operand_position] == '\t'))
    {
      operand_position++;
    }

    u64 operand = 0;
    usize digit_count = 0;
    while (operand_position < text.length && text[operand_position] >= '0' &&
           text[operand_position] <= '9')
    {
      operand = operand * 10 + static_cast<u64>(text[operand_position] - '0');
      digit_count++;
      operand_position++;
      if (operand > MAXIMUM_FOLDED_GROWTH_OPERAND) return false;
    }

    if (digit_count == 0) return false;
  }

  return true;
}

pure fn is_plain_integer_literal(StringView text) wontthrow -> bool
{
  if (text.is_empty()) return false;
  let const start_position = text[0] == '-' ? usize{1} : usize{0};
  return text.substring(start_position).is_all_decimal_digits();
}

pure fn is_split_eligible_variable_operand(const Token *token) wontthrow -> bool
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return false;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  if (word.segments.count() != 1) return false;

  const WordSegment &segment = word.segments[0];
  return segment.kind == WordSegment::Kind::VariableReference &&
         segment.is_split_eligible();
}

fn propagated_test_operand_value(const Token *token,
                                 const AnalysisContext &actx) throws
    -> Maybe<String>
{
  if (is_split_eligible_variable_operand(token)) {
    LOG(All, "declining the test operand fold, the unquoted "
             "variable splits at run time");
    return None;
  }
  return propagated_literal_word_value(token, actx);
}

fn constant_test_verdict(const ArrayList<const Token *> &args,
                         usize first_operand_index, usize operand_count,
                         const AnalysisContext &actx) throws -> Maybe<bool>
{
  if (operand_count == 0) return Maybe<bool>{false};

  if (operand_count == 1) {
    let const value =
        propagated_test_operand_value(args[first_operand_index], actx);
    if (!value.has_value()) return None;
    return Maybe<bool>{!value->is_empty()};
  }

  if (operand_count == 2) {
    let const op =
        propagated_test_operand_value(args[first_operand_index], actx);
    let const arg =
        propagated_test_operand_value(args[first_operand_index + 1], actx);
    if (!op.has_value() || !arg.has_value()) {
      return None;
    }
    if (*op == "-n") return Maybe<bool>{!arg->is_empty()};
    if (*op == "-z") return Maybe<bool>{arg->is_empty()};
    return None;
  }

  if (operand_count == 3) {
    let const lhs =
        propagated_test_operand_value(args[first_operand_index], actx);
    let const op =
        propagated_test_operand_value(args[first_operand_index + 1], actx);
    let const rhs =
        propagated_test_operand_value(args[first_operand_index + 2], actx);
    if (!lhs.has_value() || !op.has_value() || !rhs.has_value()) {
      return None;
    }
    if (*op == "=") return Maybe<bool>{*lhs == *rhs};
    if (*op == "!=") return Maybe<bool>{*lhs != *rhs};
    return None;
  }

  return None;
}

pure fn may_expand_assignment_tilde(StringView text) wontthrow -> bool
{
  if (!lexer::word_looks_like_assignment(text)) return false;

  let const equals = text.find_character('=');
  if (equals.has_value() && *equals + 1 < text.length &&
      text[*equals + 1] == '~')
    return true;

  return text.find_substring(":~").has_value();
}

} /* namespace */

fn literal_word_value(const Word &word) throws -> Maybe<String>
{
  let value = String{heap_allocator()};
  for (let const &segment : word.segments) {
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::DoubleQuotedText:
      value.append(segment.text.view());
      break;
    case WordSegment::Kind::UnquotedText:
      for (usize i = 0; i < segment.text.count(); i++) {
        if (lexer::is_expandable_char(segment.text[i])) return None;
      }
      if (!segment.text.is_empty() && segment.text[0] == '~') {
        return None;
      }
      if (&segment == &word.segments.front() &&
          may_expand_assignment_tilde(segment.text.view()))
        return None;
      value.append(segment.text.view());
      break;
    default: return None;
    }
  }
  return value;
}

fn literal_word_value(const Token *token) throws -> Maybe<String>
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return None;
  return literal_word_value(
      static_cast<const tokens::WordToken *>(token)->word());
}

namespace {

fn command_word_literal(const Token *token) throws -> Maybe<String>
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return None;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  let name = String{heap_allocator()};
  for (let const &segment : word.segments) {
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText &&
        segment.kind != WordSegment::Kind::UnquotedText)
    {
      return None;
    }
    name.append(segment.text.view());
  }
  return name;
}

} /* namespace */

fn plain_variable_reference_name(const Token *token) wontthrow
    -> Maybe<StringView>
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return None;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  if (word.segments.count() != 1) return None;

  const WordSegment &segment = word.segments[0];
  if (segment.kind != WordSegment::Kind::VariableReference) return None;
  let const name = segment.text.view();
  if (!lexer::word_is_variable_name(name)) return None;
  return name;
}

fn propagated_literal_word_value(const Token *token,
                                 const AnalysisContext &actx) throws
    -> Maybe<String>
{
  let literal = literal_word_value(token);
  if (literal.has_value()) return literal;

  let const name = plain_variable_reference_name(token);
  if (!name.has_value()) return None;
  if (let const recorded = actx.constant_variables.find(*name);
      recorded.has_value())
  {
    LOG(All, "reading the recorded constant '%.*s' = '%s'",
        static_cast<int>(name->length), name->data, recorded->c_str());
    return recorded->clone();
  }
  return None;
}

fn try_fold_constant_arithmetic(StringView expression) wontthrow -> Maybe<i64>
{
  if (expression.length == 0 || !is_constant_arithmetic_text(expression)) {
    return None;
  }

  try {
    return evaluate_constant_arithmetic(expression);
  } catch (const ErrorBase &) {
    LOG(All,
        "swallowed an arithmetic error while folding '%.*s', leaving the "
        "segment for the runtime path",
        static_cast<int>(expression.length), expression.data);
    return None;
  }
}

fn try_fold_exact_constant_arithmetic(StringView expression) wontthrow
    -> Maybe<String>
{
  if (expression.length == 0 || !is_constant_arithmetic_text(expression)) {
    return None;
  }

  try {
    return evaluate_constant_arithmetic_text(expression, heap_allocator());
  } catch (const ErrorBase &) {
    LOG(All,
        "swallowed an exact arithmetic error while folding '%.*s', leaving "
        "the segment for the runtime path",
        static_cast<int>(expression.length), expression.data);
    return None;
  }
}

fn try_fold_arithmetic_with_constants(StringView expression,
                                      const AnalysisContext &actx) wontthrow
    -> Maybe<String>
{
  if (expression.length == 0) return None;
  if (actx.constant_variables.count() == 0) return None;

  try {
    let rewritten = String{heap_allocator()};
    usize i = 0;
    while (i < expression.length) {
      let const byte = expression[i];
      if (!lexer::is_variable_name_start(byte)) {
        rewritten += byte;
        i++;
        continue;
      }

      usize start_position = i;
      while (i < expression.length && lexer::is_variable_name(expression[i]))
        i++;
      let const name =
          StringView{&expression.data[start_position], i - start_position};

      let const recorded = actx.constant_variables.find(name);
      if (!recorded.has_value()) {
        LOG(All,
            "skipping the arithmetic fold, '%.*s' is not a recorded constant",
            static_cast<int>(name.length), name.data);
        return None;
      }
      if (!is_plain_integer_literal(recorded->view())) {
        LOG(All,
            "skipping the arithmetic fold, the value of '%.*s' is not a plain "
            "integer",
            static_cast<int>(name.length), name.data);
        return None;
      }
      rewritten.append(recorded->view());
    }

    if (!is_constant_arithmetic_text(rewritten.view())) return None;
    return evaluate_constant_arithmetic_text(rewritten.view(),
                                             heap_allocator());
  } catch (const ErrorBase &) {
    LOG(All,
        "swallowed an arithmetic error while folding '%.*s' with constants",
        static_cast<int>(expression.length), expression.data);
    return None;
  }
}

enum class static_verdict_kind : u8
{
  always_true,
  always_false,
  test_like,
};

constexpr static_string_entry<static_verdict_kind> STATIC_VERDICT_ENTRIES[] = {
    {SSK("true"),  static_verdict_kind::always_true },
    {SSK(":"),     static_verdict_kind::always_true },
    {SSK("false"), static_verdict_kind::always_false},
    {SSK("test"),  static_verdict_kind::test_like   },
    {SSK("["),     static_verdict_kind::test_like   },
};
constexpr StaticStringMap STATIC_VERDICT_KINDS{STATIC_VERDICT_ENTRIES};

fn simple_command_static_verdict(const ArrayList<const Token *> &args,
                                 const AnalysisContext &actx) throws
    -> Maybe<bool>
{
  if (args.is_empty()) return None;

  let const name = command_word_literal(args[0]);
  if (!name.has_value()) return None;

  if (actx.functions.defined.contains(name->view())) {
    LOG(All, "declining the static verdict, a function shadows '%s'",
        name->c_str());
    return None;
  }
  if (actx.functions.aliases.contains(name->view())) {
    LOG(All, "declining the static verdict, an alias shadows '%s'",
        name->c_str());
    return None;
  }

  let const kind = STATIC_VERDICT_KINDS.find(name->view());
  if (!kind.has_value()) return None;

  switch (*kind) {
  case static_verdict_kind::always_true:
    if (args.count() != 1) return None;
    LOG(All, "the builtin '%s' always succeeds, verdict is true",
        name->c_str());
    return Maybe<bool>{true};
  case static_verdict_kind::always_false:
    if (args.count() != 1) return None;
    LOG(All, "the builtin 'false' always fails, verdict is false");
    return Maybe<bool>{false};
  case static_verdict_kind::test_like: {
    usize last_index = args.count();
    if (*name == "[") {
      let const closing = literal_word_value(args[args.count() - 1]);
      if (!closing.has_value() || *closing != "]") {
        return None;
      }
      last_index -= 1;
    }
    return constant_test_verdict(args, 1, last_index - 1, actx);
  }
  }

  return None;
}

pure fn word_segment_has_glob_metacharacter(
    const WordSegment &segment) wontthrow -> bool
{
  let const text = segment.text.view();

  for (usize i = 0; i < text.length; i++) {
    switch (text[i]) {
    case '*':
    case '?':
    case '[': return true;

    case '+':
    case '@':
    case '!':
      if (i + 1 < text.length && text[i + 1] == '(') return true;
      break;

    default: break;
    }
  }

  return false;
}

pure fn classify_plain_literal(const Word &word) wontthrow -> Word::PlainLiteral
{
  if (word.segments.is_empty()) return Word::PlainLiteral::NotPlain;

  if (word.segments.count() == 1 &&
      word.segments[0].kind == WordSegment::Kind::UnquotedText)
  {
    const WordSegment &only = word.segments[0];
    if (word_segment_has_glob_metacharacter(only))
      return Word::PlainLiteral::NotPlain;
    if (!only.text.is_empty() && only.text[0] == '~') {
      return Word::PlainLiteral::NotPlain;
    }
    if (may_expand_assignment_tilde(only.text.view()))
      return Word::PlainLiteral::NotPlain;
    return Word::PlainLiteral::PlainUnquotedOneSegment;
  }

  for (let const &segment : word.segments) {
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText)
    {
      return Word::PlainLiteral::NotPlain;
    }
  }
  return Word::PlainLiteral::PlainNoSplit;
}

namespace {

pure fn arithmetic_has_side_effect(StringView text) wontthrow -> bool
{
  for (usize i = 0; i < text.length; i++) {
    if (i + 1 < text.length && ((text[i] == '+' && text[i + 1] == '+') ||
                                (text[i] == '-' && text[i + 1] == '-')))
    {
      return true;
    }

    if (text[i] == '=') {
      let const previous = i > 0 ? text[i - 1] : '\0';
      let const next = i + 1 < text.length ? text[i + 1] : '\0';
      if (i >= 2 && ((previous == '<' && text[i - 2] == '<') ||
                     (previous == '>' && text[i - 2] == '>')))
      {
        return true;
      }
      if (previous != '=' && previous != '!' && previous != '<' &&
          previous != '>' && next != '=')
      {
        return true;
      }
    }
  }
  return false;
}

fn fold_constant_arithmetic_in_word(
    const Word &word, AnalysisContext &actx,
    const SourceLocation &fallback_location) throws -> bool
{
  bool did_fold = false;
  for (let const &segment : word.segments) {
    if (segment.kind != WordSegment::Kind::ArithmeticExpansion) continue;
    if (segment.has_optimizer_arithmetic_result()) continue;

    if (arithmetic_has_side_effect(segment.text.view())) {
      actx.constant_variables.clear();
      continue;
    }

    let result = try_fold_exact_constant_arithmetic(segment.text.view());
    let const is_direct_constant = result.has_value();
    if (!result.has_value())
      result = try_fold_arithmetic_with_constants(segment.text.view(), actx);
    if (result.has_value()) {
      LOG(All, "folded the constant arithmetic '%.*s' to %s",
          static_cast<int>(segment.text.view().length),
          segment.text.view().data, result->c_str());
      segment.set_optimizer_arithmetic_result(
          is_direct_constant ? result->view() : StringView{},
          actx.eval_context != nullptr
              ? actx.eval_context->arena_store().parse_arena()
              : nullptr);
      did_fold = true;
      actx.optimizer_eliminated_count++;
      if (actx.options.should_report_optimizer_diagnostics) {
        let const segment_location =
            segment.get_source_location(fallback_location.source_name_index);
        actx.report_diagnostic(diagnostic_id::optimizer_folded_arithmetic,
                               segment_location.has_value() ? *segment_location
                                                            : fallback_location,
                               {segment.text.view(), result->view()});
      }
    }
  }
  return did_fold;
}

fn fold_constant_arithmetic_in_token(const Token *token,
                                     AnalysisContext &actx) throws -> bool
{
  ASSERT(token != nullptr);

  if (token->kind() != Token::Kind::Word) return false;
  return fold_constant_arithmetic_in_word(
      static_cast<const tokens::WordToken *>(token)->word(), actx,
      token->source_location());
}

fn rule_fold_constant_arithmetic(const Expression *node,
                                 AnalysisContext &actx) throws -> bool
{
  if (const expressions::AssignCommand *assign = node->as_assign_command();
      assign != nullptr)
  {
    return fold_constant_arithmetic_in_word(assign->assignment()->value_word(),
                                            actx, node->source_location());
  }

  if (const expressions::SimpleCommand *cmd = node->as_simple_command();
      cmd != nullptr)
  {
    bool did_fold = false;
    for (let const t : cmd->args()) {
      if (fold_constant_arithmetic_in_token(t, actx)) did_fold = true;
    }
    for (let const &var : cmd->local_vars()) {
      if (fold_constant_arithmetic_in_word(var.get_value(), actx,
                                           node->source_location()))
        did_fold = true;
    }
    return did_fold;
  }

  return false;
}

fn rule_dead_branch_elimination(const Expression *node,
                                AnalysisContext &actx) throws -> bool
{
  const expressions::IfClause *clause = node->as_if_clause();
  if (clause == nullptr) return false;
  if (clause->has_folded_branch()) return false;

  for (usize i = 0; i < clause->branches().count(); i++) {
    let const verdict =
        clause->branches()[i].condition->try_static_condition_verdict(actx);
    if (!verdict.has_value()) {
      LOG(All,
          "the dead-branch fold stops, condition %zu is not statically "
          "decidable",
          i);
      return false;
    }
    if (*verdict) {
      LOG(All, "dead-branch elimination chose branch %zu", i);
      clause->set_folded_branch(i);
      actx.optimizer_eliminated_count++;
      if (actx.options.should_report_optimizer_diagnostics) {
        let const index = String::from(static_cast<i64>(i), heap_allocator());
        actx.report_diagnostic(diagnostic_id::optimizer_folded_branch,
                               node->source_location(), {index.view()});
      }
      return true;
    }
  }
  LOG(All, "every if condition is statically false, folding to the else body");
  clause->set_folded_branch(clause->branches().count());
  actx.optimizer_eliminated_count++;
  if (actx.options.should_report_optimizer_diagnostics)
    actx.report_diagnostic(diagnostic_id::optimizer_folded_else,
                           node->source_location());
  return true;
}

fn rule_loop_elimination(const Expression *node, AnalysisContext &actx) throws
    -> bool
{
  const expressions::WhileLoop *loop_node = node->as_while_loop();
  if (loop_node == nullptr) return false;
  if (loop_node->is_folded_to_skip()) return false;

  let const verdict =
      loop_node->condition()->try_static_condition_verdict(actx);
  if (!verdict.has_value()) {
    LOG(All,
        "the loop fold declines, the condition is not statically decidable");
    return false;
  }

  let const body_would_run = loop_node->is_until() ? !*verdict : *verdict;
  if (body_would_run) {
    LOG(All,
        "the loop fold declines, the body would run under the static verdict");
    return false;
  }

  LOG(All, "loop elimination folded the %s loop to a skip",
      loop_node->is_until() ? "until" : "while");
  loop_node->set_folded_to_skip();
  actx.optimizer_eliminated_count++;
  if (actx.options.should_report_optimizer_diagnostics)
    actx.report_diagnostic(
        diagnostic_id::optimizer_folded_loop, node->source_location(),
        {loop_node->is_until() ? StringView{"until"} : StringView{"while"}});
  return true;
}

fn rule_eliminate_compound_body(const Expression *node,
                                AnalysisContext &actx) throws -> bool
{
  const expressions::IfClause *clause = node->as_if_clause();
  if (clause == nullptr) return false;
  if (clause->is_fully_eliminated()) return false;
  if (!clause->has_folded_branch()) return false;

  if (clause->folded_branch_index() != clause->branches().count()) return false;
  if (clause->otherwise() != nullptr) return false;

  LOG(All, "compound-body elimination folded an if with no taken branch to a "
           "no-op");
  clause->set_fully_eliminated();
  actx.optimizer_eliminated_count++;
  if (actx.options.should_report_optimizer_diagnostics)
    actx.report_diagnostic(diagnostic_id::optimizer_eliminated_if,
                           node->source_location());
  return true;
}

fn rule_eliminate_empty_for(const Expression *node,
                            AnalysisContext &actx) throws -> bool
{
  const expressions::ForLoop *loop_node = node->as_for_loop();
  if (loop_node == nullptr) return false;
  if (loop_node->is_fully_eliminated()) return false;

  if (!loop_node->has_in_clause()) return false;
  if (!loop_node->words().is_empty()) return false;

  LOG(All, "empty for-loop elimination folded a for with an empty in clause to "
           "a no-op");
  loop_node->set_fully_eliminated();
  actx.optimizer_eliminated_count++;
  if (actx.options.should_report_optimizer_diagnostics)
    actx.report_diagnostic(diagnostic_id::optimizer_eliminated_for,
                           node->source_location());
  return true;
}

fn rule_fold_cstyle_for(const Expression *node, AnalysisContext &actx) throws
    -> bool
{
  const expressions::CStyleForLoop *loop_node = node->as_cstyle_for_loop();
  if (loop_node == nullptr) return false;
  if (loop_node->has_folded_condition()) return false;

  let const trimmed = loop_node->condition_clause().trim_blanks();
  if (trimmed.length == 0) return false;

  let const value = try_fold_constant_arithmetic(trimmed);
  if (!value.has_value()) return false;
  bool is_exact_nonzero;
  try {
    is_exact_nonzero = evaluate_constant_arithmetic_nonzero(trimmed, true);
  } catch (const Error &) {
    return false;
  }

  loop_node->set_folded_condition(*value, is_exact_nonzero);
  LOG(All, "folded the c-style for condition '%.*s' to %lld",
      static_cast<int>(trimmed.length), trimmed.data,
      static_cast<long long>(*value));
  actx.optimizer_eliminated_count++;
  if (actx.options.should_report_optimizer_diagnostics) {
    let const folded = String::from(*value, heap_allocator());
    actx.report_diagnostic(diagnostic_id::optimizer_folded_arithmetic,
                           node->source_location(), {trimmed, folded.view()});
  }

  if (*value == 0 && !is_exact_nonzero &&
      loop_node->init_clause().trim_blanks().length == 0)
  {
    loop_node->set_fully_eliminated();
    actx.optimizer_eliminated_count++;
    if (actx.options.should_report_optimizer_diagnostics)
      actx.report_diagnostic(diagnostic_id::optimizer_eliminated_cstyle_for,
                             node->source_location());
  }
  return true;
}

using OptimizationRule = fn(const Expression *, AnalysisContext &) throws->bool;

OptimizationRule *const OPTIMIZATION_RULES[] = {
    rule_fold_constant_arithmetic, rule_dead_branch_elimination,
    rule_loop_elimination,         rule_eliminate_compound_body,
    rule_eliminate_empty_for,      rule_fold_cstyle_for,
};

constexpr usize MAX_OPTIMIZATION_PASSES = 8;

} /* namespace */

fn optimize_node(const Expression *node, AnalysisContext &actx) throws -> void
{
  ASSERT(node != nullptr);

  for (usize pass = 0; pass < MAX_OPTIMIZATION_PASSES; pass++) {
    bool did_any_rule_fire = false;
    for (let rule : OPTIMIZATION_RULES) {
      if (rule(node, actx)) did_any_rule_fire = true;
    }
    if (!did_any_rule_fire) return;
    LOG(All,
        "optimization pass %zu fired a rule, running another "
        "pass over the node",
        pass);
  }
  LOG(All, "the optimizer hit the pass cap of %zu on one node",
      MAX_OPTIMIZATION_PASSES);
}

} /* namespace optimizer */

} /* namespace koshka */
