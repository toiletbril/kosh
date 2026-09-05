/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the shell interface to the vendored interactive
 * editor. It owns prompts, history, completion, highlighting, key handling,
 * and terminal-state integration. This implementation is compiled when the
 * editor is enabled, and ToiletlineStubs provides the same interface for
 * KOSH_NO_TOILETLINE builds.
 */

/* The toiletline configuration macros are defined here, so Toiletline.hpp is
   not included. */

#include "Allocator.hpp"
#include "Cli.hpp"
#include "CliColors.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace toiletline {

enum class history_duplicate_policy : u8
{
  Allow,
  IgnoreConsecutive,
  ErasePrevious,
  IgnoreConsecutiveAndErasePrevious,
};

enum class edit_mode : u8
{
  Emacs,
  Vi,
};

fn byte_offset_of_codepoint(const char *bytes, usize byte_length,
                            usize codepoint_index) -> usize;

} /* namespace toiletline */

#if !defined KOSH_NO_TOILETLINE

namespace {

constexpr usize TL_ALLOC_HEADER = 16;

fn tl_block_base(opaque *payload) -> char *
{
  return static_cast<char *>(payload) - TL_ALLOC_HEADER;
}

fn tl_block_capacity(opaque *payload) -> usize &
{
  return *reinterpret_cast<usize *>(tl_block_base(payload));
}

fn tl_arena_malloc(usize length) -> opaque *
{
  if (length > static_cast<usize>(-1) - TL_ALLOC_HEADER) return nullptr;

  let const allocation_length = length + TL_ALLOC_HEADER;
  let const base =
      koshka::heap_allocator().alloc_array<char>(allocation_length);
  if (base != NULL) {
    *reinterpret_cast<usize *>(base) = length;
    return base + TL_ALLOC_HEADER;
  }

  return NULL;
}

fn tl_arena_free(opaque *pointer) -> void
{
  if (pointer == nullptr) return;

  let const allocation_length = tl_block_capacity(pointer) + TL_ALLOC_HEADER;
  koshka::heap_allocator().free_array(tl_block_base(pointer),
                                      allocation_length);
}

fn tl_arena_realloc(opaque *pointer, usize length) -> opaque *
{
  if (pointer == nullptr) return tl_arena_malloc(length);

  let const old_capacity = tl_block_capacity(pointer);
  if (old_capacity >= length) return pointer;

  let const fresh = tl_arena_malloc(length);
  std::memcpy(fresh, pointer, old_capacity);
  tl_arena_free(pointer);

  return fresh;
}

#define TL_MALLOC  tl_arena_malloc
#define TL_REALLOC tl_arena_realloc
#define TL_FREE    tl_arena_free
#define TL_ABORT() std::abort()

#define TL_NO_SUSPEND
#define TL_ASSERT           ASSERT
#define TL_HISTORY_MAX_SIZE (1024 * 4)

} /* namespace */

#define TOILETLINE_IMPLEMENTATION
/* A release build makes TL_ASSERT a no-op, leaving some vendored helpers
 * unused. */
#if defined __clang__ || defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "toiletline/toiletline.h"
#if defined __clang__ || defined __GNUC__
#pragma GCC diagnostic pop
#endif

namespace {

koshka::EvalContext *COMPLETION_CONTEXT = nullptr;
const koshka::Path *COMPLETION_BASE_DIRECTORY = nullptr;
bool HIGHLIGHT_COLOR_ENABLED = false;
bool HIGHLIGHT_STYLED_UNDERLINES_ENABLED = false;
#if !defined NDEBUG
usize DEBUG_COMPLETION_CWD_CAPTURE_COUNT = 0;
usize DEBUG_COMPLETION_SOURCE_SCAN_COUNT = 0;
usize DEBUG_COMPLETION_MATERIALIZED_COUNT = 0;
#endif

koshka::ArrayList<const char *> COMPLETION_CANDIDATE_POINTERS{
    koshka::heap_allocator()};
koshka::ArrayList<const char *> COMPLETION_DESCRIPTION_POINTERS{
    koshka::heap_allocator()};
koshka::completion::completion_result *COMPLETION_RESULT = nullptr;

/* Toiletline edits in codepoints while the completion engine works in bytes. */
fn kosh_completion_callback(const char *buffer, size_t cursor,
                            tl_completion *out, int for_listing) -> int
{
  if (COMPLETION_CONTEXT == nullptr || COMPLETION_BASE_DIRECTORY == nullptr ||
      COMPLETION_RESULT == nullptr)
  {
    return 0;
  }

  /* Toiletline calls this through a C function pointer, so a throw unwinding
     past this frame is undefined behavior. The body is guarded and any throw is
     swallowed. */
  try {
    let const is_explicit_completion = for_listing != 0;
    if (is_explicit_completion)
      COMPLETION_CONTEXT->get_program_resolver().begin_explicit_completion(
          koshka::ProgramResolver::CompletionRefresh::Cached);
    defer
    {
      if (is_explicit_completion)
        COMPLETION_CONTEXT->get_program_resolver().end_explicit_completion();
    };

    const usize byte_length = std::strlen(buffer);
    let line = koshka::StringView{buffer, byte_length};

    const usize byte_cursor =
        toiletline::byte_offset_of_codepoint(buffer, byte_length, cursor);

    /* A completion diagnostic is armed to break onto its own line, then
       disarmed so a later command's message is unaffected. */
    koshka::arm_message_leading_newline(true);
    COMPLETION_RESULT->candidates.clear();
    COMPLETION_RESULT->descriptions.clear();
    COMPLETION_RESULT->longest_common_prefix.clear();
    *COMPLETION_RESULT = koshka::completion::complete(
        line, byte_cursor, *COMPLETION_CONTEXT, *COMPLETION_BASE_DIRECTORY,
        for_listing != 0 ? koshka::completion::completion_mode::Listing
                         : koshka::completion::completion_mode::Ghost);
    let const &result = *COMPLETION_RESULT;
#if !defined NDEBUG
    DEBUG_COMPLETION_SOURCE_SCAN_COUNT += result.source_candidate_scan_count;
    DEBUG_COMPLETION_MATERIALIZED_COUNT += result.materialized_candidate_count;
#endif
    koshka::arm_message_leading_newline(false);

    if (result.candidate_count == 0) return 0;

    COMPLETION_CANDIDATE_POINTERS.clear();
    if (for_listing != 0) {
      COMPLETION_CANDIDATE_POINTERS.reserve(result.candidates.count());
      for (let const &candidate : result.candidates)
        COMPLETION_CANDIDATE_POINTERS.push(candidate.c_str());
    }

    /* The candidate text keys the description lookup, and the build is skipped
       when none was produced. */
    COMPLETION_DESCRIPTION_POINTERS.clear();
    out->descriptions = nullptr;
    if (for_listing != 0 && result.descriptions.count() > 0) {
      COMPLETION_DESCRIPTION_POINTERS.reserve(result.candidates.count());
      for (let const &candidate : result.candidates) {
        if (const koshka::String *found_description =
                result.descriptions.find(candidate.view());
            found_description != nullptr)
          COMPLETION_DESCRIPTION_POINTERS.push(found_description->c_str());
        else
          COMPLETION_DESCRIPTION_POINTERS.push("");
      }
      out->descriptions = COMPLETION_DESCRIPTION_POINTERS.begin();
    }

    out->candidates =
        for_listing != 0 ? COMPLETION_CANDIDATE_POINTERS.begin() : nullptr;
    out->count = result.candidate_count;
    out->longest_common_prefix = result.longest_common_prefix.c_str();
    /* The engine reports the span in bytes, converted to codepoint indices. */
    out->token_start = ::tl_utf8_strnlen(buffer, result.token_start);
    out->token_end = ::tl_utf8_strnlen(buffer, result.token_end);

    return 1;
  } catch (koshka::ErrorBase &error) {
    /* A throw skips the disarm above, so it runs here too. */
    koshka::arm_message_leading_newline(false);
    LOG(Debug, "completion swallowed an error: %s", error.message().c_str());
    return 0;
  } catch (...) {
    koshka::arm_message_leading_newline(false);
    LOG(Debug, "completion swallowed an unknown throw");
    return 0;
  }
}

/* The body is guarded since toiletline calls through a C function pointer. */
fn kosh_highlight_callback(const char *buffer, tl_highlight *out) -> int
{
  if (COMPLETION_CONTEXT == nullptr) return 0;
  if (!HIGHLIGHT_COLOR_ENABLED) return 0;

  try {
    const usize byte_length = std::strlen(buffer);
    let line = koshka::StringView{buffer, byte_length};

    koshka::ArrayList<koshka::highlight_span> result =
        koshka::completion::highlight_line(line, *COMPLETION_CONTEXT);
    let const &theme = HIGHLIGHT_STYLED_UNDERLINES_ENABLED
                           ? koshka::colors::SHELL_HIGHLIGHT_THEME
                           : koshka::colors::NONINTERACTIVE_HIGHLIGHT_THEME;

    size_t filled = 0;
    usize byte_position = 0;
    usize codepoint_position = 0;
    for (let const &span : result) {
      if (filled >= out->capacity) break;
      while (byte_position < span.start) {
        if ((static_cast<unsigned char>(buffer[byte_position]) & 0xC0) != 0x80)
          codepoint_position++;
        byte_position++;
      }
      out->spans[filled].start = codepoint_position;
      while (byte_position < span.end) {
        if ((static_cast<unsigned char>(buffer[byte_position]) & 0xC0) != 0x80)
          codepoint_position++;
        byte_position++;
      }
      out->spans[filled].end = codepoint_position;
      let const style = theme.style_for(span.role);
      if (style.is_empty()) continue;
      out->spans[filled].sgr = style.data;
      filled++;
    }
    out->count = filled;
    return filled > 0 ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

koshka::EvalContext *JOB_CONTEXT = nullptr;
koshka::String WAKE_NOTIFICATION_STASH{koshka::heap_allocator()};

/* The two-phase wake hook for set -b. Phase 0 formats the Done rows, phase 1
   prints them after the editor cleared its render block. The body is guarded
   since toiletline calls through a C function pointer. */
fn kosh_wake_callback(int phase) -> int
{
  try {
    if (phase == 0) {
      if (koshka::os::CHILD_STATE_CHANGED == 0) return 0;
      /* The flag clears only when this hook consumes it. */
      if (JOB_CONTEXT == nullptr || !JOB_CONTEXT->notify()) return 0;
      koshka::os::CHILD_STATE_CHANGED = 0;
      WAKE_NOTIFICATION_STASH =
          JOB_CONTEXT->format_done_job_notifications("\r\n");
      return WAKE_NOTIFICATION_STASH.is_empty() ? 0 : 1;
    }
    koshka::print_error(WAKE_NOTIFICATION_STASH.view());
    koshka::flush();
    WAKE_NOTIFICATION_STASH.clear();
    return 0;
  } catch (...) {
    return 0;
  }
}

/* An entry whose command word no longer resolves is rejected. A throw accepts
   the entry. */
fn kosh_ghost_validate_callback(const char *entry) -> int
{
  if (COMPLETION_CONTEXT == nullptr) return 1;
  try {
    const usize byte_length = std::strlen(entry);
    return koshka::completion::command_word_resolves(
               koshka::StringView{entry, byte_length}, *COMPLETION_CONTEXT)
               ? 1
               : 0;
  } catch (...) {
    return 1;
  }
}

} /* namespace */

namespace toiletline {

using koshka::EvalContext;
using koshka::Maybe;
using koshka::Path;
using koshka::String;
using koshka::StringView;
namespace colors = koshka::colors;
namespace os = koshka::os;
namespace utils = koshka::utils;

struct input_result
{
  i32 code;
  String text;
  koshka::Maybe<usize> history_event_number{koshka::None};
};

static char TL_BUFFER[ITL_STRING_MAX_LEN];

static constexpr char KOSH_HISTORY_FILE[] = ".kosh_history";

static fn resolve_history_path(StringView env_name, StringView default_file)
    -> koshka::Maybe<koshka::Path>
{
  if (let const override_path = koshka::os::get_environment_variable(env_name);
      override_path.has_value() && !override_path->is_empty())
  {
    return koshka::Path{override_path->view()};
  }
  let home = koshka::os::get_home_directory();
  if (!home.has_value()) return koshka::None;
  let path = home->clone();
  path.push_component(default_file);
  return path;
}

static constexpr char KOSH_CALC_HISTORY_FILE[] = ".kosh_calc_history";

static fn history_file_path() -> koshka::Maybe<koshka::Path>
{
  return resolve_history_path("KOSH_HISTORY", KOSH_HISTORY_FILE);
}

static fn calc_history_file_path() -> koshka::Maybe<koshka::Path>
{
  return resolve_history_path("KOSH_CALC_HISTORY", KOSH_CALC_HISTORY_FILE);
}

/* The history is swapped to the calc file on entry and back on leave, so the
   two histories never mix. The shell history is dumped first for a later
   reload. */
fn enter_calc_history() -> void
{
  if (koshka::Maybe<koshka::Path> shell = history_file_path();
      shell.has_value())
    ::tl_history_dump(shell->c_str());

  if (koshka::Maybe<koshka::Path> calc = calc_history_file_path();
      calc.has_value())
    ::tl_history_load(calc->c_str());
}

fn leave_calc_history() -> void
{
  if (koshka::Maybe<koshka::Path> calc = calc_history_file_path();
      calc.has_value())
    ::tl_history_dump(calc->c_str());

  if (koshka::Maybe<koshka::Path> shell = history_file_path();
      shell.has_value())
    ::tl_history_load(shell->c_str());
}

fn history_path() -> koshka::Maybe<koshka::Path> { return history_file_path(); }

fn history_write() -> bool
{
  if (!::itl_g_is_active) return true;
  let const path = history_file_path();
  if (!path.has_value()) return false;
  int status = ::tl_history_dump(path->c_str());
  return status == TL_SUCCESS || status == -EINVAL;
}

fn history_read() -> bool
{
  let const path = history_file_path();
  if (!path.has_value()) return false;
  return ::tl_history_load(path->c_str()) == TL_SUCCESS;
}

fn history_clear() -> bool
{
  let const path = history_file_path();
  if (!path.has_value()) return false;
  let opened = koshka::os::open_file_descriptor(
      path->text().view(), koshka::os::file_open_mode::Truncate);
  if (!opened.has_value()) return false;
  koshka::os::close_fd(opened.take());
  ::tl_history_load(path->c_str());
  return true;
}

fn set_history_enabled(bool is_enabled) -> void
{
  ::tl_set_history_enabled(is_enabled);
}

fn set_history_limit(usize entry_count) -> void
{
  ::tl_set_history_limit(entry_count);
}

struct history_event
{
  usize number;
  String command;
};

fn history_events(koshka::Allocator allocator)
    -> koshka::ArrayList<history_event>
{
  let events = koshka::ArrayList<history_event>{allocator};
  if (!history_read() || ::itl_g_history_count == 0) return events;
  if (!::itl_history_ensure_read_buffer()) return events;

  let const first_number =
      ::itl_g_history_total_count - ::itl_g_history_count + 1;
  char decoded[ITL_STRING_MAX_LEN + 1];

  for (usize index = 0; index < ::itl_g_history_count; index++) {
    usize decoded_size = 0;
    if (!::itl_history_decode_entry_buffered(
            ::itl_history_index_to_offset(index), decoded, sizeof(decoded),
            &decoded_size))
    {
      events.clear();
      return events;
    }

    events.push(history_event{
        first_number + index,
        koshka::String{allocator, koshka::StringView{decoded, decoded_size}}
    });
  }

  return events;
}

template <class Match>
static fn find_history_event(koshka::Allocator allocator,
                             koshka::Maybe<usize> before_event_number,
                             Match do_match) -> koshka::Maybe<history_event>
{
  if (::itl_g_history_path == nullptr && !history_read()) return koshka::None;
  if (::itl_g_history_count == 0) return koshka::None;
  if (!::itl_history_ensure_read_buffer()) return koshka::None;

  let const first_number =
      ::itl_g_history_total_count - ::itl_g_history_count + 1;
  char decoded[ITL_STRING_MAX_LEN + 1];
  for (usize index = ::itl_g_history_count; index > 0; index--) {
    let const number = first_number + index - 1;
    if (before_event_number.has_value() && number >= *before_event_number)
      continue;

    usize decoded_size = 0;
    if (!::itl_history_decode_entry_buffered(
            ::itl_history_index_to_offset(index - 1), decoded, sizeof(decoded),
            &decoded_size))
    {
      return koshka::None;
    }
    let const command = koshka::StringView{decoded, decoded_size};
    if (do_match(number, command))
      return history_event{
          number, koshka::String{allocator, command}
      };
  }

  return koshka::None;
}

fn relative_history_event(koshka::Allocator allocator, usize distance,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  if (distance == 0) return koshka::None;
  usize remaining_event_count = distance;
  return find_history_event(
      allocator, before_event_number,
      [&](usize, StringView) { return --remaining_event_count == 0; });
}

fn numbered_history_event(koshka::Allocator allocator, usize wanted_number,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return find_history_event(
      allocator, before_event_number,
      [&](usize number, StringView) { return number == wanted_number; });
}

fn prefixed_history_event(koshka::Allocator allocator, StringView prefix,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return find_history_event(
      allocator, before_event_number,
      [&](usize, StringView command) { return command.starts_with(prefix); });
}

fn containing_history_event(koshka::Allocator allocator, StringView text,
                            koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return find_history_event(allocator, before_event_number,
                            [&](usize, StringView command) {
                              return command.find_substring(text).has_value();
                            });
}

static fn newest_history_event_equals(StringView command) -> bool
{
  if (!history_read() || ::itl_g_history_count == 0 ||
      !::itl_history_ensure_read_buffer())
  {
    return false;
  }

  char decoded[ITL_STRING_MAX_LEN + 1];
  usize decoded_size = 0;
  let const newest_offset =
      ::itl_history_index_to_offset(::itl_g_history_count - 1);
  return ::itl_history_decode_entry_buffered(newest_offset, decoded,
                                             sizeof(decoded), &decoded_size) &&
         StringView{decoded, decoded_size} == command;
}

static fn erase_previous_history_events(StringView command) -> bool
{
  let const path = history_file_path();
  if (!path.has_value() || !history_read()) return false;
  if (::itl_g_history_count == 0) return true;
  if (!::itl_history_ensure_read_buffer()) return false;

  let const contents = path->read_entire_file();
  if (!contents.has_value()) return false;
  let rewritten = String{koshka::heap_allocator()};
  rewritten.reserve(contents->count());
  rewritten.append(
      contents->substring_of_length(0, ::itl_history_index_to_offset(0)));

  bool did_remove = false;
  char decoded[ITL_STRING_MAX_LEN + 1];
  for (usize index = 0; index < ::itl_g_history_count; index++) {
    let const start_offset = ::itl_history_index_to_offset(index);
    let const end_offset = index + 1 < ::itl_g_history_count
                               ? ::itl_history_index_to_offset(index + 1)
                               : contents->count();
    usize decoded_size = 0;
    if (!::itl_history_decode_entry_buffered(start_offset, decoded,
                                             sizeof(decoded), &decoded_size))
    {
      return false;
    }
    if (StringView{decoded, decoded_size} == command) {
      did_remove = true;
      continue;
    }
    rewritten.append(
        contents->substring_of_length(start_offset, end_offset - start_offset));
  }

  if (!did_remove) return true;
  let replacement_path = os::write_to_named_temp_file(
      path->parent(), ".kosh_history_erasedups", rewritten.view());
  if (!replacement_path.has_value()) return false;
  defer { unused(os::remove_file(replacement_path->text().view())); };

  let const current_contents = path->read_entire_file();
  if (!current_contents.has_value() ||
      current_contents->view() != contents->view())
  {
    return false;
  }
  if (!os::rename_path(replacement_path->text().view(), path->text().view()))
    return false;
  return ::tl_history_load(path->c_str()) == TL_SUCCESS;
}

fn history_append_event(StringView command,
                        history_duplicate_policy duplicate_policy)
    -> koshka::Maybe<usize>
{
  if (command.is_empty() || command.length > ITL_HISTORY_ENTRY_MAX_BYTES) {
    return koshka::None;
  }

  let const path = history_file_path();
  if (!path.has_value()) return koshka::None;
  if (::itl_g_history_path == nullptr) unused(::tl_history_load(path->c_str()));

  let const should_erase_previous =
      duplicate_policy == history_duplicate_policy::ErasePrevious ||
      duplicate_policy ==
          history_duplicate_policy::IgnoreConsecutiveAndErasePrevious;
  if (duplicate_policy ==
          history_duplicate_policy::IgnoreConsecutiveAndErasePrevious &&
      newest_history_event_equals(command))
  {
    return koshka::None;
  }
  if (should_erase_previous && !erase_previous_history_events(command))
    return koshka::None;

  itl_string_t *entry = ::itl_string_alloc();
  defer { ITL_STRING_FREE(entry); };
  if (!::itl_string_from_bytes(entry, command.data, command.length))
    return koshka::None;
  let const should_allow_duplicate =
      duplicate_policy != history_duplicate_policy::IgnoreConsecutive;
  if (!::itl_history_append_to_file(entry, false, should_allow_duplicate))
    return koshka::None;

  return ::itl_g_last_history_event_number;
}

fn history_rewrite_event(usize number, StringView expected,
                         const koshka::ArrayList<koshka::String> &replacements)
    -> bool;

fn history_rewrite_event(usize number, StringView expected,
                         StringView replacement) -> bool
{
  let replacements =
      koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  if (!replacement.is_empty())
    replacements.push(koshka::String{koshka::heap_allocator(), replacement});

  return history_rewrite_event(number, expected, replacements);
}

fn history_rewrite_event(usize number, StringView expected,
                         const koshka::ArrayList<koshka::String> &replacements)
    -> bool
{
  let const path = history_file_path();
  if (!path.has_value() || !history_read()) return false;
  if (::itl_g_history_count == 0) return false;

  let const first_number =
      ::itl_g_history_total_count - ::itl_g_history_count + 1;
  if (number < first_number || number >= first_number + ::itl_g_history_count) {
    return false;
  }

  let const index = number - first_number;
  let const start_offset = ::itl_history_index_to_offset(index);
  let const contents = path->read_entire_file();
  if (!contents.has_value()) return false;
  usize end_offset = start_offset;
  while (end_offset < contents->count() && (*contents)[end_offset] != '\n')
    end_offset++;
  if (end_offset >= contents->count()) return false;
  end_offset++;

  char decoded[ITL_STRING_MAX_LEN + 1];
  usize decoded_size = 0;
  if (!::itl_history_ensure_read_buffer() ||
      !::itl_history_decode_entry_buffered(start_offset, decoded,
                                           sizeof(decoded), &decoded_size) ||
      koshka::StringView{decoded, decoded_size} != expected)
  {
    return false;
  }

  let rewritten = koshka::String{koshka::heap_allocator()};
  rewritten.append(contents->substring_of_length(0, start_offset));

  for (let const &replacement : replacements) {
    if (replacement.count() > ITL_HISTORY_ENTRY_MAX_BYTES) return false;
    itl_string_t *entry = ::itl_string_alloc();
    defer { ITL_STRING_FREE(entry); };
    if (!::itl_string_from_bytes(entry, replacement.data(),
                                 replacement.count()))
      return false;
    itl_char_buf_t *encoded = ::itl_char_buf_alloc();
    defer { ITL_CHAR_BUF_FREE(encoded); };
    ::itl_char_buf_append_string_escaped(encoded, entry);
    rewritten.append(koshka::StringView{encoded->data, encoded->size});
    rewritten.push('\n');
  }

  rewritten.append(contents->substring(end_offset));
  let replacement_path = koshka::os::write_to_named_temp_file(
      path->parent(), ".kosh_history_fc", rewritten.view());
  if (!replacement_path.has_value()) return false;
  defer { unused(koshka::os::remove_file(replacement_path->text().view())); };

  let const current_contents = path->read_entire_file();
  if (!current_contents.has_value() ||
      current_contents->view() != contents->view())
  {
    return false;
  }
  if (!koshka::os::rename_path(replacement_path->text().view(),
                               path->text().view()))
  {
    return false;
  }
  return ::tl_history_load(path->c_str()) == TL_SUCCESS;
}

static fn strip_ansi_color(StringView text) throws -> String;

fn set_title(StringView title) -> void
{
  let const output = koshka::colors::stdout_is_a_terminal()   ? KOSH_STDOUT
                     : koshka::colors::stderr_is_a_terminal() ? KOSH_STDERR
                                                              : KOSH_INVALID_FD;
  if (output == KOSH_INVALID_FD) return;

  static constexpr usize MAX_TITLE_LENGTH = 4096;
  let sequence = String{koshka::heap_allocator()};
  sequence.reserve(title.count() < MAX_TITLE_LENGTH ? title.count() + 5
                                                    : MAX_TITLE_LENGTH + 5);
  sequence += "\x1b]0;";
  bool was_space_appended = false;
  for (usize position = 0;
       position < title.count() && sequence.count() < MAX_TITLE_LENGTH + 4;)
  {
    let const byte = title[position];
    let const unsigned_byte = static_cast<unsigned char>(byte);
    if (byte == '\t' || byte == '\n' || byte == '\r') {
      if (!was_space_appended) sequence.push(' ');
      was_space_appended = true;
      position++;
      continue;
    }
    if (byte == '\x1b' && position + 1 < title.count() &&
        title[position + 1] == '[')
    {
      let end_position = position + 2;
      while (end_position < title.count() &&
             (title[end_position] < '@' || title[end_position] > '~'))
      {
        end_position++;
      }
      if (end_position < title.count() && title[end_position] == 'm') {
        position = end_position + 1;
        continue;
      }
    }
    if (unsigned_byte < 0x20 || unsigned_byte == 0x7f) {
      position++;
      continue;
    }
    if (unsigned_byte < 0x80) {
      sequence.push(byte);
      was_space_appended = byte == ' ';
      position++;
      continue;
    }

    usize codepoint_length = 0;
    u32 codepoint = 0;
    if (unsigned_byte >= 0xc2 && unsigned_byte <= 0xdf) {
      codepoint_length = 2;
      codepoint = unsigned_byte & 0x1f;
    } else if (unsigned_byte >= 0xe0 && unsigned_byte <= 0xef) {
      codepoint_length = 3;
      codepoint = unsigned_byte & 0x0f;
    } else if (unsigned_byte >= 0xf0 && unsigned_byte <= 0xf4) {
      codepoint_length = 4;
      codepoint = unsigned_byte & 0x07;
    }
    if (codepoint_length == 0 || position + codepoint_length > title.count()) {
      position++;
      continue;
    }
    bool is_valid = true;
    for (usize continuation_index = 1; continuation_index < codepoint_length;
         continuation_index++)
    {
      let const continuation_byte =
          static_cast<unsigned char>(title[position + continuation_index]);
      if ((continuation_byte & 0xc0) != 0x80) {
        is_valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (continuation_byte & 0x3f);
    }
    if (sequence.count() + codepoint_length > MAX_TITLE_LENGTH + 4) break;

    let const minimum_codepoint = codepoint_length == 2   ? 0x80u
                                  : codepoint_length == 3 ? 0x800u
                                                          : 0x10000u;
    if (!is_valid || codepoint < minimum_codepoint || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
        (codepoint >= 0x80u && codepoint <= 0x9fu))
    {
      position++;
      continue;
    }
    sequence.append(title.substring_of_length(position, codepoint_length));
    was_space_appended = false;
    position += codepoint_length;
  }

  sequence.push('\a');

  /* A loop body sets the same title on every iteration. A redirection that
     rebinds a standard descriptor retires the cached one. */
  static String LAST_TITLE_SEQUENCE{koshka::heap_allocator()};
  static u64 LAST_TITLE_EPOCH = static_cast<u64>(-1);
  let const current_epoch = os::get_descriptor_epoch();
  if (current_epoch == LAST_TITLE_EPOCH &&
      sequence.view() == LAST_TITLE_SEQUENCE.view())
  {
    return;
  }

  LAST_TITLE_EPOCH = current_epoch;
  LAST_TITLE_SEQUENCE = sequence;
  unused(os::write_all(output, sequence.data(), sequence.count()));
}

fn set_idle_title() -> void
{
  static const String user = os::get_current_user().value_or("???");
  let const directory = Path::current_directory().text();
  let title = String{koshka::heap_allocator()};
  title.reserve(user.count() + directory.count() + 3);
  title += user;
  title += " @ ";
  title += directory;
  set_title(title.view());
}

fn enable_completion(koshka::EvalContext &context) -> void
{
  COMPLETION_CONTEXT = &context;
  ::tl_set_complete_callback(kosh_completion_callback);
  ::tl_set_highlight_callback(kosh_highlight_callback);
  ::tl_set_ghost_validate_callback(kosh_ghost_validate_callback);
}

fn disable_completion() -> void
{
  COMPLETION_CONTEXT = nullptr;
  ::tl_set_complete_callback(nullptr);
  ::tl_set_highlight_callback(nullptr);
  ::tl_set_ghost_validate_callback(nullptr);
}

fn completion_is_enabled() -> bool { return COMPLETION_CONTEXT != nullptr; }

fn enable_job_notifications(koshka::EvalContext &context) -> void
{
  /* Registered even under -T, since set -b is job reporting, not completion. */
  JOB_CONTEXT = &context;
  ::tl_set_wake_callback(kosh_wake_callback);
}

fn set_ghost_enabled(bool enabled) -> void
{
  ::tl_set_ghost_enabled(enabled ? 1 : 0);
}

fn set_highlight_enabled(bool enabled) -> void
{
  ::tl_set_highlight_callback(enabled ? kosh_highlight_callback : nullptr);
}

fn set_edit_mode(edit_mode mode) -> void
{
  ::tl_set_edit_mode(mode == edit_mode::Vi ? TL_EDIT_MODE_VI_INSERT
                                           : TL_EDIT_MODE_EMACS);
}

fn utf8_strlen(const String &s, usize count) -> usize
{
  return (count != static_cast<usize>(-1)) ? ::tl_utf8_strnlen(s.c_str(), count)
                                           : ::tl_utf8_strlen(s.c_str());
}

fn utf8_strnlen(const char *bytes, usize byte_count) -> usize
{
  return ::tl_utf8_strnlen(bytes, byte_count);
}

fn display_width(StringView text) -> usize
{
  return ::itl_strn_display_width(text.data, text.length);
}

fn byte_offset_at_or_before_display_cell(StringView text, usize cell_position,
                                         usize &actual_cell_position) -> usize
{
  usize byte_offset = 0;
  actual_cell_position = ::itl_strn_width_walk(text.data, text.length,
                                               cell_position, &byte_offset);
  if (actual_cell_position <= cell_position) return byte_offset;

  usize previous_byte_offset = byte_offset - 1;
  while (previous_byte_offset > 0 &&
         (static_cast<unsigned char>(text[previous_byte_offset]) & 0xC0) ==
             0x80)
    previous_byte_offset--;

  actual_cell_position =
      ::itl_strn_display_width(text.data, previous_byte_offset);
  return previous_byte_offset;
}

fn is_active() -> bool { return ::itl_g_is_active; }

fn initialize() -> void
{
  if (koshka::Maybe<koshka::Path> kosh_history = history_file_path();
      kosh_history.has_value())
  {
    ::tl_history_load(kosh_history->c_str());
  }

  if (::tl_init() != TL_SUCCESS) {
    throw koshka::ErrorWithDetails{
        "Toiletline: could not initialize the terminal: " +
            koshka::os::last_system_error_message(),
        "The input is not a terminal, pass `-` to read stdin or `-c`/`-s`"};
  }
}

static fn compact_history_file(usize entry_limit,
                               bool should_skip_if_within_limit) -> bool
{
  let const path = history_file_path();
  if (!path.has_value()) return true;
  let parent = path->parent();
  if (parent.text().is_empty()) parent = Path{"."};
  let lock = os::acquire_process_lock(parent.text().view());
  if (!lock.has_value()) return false;
  defer { os::release_process_lock(lock.take()); };

  ::tl_set_history_limit(entry_limit);
  if (!history_read()) return false;
  if (should_skip_if_within_limit && ::itl_g_history_total_count <= entry_limit)
  {
    return true;
  }

  let contents = String{koshka::heap_allocator()};
  if (!::itl_history_ensure_read_buffer() && ::itl_g_history_count != 0)
    return false;
  char decoded[ITL_STRING_MAX_LEN + 1];
  for (usize index = 0; index < ::itl_g_history_count; index++) {
    usize decoded_size = 0;
    if (!::itl_history_decode_entry_buffered(
            ::itl_history_index_to_offset(index), decoded, sizeof(decoded),
            &decoded_size))
    {
      return false;
    }
    for (usize position = 0; position < decoded_size; position++) {
      if (decoded[position] == '\\')
        contents += "\\\\";
      else if (decoded[position] == '\n')
        contents += "\\n";
      else
        contents.push(decoded[position]);
    }
    contents.push('\n');
  }

  let replacement = os::write_to_named_temp_file(
      parent, ".kosh_history_compact", contents.view());
  if (!replacement.has_value()) return false;
  defer { unused(os::remove_file(replacement->text().view())); };
  if (!os::rename_path(replacement->text().view(), path->text().view()))
    return false;
  return ::tl_history_load(path->c_str()) == TL_SUCCESS;
}

fn exit(bool should_append_history, usize history_size_limit,
        usize history_file_size_limit) -> void
{
  let const retained_limit = should_append_history
                                 ? history_file_size_limit
                                 : (history_size_limit < history_file_size_limit
                                        ? history_size_limit
                                        : history_file_size_limit);
  if (!compact_history_file(retained_limit, should_append_history)) {
    koshka::Error error{"Toiletline: Could not save history: " +
                        koshka::os::last_system_error_message()};
    koshka::show_message(error.to_string());
  }

  if (::tl_exit() != TL_SUCCESS) {
    throw koshka::ErrorWithDetails{
        "Toiletline: could not exit the line editor: " +
            koshka::os::last_system_error_message(),
        "The terminal may be left in raw mode, run `reset` to recover"};
  }
}

fn get_input(const String &prompt) -> input_result
{
  let completion_base_directory = koshka::Maybe<Path>{};
  let completion_storage =
      koshka::Maybe<koshka::completion::completion_result>{};
#if !defined NDEBUG
  let const cwd_capture_count_before = DEBUG_COMPLETION_CWD_CAPTURE_COUNT;
#endif
  if (completion_is_enabled()) {
    HIGHLIGHT_COLOR_ENABLED = colors::stdout_wants_color();
    HIGHLIGHT_STYLED_UNDERLINES_ENABLED =
        colors::terminal_supports_styled_underlines();
    completion_base_directory = Path::current_directory();
    completion_storage = koshka::completion::completion_result{
        koshka::ArrayList<koshka::String>{koshka::heap_allocator()},
        koshka::StringMap<koshka::String>{koshka::heap_allocator()},
        koshka::String{koshka::heap_allocator()},
        0,
        0,
        0,
        0,
        0,
        false};
    COMPLETION_BASE_DIRECTORY = &*completion_base_directory;
    COMPLETION_RESULT = &*completion_storage;
#if !defined NDEBUG
    DEBUG_COMPLETION_CWD_CAPTURE_COUNT++;
#endif
  }
#if !defined NDEBUG
  let const append_refresh_count_before = ::itl_g_debug_append_refresh_count;
  let const full_refresh_count_before = ::itl_g_debug_full_refresh_count;
  let const metrics_scan_count_before = ::itl_g_debug_metrics_scan_count;
  let const line_serialization_count_before =
      ::itl_g_debug_line_serialization_count;
  let const history_scan_count_before = ::itl_g_debug_ghost_history_scan_count;
  let const history_buffer_load_count_before =
      ::itl_g_debug_history_buffer_load_count;
  let const source_scan_count_before = DEBUG_COMPLETION_SOURCE_SCAN_COUNT;
  let const materialized_count_before = DEBUG_COMPLETION_MATERIALIZED_COUNT;
  let const directory_stat_count_before = utils::debug_directory_stat_count();
  let const directory_read_count_before = utils::debug_directory_read_count();
  let const directory_sort_count_before = utils::debug_directory_sort_count();
  let const executable_probe_count_before =
      utils::debug_executable_probe_count();
  let const program_path_candidate_count_before =
      utils::debug_program_path_candidate_count();
#endif
  ::itl_g_last_history_event_number = 0;
  i32 code = ::tl_get_input(TL_BUFFER, sizeof(TL_BUFFER), prompt.c_str());
  COMPLETION_BASE_DIRECTORY = nullptr;
  COMPLETION_RESULT = nullptr;
#if !defined NDEBUG
  if (koshka::os::get_environment_variable("KOSH_TEST_EDITOR_STATS")
          .has_value())
  {
    koshka::print_error(
        "editor-refresh append=" +
        koshka::String::from(::itl_g_debug_append_refresh_count -
                                 append_refresh_count_before,
                             koshka::heap_allocator()) +
        " full=" +
        koshka::String::from(::itl_g_debug_full_refresh_count -
                                 full_refresh_count_before,
                             koshka::heap_allocator()) +
        " metrics=" +
        koshka::String::from(::itl_g_debug_metrics_scan_count -
                                 metrics_scan_count_before,
                             koshka::heap_allocator()) +
        " serializations=" +
        koshka::String::from(::itl_g_debug_line_serialization_count -
                                 line_serialization_count_before,
                             koshka::heap_allocator()) +
        " history-scans=" +
        koshka::String::from(::itl_g_debug_ghost_history_scan_count -
                                 history_scan_count_before,
                             koshka::heap_allocator()) +
        " history-loads=" +
        koshka::String::from(::itl_g_debug_history_buffer_load_count -
                                 history_buffer_load_count_before,
                             koshka::heap_allocator()) +
        " cwd=" +
        koshka::String::from(DEBUG_COMPLETION_CWD_CAPTURE_COUNT -
                                 cwd_capture_count_before,
                             koshka::heap_allocator()) +
        " stats=" +
        koshka::String::from(utils::debug_directory_stat_count() -
                                 directory_stat_count_before,
                             koshka::heap_allocator()) +
        " reads=" +
        koshka::String::from(utils::debug_directory_read_count() -
                                 directory_read_count_before,
                             koshka::heap_allocator()) +
        " sorts=" +
        koshka::String::from(utils::debug_directory_sort_count() -
                                 directory_sort_count_before,
                             koshka::heap_allocator()) +
        " probes=" +
        koshka::String::from(utils::debug_executable_probe_count() -
                                 executable_probe_count_before,
                             koshka::heap_allocator()) +
        " resolutions=" +
        koshka::String::from(utils::debug_program_path_candidate_count() -
                                 program_path_candidate_count_before,
                             koshka::heap_allocator()) +
        " scans=" +
        koshka::String::from(DEBUG_COMPLETION_SOURCE_SCAN_COUNT -
                                 source_scan_count_before,
                             koshka::heap_allocator()) +
        " materialized=" +
        koshka::String::from(DEBUG_COMPLETION_MATERIALIZED_COUNT -
                                 materialized_count_before,
                             koshka::heap_allocator()) +
        "\n");
  }
#endif
  if (code == TL_ERROR) {
    throw koshka::ErrorWithDetails{
        "Toiletline: could not read the input: " +
            koshka::os::last_system_error_message(),
        "Pass `-s` to read stdin without the editor"};
  }
  let const history_event_number =
      ::itl_g_last_history_event_number == 0
          ? koshka::Maybe<usize>{koshka::None}
          : koshka::Maybe<usize>{::itl_g_last_history_event_number};
  return input_result{code, String{TL_BUFFER}, history_event_number};
}

fn set_input(const String &input) -> void
{
  ::tl_set_predefined_input(input.c_str());
}

fn enter_raw_mode() -> void
{
  if (::tl_enter_raw_mode() == TL_SUCCESS) return;
  /* An in-process exec redirection can leave fd 0 off the terminal, so the tty
     is reopened onto fd 0 and raw mode retried. */
  if (koshka::os::reopen_terminal_as_stdin() &&
      ::tl_enter_raw_mode() == TL_SUCCESS)
  {
    return;
  }
  throw koshka::ErrorWithDetails{"Toiletline: could not enter raw mode: " +
                                     koshka::os::last_system_error_message(),
                                 "The input is not an interactive terminal"};
}

fn exit_raw_mode() -> void
{
  if (::tl_exit_raw_mode() != TL_SUCCESS) {
    throw koshka::ErrorWithDetails{
        "Toiletline: could not leave raw mode: " +
            koshka::os::last_system_error_message(),
        "The terminal may be left in raw mode, run `reset` to recover"};
  }
}

fn emit_newlines(StringView buffer) -> void
{
  if (::tl_emit_newlines(buffer.data) != TL_SUCCESS)
    throw koshka::Error{"Toiletline: could not write to the terminal: " +
                        koshka::os::last_system_error_message()};
}

fn debug_allocation_failure() -> bool
{
  let const allocation = tl_arena_malloc(static_cast<usize>(-1));
  if (allocation != NULL) return false;
  return true;
}

static constexpr usize PROMPT_PWD_LENGTH = 24;

static fn shorten_path_with_ellipsis(StringView path, usize max_length) throws
    -> String
{
  if (path.length <= max_length) return String{path};
  if (max_length < 3) return String{path};
  /* The byte cut is advanced to the next codepoint boundary. */
  usize tail_start = path.length - max_length + 3;
  while (tail_start < path.length &&
         (static_cast<unsigned char>(path[tail_start]) & 0xC0) == 0x80)
    tail_start++;
  let shortened = String{koshka::heap_allocator()};
  shortened += "...";
  shortened += StringView{path.data + tail_start, path.length - tail_start};
  return shortened;
}

static fn git_branch() throws -> String { return utils::current_git_branch(); }

static fn format_prompt_duration(u64 nanos) throws -> String
{
  const u64 milliseconds = nanos / 1000000ULL;
  if (milliseconds < 5) return String{koshka::heap_allocator()};
  let out = String{koshka::heap_allocator()};
  if (milliseconds < 1000) {
    out.append(
        String::from(static_cast<i64>(milliseconds), koshka::heap_allocator()));
    out += "ms";
    return out;
  }
  const u64 tenths = nanos / 100000000ULL;
  out.append(
      String::from(static_cast<i64>(tenths / 10), koshka::heap_allocator()));
  out += '.';
  out.append(
      String::from(static_cast<i64>(tenths % 10), koshka::heap_allocator()));
  out += 's';
  return out;
}

/* localtime runs on the single interactive thread, so its shared static tm is
   not a race. */
static fn prompt_strftime(const char *format) throws -> String
{
  std::time_t now = std::time(nullptr);
  std::tm *local = std::localtime(&now);
  if (local == nullptr) return String{koshka::heap_allocator()};
  char buffer[128];
  usize written = std::strftime(buffer, sizeof(buffer), format, local);
  return String{
      StringView{buffer, written}
  };
}

static fn prompt_hostname(bool should_use_full_hostname) throws -> String
{
  String host = os::get_hostname().value_or(
      os::get_environment_variable("HOSTNAME").value_or("localhost"));
  if (should_use_full_hostname) return host;
  let const dot = host.view().find_character('.');
  return String{
      host.view().substring_of_length(0, dot.value_or(host.length()))};
}

/* The home prefix collapses to ~ only when it ends on a path boundary, so
   HOME=/home/sd with cwd=/home/sderp keeps the full path. */
static fn collapse_home_prefix(StringView path) throws -> String
{
  let shown = String{path};
  Maybe<Path> home = os::get_home_directory();
  if (!home.has_value()) return shown;

  let const home_length = home->count();
  if (shown.starts_with(home->text()) &&
      (shown.length() == home_length || shown.view()[home_length] == '/'))
  {
    let collapsed = String{koshka::heap_allocator()};
    collapsed += "~";
    collapsed += shown.substring(home_length);
    shown = steal(collapsed);
  }
  return shown;
}

static fn expand_prompt_escapes(StringView prompt, StringView user,
                                StringView working_directory,
                                EvalContext &context) throws -> String
{
  let out = String{koshka::heap_allocator()};
  for (usize i = 0; i < prompt.length; i++) {
    if (prompt[i] != '\\' || i + 1 >= prompt.length) {
      out += prompt[i];
      continue;
    }
    u8 escaped = static_cast<u8>(prompt[i + 1]);

    if (escaped >= '0' && escaped <= '7') {
      u32 value = 0;
      usize digits = 0;
      while (digits < 3 && i + 1 < prompt.length && prompt[i + 1] >= '0' &&
             prompt[i + 1] <= '7')
      {
        value = value * 8 + static_cast<u32>(prompt[i + 1] - '0');
        i++;
        digits++;
      }
      out += static_cast<char>(value & 0xFF);
      continue;
    }

    i++;
    switch (escaped) {
    case 'u': out += user; break;
    case 'h': out += prompt_hostname(false); break;
    case 'H': out += prompt_hostname(true); break;
    case 'w': out += collapse_home_prefix(working_directory); break;
    case 'W': out += Path{working_directory}.filename(); break;
    case 'P':
      out += shorten_path_with_ellipsis(
          collapse_home_prefix(working_directory).view(), PROMPT_PWD_LENGTH);
      break;
    case 'g': out += git_branch(); break;
    case '$': out += (user == "root") ? '#' : '$'; break;
    case 'n': out += '\n'; break;
    case 'r': out += '\r'; break;
    case 'e': out += '\x1b'; break;
    case 'a': out += '\a'; break;
    /* The editor skips ANSI runs already, so the markers are dropped and the
       bytes between them emitted plainly. */
    case '[': break;
    case ']': break;
    case 't': out += prompt_strftime("%H:%M:%S"); break;
    case 'T': out += prompt_strftime("%I:%M:%S"); break;
    case '@': out += prompt_strftime("%I:%M %p"); break;
    case 'A': out += prompt_strftime("%H:%M"); break;
    case 'd': out += prompt_strftime("%a %b %d"); break;
    case 's': {
      if (Maybe<String> argv0 = context.get_variable_value("0");
          argv0.has_value())
        out += Path{argv0->view()}.filename();
    } break;
    case 'v':
    case 'V':
      if (Maybe<String> version = context.get_variable_value("BASH_VERSION");
          version.has_value())
        out += *version;
      break;
    case '?': {
      const i32 status = context.last_exit_status();
      let const should_use_color = colors::stdout_wants_color();
      if (should_use_color)
        out += status == 0 ? colors::ansi::GREEN : colors::ansi::RED;
      out += String::from(status, koshka::heap_allocator());
      if (should_use_color) out += colors::ansi::RESET;
    } break;
    case '.': {
      const i32 status = context.last_exit_status();
      let const should_use_color = colors::stdout_wants_color();
      if (should_use_color && status != 0) out += colors::ansi::BOLD_BRIGHT_RED;
      out += "•";
      if (should_use_color && status != 0) out += colors::ansi::RESET;
    } break;
    case 'j':
      out += String::from(static_cast<i64>(context.jobs().count()),
                          koshka::heap_allocator());
      break;
    case 'D':
      out += format_prompt_duration(context.last_command_duration_nanos());
      break;
    /* \! and \# are untracked here, so they expand to nothing. */
    case '!': break;
    case '#': break;
    case '\\': out += '\\'; break;
    default:
      out += '\\';
      out += static_cast<char>(escaped);
      break;
    }
  }
  return out;
}

/* The previous PS1 expansion, reusable only while every parameter it read is
   unchanged. A template holding a substitution, funsub, or assigning parameter
   form has inputs the names cannot capture, so it never caches. */
struct prompt_cache_input
{
  String name{koshka::heap_allocator()};
  Maybe<String> value{};
};
static String PROMPT_CACHE_TEMPLATE{koshka::heap_allocator()};
static koshka::ArrayList<prompt_cache_input> PROMPT_CACHE_INPUTS{
    koshka::heap_allocator()};
static String PROMPT_CACHE_EXPANSION{koshka::heap_allocator()};
static bool PROMPT_CACHE_VALID = false;

/* A $ that opens anything but a plain name or a non-assigning braced parameter
   form marks the template impure. */
static fn
scan_prompt_template_inputs(StringView text,
                            koshka::ArrayList<prompt_cache_input> &names) throws
    -> bool
{
  let const do_is_name_byte = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  let const do_add_name = [&](StringView name) throws {
    for (let const &known : names)
      if (known.name.view() == name) return;
    let input = prompt_cache_input{};
    input.name = String{name};
    names.push(steal(input));
  };

  for (usize i = 0; i < text.length; i++) {
    let const byte = text[i];
    if (byte == '`') return false;
    if (byte != '$') continue;
    if (i + 1 >= text.length) continue;
    let const next = text[i + 1];
    if (next == '(') return false;
    if (next == '{') {
      usize j = i + 2;
      /* A brace followed by whitespace or a pipe is a funsub. */
      if (j < text.length && (text[j] == ' ' || text[j] == '\t' ||
                              text[j] == '\n' || text[j] == '|'))
      {
        return false;
      }
      if (j < text.length && (text[j] == '#' || text[j] == '!')) j++;
      usize name_start = j;
      while (j < text.length && do_is_name_byte(text[j]))
        j++;
      if (j == name_start) return false;
      do_add_name(text.substring_of_length(name_start, j - name_start));
      /* An assigning form has an input the cache cannot key. */
      if (j < text.length && text[j] == '=') return false;
      if (j + 1 < text.length && text[j] == ':' && text[j + 1] == '=') {
        return false;
      }
      i = j - 1;
      continue;
    }
    usize j = i + 1;
    while (j < text.length && do_is_name_byte(text[j]))
      j++;
    if (j > i + 1) {
      do_add_name(text.substring_of_length(i + 1, j - i - 1));
      i = j - 1;
      continue;
    }
    /* A special parameter such as $? reads one byte, keyed like a name. */
    do_add_name(text.substring_of_length(i + 1, 1));
    i++;
  }
  return true;
}

fn default_prompt_template() -> String
{
  let template_string = String{koshka::heap_allocator()};
  let const should_use_color = colors::stdout_wants_color();

  if (should_use_color) {
    template_string += R"(${KOSH_GIT_BRANCH:+)";
    template_string += colors::ansi::CYAN;
    template_string += R"($KOSH_GIT_BRANCH)";
    template_string += colors::ansi::RESET;
    template_string += R"(})";
    template_string += R"(${KOSH_GIT_AHEAD:+ )";
    template_string += colors::ansi::BOLD_YELLOW;
    template_string += "\xe2\x86\x91";
    template_string += R"($KOSH_GIT_AHEAD)";
    template_string += colors::ansi::RESET;
    template_string += R"(})";
    template_string += R"(${KOSH_GIT_BEHIND:+ )";
    template_string += colors::ansi::BOLD_YELLOW;
    template_string += "\xe2\x86\x93";
    template_string += R"($KOSH_GIT_BEHIND)";
    template_string += colors::ansi::RESET;
    template_string += R"(})";
    template_string += R"(${KOSH_GIT_BRANCH:+ at }\u@\h )";
    template_string += colors::ansi::GREEN;
    template_string += R"(\P)";
    template_string += colors::ansi::RESET;
  } else {
    template_string += R"([${KOSH_GIT_BRANCH:+$KOSH_GIT_BRANCH})";
    template_string += R"(${KOSH_GIT_AHEAD:+ )";
    template_string += "\xe2\x86\x91";
    template_string += R"($KOSH_GIT_AHEAD})";
    template_string += R"(${KOSH_GIT_BEHIND:+ )";
    template_string += "\xe2\x86\x93";
    template_string += R"($KOSH_GIT_BEHIND})";
    template_string += R"(${KOSH_GIT_BRANCH:+ at }\u@\h \P)";
  }
  template_string += R"( \. )";
  return template_string;
}

/* The prompt backslash escapes are mapped to control-byte markers so the
   parameter pass does not unescape them before the escape pass runs. */
static constexpr char PROMPT_GUARD_DOLLAR = '\x01';
static constexpr char PROMPT_GUARD_BACKSLASH = '\x02';
static constexpr char PROMPT_GUARD_BACKTICK = '\x03';

static fn guard_prompt_backslashes(StringView template_string) throws -> String
{
  let out = String{koshka::heap_allocator()};
  for (usize i = 0; i < template_string.length; i++) {
    if (template_string[i] == '\\' && i + 1 < template_string.length) {
      switch (template_string[i + 1]) {
      case '$':
        out.push(PROMPT_GUARD_DOLLAR);
        i++;
        continue;
      case '\\':
        out.push(PROMPT_GUARD_BACKSLASH);
        i++;
        continue;
      case '`':
        out.push(PROMPT_GUARD_BACKTICK);
        i++;
        continue;
      default: break;
      }
    }
    out.push(template_string[i]);
  }
  return out;
}

static fn unguard_prompt_backslashes(StringView expanded) throws -> String
{
  let out = String{koshka::heap_allocator()};
  for (usize i = 0; i < expanded.length; i++) {
    switch (expanded[i]) {
    case PROMPT_GUARD_DOLLAR: out += "\\$"; break;
    case PROMPT_GUARD_BACKSLASH: out += "\\\\"; break;
    case PROMPT_GUARD_BACKTICK: out += "\\`"; break;
    default: out.push(expanded[i]); break;
    }
  }
  return out;
}

/* Only an SGR sequence ending in 'm' is stripped, a non-color CSI is left. */
static fn strip_ansi_color(StringView text) throws -> String
{
  let out = String{koshka::heap_allocator()};
  usize i = 0;
  while (i < text.length) {
    if (text[i] == '\x1b' && i + 1 < text.length && text[i + 1] == '[') {
      usize end = i + 2;
      while (end < text.length && (text[end] < '@' || text[end] > '~'))
        end++;
      if (end < text.length && text[end] == 'm') {
        i = end + 1;
        continue;
      }
    }
    out.push(text[i]);
    i++;
  }
  return out;
}

fn expand_prompt_template(StringView prompt, EvalContext &context) throws
    -> String
{
  let const working_directory = Path::current_directory().text();
  let const user = os::get_current_user().value_or(String{"???"});
  return expand_prompt_escapes(prompt, user.view(), working_directory.view(),
                               context);
}

fn build_prompt(EvalContext &context) -> String
{
  let const full_pwd = Path::current_directory().text().clone();

  /* The user is stable for the session, so it is resolved once and reused. */
  static String CACHED_USER{koshka::heap_allocator()};
  static bool was_user_resolved = false;
  if (!was_user_resolved) {
    CACHED_USER = os::get_current_user().value_or("???");
    was_user_resolved = true;
  }

  String ps1_template{koshka::heap_allocator()};
  if (Maybe<String> ps1 = context.get_variable_value("PS1");
      ps1.has_value() && !ps1->is_empty())
    ps1_template = steal(*ps1);
  else
    ps1_template = default_prompt_template();

  /* The raw template expands before the backslash escapes are decoded, so the
     escape-inserted cwd and user are literal and never re-expanded. A directory
     named $(...) therefore cannot run a command at the prompt. */

  let scanned_inputs =
      koshka::ArrayList<prompt_cache_input>{koshka::heap_allocator()};
  let const is_cacheable =
      scan_prompt_template_inputs(ps1_template.view(), scanned_inputs);
  if (is_cacheable && PROMPT_CACHE_VALID &&
      ps1_template.view() == PROMPT_CACHE_TEMPLATE.view())
  {
    bool is_every_input_unchanged = true;
    for (let const &input : PROMPT_CACHE_INPUTS) {
      let current = context.get_variable_value(input.name.view());
      let const both_unset = !current.has_value() && !input.value.has_value();
      let const both_equal = current.has_value() && input.value.has_value() &&
                             current->view() == input.value->view();
      if (!both_unset && !both_equal) {
        is_every_input_unchanged = false;
        break;
      }
    }
    if (is_every_input_unchanged) {
      String rendered =
          expand_prompt_escapes(PROMPT_CACHE_EXPANSION.view(),
                                CACHED_USER.view(), full_pwd.view(), context);
      if (!colors::stdout_wants_color())
        return strip_ansi_color(rendered.view());
      return rendered;
    }
  }

  const i32 saved_status = context.last_exit_status();
  String guarded = guard_prompt_backslashes(ps1_template.view());
  String expanded{koshka::heap_allocator()};
  try {
    expanded = unguard_prompt_backslashes(
        context.expand_heredoc_body(guarded.view()).view());
  } catch (const koshka::ErrorBase &) {
    /* A prompt draw error leaves the template standing rather than taking down
       the shell. */
    expanded = ps1_template;
  }
  context.set_last_exit_status(saved_status);

  String rendered = expand_prompt_escapes(expanded.view(), CACHED_USER.view(),
                                          full_pwd.view(), context);

  PROMPT_CACHE_VALID = false;
  if (is_cacheable) {
    for (let &input : scanned_inputs)
      input.value = context.get_variable_value(input.name.view());
    PROMPT_CACHE_TEMPLATE = ps1_template;
    PROMPT_CACHE_INPUTS = steal(scanned_inputs);
    PROMPT_CACHE_EXPANSION = steal(expanded);
    PROMPT_CACHE_VALID = true;
  }

  if (!colors::stdout_wants_color()) return strip_ansi_color(rendered.view());
  return rendered;
}

fn render_ps0(EvalContext &context) -> String
{
  Maybe<String> ps0 = context.get_variable_value("PS0");
  if (!ps0.has_value() || ps0->is_empty()) {
    return String{koshka::heap_allocator()};
  }

  const i32 saved_status = context.last_exit_status();
  String guarded = guard_prompt_backslashes(ps0->view());
  String expanded{koshka::heap_allocator()};
  try {
    expanded = unguard_prompt_backslashes(
        context.expand_heredoc_body(guarded.view()).view());
  } catch (const koshka::ErrorBase &) {
    context.set_last_exit_status(saved_status);
    return String{koshka::heap_allocator()};
  }
  context.set_last_exit_status(saved_status);

  let const working_directory = Path::current_directory().text();
  let const user = os::get_current_user().value_or(String{"???"});
  String rendered = expand_prompt_escapes(expanded.view(), user.view(),
                                          working_directory.view(), context);
  if (!colors::stdout_wants_color()) return strip_ansi_color(rendered.view());
  return rendered;
}

} /* namespace toiletline */

#endif /* KOSH_NO_TOILETLINE */
