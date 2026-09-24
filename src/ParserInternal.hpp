/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares small token predicates and masks shared by parser source
 * files. It keeps hot parser helpers inline without adding them to the public
 * Parser interface.
 */

#pragma once

#include "base/Common.hpp"
#include "base/StringView.hpp"

namespace koshka {

class Token;
class SegmentText;

namespace internal {

template <typename... Kinds>
consteval fn token_kind_mask(Kinds... kinds) -> u64
{
  return ((u64{1} << static_cast<u8>(kinds)) | ... | u64{0});
}

hot pure fn get_unquoted_word_text(const Token *token) wontthrow
    -> const SegmentText *;
hot pure fn is_unquoted_word(const Token *token, StringView expected) wontthrow
    -> bool;
cold [[noreturn]] fn throw_unterminated(const SourceLocation &opener,
                                        StringView what, StringView source,
                                        StringView keyword,
                                        SourceLocation fallback) throws -> void;

} /* namespace internal */

} /* namespace koshka */
