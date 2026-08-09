#include "ArrayList.hpp"
#include "Common.hpp"
#include "Path.hpp"
#include "String.hpp"
#include "StringView.hpp"
#include "toiletline/toiletline.h"

namespace shit {
class EvalContext;
} /* namespace shit */

namespace toiletline {

using shit::String;
using shit::StringView;

String default_prompt_template();

String build_prompt(shit::EvalContext &context);

String expand_prompt_template(StringView prompt, shit::EvalContext &context);

String render_ps0(shit::EvalContext &context);

/* Called only on the interactive path, so a non-interactive run never enables
   it. */
void enable_completion(shit::EvalContext &context);
void disable_completion();

bool completion_is_enabled();

void enter_calc_history();
void leave_calc_history();

shit::Maybe<shit::Path> history_path();
bool history_write();
bool history_read();
bool history_clear();

struct history_event
{
  usize number;
  String command;
};

shit::ArrayList<history_event> history_events(shit::Allocator allocator);
shit::Maybe<usize> history_append_event(StringView command);
bool history_rewrite_event(usize number, StringView expected,
                           StringView replacement);

void enable_job_notifications(shit::EvalContext &context);

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

void exit();

struct input_result
{
  i32 code;
  String text{shit::heap_allocator()};
  shit::Maybe<usize> history_event_number{shit::None};
};

input_result get_input(const String &prompt);

void set_input(const String &input);

void enter_raw_mode();

void exit_raw_mode();

void emit_newlines(StringView buffer);

} /* namespace toiletline */
