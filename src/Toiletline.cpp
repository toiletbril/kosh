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

#include "CLI.hpp"
#include "CLIColors.hpp"
#include "Completion.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "ExecContext.hpp"
#include "Platform.hpp"
#include "ToiletlineHistory.hpp"
#include "Utils.hpp"
#include "base/Allocator.hpp"
#include "base/Debug.hpp"
#include "base/ErrorOr.hpp"
#include "base/Path.hpp"
#include "base/Trace.hpp"

namespace toiletline {

enum class edit_mode : u8
{
  Emacs,
  Vi,
};

fn get_codepoint_byte_offset(const char *bytes, usize byte_length,
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
  if (base != nullptr) {
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

/* The shell-facing limit is declared where the implementation macro is not
   visible. The two values are compared here. */
static_assert(toiletline::HISTORY_RECORD_MAX_DECODED_BYTE_COUNT ==
              ITL_STRING_MAX_LEN);

namespace {

koshka::EvalContext *COMPLETION_CONTEXT = nullptr;
const koshka::Path *COMPLETION_BASE_DIRECTORY = nullptr;
bool HIGHLIGHT_COLOR_ENABLED = false;
bool HIGHLIGHT_STYLED_UNDERLINES_ENABLED = false;
#if !defined NDEBUG
usize DEBUG_COMPLETION_CWD_CAPTURE_COUNT = 0;
usize DEBUG_COMPLETION_SOURCE_SCAN_COUNT = 0;
usize DEBUG_COMPLETION_MATERIALIZED_COUNT = 0;
usize DEBUG_COMPLETION_LISTING_COUNT = 0;
#endif

koshka::ArrayList<const char *> COMPLETION_CANDIDATE_POINTERS{
    koshka::heap_allocator()};
koshka::ArrayList<const char *> COMPLETION_DESCRIPTION_POINTERS{
    koshka::heap_allocator()};
koshka::completion::completion_result *COMPLETION_RESULT = nullptr;

/* The presentation the shell pushes before each prompt. */
koshka::tab_selector_mode TAB_SELECTOR{koshka::tab_selector_mode::Interactive};

/* The external selector hands the candidates to a filtering program, and what
   it prints replaces them. The engine, the replaced token span, and the quoting
   are untouched. Only the presentation moves. Every failure below leaves the
   result alone. The printed list still appears. */
constexpr koshka::StringView SELECTOR_COMMAND_VARIABLE{
    "KOSH_FZF_COMPLETION_COMMAND"};
constexpr koshka::StringView SELECTOR_OPTIONS_VARIABLE{
    "KOSH_FZF_COMPLETION_OPTS"};

/* Seeded into the shell before the first prompt. The picker and its
   presentation are visible and editable. The record framing is absent, since
   --read0 and --print0 are the protocol between the shell and the picker. */
constexpr koshka::StringView DEFAULT_SELECTOR_COMMAND{"fzf"};
constexpr koshka::StringView DEFAULT_SELECTOR_OPTIONS{
    "--multi --layout=reverse --height=~40% --min-height=3"};

/* The picker the variable names, or the reason the selector cannot run. */
fn resolve_selector_program(koshka::EvalContext &context) throws
    -> koshka::ErrorOr<koshka::Path>
{
  let const configured = context.get_variable_value(SELECTOR_COMMAND_VARIABLE);
  if (configured.has_value() && configured->is_empty()) {
    return koshka::Error{SELECTOR_COMMAND_VARIABLE +
                         " is empty and names no tab selector"};
  }

  let const command_name =
      configured.has_value() ? configured->view() : DEFAULT_SELECTOR_COMMAND;

  let const resolved = context.get_program_resolver().search(
      command_name, koshka::ProgramResolver::SearchMode::First,
      koshka::ProgramResolver::Requirement::Execution,
      koshka::ProgramResolver::CachePolicy::ReadOnly);
  if (resolved.is_empty()) {
    return koshka::Error{koshka::StringView{"The tab selector '"} +
                         command_name + "' was not found"};
  }

  return koshka::Path{resolved[0].text()};
}

/* The variables the external selector reads, with the value each one holds.
   Every selector failure carries this as its note. */
fn selector_variable_note(koshka::EvalContext &context) throws -> koshka::String
{
  let note = koshka::String{koshka::heap_allocator()};

  let const do_append_variable =
      [&note, &context](koshka::StringView name,
                        koshka::StringView fallback) -> void {
    note.append(name);
    note.append(" is ");

    let const value = context.get_variable_value(name);
    if (!value.has_value()) {
      note.append("unset and defaults to '");
      note.append(fallback);
      note.push('\'');
      return;
    }

    note.push('\'');
    note.append(value->view());
    note.push('\'');
  };

  do_append_variable(SELECTOR_COMMAND_VARIABLE, DEFAULT_SELECTOR_COMMAND);
  note.append(", and ");
  do_append_variable(SELECTOR_OPTIONS_VARIABLE, DEFAULT_SELECTOR_OPTIONS);

  note.append(". Set ");
  note.append(SELECTOR_COMMAND_VARIABLE);
  note.append(" to a program that filters the candidates, or choose another "
              "presentation with `set --tab-selector`");

  return note;
}

/* Records are NUL separated in both directions, so a candidate carrying a
   newline still travels as one field. A description rides after a tab, which
   makes it searchable while never reaching the line. */
fn build_selector_input(const koshka::completion::completion_result &result)
    throws -> koshka::String
{
  let input = koshka::String{koshka::heap_allocator()};
  let const has_descriptions = result.descriptions.count() > 0;
  for (let const &candidate : result.candidates) {
    input.append(candidate.view());
    if (has_descriptions) {
      if (const koshka::String *description =
              result.descriptions.find(candidate.view());
          description != nullptr && !description->is_empty())
      {
        input.push('\t');
        input.append(description->view());
      }
    }
    input.push('\0');
  }
  return input;
}

fn build_selector_args(koshka::EvalContext &context,
                       const koshka::Path &program) throws
    -> koshka::ArrayList<koshka::String>
{
  let args = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  args.push(koshka::String{program.view()});

  /* The records travel NUL separated in both directions. The framing belongs
     to the shell and stays out of the variable. */
  args.push(koshka::String{"--read0"});
  args.push(koshka::String{"--print0"});

  /* The value is split on blanks with no expansion, since a completion
     keystroke must never run a substitution. An unset variable carries the
     defaults. The seeded value holds the defaults. */
  let const options = context.get_variable_value(SELECTOR_OPTIONS_VARIABLE);
  let const option_text =
      options.has_value() ? options->view() : DEFAULT_SELECTOR_OPTIONS;

  for (let const &option :
       context.expand_wordlist_to_fields(option_text, false))
    args.push(koshka::String{option.view()});

  return args;
}

/* The selected records, with each description stripped back off. A source that
   sent no descriptions keeps its tabs, since they are data there. */
fn parse_selector_reply(koshka::StringView reply, bool has_descriptions) throws
    -> koshka::ArrayList<koshka::String>
{
  let selected = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  usize start = 0;
  for (usize i = 0; i <= reply.length; i++) {
    if (i != reply.length && reply[i] != '\0') continue;
    if (i > start) {
      let record = reply.substring_of_length(start, i - start);
      if (has_descriptions) {
        if (let const tab = record.find_character('\t'); tab.has_value())
          record = record.substring_of_length(0, *tab);
      }
      /* fzf writes a trailing newline of its own under some option sets. */
      while (!record.is_empty() && record[record.length - 1] == '\n')
        record = record.substring_of_length(0, record.length - 1);
      if (!record.is_empty()) selected.push(koshka::String{record});
    }
    start = i + 1;
  }
  return selected;
}

enum class selector_outcome : u8
{
  /* The selector did not run, so the printed list still answers the key. */
  NotRun,
  /* The result now holds the replacement the user chose. */
  Selected,
  /* The user dismissed the selector, so the key does nothing at all. Printing
     the list here would dump every candidate over the screen the user just
     chose to keep. */
  Dismissed,
};

/* Handing the editor screen back erases every row below the prompt. A message
   survives only after that. A failure that happens before the picker starts
   cycles the screen itself to reach the same place. The message is left above
   the repainted prompt. */
fn report_selector_failure(koshka::StringView message, koshka::StringView note,
                           bool should_hand_back_screen) throws -> void
{
  if (should_hand_back_screen) {
    koshka::flush();
    if (::tl_begin_external_screen() != TL_SUCCESS) return;
    if (::tl_end_external_screen() != TL_SUCCESS) return;
  }

  koshka::show_message(koshka::ErrorWithDetails{message, note}.to_string());
}

/* Hand the framed records to the configured picker and collect what it printed
   back. The editor screen belongs to the child for the length of the run, and
   every failure leaves out_selected empty so the caller can answer the key on
   its own. */
fn run_selector_program(koshka::EvalContext &context, koshka::StringView input,
                        bool has_descriptions,
                        koshka::ArrayList<koshka::String> &out_selected) throws
    -> selector_outcome
{
  if (!::itl_g_is_active) return selector_outcome::NotRun;
  if (!koshka::os::shell_has_controlling_terminal())
    return selector_outcome::NotRun;

  let const program = resolve_selector_program(context);
  if (program.is_error()) {
    let const note = selector_variable_note(context);
    report_selector_failure(program.error().message(), note.view(), true);
    return selector_outcome::NotRun;
  }
  let const &selector_program = program.value();

  let const input_fd = koshka::os::write_to_temp_file(input);
  if (!input_fd.has_value()) return selector_outcome::NotRun;

  let const output_pipe = koshka::os::make_pipe();
  if (!output_pipe.has_value()) {
    koshka::os::close_fd(*input_fd);
    return selector_outcome::NotRun;
  }

  /* execute_program owns both descriptors it is handed and closes them on every
     path, so only the read end is released here. */
  bool are_child_descriptors_owned = true;
  bool is_read_end_open = true;
  defer
  {
    if (are_child_descriptors_owned) {
      koshka::os::close_fd(*input_fd);
      koshka::os::close_fd(output_pipe->out);
    }
    if (is_read_end_open) koshka::os::close_fd(output_pipe->in);
  };

  let arg_locations =
      koshka::ArrayList<koshka::SourceLocation>{koshka::heap_allocator()};
  let selector = koshka::ExecContext::from_resolved(
      koshka::SourceLocation{},
      koshka::ResolvedCommand::from_program(
          koshka::Path{selector_program.text()}),
      build_selector_args(context, selector_program), steal(arg_locations));
  selector.in_fd = *input_fd;
  selector.out_fd = output_pipe->out;

  koshka::flush();
  if (::tl_begin_external_screen() != TL_SUCCESS)
    return selector_outcome::NotRun;
  bool is_editor_suspended = true;
  defer
  {
    if (is_editor_suspended) unused(::tl_end_external_screen());
  };

  are_child_descriptors_owned = false;
  let const child =
      koshka::os::execute_program(selector, koshka::StringView{}, 0,
                                  koshka::os::script_fallback_policy::Reject,
                                  koshka::os::terminal_handoff::BeforeStart,
                                  koshka::os::process_group_mode::Inherit);

  let const captured =
      koshka::os::read_fd_to_string(output_pipe->in, koshka::heap_allocator());
  koshka::os::close_fd(output_pipe->in);
  is_read_end_open = false;

  let const status = koshka::os::wait_and_monitor_process(child);
  koshka::os::reclaim_controlling_terminal();
  /* A cancelled picker exits nonzero, and its own SIGINT must not abort the
     next command the way an interrupted prompt would. */
  koshka::os::INTERRUPT_REQUESTED = 0;

  is_editor_suspended = false;
  if (::tl_end_external_screen() != TL_SUCCESS) return selector_outcome::NotRun;

  /* A picker reports 0 for a selection, 1 when nothing matched, and 130 when it
     was dismissed. A status it never uses for either says the picker could not
     do its work. The failure is reported before the printed list answers the
     key. */
  if (status != 0) {
    if (status == 1 || status == 130) return selector_outcome::Dismissed;

    let const note = selector_variable_note(context);
    report_selector_failure(
        koshka::StringView{"The tab selector '"} + selector_program.view() +
            "' exited with status " +
            koshka::String::from(status, koshka::heap_allocator()),
        note.view(), false);

    return selector_outcome::NotRun;
  }

  if (!captured.has_value()) return selector_outcome::NotRun;

  out_selected = parse_selector_reply(captured->view(), has_descriptions);
  if (out_selected.is_empty()) return selector_outcome::Dismissed;

  return selector_outcome::Selected;
}

fn run_completion_selector(koshka::EvalContext &context,
                           koshka::completion::completion_result &result,
                           usize token_codepoint_count) throws
    -> selector_outcome
{
  if (TAB_SELECTOR != koshka::tab_selector_mode::External)
    return selector_outcome::NotRun;

  if (::tl_utf8_strlen(result.longest_common_prefix.c_str()) >
      token_codepoint_count)
  {
    return selector_outcome::NotRun;
  }

  if (result.candidates.count() < 2) return selector_outcome::NotRun;

  let selected = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  let const outcome =
      run_selector_program(context, build_selector_input(result).view(),
                           result.descriptions.count() > 0, selected);
  if (outcome != selector_outcome::Selected) return outcome;

  /* Several picks join into one replacement, the same shape the inline glob
     expansion produces, so the token is rewritten once. */
  let replacement = koshka::String{koshka::heap_allocator()};
  for (usize i = 0; i < selected.count(); i++) {
    if (i > 0) replacement.push(' ');
    replacement.append(selected[i].view());
  }

  result.candidates.clear();
  result.descriptions.clear();
  result.longest_common_prefix =
      koshka::String{koshka::heap_allocator(), replacement.view()};
  result.candidates.push(steal(replacement));
  result.candidate_count = 1;
  return selector_outcome::Selected;
}

/* The history entry the picker last chose. Toiletline reads the pointer after
   the callback returns. The bytes outlive the call and are replaced by the
   next one. */
koshka::String SELECTED_HISTORY_ENTRY{koshka::heap_allocator()};

/* Ctrl-R reaches the same picker the completion candidates use, with the
   matching history entries as its records. A picker that runs and is dismissed
   answers the key on its own, and every other path hands ctrl-R back to the
   editor. */
fn kosh_history_select_callback(const char *const *entries, size_t count,
                                const char **out_selected) -> int
{
  if (COMPLETION_CONTEXT == nullptr) return 0;
  if (TAB_SELECTOR != koshka::tab_selector_mode::External) return 0;
  if (entries == nullptr || count == 0) return 0;

  /* Toiletline calls this through a C function pointer. A throw unwinding past
     this frame is undefined behavior. */
  try {
    let input = koshka::String{koshka::heap_allocator()};
    for (size_t i = 0; i < count; i++) {
      input.append(koshka::StringView{entries[i]});
      input.push('\0');
    }

    let selected = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
    let const outcome = run_selector_program(*COMPLETION_CONTEXT, input.view(),
                                             false, selected);
    if (outcome == selector_outcome::NotRun) return 0;
    if (outcome == selector_outcome::Dismissed) return -1;
    if (selected.is_empty()) return 0;

    /* A multiple selection has no meaning for one line. The first pick wins. */
    SELECTED_HISTORY_ENTRY =
        koshka::String{koshka::heap_allocator(), selected[0].view()};
    *out_selected = SELECTED_HISTORY_ENTRY.c_str();
    return 1;
  } catch (...) {
    return 0;
  }
}

/* Toiletline edits in codepoints while the completion engine works in bytes. */
fn kosh_completion_callback(const char *buffer, size_t cursor,
                            tl_completion *out, int for_listing) -> int
{
  if (COMPLETION_CONTEXT == nullptr || COMPLETION_BASE_DIRECTORY == nullptr ||
      COMPLETION_RESULT == nullptr)
  {
    return 0;
  }

  /* Toiletline calls this through a C function pointer. A throw unwinding past
     this frame is undefined behavior. The body is guarded and any throw is
     swallowed. */
  try {
    let const is_explicit_completion = for_listing != 0;
    if (is_explicit_completion) {
#if !defined NDEBUG
      DEBUG_COMPLETION_LISTING_COUNT++;
#endif
      COMPLETION_CONTEXT->get_program_resolver().begin_explicit_completion(
          koshka::ProgramResolver::CompletionRefresh::Cached);
    }
    defer
    {
      if (is_explicit_completion)
        COMPLETION_CONTEXT->get_program_resolver().end_explicit_completion();
    };

    /* A completion spec can shell out, and a command talking to an unreachable
       host blocks until its own timeout. Raw mode swallows the interrupt key as
       an ordinary byte, so it is handed back to the terminal for the length of
       the completion and reaches that command as SIGINT the way it would at a
       prompt. The ghost path runs no spec, so it is left alone. */
    let const are_signal_keys_enabled =
        is_explicit_completion && ::tl_set_signal_keys(1) == TL_SUCCESS;
    defer
    {
      if (are_signal_keys_enabled) unused(::tl_set_signal_keys(0));
      /* An interrupt belongs to the abandoned completion, not to whatever the
         user types next. */
      koshka::os::INTERRUPT_REQUESTED = 0;
    };

    const usize byte_length = std::strlen(buffer);
    let line = koshka::StringView{buffer, byte_length};

    const usize byte_cursor =
        toiletline::get_codepoint_byte_offset(buffer, byte_length, cursor);

    /* A completion diagnostic is armed to break onto its own line, then
       disarmed so a later command's message is unaffected. */
    koshka::arm_message_leading_newline(true);
    COMPLETION_RESULT->candidates.clear();
    COMPLETION_RESULT->descriptions.clear();
    COMPLETION_RESULT->longest_common_prefix.clear();
    *COMPLETION_RESULT = koshka::completion::complete(
        line, byte_cursor, *COMPLETION_CONTEXT, *COMPLETION_BASE_DIRECTORY,
        nullptr, false,
        for_listing != 0 ? koshka::completion::completion_mode::Listing
                         : koshka::completion::completion_mode::Ghost);
    let const &result = *COMPLETION_RESULT;
#if !defined NDEBUG
    DEBUG_COMPLETION_SOURCE_SCAN_COUNT += result.source_candidate_scan_count;
    DEBUG_COMPLETION_MATERIALIZED_COUNT += result.materialized_candidate_count;
#endif
    koshka::arm_message_leading_newline(false);

    /* An abandoned completion offers nothing, so the key leaves the line as it
       was rather than acting on whatever the interrupted spec had produced. */
    if (koshka::os::INTERRUPT_REQUESTED) return 0;

    /* An explicit tab may route the candidates through a filtering picker
       first. The ghost never does, since it draws no list and must not fork. */
    let const token_start_codepoint =
        ::tl_utf8_strnlen(buffer, result.token_start);
    let const token_codepoint_count =
        cursor >= token_start_codepoint ? cursor - token_start_codepoint : 0;
    if (is_explicit_completion &&
        run_completion_selector(*COMPLETION_CONTEXT, *COMPLETION_RESULT,
                                token_codepoint_count) ==
            selector_outcome::Dismissed)
    {
      return 0;
    }

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
      if (JOB_CONTEXT == nullptr || !JOB_CONTEXT->notify()) return 0;
      WAKE_NOTIFICATION_STASH =
          JOB_CONTEXT->format_done_job_notifications("\r\n");
      koshka::os::CHILD_STATE_CHANGED = 0;
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

fn is_history_contents_valid(koshka::StringView contents) -> bool;
fn encode_history_record(koshka::String &output, koshka::StringView command)
    -> void;

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

static constexpr char DEFAULT_HISTORY_FILE[] = ".kosh_history";

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

static os::file_status HISTORY_FILE_STATUS{};
static bool HAS_HISTORY_FILE_STATUS = false;
static bool CAN_REWRITE_HISTORY = false;

/* A rename that commits leaves the file correct even when the reload that
   follows it fails. A caller that only wrote bytes accepts the second outcome,
   while a caller that rewrote entries in place cannot. */
enum class history_replacement : u8
{
  Replaced,
  NotReloaded,
  Failed,
};

static fn load_history(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>;
static fn ensure_history_loaded(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>;
static fn sync_history(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>;
static fn commit_history_replacement(const Path &path, const Path &parent,
                                     StringView name_prefix,
                                     StringView contents) -> bool;
static fn replace_history_file(const Path &path, const Path &parent,
                               StringView name_prefix, StringView contents)
    -> history_replacement;
static fn record_history_file_status(const Path &path) -> void;

static fn get_history_file_path() -> koshka::Maybe<koshka::Path>
{
  return resolve_history_path("KOSH_HISTORY_FILE", DEFAULT_HISTORY_FILE);
}

static fn get_calc_history_file_path() -> koshka::Maybe<koshka::Path>
{
  return resolve_history_path("KOSH_CALC_HISTORY", KOSH_CALC_HISTORY_FILE);
}

static String HISTORY_SEARCH_SNAPSHOT{koshka::heap_allocator()};

static fn provide_history_search_snapshot(const char **out_contents,
                                          size_t *out_size) -> int
{
  if (out_contents == nullptr || out_size == nullptr) return 0;

  try {
    let const path = get_history_file_path();
    if (!path.has_value()) return 0;
    let const parent = path->parent_or_current();
    let lock = os::acquire_process_lock(parent.view());
    if (!lock.has_value()) return 0;
    defer { os::release_process_lock(lock.take()); };

    for (int attempt_index = 0; attempt_index < HISTORY_RACE_ATTEMPT_COUNT;
         attempt_index++)
    {
      let status_before = os::file_status{};
      if (!os::stat_path_following(path->text().view(), status_before)) {
        if (!os::last_system_error_is_missing_file()) return 0;

        HISTORY_SEARCH_SNAPSHOT.clear();
        *out_contents = HISTORY_SEARCH_SNAPSHOT.data();
        *out_size = 0;
        return 1;
      }

      let contents = path->read_entire_file();
      if (!contents.has_value()) continue;
      let status_after = os::file_status{};
      if (!os::stat_path_following(path->text().view(), status_after)) continue;
      if (!os::file_status_matches(status_before, status_after)) continue;
      if (!is_history_contents_valid(contents->view())) return 0;

      HISTORY_SEARCH_SNAPSHOT = contents.take();
      *out_contents = HISTORY_SEARCH_SNAPSHOT.data();
      *out_size = HISTORY_SEARCH_SNAPSHOT.count();
      return 1;
    }
  } catch (...) {}

  return 0;
}

static bool IS_CALC_HISTORY_ACTIVE = false;

struct history_snapshot
{
  String path{koshka::heap_allocator()};
  String encoded_contents{koshka::heap_allocator()};
  koshka::ArrayList<usize> offsets{koshka::heap_allocator()};
  koshka::ArrayList<usize> durable_offsets{koshka::heap_allocator()};
  os::file_status file_status{};
  usize total_count{0};
  usize last_event_number{0};
  usize file_byte_count{0};
  usize entry_limit{TL_HISTORY_MAX_SIZE};
  usize buffer_byte_offset{0};
  usize buffer_start_byte_offset{0};
  bool is_enabled{true};
  bool is_file_bad{false};
  bool does_file_end_with_newline{true};
  bool has_buffer{false};
  bool is_buffer_loaded{false};
  bool has_file_status{false};
  bool can_rewrite{false};
  bool is_valid{false};
};

static history_snapshot SHELL_HISTORY_SNAPSHOT{};
static history_snapshot CALC_HISTORY_SNAPSHOT{};

static fn capture_history_snapshot(history_snapshot &snapshot) -> void
{
  snapshot.path.clear();
  if (::itl_g_history_path != nullptr)
    snapshot.path.append(StringView{::itl_g_history_path});

  snapshot.encoded_contents.clear();
  snapshot.has_buffer = ::itl_g_history_read_buffer != nullptr;
  if (snapshot.has_buffer) {
    snapshot.encoded_contents.append(StringView{
        ::itl_g_history_read_buffer->data, ::itl_g_history_read_buffer->size});
  }

  snapshot.offsets.clear();
  snapshot.durable_offsets.clear();
  snapshot.offsets.reserve(::itl_g_history_count);
  snapshot.durable_offsets.reserve(::itl_g_history_count);
  for (usize index = 0; index < ::itl_g_history_count; index++) {
    let const slot = (::itl_g_history_head + index) % TL_HISTORY_MAX_SIZE;
    snapshot.offsets.push(::itl_g_history_offsets[slot]);
    snapshot.durable_offsets.push(::itl_g_history_durable_offsets[slot]);
  }
  snapshot.file_status = HISTORY_FILE_STATUS;
  snapshot.total_count = ::itl_g_history_total_count;
  snapshot.last_event_number = ::itl_g_last_history_event_number;
  snapshot.file_byte_count = ::itl_g_history_file_size;
  snapshot.entry_limit = ::itl_g_history_limit;
  snapshot.buffer_byte_offset = ::itl_g_history_read_buffer_offset;
  snapshot.buffer_start_byte_offset = ::itl_g_history_read_buffer_start;
  snapshot.is_enabled = ::itl_g_history_enabled;
  snapshot.is_file_bad = ::itl_g_history_file_is_bad;
  snapshot.does_file_end_with_newline = ::itl_g_history_ends_with_newline;
  snapshot.is_buffer_loaded = ::itl_g_history_read_buffer_loaded;
  snapshot.has_file_status = HAS_HISTORY_FILE_STATUS;
  snapshot.can_rewrite = CAN_REWRITE_HISTORY;
  snapshot.is_valid = true;
}

static fn restore_history_snapshot(const history_snapshot &snapshot) -> void
{
  ::itl_g_history_free();

  if (!snapshot.path.is_empty()) {
    ::itl_g_history_path =
        static_cast<char *>(::itl_malloc(snapshot.path.count() + 1));
    std::memcpy(::itl_g_history_path, snapshot.path.data(),
                snapshot.path.count());
    ::itl_g_history_path[snapshot.path.count()] = '\0';
  }

  for (usize index = 0; index < snapshot.offsets.count(); index++) {
    ::itl_g_history_offsets[index] = snapshot.offsets[index];
    ::itl_g_history_durable_offsets[index] = snapshot.durable_offsets[index];
  }
  ::itl_g_history_head = 0;
  ::itl_g_history_count = snapshot.offsets.count();
  ::itl_g_history_total_count = snapshot.total_count;
  ::itl_g_last_history_event_number = snapshot.last_event_number;
  ::itl_g_history_file_size = snapshot.file_byte_count;
  ::itl_g_history_limit = snapshot.entry_limit;
  ::itl_g_history_enabled = snapshot.is_enabled;
  ::itl_g_history_file_is_bad = snapshot.is_file_bad;
  ::itl_g_history_ends_with_newline = snapshot.does_file_end_with_newline;

  if (snapshot.has_buffer) {
    ::itl_g_history_read_buffer = ::itl_char_buf_alloc();
    ::itl_char_buf_append_bytes(::itl_g_history_read_buffer,
                                snapshot.encoded_contents.data(),
                                snapshot.encoded_contents.count());
  }
  ::itl_g_history_read_buffer_loaded = snapshot.is_buffer_loaded;
  ::itl_g_history_read_buffer_offset = snapshot.buffer_byte_offset;
  ::itl_g_history_read_buffer_start = snapshot.buffer_start_byte_offset;

  HISTORY_FILE_STATUS = snapshot.file_status;
  HAS_HISTORY_FILE_STATUS = snapshot.has_file_status;
  CAN_REWRITE_HISTORY = snapshot.can_rewrite;
}

/* Every read and append resolves the file the swap currently points at. A calc
   prompt never reloads the shell file over the calc entries. */
static fn get_active_history_file_path() -> koshka::Maybe<koshka::Path>
{
  if (IS_CALC_HISTORY_ACTIVE) return get_calc_history_file_path();

  return get_history_file_path();
}

fn enter_calc_history() -> void
{
  capture_history_snapshot(SHELL_HISTORY_SNAPSHOT);

  IS_CALC_HISTORY_ACTIVE = true;

  if (CALC_HISTORY_SNAPSHOT.is_valid) {
    restore_history_snapshot(CALC_HISTORY_SNAPSHOT);
    CALC_HISTORY_SNAPSHOT = history_snapshot{};
  } else if (koshka::Maybe<koshka::Path> calc = get_calc_history_file_path();
             calc.has_value())
  {
    unused(load_history(*calc, true));
  }
}

fn leave_calc_history() -> void
{
  capture_history_snapshot(CALC_HISTORY_SNAPSHOT);

  IS_CALC_HISTORY_ACTIVE = false;

  if (SHELL_HISTORY_SNAPSHOT.is_valid) {
    restore_history_snapshot(SHELL_HISTORY_SNAPSHOT);
    SHELL_HISTORY_SNAPSHOT = history_snapshot{};
  }
}

fn get_history_path() -> koshka::Maybe<koshka::Path>
{
  return get_history_file_path();
}

/* Every entry is appended to the file as it is stored. A write only has to
   drop the leading records the bounded list no longer reaches. */
fn write_history() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};

  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return koshka::Error{os::last_system_error_message()};
  defer { os::release_process_lock(lock.take()); };
  TRY(ensure_history_loaded(*path, true));
  if (!::itl_history_ensure_read_buffer() && ::itl_g_history_count != 0)
    return koshka::Error{"the file contains invalid data"};

  let written = String{koshka::heap_allocator()};
  let durable_offsets = koshka::ArrayList<usize>{koshka::heap_allocator()};
  durable_offsets.reserve(::itl_g_history_count);
  let const private_contents =
      ::itl_g_history_read_buffer == nullptr
          ? StringView{}
          : StringView{::itl_g_history_read_buffer->data,
                       ::itl_g_history_read_buffer->size};
  for (usize index = 0; index < ::itl_g_history_count; index++) {
    let const start_offset = ::itl_history_index_to_offset(index);
    if (start_offset < ::itl_g_history_read_buffer_offset)
      return koshka::Error{"the file contains invalid data"};
    let const start_offset_in_buffer =
        start_offset - ::itl_g_history_read_buffer_offset;
    usize end_offset_in_buffer = start_offset_in_buffer;
    bool is_escape_pending = false;
    bool did_find_end = false;
    while (end_offset_in_buffer < private_contents.length) {
      let const byte = private_contents[end_offset_in_buffer++];
      if (is_escape_pending) {
        is_escape_pending = false;
      } else if (byte == '\\') {
        is_escape_pending = true;
      } else if (byte == '\n') {
        did_find_end = true;
        break;
      }
    }
    if (!did_find_end) return koshka::Error{"the file contains invalid data"};
    durable_offsets.push(written.count());
    written.append(private_contents.substring_of_length(
        start_offset_in_buffer, end_offset_in_buffer - start_offset_in_buffer));
  }

  if (!commit_history_replacement(*path, parent, ".kosh_history_write",
                                  written.view()))
  {
    return koshka::Error{os::last_system_error_message()};
  }

  for (usize index = 0; index < ::itl_g_history_count; index++) {
    let const slot = (::itl_g_history_head + index) % TL_HISTORY_MAX_SIZE;
    ::itl_g_history_durable_offsets[slot] = durable_offsets[index];
  }
  ::itl_g_history_file_size = written.count();
  ::itl_g_history_file_is_bad = false;
  record_history_file_status(*path);
  CAN_REWRITE_HISTORY =
      HAS_HISTORY_FILE_STATUS && HISTORY_FILE_STATUS.has_file_identity;
  return koshka::Success;
}

static fn load_history(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>
{
  HAS_HISTORY_FILE_STATUS = false;
  CAN_REWRITE_HISTORY = false;
  for (int attempt_index = 0; attempt_index < HISTORY_RACE_ATTEMPT_COUNT;
       attempt_index++)
  {
    let status_before = os::file_status{};
    let const had_status_before =
        os::stat_path_following(path.view(), status_before);

    if (::tl_history_load(path.c_str()) != TL_SUCCESS) {
      let const history_errno = errno;
      let const was_missing = !::itl_g_history_file_is_bad;
      if (!should_allow_missing || !was_missing) {
        return koshka::Error{history_errno == EINVAL
                                 ? StringView{"the file contains invalid data"}
                                 : StringView{strerror(history_errno)}};
      }

      let status_after = os::file_status{};
      if (!os::stat_path_following(path.view(), status_after)) {
        if (os::last_system_error_is_missing_file()) {
          ::itl_history_offsets_reset();
          ::itl_g_last_history_event_number = 0;
          ::itl_g_history_file_size = 0;
          ::itl_g_history_ends_with_newline = true;
          ::itl_g_history_file_is_bad = false;
          CAN_REWRITE_HISTORY = false;
          ::itl_g_history_read_buffer = ::itl_char_buf_alloc();
          ::itl_g_history_read_buffer_loaded = true;
          ::itl_g_history_read_buffer_offset = 0;
          ::itl_g_history_read_buffer_start = 0;
          return koshka::Success;
        }

        return koshka::Error{os::last_system_error_message()};
      }
      continue;
    }

    let status_after = os::file_status{};
    if (!os::stat_path_following(path.view(), status_after)) continue;
    if (!had_status_before ||
        !os::file_status_matches(status_before, status_after))
    {
      continue;
    }

    HISTORY_FILE_STATUS = status_after;
    HAS_HISTORY_FILE_STATUS = true;
    CAN_REWRITE_HISTORY = status_after.has_file_identity;
    return koshka::Success;
  }

  return koshka::Error{"the file kept changing"};
}

static fn sync_history(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>
{
  let status = os::file_status{};
  if (::itl_g_history_path != nullptr &&
      StringView{::itl_g_history_path} == path.view() &&
      !::itl_g_history_file_is_bad && HAS_HISTORY_FILE_STATUS &&
      os::stat_path_following(path.view(), status) &&
      os::file_status_matches(HISTORY_FILE_STATUS, status))
  {
    return koshka::Success;
  }

  return load_history(path, should_allow_missing);
}

static fn ensure_history_loaded(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>
{
  if (::itl_g_history_path != nullptr &&
      StringView{::itl_g_history_path} == path.view())
  {
    return koshka::Success;
  }

  return load_history(path, should_allow_missing);
}

/* The replacement is written beside the history file and renamed over it. No
   reader observes a partial file. The caller holds the process lock. */
static fn commit_history_replacement(const Path &path, const Path &parent,
                                     StringView name_prefix,
                                     StringView contents) -> bool
{
  let const replacement_path =
      os::write_to_named_temp_file(parent, name_prefix, contents);
  if (!replacement_path.has_value()) return false;
  defer { unused(os::remove_file(replacement_path->view())); };

  return os::rename_path(replacement_path->view(), path.view());
}

/* A caller that cannot describe the new file itself reloads it here. */
static fn replace_history_file(const Path &path, const Path &parent,
                               StringView name_prefix, StringView contents)
    -> history_replacement
{
  if (!commit_history_replacement(path, parent, name_prefix, contents))
    return history_replacement::Failed;

  if (load_history(path, false).is_error())
    return history_replacement::NotReloaded;

  return history_replacement::Replaced;
}

/* A session that does not hold the process lock can append between the write
   and the stat. A size the vendored state does not track leaves the status
   unrecorded and the next sync loads the file. */
static fn record_history_file_status(const Path &path) -> void
{
  HAS_HISTORY_FILE_STATUS =
      os::stat_path_following(path.view(), HISTORY_FILE_STATUS);
  if (HAS_HISTORY_FILE_STATUS &&
      HISTORY_FILE_STATUS.size != ::itl_g_history_file_size)
  {
    HAS_HISTORY_FILE_STATUS = false;
  }
}

static fn update_history_rewrite_safety_after_append(
    bool was_empty, const os::file_status &previous_status,
    bool had_previous_status, const os::file_status &appended_status) -> void
{
  if (!appended_status.has_file_identity) {
    CAN_REWRITE_HISTORY = false;
  } else if (had_previous_status && previous_status.has_file_identity) {
    CAN_REWRITE_HISTORY =
        CAN_REWRITE_HISTORY &&
        previous_status.device_id == appended_status.device_id &&
        previous_status.file_id == appended_status.file_id;
  } else {
    CAN_REWRITE_HISTORY = was_empty;
  }
}

fn read_history() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};

  return sync_history(*path, false);
}

fn sync_history() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return koshka::Error{os::last_system_error_message()};
  defer { os::release_process_lock(lock.take()); };

  return load_history(*path, true);
}

fn clear_history() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return koshka::Error{os::last_system_error_message()};
  defer { os::release_process_lock(lock.take()); };
  let opened = koshka::os::open_file_descriptor(
      path->view(), koshka::os::file_open_mode::Truncate);
  if (!opened.has_value())
    return koshka::Error{os::last_system_error_message()};
  if (!koshka::os::close_fd(opened.take()))
    return koshka::Error{os::last_system_error_message()};

  return load_history(*path, false);
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

fn get_newest_history_event_number() -> koshka::Maybe<usize>
{
  let const path = get_history_file_path();
  if (!path.has_value() || ensure_history_loaded(*path, true).is_error())
    return koshka::None;
  if (::itl_g_history_count == 0) return koshka::None;

  return ::itl_g_history_total_count;
}

fn get_history_events(koshka::Allocator allocator,
                      koshka::Maybe<usize> after_event_number)
    -> koshka::ErrorOr<koshka::ArrayList<history_event>>
{
  let events = koshka::ArrayList<history_event>{allocator};
  let const path = get_history_file_path();
  if (!path.has_value()) return events;

  /* The load is allowed to miss the file. An absent history reads as an empty
     list and a damaged or unreadable file reads as a failure. */
  TRY(ensure_history_loaded(*path, true));
  if (::itl_g_history_count == 0) return events;
  if (!::itl_history_ensure_read_buffer())
    return koshka::Error{"the file contains invalid data"};

  let const first_number =
      ::itl_g_history_total_count - ::itl_g_history_count + 1;
  usize first_index = 0;
  if (after_event_number.has_value() && *after_event_number >= first_number) {
    first_index = *after_event_number - first_number + 1;
  }

  char decoded[ITL_STRING_MAX_LEN + 1];

  for (usize index = first_index; index < ::itl_g_history_count; index++) {
    usize decoded_size = 0;
    if (!::itl_history_decode_entry_buffered(
            ::itl_history_index_to_offset(index), decoded, sizeof(decoded),
            &decoded_size))
    {
      return koshka::Error{"the file contains invalid data"};
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
  let const path = get_history_file_path();
  if (!path.has_value() || ensure_history_loaded(*path, true).is_error())
    return koshka::None;
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

fn get_relative_history_event(koshka::Allocator allocator, usize distance,
                              koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  if (distance == 0) return koshka::None;
  usize remaining_event_count = distance;
  return find_history_event(
      allocator, before_event_number,
      [&](usize, StringView) { return --remaining_event_count == 0; });
}

fn get_numbered_history_event(koshka::Allocator allocator, usize wanted_number,
                              koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return find_history_event(
      allocator, before_event_number,
      [&](usize number, StringView) { return number == wanted_number; });
}

fn get_prefixed_history_event(koshka::Allocator allocator, StringView prefix,
                              koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return find_history_event(
      allocator, before_event_number,
      [&](usize, StringView command) { return command.starts_with(prefix); });
}

fn get_containing_history_event(koshka::Allocator allocator, StringView text,
                                koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return find_history_event(allocator, before_event_number,
                            [&](usize, StringView command) {
                              return command.find_substring(text).has_value();
                            });
}

fn append_history_event(StringView command) -> koshka::Maybe<usize>
{
  if (command.is_empty() || command.length > ITL_HISTORY_ENTRY_MAX_BYTES ||
      !is_history_contents_valid(command))
  {
    return koshka::None;
  }

  let const path = get_active_history_file_path();
  if (!path.has_value()) return koshka::None;
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return koshka::None;
  defer { os::release_process_lock(lock.take()); };
  if (ensure_history_loaded(*path, true).is_error()) return koshka::None;
  if (::itl_g_history_limit == 0) return ::itl_g_history_total_count;

  itl_string_t *entry = ::itl_string_alloc();
  defer { ITL_STRING_FREE(entry); };
  if (!::itl_string_from_bytes(entry, command.data, command.length))
    return koshka::None;

  let const was_empty = ::itl_g_history_total_count == 0;
  let const previous_status = HISTORY_FILE_STATUS;
  let const had_previous_status = HAS_HISTORY_FILE_STATUS;
  if (!::itl_history_append_to_file(entry, false, false)) return koshka::None;
  let appended_status = os::file_status{};
  unused(os::stat_path_following(path->text().view(), appended_status));
  update_history_rewrite_safety_after_append(
      was_empty, previous_status, had_previous_status, appended_status);
  record_history_file_status(*path);

  return ::itl_g_last_history_event_number;
}

fn rewrite_history_event(usize number, StringView expected,
                         const koshka::ArrayList<koshka::String> &replacements)
    -> bool;

fn rewrite_history_event(usize number, StringView expected,
                         StringView replacement) -> bool
{
  let replacements =
      koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  if (!replacement.is_empty())
    replacements.push(koshka::String{koshka::heap_allocator(), replacement});

  return rewrite_history_event(number, expected, replacements);
}

fn rewrite_history_event(usize number, StringView expected,
                         const koshka::ArrayList<koshka::String> &replacements)
    -> bool
{
  let const path = get_history_file_path();
  if (!path.has_value()) return false;
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return false;
  defer { os::release_process_lock(lock.take()); };
  if (ensure_history_loaded(*path, false).is_error()) return false;
  if (::itl_g_history_count == 0) return false;

  let const first_number =
      ::itl_g_history_total_count - ::itl_g_history_count + 1;
  if (number < first_number || number >= first_number + ::itl_g_history_count) {
    return false;
  }

  let const index = number - first_number;
  let const slot = (::itl_g_history_head + index) % TL_HISTORY_MAX_SIZE;
  let const private_start_offset = ::itl_g_history_offsets[slot];
  let const durable_start_offset = ::itl_g_history_durable_offsets[slot];
  let const contents = path->read_entire_file();
  if (!contents.has_value()) return false;
  let current_status = os::file_status{};
  if (!os::stat_path_following(path->text().view(), current_status))
    return false;
  if (!CAN_REWRITE_HISTORY || !HAS_HISTORY_FILE_STATUS ||
      !HISTORY_FILE_STATUS.has_file_identity ||
      !current_status.has_file_identity ||
      HISTORY_FILE_STATUS.device_id != current_status.device_id ||
      HISTORY_FILE_STATUS.file_id != current_status.file_id)
  {
    return false;
  }

  usize durable_end_offset = durable_start_offset;
  bool is_escape_pending = false;
  bool did_find_end = false;
  while (durable_end_offset < contents->count()) {
    let const byte = (*contents)[durable_end_offset++];
    if (is_escape_pending) {
      is_escape_pending = false;
    } else if (byte == '\\') {
      is_escape_pending = true;
    } else if (byte == '\n') {
      did_find_end = true;
      break;
    }
  }
  if (!did_find_end) return false;
  let const durable_record = contents->substring_of_length(
      durable_start_offset, durable_end_offset - durable_start_offset);

  if (!::itl_history_ensure_read_buffer() ||
      private_start_offset < ::itl_g_history_read_buffer_offset)
  {
    return false;
  }
  let const private_contents = StringView{::itl_g_history_read_buffer->data,
                                          ::itl_g_history_read_buffer->size};
  let const private_start_offset_in_buffer =
      private_start_offset - ::itl_g_history_read_buffer_offset;
  usize private_end_offset_in_buffer = private_start_offset_in_buffer;
  is_escape_pending = false;
  did_find_end = false;
  while (private_end_offset_in_buffer < private_contents.length) {
    let const byte = private_contents[private_end_offset_in_buffer++];
    if (is_escape_pending) {
      is_escape_pending = false;
      continue;
    }
    if (byte == '\\') {
      is_escape_pending = true;
      continue;
    }
    if (byte == '\n') {
      did_find_end = true;
      break;
    }
  }
  if (!did_find_end) return false;

  char decoded[ITL_STRING_MAX_LEN + 1];
  usize decoded_size = 0;
  if (!::itl_history_decode_entry_buffered(private_start_offset, decoded,
                                           sizeof(decoded), &decoded_size) ||
      koshka::StringView{decoded, decoded_size} != expected)
  {
    return false;
  }

  let encoded_replacements = koshka::String{koshka::heap_allocator()};
  let replacement_byte_offsets =
      koshka::ArrayList<usize>{koshka::heap_allocator()};
  for (let const &replacement : replacements) {
    if (replacement.count() > ITL_HISTORY_ENTRY_MAX_BYTES) return false;
    replacement_byte_offsets.push(encoded_replacements.count());
    encode_history_record(encoded_replacements, replacement.view());
  }

  let expected_encoded = koshka::String{koshka::heap_allocator()};
  encode_history_record(expected_encoded, expected);
  if (durable_record != expected_encoded.view()) {
    return false;
  }

  let durable_rewritten = koshka::String{koshka::heap_allocator()};
  durable_rewritten.reserve(contents->count() -
                            (durable_end_offset - durable_start_offset) +
                            encoded_replacements.count());
  durable_rewritten.append(
      contents->substring_of_length(0, durable_start_offset));
  durable_rewritten.append(encoded_replacements.view());
  durable_rewritten.append(contents->substring(durable_end_offset));

  let private_rewritten = koshka::String{koshka::heap_allocator()};
  private_rewritten.reserve(
      private_contents.length -
      (private_end_offset_in_buffer - private_start_offset_in_buffer) +
      encoded_replacements.count());
  private_rewritten.append(
      private_contents.substring_of_length(0, private_start_offset_in_buffer));
  private_rewritten.append(encoded_replacements.view());
  private_rewritten.append(
      private_contents.substring(private_end_offset_in_buffer));

  let private_offsets = koshka::ArrayList<usize>{koshka::heap_allocator()};
  let durable_offsets = koshka::ArrayList<usize>{koshka::heap_allocator()};
  let const private_removed_byte_count =
      private_end_offset_in_buffer - private_start_offset_in_buffer;
  let const durable_removed_byte_count =
      durable_end_offset - durable_start_offset;
  let const resulting_count = ::itl_g_history_count - 1 + replacements.count();
  private_offsets.reserve(resulting_count);
  durable_offsets.reserve(resulting_count);
  for (usize old_index = 0; old_index < index; old_index++) {
    let const old_slot =
        (::itl_g_history_head + old_index) % TL_HISTORY_MAX_SIZE;
    private_offsets.push(::itl_g_history_offsets[old_slot] -
                         ::itl_g_history_read_buffer_offset);
    durable_offsets.push(::itl_g_history_durable_offsets[old_slot]);
  }
  for (let const replacement_byte_offset : replacement_byte_offsets) {
    private_offsets.push(private_start_offset_in_buffer +
                         replacement_byte_offset);
    durable_offsets.push(durable_start_offset + replacement_byte_offset);
  }
  for (usize old_index = index + 1; old_index < ::itl_g_history_count;
       old_index++)
  {
    let const old_slot =
        (::itl_g_history_head + old_index) % TL_HISTORY_MAX_SIZE;
    private_offsets.push(
        ::itl_g_history_offsets[old_slot] - ::itl_g_history_read_buffer_offset -
        private_removed_byte_count + encoded_replacements.count());
    durable_offsets.push(::itl_g_history_durable_offsets[old_slot] -
                         durable_removed_byte_count +
                         encoded_replacements.count());
  }

  let replacement_path = koshka::os::write_to_named_temp_file(
      path->parent(), ".kosh_history_fc", durable_rewritten.view());
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

  let const new_total_count =
      ::itl_g_history_total_count - 1 + replacements.count();
  let const retained_count = private_offsets.count() < ::itl_g_history_limit
                                 ? private_offsets.count()
                                 : ::itl_g_history_limit;
  let const first_retained_index = private_offsets.count() - retained_count;
  for (usize new_index = 0; new_index < retained_count; new_index++) {
    ::itl_g_history_offsets[new_index] =
        private_offsets[first_retained_index + new_index];
    ::itl_g_history_durable_offsets[new_index] =
        durable_offsets[first_retained_index + new_index];
  }
  ::itl_g_history_head = 0;
  ::itl_g_history_count = retained_count;
  ::itl_g_history_total_count = new_total_count;
  ::itl_g_last_history_event_number = 0;
  ::itl_g_history_file_size = durable_rewritten.count();
  ::itl_g_history_ends_with_newline =
      private_rewritten.is_empty() || private_rewritten.back() == '\n';
  ::itl_g_history_file_is_bad = false;
  ::itl_history_read_fd_invalidate();
  ::itl_g_history_read_buffer = ::itl_char_buf_alloc();
  ::itl_char_buf_append_bytes(::itl_g_history_read_buffer,
                              private_rewritten.data(),
                              private_rewritten.count());
  ::itl_g_history_read_buffer_loaded = true;
  ::itl_g_history_read_buffer_offset = 0;
  ::itl_g_history_read_buffer_start = 0;
  record_history_file_status(*path);
  CAN_REWRITE_HISTORY =
      HAS_HISTORY_FILE_STATUS && HISTORY_FILE_STATUS.has_file_identity;
  return CAN_REWRITE_HISTORY;
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
  ::tl_set_history_select_callback(kosh_history_select_callback);

  /* The selector configuration is seeded after the startup files have run. A
     value they set wins and an unset one becomes visible and editable. */
  if (!context.get_variable_value(SELECTOR_COMMAND_VARIABLE).has_value())
    context.set_shell_variable(SELECTOR_COMMAND_VARIABLE,
                               DEFAULT_SELECTOR_COMMAND);

  if (!context.get_variable_value(SELECTOR_OPTIONS_VARIABLE).has_value())
    context.set_shell_variable(SELECTOR_OPTIONS_VARIABLE,
                               DEFAULT_SELECTOR_OPTIONS);
}

fn disable_completion() -> void
{
  COMPLETION_CONTEXT = nullptr;
  ::tl_set_complete_callback(nullptr);
  ::tl_set_highlight_callback(nullptr);
  ::tl_set_ghost_validate_callback(nullptr);
  ::tl_set_history_select_callback(nullptr);
}

fn is_completion_enabled() -> bool { return COMPLETION_CONTEXT != nullptr; }

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

fn set_space_after_completion(bool enabled) -> void
{
  ::tl_set_space_after_completion(enabled ? 1 : 0);
}

fn set_highlight_enabled(bool enabled) -> void
{
  ::tl_set_highlight_callback(enabled ? kosh_highlight_callback : nullptr);
}

fn set_colors_enabled(bool enabled) -> void
{
  ::tl_set_colors_enabled(enabled ? 1 : 0);
}

fn set_edit_mode(edit_mode mode) -> void
{
  ::tl_set_edit_mode(mode == edit_mode::Vi ? TL_EDIT_MODE_VI_INSERT
                                           : TL_EDIT_MODE_EMACS);
}

fn set_tab_selector(koshka::tab_selector_mode selector) -> void
{
  TAB_SELECTOR = selector;
  ::tl_set_completion_menu_enabled(selector ==
                                   koshka::tab_selector_mode::Interactive);
}

fn is_active() -> bool { return ::itl_g_is_active; }

fn initialize() -> void
{
  if (koshka::Maybe<koshka::Path> kosh_history = get_history_file_path();
      kosh_history.has_value())
  {
    let const result = load_history(*kosh_history, true);
    if (result.is_error()) {
      koshka::show_message(
          koshka::StringView{"Unable to read the history at '"} +
          kosh_history->text().view() + "': " + result.error().message());
    }
  }

  if (::tl_init() != TL_SUCCESS) {
    throw koshka::ErrorWithDetails{
        "Toiletline: could not initialize the terminal: " +
            koshka::os::last_system_error_message(),
        "The input is not a terminal, pass `-` to read stdin or `-c`/`-s`"};
  }

  ::tl_set_history_search_snapshot_callback(provide_history_search_snapshot);
}

static fn compact_history_file(usize entry_limit) -> bool
{
  let const path = get_history_file_path();
  if (!path.has_value()) return true;
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return false;
  defer { os::release_process_lock(lock.take()); };

  set_history_limit(entry_limit);
  if (sync_history(*path, true).is_error()) return false;
  let const retained_entry_limit = ::itl_g_history_limit;
  if (entry_limit == 0 || ::itl_g_history_total_count <= entry_limit) {
    return true;
  }
  if (entry_limit > retained_entry_limit) return true;

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
    encode_history_record(contents, StringView{decoded, decoded_size});
  }

  return replace_history_file(*path, parent, ".kosh_history_compact",
                              contents.view()) == history_replacement::Replaced;
}

fn exit(usize history_size_limit) -> void
{
  if (!compact_history_file(history_size_limit)) {
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

  ::tl_set_history_search_snapshot_callback(nullptr);

  ::tl_set_wake_callback(nullptr);
  JOB_CONTEXT = nullptr;
  WAKE_NOTIFICATION_STASH.clear();
}

fn get_input(const String &prompt) -> input_result
{
  let completion_base_directory = koshka::Maybe<Path>{};
  let completion_storage =
      koshka::Maybe<koshka::completion::completion_result>{};
#if !defined NDEBUG
  let const cwd_capture_count_before = DEBUG_COMPLETION_CWD_CAPTURE_COUNT;
#endif
  if (is_completion_enabled()) {
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
  let const listing_count_before = DEBUG_COMPLETION_LISTING_COUNT;
  let const directory_stat_count_before = utils::debug_directory_stat_count();
  let const directory_read_count_before = utils::debug_directory_read_count();
  let const directory_sort_count_before = utils::debug_directory_sort_count();
  let const executable_probe_count_before =
      utils::debug_executable_probe_count();
  let const program_path_candidate_count_before =
      utils::debug_program_path_candidate_count();
#endif
  let const history_path = get_active_history_file_path();
  if (history_path.has_value())
    unused(ensure_history_loaded(*history_path, true));
  ::itl_g_last_history_event_number = 0;
  let const previous_history_total_count = ::itl_g_history_total_count;
  let const was_history_empty = previous_history_total_count == 0;
  let const previous_history_status = HISTORY_FILE_STATUS;
  let const had_previous_history_status = HAS_HISTORY_FILE_STATUS;
  i32 code = ::tl_get_input(TL_BUFFER, sizeof(TL_BUFFER), prompt.c_str());
  if (history_path.has_value() &&
      ::itl_g_history_total_count != previous_history_total_count)
  {
    let appended_status = os::file_status{};
    unused(
        os::stat_path_following(history_path->text().view(), appended_status));
    update_history_rewrite_safety_after_append(
        was_history_empty, previous_history_status, had_previous_history_status,
        appended_status);
    record_history_file_status(*history_path);
  }
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
        " listings=" +
        koshka::String::from(DEBUG_COMPLETION_LISTING_COUNT -
                                 listing_count_before,
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

fn did_debug_allocation_fail() -> bool
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

fn get_default_prompt_template() -> String
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
    ps1_template = get_default_prompt_template();

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
    let const source_name = koshka::intern_source_name("$PS1");
    let const source_location =
        koshka::SourceLocation{0, guarded.count(), source_name};
    expanded = unguard_prompt_backslashes(
        context.expand_heredoc_body(guarded.view(), &source_location).view());
  } catch (const koshka::ErrorBase &error) {
    /* A prompt draw error leaves the template standing rather than taking down
       the shell. */
    koshka::show_message(error.to_string(guarded.view(), &context));
    if (let const definition =
            context.special_variable_definition_location("PS1");
        definition.has_value())
      context.print_source_backtrace(definition);
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
    let const source_name = koshka::intern_source_name("$PS0");
    let const source_location =
        koshka::SourceLocation{0, guarded.count(), source_name};
    expanded = unguard_prompt_backslashes(
        context.expand_heredoc_body(guarded.view(), &source_location).view());
  } catch (const koshka::ErrorBase &error) {
    koshka::show_message(error.to_string(guarded.view(), &context));
    if (let const definition =
            context.special_variable_definition_location("PS0");
        definition.has_value())
      context.print_source_backtrace(definition);
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
