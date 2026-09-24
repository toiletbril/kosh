/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines the history constants shared by both editor
 * configurations. It stays apart from Toiletline.hpp because Toiletline.cpp
 * defines the vendored editor configuration macros itself and cannot include
 * that header.
 */

#pragma once

#include "base/Common.hpp"

namespace toiletline {

/* A history operation that loses a race against another shell rereads the file
   this many times before it reports the failure. */
inline constexpr int HISTORY_RACE_ATTEMPT_COUNT = 3;

/* The decoder writes one record into a fixed buffer. A longer record cannot be
   read back and is rejected before it reaches a file. The vendored
   ITL_STRING_MAX_LEN holds the same value and is visible only to the editor
   implementation. Toiletline.cpp asserts that the two agree. */
#if defined _WIN32
inline constexpr usize HISTORY_RECORD_MAX_DECODED_BYTE_COUNT = 8191;
#else
inline constexpr usize HISTORY_RECORD_MAX_DECODED_BYTE_COUNT = 4095;
#endif

} /* namespace toiletline */
