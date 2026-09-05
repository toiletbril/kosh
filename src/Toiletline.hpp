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
#include "Common.hpp"
#include "Path.hpp"
#include "String.hpp"
#include "StringView.hpp"

#define TL_HISTORY_MAX_SIZE (1024 * 4)
#include "toiletline/toiletline.h"

namespace koshka {
class EvalContext;
} /* namespace koshka */

namespace toiletline {

using koshka::String;
using koshka::StringView;

String default_prompt_template();

String build_prompt(koshka::EvalContext &context);

String expand_prompt_template(StringView prompt, koshka::EvalContext &context);

String render_ps0(koshka::EvalContext &context);

/* Called only on the interactive path, so a non-interactive run never enables
   it. */
void enable_completion(koshka::EvalContext &context);
void disable_completion();

bool completion_is_enabled();

void enter_calc_history();
void leave_calc_history();

koshka::Maybe<koshka::Path> history_path();
bool is_history_contents_valid(StringView contents);
bool history_write();
bool history_read();
bool history_clear();
void set_history_enabled(bool is_enabled);
void set_history_limit(usize entry_count);

struct history_event
{
  usize number;
  String command;
};

koshka::ArrayList<history_event> history_events(koshka::Allocator allocator);
koshka::Maybe<history_event>
relative_history_event(koshka::Allocator allocator, usize distance,
                       koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<history_event>
numbered_history_event(koshka::Allocator allocator, usize number,
                       koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<history_event>
prefixed_history_event(koshka::Allocator allocator, StringView prefix,
                       koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<history_event> containing_history_event(
    koshka::Allocator allocator, StringView text,
    koshka::Maybe<usize> before_event_number = koshka::None);
koshka::Maybe<usize> history_append_event(StringView command);
bool history_rewrite_event(usize number, StringView expected,
                           StringView replacement);
bool history_rewrite_event(usize number, StringView expected,
                           const koshka::ArrayList<String> &replacements);

void enable_job_notifications(koshka::EvalContext &context);

void set_ghost_enabled(bool enabled);

void set_highlight_enabled(bool enabled);

enum class edit_mode : u8
{
  Emacs,
  Vi,
};

void set_edit_mode(edit_mode mode);

usize utf8_strlen(const String &s, usize byte_count = static_cast<usize>(-1));

usize utf8_strnlen(const char *bytes, usize byte_count);

usize byte_offset_of_codepoint(const char *bytes, usize byte_length,
                               usize codepoint_index);

usize display_width(StringView text);

usize byte_offset_at_or_before_display_cell(StringView text,
                                            usize cell_position,
                                            usize &actual_cell_position);

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

bool debug_allocation_failure();

} /* namespace toiletline */
