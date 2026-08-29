#pragma once

#include "../Common.hpp"
#include "../Maybe.hpp"
#include "../Path.hpp"
#include "../StringView.hpp"

namespace koshka {

class EvalContext;
class ExecContext;

namespace koshkit {

fn resolve_user_id(StringView text) throws -> Maybe<u32>;
fn resolve_group_id(StringView text) throws -> Maybe<u32>;
fn change_path_ownership(const ExecContext &ec, EvalContext &cxt,
                         StringView utility_name, const Path &path,
                         i64 owner_id, i64 group_id, bool should_recurse,
                         bool should_follow_symlink,
                         bool should_follow_nested_symlinks) throws -> bool;

} // namespace koshkit

} // namespace koshka
