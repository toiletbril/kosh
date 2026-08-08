/* The toiletline configuration macros are defined here, so Toiletline.hpp is
   not included. */

#include "Arena.hpp"
#include "Cli.hpp"
#include "Colors.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace toiletline {

enum class edit_mode : u8
{
  Emacs,
  Vi,
};

fn byte_offset_of_codepoint(const char *bytes, usize byte_length,
                            usize codepoint_index) -> usize
{
  usize byte_offset = 0;
  usize seen_codepoints = 0;
  while (byte_offset < byte_length && seen_codepoints < codepoint_index) {
    if ((static_cast<unsigned char>(bytes[byte_offset]) & 0xC0) != 0x80)
      seen_codepoints += 1;
    byte_offset += 1;
  }
  /* Step over the trailing continuation bytes of the last counted codepoint. */
  while (byte_offset < byte_length &&
         (static_cast<unsigned char>(bytes[byte_offset]) & 0xC0) == 0x80)
    byte_offset += 1;
  return byte_offset;
}

} /* namespace toiletline */

#if !defined SHIT_NO_TOILETLINE

namespace {

/* The bump arena cannot free a single block, so a free returns the block to a
   free list the next allocation reuses. A header before each block records its
   capacity for realloc and for sizing a reused block. */
shit::BumpArena TOILETLINE_ARENA{};

constexpr usize TL_ALLOC_HEADER = 16;

struct tl_free_block
{
  tl_free_block *next;
};
tl_free_block *TOILETLINE_FREE_LIST = nullptr;

fn tl_block_capacity(opaque *payload) -> usize &
{
  return *reinterpret_cast<usize *>(static_cast<char *>(payload) -
                                    TL_ALLOC_HEADER);
}

fn tl_arena_malloc(usize length) -> opaque *
{
  for (tl_free_block **link = &TOILETLINE_FREE_LIST; *link != nullptr;
       link = &(*link)->next)
  {
    opaque *payload = *link;
    if (tl_block_capacity(payload) >= length) {
      *link = (*link)->next;
      return payload;
    }
  }

  char *base = static_cast<char *>(
      TOILETLINE_ARENA.allocate(length + TL_ALLOC_HEADER, TL_ALLOC_HEADER));
  *reinterpret_cast<usize *>(base) = length;
  return base + TL_ALLOC_HEADER;
}

fn tl_arena_free(opaque *pointer) -> void
{
  if (pointer == nullptr) return;
  tl_free_block *block = static_cast<tl_free_block *>(pointer);
  block->next = TOILETLINE_FREE_LIST;
  TOILETLINE_FREE_LIST = block;
}

fn tl_arena_realloc(opaque *pointer, usize length) -> opaque *
{
  if (pointer == nullptr) return tl_arena_malloc(length);
  usize old_capacity = tl_block_capacity(pointer);
  if (old_capacity >= length) return pointer;
  opaque *fresh = tl_arena_malloc(length);
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

shit::EvalContext *COMPLETION_CONTEXT = nullptr;
const shit::Path *COMPLETION_BASE_DIRECTORY = nullptr;
bool HIGHLIGHT_COLOR_ENABLED = false;
bool HIGHLIGHT_STYLED_UNDERLINES_ENABLED = false;
#if !defined NDEBUG
usize DEBUG_COMPLETION_CWD_CAPTURE_COUNT = 0;
usize DEBUG_COMPLETION_SOURCE_SCAN_COUNT = 0;
usize DEBUG_COMPLETION_MATERIALIZED_COUNT = 0;
#endif

shit::ArrayList<const char *> COMPLETION_CANDIDATE_POINTERS{
    shit::heap_allocator()};
shit::ArrayList<const char *> COMPLETION_DESCRIPTION_POINTERS{
    shit::heap_allocator()};
shit::completion::completion_result *COMPLETION_RESULT = nullptr;

/* Toiletline edits in codepoints while the completion engine works in bytes. */
fn shit_completion_callback(const char *buffer, size_t cursor,
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
    const bool is_explicit_completion = for_listing != 0;
    if (is_explicit_completion)
      COMPLETION_CONTEXT->get_program_resolver().begin_explicit_completion(
          shit::ProgramResolver::CompletionRefresh::Cached);
    defer
    {
      if (is_explicit_completion)
        COMPLETION_CONTEXT->get_program_resolver().end_explicit_completion();
    };

    const usize byte_length = std::strlen(buffer);
    let line = shit::StringView{buffer, byte_length};

    const usize byte_cursor =
        toiletline::byte_offset_of_codepoint(buffer, byte_length, cursor);

    /* A completion diagnostic is armed to break onto its own line, then
       disarmed so a later command's message is unaffected. */
    shit::arm_message_leading_newline(true);
    COMPLETION_RESULT->candidates.clear();
    COMPLETION_RESULT->descriptions.clear();
    COMPLETION_RESULT->longest_common_prefix.clear();
    *COMPLETION_RESULT = shit::completion::complete(
        line, byte_cursor, *COMPLETION_CONTEXT, *COMPLETION_BASE_DIRECTORY,
        for_listing != 0 ? shit::completion::completion_mode::Listing
                         : shit::completion::completion_mode::Ghost);
    let const &result = *COMPLETION_RESULT;
#if !defined NDEBUG
    DEBUG_COMPLETION_SOURCE_SCAN_COUNT += result.source_candidate_scan_count;
    DEBUG_COMPLETION_MATERIALIZED_COUNT += result.materialized_candidate_count;
#endif
    shit::arm_message_leading_newline(false);

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
        if (const shit::String *found_description =
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
  } catch (shit::ErrorBase &error) {
    /* A throw skips the disarm above, so it runs here too. */
    shit::arm_message_leading_newline(false);
    LOG(Debug, "completion swallowed an error: %s", error.message().c_str());
    return 0;
  } catch (...) {
    shit::arm_message_leading_newline(false);
    LOG(Debug, "completion swallowed an unknown throw");
    return 0;
  }
}

/* The body is guarded since toiletline calls through a C function pointer. */
fn shit_highlight_callback(const char *buffer, tl_highlight *out) -> int
{
  if (COMPLETION_CONTEXT == nullptr) return 0;
  if (!HIGHLIGHT_COLOR_ENABLED) return 0;

  try {
    const usize byte_length = std::strlen(buffer);
    let line = shit::StringView{buffer, byte_length};

    shit::ArrayList<shit::highlight_span> result =
        shit::completion::highlight_line(line, *COMPLETION_CONTEXT);
    let const &theme = HIGHLIGHT_STYLED_UNDERLINES_ENABLED
                           ? shit::colors::SHELL_HIGHLIGHT_THEME
                           : shit::colors::NONINTERACTIVE_HIGHLIGHT_THEME;

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

shit::EvalContext *JOB_CONTEXT = nullptr;
shit::String WAKE_NOTIFICATION_STASH{shit::heap_allocator()};

/* The two-phase wake hook for set -b. Phase 0 formats the Done rows, phase 1
   prints them after the editor cleared its render block. The body is guarded
   since toiletline calls through a C function pointer. */
fn shit_wake_callback(int phase) -> int
{
  try {
    if (phase == 0) {
      if (shit::os::CHILD_STATE_CHANGED == 0) return 0;
      /* The flag clears only when this hook consumes it. */
      if (JOB_CONTEXT == nullptr || !JOB_CONTEXT->notify()) return 0;
      shit::os::CHILD_STATE_CHANGED = 0;
      WAKE_NOTIFICATION_STASH =
          JOB_CONTEXT->format_done_job_notifications("\r\n");
      return WAKE_NOTIFICATION_STASH.is_empty() ? 0 : 1;
    }
    shit::print_error(WAKE_NOTIFICATION_STASH.view());
    shit::flush();
    WAKE_NOTIFICATION_STASH.clear();
    return 0;
  } catch (...) {
    return 0;
  }
}

/* An entry whose command word no longer resolves is rejected. A throw accepts
   the entry. */
fn shit_ghost_validate_callback(const char *entry) -> int
{
  if (COMPLETION_CONTEXT == nullptr) return 1;
  try {
    const usize byte_length = std::strlen(entry);
    return shit::completion::command_word_resolves(
               shit::StringView{entry, byte_length}, *COMPLETION_CONTEXT)
               ? 1
               : 0;
  } catch (...) {
    return 1;
  }
}

} /* namespace */

namespace toiletline {

using shit::EvalContext;
using shit::Maybe;
using shit::Path;
using shit::String;
using shit::StringView;
namespace colors = shit::colors;
namespace os = shit::os;
namespace utils = shit::utils;

struct input_result
{
  i32 code;
  String text;
};

static char TL_BUFFER[ITL_STRING_MAX_LEN];

static constexpr char SHIT_HISTORY_FILE[] = ".shit_history";

static fn resolve_history_path(StringView env_name, StringView default_file)
    -> shit::Maybe<shit::Path>
{
  if (let const override_path = shit::os::get_environment_variable(env_name);
      override_path.has_value() && !override_path->is_empty())
  {
    return shit::Path{override_path->view()};
  }
  let home = shit::os::get_home_directory();
  if (!home.has_value()) return shit::None;
  let path = home->clone();
  path.push_component(default_file);
  return path;
}

static constexpr char SHIT_CALC_HISTORY_FILE[] = ".shit_calc_history";

static fn history_file_path() -> shit::Maybe<shit::Path>
{
  return resolve_history_path("SHIT_HISTORY", SHIT_HISTORY_FILE);
}

static fn calc_history_file_path() -> shit::Maybe<shit::Path>
{
  return resolve_history_path("SHIT_CALC_HISTORY", SHIT_CALC_HISTORY_FILE);
}

/* The history is swapped to the calc file on entry and back on leave, so the
   two histories never mix. The shell history is dumped first for a later
   reload. */
fn enter_calc_history() -> void
{
  if (shit::Maybe<shit::Path> shell = history_file_path(); shell.has_value())
    ::tl_history_dump(shell->c_str());

  if (shit::Maybe<shit::Path> calc = calc_history_file_path(); calc.has_value())
    ::tl_history_load(calc->c_str());
}

fn leave_calc_history() -> void
{
  if (shit::Maybe<shit::Path> calc = calc_history_file_path(); calc.has_value())
    ::tl_history_dump(calc->c_str());

  if (shit::Maybe<shit::Path> shell = history_file_path(); shell.has_value())
    ::tl_history_load(shell->c_str());
}

fn history_path() -> shit::Maybe<shit::Path> { return history_file_path(); }

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
  let opened = shit::os::open_file_descriptor(
      path->text().view(), shit::os::file_open_mode::Truncate);
  if (!opened.has_value()) return false;
  shit::os::close_fd(opened.take());
  ::tl_history_load(path->c_str());
  return true;
}

static fn strip_ansi_color(StringView text) throws -> String;

fn enable_completion(shit::EvalContext &context) -> void
{
  COMPLETION_CONTEXT = &context;
  ::tl_set_complete_callback(shit_completion_callback);
  ::tl_set_highlight_callback(shit_highlight_callback);
  ::tl_set_ghost_validate_callback(shit_ghost_validate_callback);
}

fn disable_completion() -> void
{
  COMPLETION_CONTEXT = nullptr;
  ::tl_set_complete_callback(nullptr);
  ::tl_set_highlight_callback(nullptr);
  ::tl_set_ghost_validate_callback(nullptr);
}

fn completion_is_enabled() -> bool { return COMPLETION_CONTEXT != nullptr; }

fn enable_job_notifications(shit::EvalContext &context) -> void
{
  /* Registered even under -T, since set -b is job reporting, not completion. */
  JOB_CONTEXT = &context;
  ::tl_set_wake_callback(shit_wake_callback);
}

fn set_ghost_enabled(bool enabled) -> void
{
  ::tl_set_ghost_enabled(enabled ? 1 : 0);
}

fn set_highlight_enabled(bool enabled) -> void
{
  ::tl_set_highlight_callback(enabled ? shit_highlight_callback : nullptr);
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
  if (shit::Maybe<shit::Path> shit_history = history_file_path();
      shit_history.has_value())
  {
    ::tl_history_load(shit_history->c_str());
  }

  if (::tl_init() != TL_SUCCESS) {
    throw shit::ErrorWithDetails{
        "Toiletline: could not initialize the terminal: " +
            shit::os::last_system_error_message(),
        "The input is not a terminal, pass `-` to read stdin or `-c`/`-s`"};
  }
}

fn exit() -> void
{
  if (shit::Maybe<shit::Path> shit_history = history_file_path();
      shit_history.has_value())
  {
    if (int dump_status = ::tl_history_dump(shit_history->c_str());
        dump_status != TL_SUCCESS && dump_status != -EINVAL)
    {
      shit::Error error{"Toiletline: Could not dump history: " +
                        shit::os::last_system_error_message()};
      shit::show_message(error.to_string());
    }
  }

  if (::tl_exit() != TL_SUCCESS) {
    throw shit::ErrorWithDetails{
        "Toiletline: could not exit the line editor: " +
            shit::os::last_system_error_message(),
        "The terminal may be left in raw mode, run `reset` to recover"};
  }
}

fn get_input(const String &prompt) -> input_result
{
#if !defined NDEBUG
  let const preprompt_directory_stat_count_before =
      utils::debug_directory_stat_count();
  let const preprompt_directory_read_count_before =
      utils::debug_directory_read_count();
  let const preprompt_directory_sort_count_before =
      utils::debug_directory_sort_count();
  let const preprompt_executable_probe_count_before =
      utils::debug_executable_probe_count();
  let const preprompt_program_path_candidate_count_before =
      utils::debug_program_path_candidate_count();
  let const preprompt_history_buffer_load_count_before =
      ::itl_g_debug_history_buffer_load_count;
#endif
  unused(::itl_history_ensure_read_buffer());
#if !defined NDEBUG
  let const preprompt_directory_stat_count =
      utils::debug_directory_stat_count() -
      preprompt_directory_stat_count_before;
  let const preprompt_directory_read_count =
      utils::debug_directory_read_count() -
      preprompt_directory_read_count_before;
  let const preprompt_directory_sort_count =
      utils::debug_directory_sort_count() -
      preprompt_directory_sort_count_before;
  let const preprompt_executable_probe_count =
      utils::debug_executable_probe_count() -
      preprompt_executable_probe_count_before;
  let const preprompt_program_path_candidate_count =
      utils::debug_program_path_candidate_count() -
      preprompt_program_path_candidate_count_before;
  let const preprompt_history_buffer_load_count =
      ::itl_g_debug_history_buffer_load_count -
      preprompt_history_buffer_load_count_before;
#endif
  HIGHLIGHT_COLOR_ENABLED = colors::stdout_wants_color();
  HIGHLIGHT_STYLED_UNDERLINES_ENABLED =
      colors::terminal_supports_styled_underlines();
  let const completion_base_directory = Path::current_directory();
  let completion_result = shit::completion::completion_result{
      shit::ArrayList<shit::String>{shit::heap_allocator()},
      shit::StringMap<shit::String>{shit::heap_allocator()},
      shit::String{shit::heap_allocator()},
      0,
      0,
      0,
      0,
      0,
      false};
  COMPLETION_BASE_DIRECTORY = &completion_base_directory;
  COMPLETION_RESULT = &completion_result;
#if !defined NDEBUG
  let const append_refresh_count_before = ::itl_g_debug_append_refresh_count;
  let const full_refresh_count_before = ::itl_g_debug_full_refresh_count;
  let const metrics_scan_count_before = ::itl_g_debug_metrics_scan_count;
  let const line_serialization_count_before =
      ::itl_g_debug_line_serialization_count;
  let const history_scan_count_before = ::itl_g_debug_ghost_history_scan_count;
  let const history_buffer_load_count_before =
      ::itl_g_debug_history_buffer_load_count;
  let const cwd_capture_count_before = DEBUG_COMPLETION_CWD_CAPTURE_COUNT++;
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
  i32 code = ::tl_get_input(TL_BUFFER, sizeof(TL_BUFFER), prompt.c_str());
  COMPLETION_BASE_DIRECTORY = nullptr;
  COMPLETION_RESULT = nullptr;
#if !defined NDEBUG
  if (shit::os::get_environment_variable("SHIT_TEST_EDITOR_STATS").has_value())
  {
    shit::print_error(
        "editor-refresh append=" +
        shit::String::from(::itl_g_debug_append_refresh_count -
                               append_refresh_count_before,
                           shit::heap_allocator()) +
        " full=" +
        shit::String::from(::itl_g_debug_full_refresh_count -
                               full_refresh_count_before,
                           shit::heap_allocator()) +
        " metrics=" +
        shit::String::from(::itl_g_debug_metrics_scan_count -
                               metrics_scan_count_before,
                           shit::heap_allocator()) +
        " serializations=" +
        shit::String::from(::itl_g_debug_line_serialization_count -
                               line_serialization_count_before,
                           shit::heap_allocator()) +
        " history-scans=" +
        shit::String::from(::itl_g_debug_ghost_history_scan_count -
                               history_scan_count_before,
                           shit::heap_allocator()) +
        " history-loads=" +
        shit::String::from(::itl_g_debug_history_buffer_load_count -
                               history_buffer_load_count_before,
                           shit::heap_allocator()) +
        " preprompt-stats=" +
        shit::String::from(preprompt_directory_stat_count,
                           shit::heap_allocator()) +
        " preprompt-reads=" +
        shit::String::from(preprompt_directory_read_count,
                           shit::heap_allocator()) +
        " preprompt-sorts=" +
        shit::String::from(preprompt_directory_sort_count,
                           shit::heap_allocator()) +
        " preprompt-probes=" +
        shit::String::from(preprompt_executable_probe_count,
                           shit::heap_allocator()) +
        " preprompt-resolutions=" +
        shit::String::from(preprompt_program_path_candidate_count,
                           shit::heap_allocator()) +
        " preprompt-history-loads=" +
        shit::String::from(preprompt_history_buffer_load_count,
                           shit::heap_allocator()) +
        " cwd=" +
        shit::String::from(DEBUG_COMPLETION_CWD_CAPTURE_COUNT -
                               cwd_capture_count_before,
                           shit::heap_allocator()) +
        " stats=" +
        shit::String::from(utils::debug_directory_stat_count() -
                               directory_stat_count_before,
                           shit::heap_allocator()) +
        " reads=" +
        shit::String::from(utils::debug_directory_read_count() -
                               directory_read_count_before,
                           shit::heap_allocator()) +
        " sorts=" +
        shit::String::from(utils::debug_directory_sort_count() -
                               directory_sort_count_before,
                           shit::heap_allocator()) +
        " probes=" +
        shit::String::from(utils::debug_executable_probe_count() -
                               executable_probe_count_before,
                           shit::heap_allocator()) +
        " resolutions=" +
        shit::String::from(utils::debug_program_path_candidate_count() -
                               program_path_candidate_count_before,
                           shit::heap_allocator()) +
        " scans=" +
        shit::String::from(DEBUG_COMPLETION_SOURCE_SCAN_COUNT -
                               source_scan_count_before,
                           shit::heap_allocator()) +
        " materialized=" +
        shit::String::from(DEBUG_COMPLETION_MATERIALIZED_COUNT -
                               materialized_count_before,
                           shit::heap_allocator()) +
        "\n");
  }
#endif
  if (code == TL_ERROR) {
    throw shit::ErrorWithDetails{"Toiletline: could not read the input: " +
                                     shit::os::last_system_error_message(),
                                 "Pass `-s` to read stdin without the editor"};
  }
  return input_result{code, String{TL_BUFFER}};
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
  if (shit::os::reopen_terminal_as_stdin() &&
      ::tl_enter_raw_mode() == TL_SUCCESS)
  {
    return;
  }
  throw shit::ErrorWithDetails{"Toiletline: could not enter raw mode: " +
                                   shit::os::last_system_error_message(),
                               "The input is not an interactive terminal"};
}

fn exit_raw_mode() -> void
{
  if (::tl_exit_raw_mode() != TL_SUCCESS) {
    throw shit::ErrorWithDetails{
        "Toiletline: could not leave raw mode: " +
            shit::os::last_system_error_message(),
        "The terminal may be left in raw mode, run `reset` to recover"};
  }
}

fn emit_newlines(StringView buffer) -> void
{
  if (::tl_emit_newlines(buffer.data) != TL_SUCCESS)
    throw shit::Error{"Toiletline: could not write to the terminal: " +
                      shit::os::last_system_error_message()};
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
  let shortened = String{shit::heap_allocator()};
  shortened += "...";
  shortened += StringView{path.data + tail_start, path.length - tail_start};
  return shortened;
}

static fn git_branch() throws -> String { return utils::current_git_branch(); }

static fn format_prompt_duration(u64 nanos) throws -> String
{
  const u64 milliseconds = nanos / 1000000ULL;
  if (milliseconds < 5) return String{shit::heap_allocator()};
  let out = String{shit::heap_allocator()};
  if (milliseconds < 1000) {
    out.append(
        String::from(static_cast<i64>(milliseconds), shit::heap_allocator()));
    out += "ms";
    return out;
  }
  const u64 tenths = nanos / 100000000ULL;
  out.append(
      String::from(static_cast<i64>(tenths / 10), shit::heap_allocator()));
  out += '.';
  out.append(
      String::from(static_cast<i64>(tenths % 10), shit::heap_allocator()));
  out += 's';
  return out;
}

/* localtime runs on the single interactive thread, so its shared static tm is
   not a race. */
static fn prompt_strftime(const char *format) throws -> String
{
  std::time_t now = std::time(nullptr);
  std::tm *local = std::localtime(&now);
  if (local == nullptr) return String{shit::heap_allocator()};
  char buffer[128];
  usize written = std::strftime(buffer, sizeof(buffer), format, local);
  return String{
      StringView{buffer, written}
  };
}

static fn prompt_hostname(bool need_full) throws -> String
{
  String host = os::get_hostname().value_or(
      os::get_environment_variable("HOSTNAME").value_or("localhost"));
  if (need_full) return host;
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
    let collapsed = String{shit::heap_allocator()};
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
  let out = String{shit::heap_allocator()};
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
      const bool should_use_color = colors::stdout_wants_color();
      if (should_use_color)
        out += status == 0 ? colors::ansi::GREEN : colors::ansi::RED;
      out += String::from(status, shit::heap_allocator());
      if (should_use_color) out += colors::ansi::RESET;
    } break;
    case '.': {
      const i32 status = context.last_exit_status();
      const bool should_use_color = colors::stdout_wants_color();
      if (should_use_color && status != 0) out += colors::ansi::BOLD_BRIGHT_RED;
      out += "•";
      if (should_use_color && status != 0) out += colors::ansi::RESET;
    } break;
    case 'j':
      out += String::from(static_cast<i64>(context.jobs().count()),
                          shit::heap_allocator());
      break;
    case 'D':
      out += format_prompt_duration(context.last_command_duration_ns());
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
  String name{shit::heap_allocator()};
  Maybe<String> value{};
};
static String PROMPT_CACHE_TEMPLATE{shit::heap_allocator()};
static shit::ArrayList<prompt_cache_input> PROMPT_CACHE_INPUTS{
    shit::heap_allocator()};
static String PROMPT_CACHE_EXPANSION{shit::heap_allocator()};
static bool PROMPT_CACHE_VALID = false;

/* A $ that opens anything but a plain name or a non-assigning braced parameter
   form marks the template impure. */
static fn scan_prompt_template_inputs(
    StringView text, shit::ArrayList<prompt_cache_input> &names) throws -> bool
{
  let is_name_byte = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  let do_add_name = [&](StringView name) throws {
    for (let const &known : names)
      if (known.name.view() == name) return;
    let input = prompt_cache_input{};
    input.name = String{name};
    names.push(steal(input));
  };

  for (usize i = 0; i < text.length; i++) {
    const char byte = text[i];
    if (byte == '`') return false;
    if (byte != '$') continue;
    if (i + 1 >= text.length) continue;
    const char next = text[i + 1];
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
      while (j < text.length && is_name_byte(text[j]))
        j++;
      if (j == name_start) return false;
      do_add_name(text.substring_of_length(name_start, j - name_start));
      /* An assigning form has an input the cache cannot key. */
      if (j < text.length && text[j] == '=') return false;
      if (j + 1 < text.length && text[j] == ':' && text[j + 1] == '=')
        return false;
      i = j - 1;
      continue;
    }
    usize j = i + 1;
    while (j < text.length && is_name_byte(text[j]))
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
  let template_string = String{shit::heap_allocator()};
  const bool should_use_color = colors::stdout_wants_color();

  if (should_use_color) {
    template_string += R"(${SHIT_GIT_BRANCH:+)";
    template_string += colors::ansi::CYAN;
    template_string += R"($SHIT_GIT_BRANCH)";
    template_string += colors::ansi::RESET;
    template_string += R"(})";
    template_string += R"(${SHIT_GIT_AHEAD:+ )";
    template_string += colors::ansi::BOLD_YELLOW;
    template_string += "\xe2\x86\x91";
    template_string += R"($SHIT_GIT_AHEAD)";
    template_string += colors::ansi::RESET;
    template_string += R"(})";
    template_string += R"(${SHIT_GIT_BEHIND:+ )";
    template_string += colors::ansi::BOLD_YELLOW;
    template_string += "\xe2\x86\x93";
    template_string += R"($SHIT_GIT_BEHIND)";
    template_string += colors::ansi::RESET;
    template_string += R"(})";
    template_string += R"(${SHIT_GIT_BRANCH:+ at }\u@\h )";
    template_string += colors::ansi::GREEN;
    template_string += R"(\P)";
    template_string += colors::ansi::RESET;
  } else {
    template_string += R"([${SHIT_GIT_BRANCH:+$SHIT_GIT_BRANCH})";
    template_string += R"(${SHIT_GIT_AHEAD:+ )";
    template_string += "\xe2\x86\x91";
    template_string += R"($SHIT_GIT_AHEAD})";
    template_string += R"(${SHIT_GIT_BEHIND:+ )";
    template_string += "\xe2\x86\x93";
    template_string += R"($SHIT_GIT_BEHIND})";
    template_string += R"(${SHIT_GIT_BRANCH:+ at }\u@\h \P)";
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
  let out = String{shit::heap_allocator()};
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
  let out = String{shit::heap_allocator()};
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
  let out = String{shit::heap_allocator()};
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
  static String CACHED_USER{shit::heap_allocator()};
  static bool was_user_resolved = false;
  if (!was_user_resolved) {
    CACHED_USER = os::get_current_user().value_or("???");
    was_user_resolved = true;
  }

  String ps1_template{shit::heap_allocator()};
  if (Maybe<String> ps1 = context.get_variable_value("PS1");
      ps1.has_value() && !ps1->is_empty())
    ps1_template = steal(*ps1);
  else
    ps1_template = default_prompt_template();

  /* The raw template expands before the backslash escapes are decoded, so the
     escape-inserted cwd and user are literal and never re-expanded. A directory
     named $(...) therefore cannot run a command at the prompt. */

  let scanned_inputs =
      shit::ArrayList<prompt_cache_input>{shit::heap_allocator()};
  const bool is_cacheable =
      scan_prompt_template_inputs(ps1_template.view(), scanned_inputs);
  if (is_cacheable && PROMPT_CACHE_VALID &&
      ps1_template.view() == PROMPT_CACHE_TEMPLATE.view())
  {
    bool is_every_input_unchanged = true;
    for (let const &input : PROMPT_CACHE_INPUTS) {
      let current = context.get_variable_value(input.name.view());
      const bool both_unset = !current.has_value() && !input.value.has_value();
      const bool both_equal = current.has_value() && input.value.has_value() &&
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
  String expanded{shit::heap_allocator()};
  try {
    expanded = unguard_prompt_backslashes(
        context.expand_heredoc_body(guarded.view()).view());
  } catch (const shit::ErrorBase &) {
    /* A prompt draw error leaves the template standing rather than taking down
       the shell. */
    expanded = ps1_template;
  }
  context.set_last_exit_status(saved_status);

  PROMPT_CACHE_VALID = false;
  if (is_cacheable) {
    for (let &input : scanned_inputs)
      input.value = context.get_variable_value(input.name.view());
    PROMPT_CACHE_TEMPLATE = ps1_template;
    PROMPT_CACHE_INPUTS = steal(scanned_inputs);
    PROMPT_CACHE_EXPANSION = expanded;
    PROMPT_CACHE_VALID = true;
  }

  String rendered = expand_prompt_escapes(expanded.view(), CACHED_USER.view(),
                                          full_pwd.view(), context);
  if (!colors::stdout_wants_color()) return strip_ansi_color(rendered.view());
  return rendered;
}

fn render_ps0(EvalContext &context) -> String
{
  Maybe<String> ps0 = context.get_variable_value("PS0");
  if (!ps0.has_value() || ps0->is_empty())
    return String{shit::heap_allocator()};

  const i32 saved_status = context.last_exit_status();
  String guarded = guard_prompt_backslashes(ps0->view());
  String expanded{shit::heap_allocator()};
  try {
    expanded = unguard_prompt_backslashes(
        context.expand_heredoc_body(guarded.view()).view());
  } catch (const shit::ErrorBase &) {
    context.set_last_exit_status(saved_status);
    return String{shit::heap_allocator()};
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

#else /* SHIT_NO_TOILETLINE */

/* The line editor is compiled out, so these stubs keep the shell linking. */
namespace toiletline {

using shit::String;
using shit::StringView;

struct input_result
{
  i32 code;
  String text;
};

fn enable_completion(shit::EvalContext &context) -> void { unused(context); }

fn disable_completion() -> void {}

fn completion_is_enabled() -> bool { return false; }

fn enter_calc_history() -> void {}

fn leave_calc_history() -> void {}

fn history_path() -> shit::Maybe<shit::Path> { return shit::None; }

fn history_write() -> bool { return true; }

fn history_read() -> bool { return true; }

fn history_clear() -> bool { return true; }

fn enable_job_notifications(shit::EvalContext &context) -> void
{
  unused(context);
}

fn set_ghost_enabled(bool enabled) -> void { unused(enabled); }

fn set_highlight_enabled(bool enabled) -> void { unused(enabled); }

fn set_edit_mode(edit_mode mode) -> void { unused(mode); }

fn utf8_strlen(const String &s, usize count) -> usize
{
  return (count != static_cast<usize>(-1) && count < s.length()) ? count
                                                                 : s.length();
}

fn utf8_strnlen(const char *bytes, usize byte_count) -> usize
{
  unused(bytes);
  return byte_count;
}

fn display_width(StringView text) -> usize { return text.length; }

fn byte_offset_at_or_before_display_cell(StringView text, usize cell_position,
                                         usize &actual_cell_position) -> usize
{
  actual_cell_position =
      cell_position < text.length ? cell_position : text.length;
  return actual_cell_position;
}

fn is_active() -> bool { return false; }

fn initialize() -> void
{
  throw shit::Error{
      "This build has no line editor, use '-c', '-s', or a file argument"};
}

fn exit() -> void {}

fn get_input(const String &prompt) -> input_result
{
  unused(prompt);
  throw shit::Error{"This build has no line editor"};
}

fn set_input(const String &input) -> void { unused(input); }

fn enter_raw_mode() -> void {}

fn exit_raw_mode() -> void {}

fn emit_newlines(StringView buffer) -> void { unused(buffer); }

fn default_prompt_template() -> String
{
  let template_string = String{};
  const bool should_use_color = shit::colors::stdout_wants_color();

  if (should_use_color) {
    template_string += "[\\u@\\h${SHIT_GIT_BRANCH:+ (";
    template_string += shit::colors::ansi::CYAN;
    template_string += "$SHIT_GIT_BRANCH";
    template_string += shit::colors::ansi::RESET;
    template_string += ")} ";
    template_string += shit::colors::ansi::GREEN;
    template_string += "\\P";
    template_string += shit::colors::ansi::RESET;
  } else {
    template_string += "[\\u@\\h${SHIT_GIT_BRANCH:+ ($SHIT_GIT_BRANCH)} \\P";
  }
  template_string += "] ";
  return template_string;
}

fn build_prompt(shit::EvalContext &context) -> String
{
  unused(context);
  throw shit::Error{"This build has no line editor"};
}

fn expand_prompt_template(StringView prompt, shit::EvalContext &context)
    -> String
{
  unused(context);
  return String{prompt};
}

fn render_ps0(shit::EvalContext &context) -> String
{
  unused(context);
  return String{};
}

} /* namespace toiletline */

#endif /* SHIT_NO_TOILETLINE */
