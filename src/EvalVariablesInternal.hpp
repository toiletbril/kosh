#pragma once

#include "StringView.hpp"

namespace koshka::internal {

inline constexpr StringView CONNECT_NAMED_PIPE{
    "KOSH_INTERNAL_CONNECT_NAMED_PIPE"};
inline constexpr StringView PREVIOUS_EXIT_STATUS{
    "KOSH_INTERNAL_PREVIOUS_EXIT_STATUS"};
inline constexpr StringView PARENT_PROCESS_ID{
    "KOSH_INTERNAL_PARENT_PROCESS_ID"};
inline constexpr StringView SHELL_PROCESS_ID{"KOSH_INTERNAL_SHELL_PROCESS_ID"};
inline constexpr StringView STATE_NAMED_PIPE{"KOSH_INTERNAL_STATE_NAMED_PIPE"};
inline constexpr StringView SUBSHELL_DEPTH{"KOSH_INTERNAL_SUBSHELL_DEPTH"};
inline constexpr StringView SUPPRESS_ROOT_TRACE{
    "KOSH_INTERNAL_SUPPRESS_ROOT_TRACE"};

inline constexpr wchar_t CONNECT_NAMED_PIPE_WIDE[] =
    L"KOSH_INTERNAL_CONNECT_NAMED_PIPE";
inline constexpr wchar_t DIAGNOSTIC_MARKER_WIDE[] =
    L"KOSH_INTERNAL_DIAGNOSTIC_MARKER";
inline constexpr wchar_t PARENT_PROCESS_ID_WIDE[] =
    L"KOSH_INTERNAL_PARENT_PROCESS_ID";
inline constexpr wchar_t STATE_NAMED_PIPE_WIDE[] =
    L"KOSH_INTERNAL_STATE_NAMED_PIPE";

} /* namespace koshka::internal */
