#pragma once

/* The analyze-time optimizer. The prepass calls into this unit to prove a fact
   about a node once, before any command runs, so the evaluator skips work on
   every later execution of that node. The unit is a small transformation-rule
   engine in the spirit of a Volcano/Cascades rewrite phase. The shell builds
   one plan rather than competing plans, so there is no cost model, only the
   rewrite half. Each optimization is a RULE, a match-and-rewrite over the arena
   AST that either annotates a node and reports it changed or leaves the node
   untouched. The driver applies the rules to a node in passes until no rule
   fires, a fixpoint, with a bounded iteration count so a buggy rule cannot
   loop. The node stores a rule's decision in its own cache field and the
   evaluator reads it, so this unit holds the algorithms and the rules and the
   nodes hold the storage and the dispatch. */

#include "Common.hpp"
#include "Containers.hpp"
#include "Maybe.hpp"
#include "Tokens.hpp"

namespace koshka {

class Token;
class Expression;
class AnalysisContext;

namespace optimizer {

/* Evaluate an arithmetic expression that holds only literals and operators,
   with no variable and no substitution, so the result is a compile-time
   constant. The analyze pass calls this to fold a constant $((...)) once
   instead of leaving the evaluator to re-parse it on every expansion. Returns
   None when the expression is not provably constant or fails to evaluate. */
fn try_fold_constant_arithmetic(StringView expression) wontthrow -> Maybe<i64>;

/* The statically-decidable success of a simple command used as a loop or if
   condition. The caller passes the command words and the analysis context after
   the node has cleared its own guards, a redirection, an async or negated
   prefix, and a prefix assignment. Some(true) means the command always succeeds
   with status 0 and no side effect, Some(false) means it always fails, and None
   means the result is only known at run time. The verdict comes from a constant
   builtin such as true, false, or a literal test. A $name operand that resolves
   in the analysis context's constant_variables table is read as its recorded
   literal, so a propagated constant decides the verdict. */
fn simple_command_static_verdict(const ArrayList<const Token *> &args,
                                 const AnalysisContext &actx) throws
    -> Maybe<bool>;

/* True when the segment text holds an unquoted glob metacharacter, one of '*',
   '?', or '['. The plain-literal fast path in the evaluator consults this to
   decide whether a word may skip pathname expansion. */
pure fn word_segment_has_glob_metacharacter(
    const WordSegment &segment) wontthrow -> bool;

/* How a word may take the evaluator's plain-literal fast path. The fast path in
   process_args reads this to push a word that needs no expansion, splitting, or
   globbing straight to the argument vector. */
pure fn classify_plain_literal(const Word &word) wontthrow
    -> Word::PlainLiteral;

/* The fully literal value of a word when every segment is literal or
   double-quoted text, the bytes the analyze pass can prove without running the
   command. None when any segment is a variable, a command substitution, an
   arithmetic expansion, or an unquoted glob. The constant-propagation rule
   reads this to decide whether an assignment records a constant value. The
   token form returns None for a token that is not a plain word. */
fn literal_word_value(const Word &word) throws -> Maybe<String>;
fn literal_word_value(const Token *token) throws -> Maybe<String>;

pure fn is_plain_variable_name(StringView name) wontthrow -> bool;

/* The single plain variable name a word references, when the word is exactly
   one VariableReference segment whose text is a bare name with no parameter
   expansion modifier. None otherwise. The constant-propagation rule reads this
   to recognise a $name argument it may replace with a recorded constant. */
fn plain_variable_reference_name(const Token *token) wontthrow
    -> Maybe<StringView>;

/* The constant value of a word after constant propagation, the literal value
   when the word is already literal, or the recorded constant when the word is a
   bare $name that resolves in the constant_variables table. None when the word
   has no statically-known value. */
fn propagated_literal_word_value(const Token *token,
                                 const AnalysisContext &actx) throws
    -> Maybe<String>;

/* Fold a constant arithmetic expansion to its decimal result, first replacing
   each identifier that names a recorded constant integer with its value. With
   x recorded as 2 the expression x+x folds to 4. Returns None when an
   identifier is not a recorded integer constant or the result is not provably
   constant. */
fn try_fold_arithmetic_with_constants(StringView expression,
                                      const AnalysisContext &actx) wontthrow
    -> Maybe<i64>;

/* True when the command name is proven not to mutate the shell environment, one
   of the read-only builtins or read-only coreutils in the static table. The
   constant-propagation bookkeeping reads this to keep a recorded constant valid
   across such a command rather than forgetting the whole table. The caller
   still clears the table on a command substitution, a shadowing function or
   alias, or any env-mutating command outside the table. */
fn command_is_environment_neutral(StringView name) throws -> bool;

/* Run the transformation rules over one node to a fixpoint. The recursive
   analyze walk calls this on each node after it has analyzed the node's
   children and populated the context. The driver re-applies the rules until a
   pass fires no rule or the bounded iteration count is reached. */
fn optimize_node(const Expression *node, AnalysisContext &actx) throws -> void;

} /* namespace optimizer */

} /* namespace koshka */
