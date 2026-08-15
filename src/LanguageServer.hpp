#pragma once

#include "Arena.hpp"
#include "Eval.hpp"

namespace koshka::language_server {

fn run(EvalContext &context, BumpArena &ast_arena) throws -> int;

}
