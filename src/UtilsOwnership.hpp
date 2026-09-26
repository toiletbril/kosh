/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares shared user, group, and recursive ownership helpers used
 * by the ownership-related koshkit utilities.
 */

#pragma once

#include "base/Common.hpp"
#include "base/Maybe.hpp"
#include "base/Path.hpp"
#include "base/StringView.hpp"

namespace koshka {

class EvalContext;
class ExecContext;

namespace utils {

enum class ownership_traversal_mode : u8
{
  SinglePath,
  Recursive,
};

fn resolve_user_id(StringView text) throws -> Maybe<u32>;
fn resolve_group_id(StringView text) throws -> Maybe<u32>;
fn change_path_ownership(const ExecContext &ec, EvalContext &cxt,
                         StringView utility_name, const Path &path,
                         i64 owner_id, i64 group_id,
                         ownership_traversal_mode traversal_mode,
                         bool should_not_dereference,
                         usize command_line_follow_position,
                         usize follow_position, usize physical_position) throws
    -> bool;

} /* namespace utils */

} /* namespace koshka */
