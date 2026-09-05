/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements language-server document state and semantic
 * operations. It coordinates analysis, completion, hover, symbols,
 * definitions, renames, formatting, and diagnostics.
 */

#pragma once

#include "Arena.hpp"
#include "Eval.hpp"

namespace koshka::language_server {

fn run(EvalContext &context, BumpArena &ast_arena) throws -> int;

}
