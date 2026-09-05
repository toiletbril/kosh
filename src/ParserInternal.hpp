#pragma once

#include "Common.hpp"
#include "StringView.hpp"

namespace koshka {

class Token;

namespace internal {

template <typename... Kinds>
consteval fn token_kind_mask(Kinds... kinds) -> u64
{
  return ((u64{1} << static_cast<u8>(kinds)) | ... | u64{0});
}

hot pure fn is_unquoted_word(const Token *token, StringView expected) wontthrow
    -> bool;
cold [[noreturn]] fn throw_unterminated(const SourceLocation &opener,
                                        StringView what, StringView source,
                                        StringView keyword,
                                        SourceLocation fallback) throws -> void;

} /* namespace internal */

} /* namespace koshka */
