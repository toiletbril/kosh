/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines the shell-facing editor interface for input, history,
 * prompts, completion, highlighting, terminal modes, and display-width
 * conversion. Toiletline.cpp adapts this interface to the vendored editor,
 * while ToiletlineStubs.cpp supplies noninteractive builds.
 */

#include "ArrayList.hpp"
#include "Completion.hpp"
#include "Common.hpp"
#include "ErrorOr.hpp"
#include "Path.hpp"
#include "String.hpp"
#include "StringView.hpp"
#include "ToiletlineHistory.hpp"

#define TL_HISTORY_MAX_SIZE (1024 * 4)
#include "toiletline/toiletline.h"

namespace koshka {
class EvalContext;
} /* namespace koshka */

namespace toiletline {

using koshka::String;
using koshka::StringView;

String get_default_prompt_template();

String build_prompt(koshka::EvalContext &context);

String expand_prompt_template(StringView prompt, koshka::EvalContext &context);

String render_ps0(koshka::EvalContext &context);

/* Called only on the interactive path, so a non-interactive run never enables
   it. */
void enable_completion(koshka::EvalContext &context);
void disable_completion();

bool is_completion_enabled();

void enter_calc_history();
void leave_calc_history();

koshka::Maybe<koshka::Path> get_history_path();
bool is_history_contents_valid(StringView contents);
void encode_history_record(String &output, StringView command);
koshka::ErrorOr<koshka::Ok> write_history();
koshka::ErrorOr<koshka::Ok> read_history();
koshka::ErrorOr<koshka::Ok> sync_history();
koshka::ErrorOr<koshka::Ok> clear_history();
void set_history_enabled(bool is_enabled);
void set_history_limit(usize entry_count);

struct history_event
{
  usize number;
  String command;
};

/* Only the events above after_event_number are decoded. A caller that writes an
   increment pays for the increment alone. A missing file is an empty list. */
koshka::ErrorOr<koshka::ArrayList<history_event>>
get_history_events(koshka::Allocator allocator,
                   koshka::Maybe<usize> after_event_number = koshka::None);
koshka::Maybe<usize> get_newest_history_event_number();
koshka::Maybe<history_event> get_relative_history_event(
    koshka::Allocator allocator, usize distance,
    koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<history_event> get_numbered_history_event(
    koshka::Allocator allocator, usize number,
    koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<history_event> get_prefixed_history_event(
    koshka::Allocator allocator, StringView prefix,
    koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<history_event> get_containing_history_event(
    koshka::Allocator allocator, StringView text,
    koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<usize> append_history_event(StringView command);
bool rewrite_history_event(usize number, StringView expected,
                           StringView replacement);
bool rewrite_history_event(usize number, StringView expected,
                           const koshka::ArrayList<String> &replacements);

void enable_job_notifications(koshka::EvalContext &context);

void set_ghost_enabled(bool enabled);

void set_space_after_completion(bool enabled);

void set_highlight_enabled(bool enabled);

void set_colors_enabled(bool enabled);

enum class edit_mode : u8
{
  Emacs,
  Vi,
};

void set_edit_mode(edit_mode mode);

void set_tab_selector(koshka::tab_selector_mode selector);

usize get_utf8_length(const String &string,
                      usize byte_count = static_cast<usize>(-1));

usize get_utf8_length(const char *bytes, usize byte_count);

usize get_codepoint_byte_offset(const char *bytes, usize byte_length,
                                usize codepoint_index);

usize get_display_width(StringView text);

usize get_byte_offset_at_or_before_display_cell(
    StringView text, usize cell_position, usize &actual_cell_position);

bool is_active();

void initialize();

void exit(usize history_size_limit = 4096);

void set_title(StringView title);
void set_idle_title();

struct input_result
{
  i32 code;
  String text{koshka::heap_allocator()};
  koshka::Maybe<usize> history_event_number{koshka::None};
};

input_result get_input(const String &prompt);

void set_input(const String &input);

void enter_raw_mode();

void exit_raw_mode();

void emit_newlines(StringView buffer);

bool did_debug_allocation_fail();

} /* namespace toiletline */
