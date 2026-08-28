#pragma once

#include "Arena.hpp"
#include "Bitset.hpp"
#include "Builtin.hpp"
#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"
#include "EvalSnapshot.hpp"
#include "EvalTypes.hpp"
#include "ExecContext.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ProgramResolver.hpp"
#include "ResolvedCommand.hpp"
#include "RuntimeState.hpp"

namespace koshka {

class EvalContext;

/* Parse and evaluate a constant arithmetic expression with no evaluation
   context. The optimizer's constant fold calls this once the byte scan proves
   the source holds no variable and no substitution, so the parser never
   dereferences a context. A malformed constant, such as a division by zero,
   throws. */
fn evaluate_constant_arithmetic(StringView expression) throws -> i64;
fn evaluate_constant_arithmetic_nonzero(StringView expression,
                                        bool is_exact) throws -> bool;

fn find_substring_length_separator(StringView body) wontthrow -> usize;

/* The abort the set -u read and the ${name:?} report perform even in the bash
   mood. */
[[noreturn]] fn throw_script_fatal(StringView message,
                                   StringView note = {}) throws -> void;

/* Source the startup files for each mood in the list, in order, the way the
   --init-moods flag and the set --init-moods builtin both ask. A kosh flavor
   reads /etc/koshrc and ~/.koshrc, a bash flavor the bash rc and completion, a
   posix flavor the ENV file, and each adds its login profiles when is_login is
   set. */
fn source_init_moods(EvalContext &context, BumpArena &ast_arena,
                     const ArrayList<mimic_mood> &moods, bool is_login_shell,
                     bool should_be_interactive) throws -> void;

} /* namespace koshka */
