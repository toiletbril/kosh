/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the file-mode parser shared by chmod and mkfifo. The
 * separate interface keeps symbolic and octal semantics consistent between
 * both utilities.
 */

#pragma once

#include "../Common.hpp"
#include "../Maybe.hpp"
#include "../StringView.hpp"

namespace koshka::koshkit {

fn parse_file_mode(StringView expression, u32 current_mode, u32 creation_mask,
                   bool is_directory) wontthrow -> Maybe<u32>;

}
