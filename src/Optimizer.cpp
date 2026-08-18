#include "Optimizer.hpp"

#include "Builtin.hpp"
#include "Common.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"

namespace koshka {

namespace optimizer {

namespace {

/* A byte that may appear in a provably-constant arithmetic expression. Every
   letter and underscore is excluded, so no variable name and no hex prefix is
   folded. */
pure fn is_constant_arithmetic_byte(char byte) wontthrow -> bool
{
  if (lexer::is_number(byte)) return true;
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
  case '(':
  case ')':
  case '+':
  case '-':
  case '*':
  case '/':
  case '%':
  case '&':
  case '|':
  case '^':
  case '~':
  case '!':
  case '<':
  case '>':
  case '=':
  case '?':
  case ':': return true;
  default: return false;
  }
}

/* A recorded constant is only substituted into arithmetic when its value is a
   plain integer, so it cannot inject an operator or another name. */
pure fn is_plain_integer_literal(StringView text) wontthrow -> bool
{
  if (text.is_empty()) return false;
  let const start_position = text[0] == '-' ? usize{1} : usize{0};
  return text.substring(start_position).is_all_decimal_digits();
}

/* True when a token is a bare unquoted $name reference that field-splits at run
   time. Its recorded value is not the single test argument the run sees, so the
   verdict must not fold from it. A quoted "$name" still folds. */
pure fn is_split_eligible_variable_operand(const Token *token) wontthrow -> bool
{
  if (token == nullptr) return false;
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

} /* namespace */

pure fn is_plain_variable_name(StringView name) wontthrow -> bool
{
  if (name.length == 0) return false;
  if (!lexer::is_variable_name_start(name[0])) return false;
  for (usize i = 1; i < name.length; i++) {
    if (!lexer::is_variable_name(name[i])) return false;
  }
  return true;
}

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
      /* An unquoted segment expands a glob metacharacter and a leading tilde,
         so its bytes are only a plain constant when it holds neither. */
      for (usize i = 0; i < segment.text.count(); i++) {
        if (lexer::is_expandable_char(segment.text[i])) return None;
      }
      if (!segment.text.is_empty() && segment.text[0] == '~') {
        return None;
      }
      value.append(segment.text.view());
      break;
    default: return None;
    }
  }
  return value;
}

fn literal_word_value(const Token *token) throws -> Maybe<String>
{
  if (token == nullptr) return None;
  if (token->kind() != Token::Kind::Word) return None;
  return literal_word_value(
      static_cast<const tokens::WordToken *>(token)->word());
}

namespace {

/* The literal program text of a command word, keeping a glob metacharacter so
   the bracket word [ reads as its two-byte name. */
fn command_word_literal(const Token *token) throws -> Maybe<String>
{
  if (token == nullptr) return None;
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
  if (token == nullptr) return None;
  if (token->kind() != Token::Kind::Word) return None;

  let const &word = static_cast<const tokens::WordToken *>(token)->word();
  if (word.segments.count() != 1) return None;

  const WordSegment &segment = word.segments[0];
  if (segment.kind != WordSegment::Kind::VariableReference) return None;
  let const name = segment.text.view();
  if (!is_plain_variable_name(name)) return None;
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
  if (const String *recorded = actx.constant_variables.find(*name);
      recorded != nullptr)
  {
    LOG(All, "reading the recorded constant '%.*s' = '%s'",
        static_cast<int>(name->length), name->data, recorded->c_str());
    return recorded->clone();
  }
  return None;
}

fn try_fold_constant_arithmetic(StringView expression) wontthrow -> Maybe<i64>
{
  if (expression.length == 0) return None;

  for (usize i = 0; i < expression.length; i++) {
    if (!is_constant_arithmetic_byte(expression[i])) return None;
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

fn try_fold_arithmetic_with_constants(StringView expression,
                                      const AnalysisContext &actx) wontthrow
    -> Maybe<i64>
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
      if (recorded == nullptr) {
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

    for (usize j = 0; j < rewritten.count(); j++) {
      if (!is_constant_arithmetic_byte(rewritten[j])) return None;
    }
    return evaluate_constant_arithmetic(rewritten.view());
  } catch (const ErrorBase &) {
    LOG(All,
        "swallowed an arithmetic error while folding '%.*s' with constants",
        static_cast<int>(expression.length), expression.data);
    return None;
  }
}

static pure fn trim_arithmetic_whitespace(StringView text) wontthrow
    -> StringView
{
  return text.trim_blanks();
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

  /* The bracket word [ holds a glob metacharacter that the operand extractor
     rejects, so the name uses the word's literal text directly. */
  let const name = command_word_literal(args[0]);
  if (!name.has_value()) return None;

  if (actx.defined_functions.contains(name->view())) {
    LOG(All, "declining the static verdict, a function shadows '%s'",
        name->c_str());
    return None;
  }
  if (actx.known_aliases.contains(name->view())) {
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

    /* An extended-glob opener such as @( still globs against names, so it keeps
       the word off the literal fast path. */
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

  /* A single unquoted segment splits and globs, so it qualifies only without a
     glob metacharacter and without a leading tilde. */
  if (word.segments.count() == 1 &&
      word.segments[0].kind == WordSegment::Kind::UnquotedText)
  {
    const WordSegment &only = word.segments[0];
    if (word_segment_has_glob_metacharacter(only))
      return Word::PlainLiteral::NotPlain;
    if (!only.text.is_empty() && only.text[0] == '~') {
      return Word::PlainLiteral::NotPlain;
    }
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

/* Fold every constant arithmetic expansion in a word once, so the evaluator
   reads the cached value instead of re-parsing on every expansion. */
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
    if (segment.has_folded_arithmetic_result) continue;

    if (arithmetic_has_side_effect(segment.text.view())) {
      actx.constant_variables.clear();
      continue;
    }

    let result = try_fold_constant_arithmetic(segment.text.view());
    if (!result.has_value())
      result = try_fold_arithmetic_with_constants(segment.text.view(), actx);
    if (result.has_value()) {
      LOG(All, "folded the constant arithmetic '%.*s' to %lld",
          static_cast<int>(segment.text.view().length),
          segment.text.view().data, static_cast<long long>(*result));
      segment.set_folded_arithmetic_result(*result);
      did_fold = true;
      actx.optimizer_eliminated_count++;
      if (actx.should_report_optimizer_diagnostics) {
        let const folded = String::from(*result, heap_allocator());
        let const segment_location = segment.get_source_location(None);
        actx.report_diagnostic(diagnostic_id::optimizer_folded_arithmetic,
                               segment_location.has_value() ? *segment_location
                                                            : fallback_location,
                               {segment.text.view(), folded.view()});
      }
    }
  }
  return did_fold;
}

fn fold_constant_arithmetic_in_token(const Token *token,
                                     AnalysisContext &actx) throws -> bool
{
  if (token == nullptr) return false;
  if (token->kind() != Token::Kind::Word) return false;
  return fold_constant_arithmetic_in_word(
      static_cast<const tokens::WordToken *>(token)->word(), actx,
      token->source_location());
}

/* RULE constant-arithmetic folding. */
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
      if (fold_constant_arithmetic_in_word(var.value, actx,
                                           node->source_location()))
        did_fold = true;
    }
    return did_fold;
  }

  return false;
}

/* RULE dead-branch elimination. The branch an if takes is recorded when every
   condition up to it is statically decidable, and the first undecidable
   condition stops the fold. */
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
      if (actx.should_report_optimizer_diagnostics) {
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
  if (actx.should_report_optimizer_diagnostics)
    actx.report_diagnostic(diagnostic_id::optimizer_folded_else,
                           node->source_location());
  return true;
}

/* RULE loop elimination. A while false or until true never runs its body and is
   recorded as skipped. A while true or until false is infinite and stays
   unfolded. */
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
  if (actx.should_report_optimizer_diagnostics)
    actx.report_diagnostic(
        diagnostic_id::optimizer_folded_loop, node->source_location(),
        {loop_node->is_until() ? StringView{"until"} : StringView{"while"}});
  return true;
}

/* RULE compound-body elimination. An if folded to a missing else body runs
   nothing, so the whole if is a proven no-op the evaluator skips. */
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
  if (actx.should_report_optimizer_diagnostics)
    actx.report_diagnostic(diagnostic_id::optimizer_eliminated_if,
                           node->source_location());
  return true;
}

/* RULE empty for-loop elimination. A for with an explicit in clause and no
   words never iterates. */
fn rule_eliminate_empty_for(const Expression *node,
                            AnalysisContext &actx) throws -> bool
{
  const expressions::ForLoop *loop_node = node->as_for_loop();
  if (loop_node == nullptr) return false;
  if (loop_node->is_fully_eliminated()) return false;

  /* A for without an in clause walks the positional parameters, whose count is
     only known at run time. */
  if (!loop_node->has_in_clause()) return false;
  if (!loop_node->words().is_empty()) return false;

  LOG(All, "empty for-loop elimination folded a for with an empty in clause to "
           "a no-op");
  loop_node->set_fully_eliminated();
  actx.optimizer_eliminated_count++;
  if (actx.should_report_optimizer_diagnostics)
    actx.report_diagnostic(diagnostic_id::optimizer_eliminated_for,
                           node->source_location());
  return true;
}

/* RULE C-style for folding. A constant condition with no run-time variable is
   folded to its value, non-zero running as an infinite loop and zero folding
   the loop to a no-op. */
fn rule_fold_cstyle_for(const Expression *node, AnalysisContext &actx) throws
    -> bool
{
  const expressions::CStyleForLoop *loop_node = node->as_cstyle_for_loop();
  if (loop_node == nullptr) return false;
  if (loop_node->has_folded_condition()) return false;

  /* A blank condition is the for ((;;)) infinite form. */
  let const condition = loop_node->condition_clause();
  let const trimmed = trim_arithmetic_whitespace(condition);
  if (trimmed.length == 0) return false;

  /* Inlining a recorded constant for the counter the condition reads would
     freeze the loop at its first verdict, so only an identifier-free condition
     folds. */
  for (usize i = 0; i < trimmed.length; i++) {
    if (lexer::is_variable_name_start(trimmed[i])) {
      LOG(All, "the c-style-for fold declines, the condition reads a variable");
      return false;
    }
  }

  let const value = try_fold_constant_arithmetic(trimmed);
  if (!value.has_value()) return false;

  loop_node->set_folded_condition(*value);
  LOG(All, "folded the c-style for condition '%.*s' to %lld",
      static_cast<int>(trimmed.length), trimmed.data,
      static_cast<long long>(*value));
  actx.optimizer_eliminated_count++;
  if (actx.should_report_optimizer_diagnostics) {
    let const folded = String::from(*value, heap_allocator());
    actx.report_diagnostic(diagnostic_id::optimizer_folded_arithmetic,
                           node->source_location(), {trimmed, folded.view()});
  }

  /* A constant zero condition skips the body, but the init clause still runs
     once the way C semantics require, so only a blank-init loop is a proven
     no-op. */
  let const init_is_blank =
      trim_arithmetic_whitespace(loop_node->init_clause()).length == 0;
  if (*value == 0 && init_is_blank) {
    loop_node->set_fully_eliminated();
    actx.optimizer_eliminated_count++;
    if (actx.should_report_optimizer_diagnostics)
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

/* The pass cap bounds the fixpoint loop, so a rule that reports a change
   without progress cannot loop the driver forever. */
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
