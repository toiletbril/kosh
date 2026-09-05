/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the mode utility in koshkit.
 * It owns the utility command semantics and status behavior.
 */

#pragma once

#include "../Common.hpp"
#include "../Maybe.hpp"
#include "../StringView.hpp"

namespace koshka::koshkit {

fn parse_file_mode(StringView expression, u32 current_mode, u32 creation_mask,
                   bool is_directory) wontthrow -> Maybe<u32>;

}
