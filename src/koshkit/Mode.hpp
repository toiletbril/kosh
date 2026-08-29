#pragma once

#include "../Common.hpp"
#include "../Maybe.hpp"
#include "../StringView.hpp"

namespace koshka::koshkit {

fn parse_file_mode(StringView expression, u32 current_mode, u32 creation_mask,
                   bool is_directory) wontthrow -> Maybe<u32>;

}
