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
  let const base = static_cast<char *>(
      koshka::heap_allocator().raw_alloc(allocation_length, alignof(char)));
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
  if (fresh == nullptr) return nullptr;
  std::memcpy(fresh, pointer, old_capacity);
  tl_arena_free(pointer);

  return fresh;
}

#define TL_MALLOC  tl_arena_malloc
#define TL_REALLOC tl_arena_realloc
#define TL_FREE    tl_arena_free
#define TL_ABORT() std::abort()

#define TL_NO_SUSPEND
#define TL_CTRL_Z_UNDO
#define TL_ASSERT           ASSERT
#define TL_HISTORY_MAX_SIZE (1024 * 4)

} /* namespace */

#define TOILETLINE_IMPLEMENTATION
#if defined __clang__ || defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "toiletline/toiletline.h"
#if defined __clang__ || defined __GNUC__
#pragma GCC diagnostic pop
#endif

static_assert(toiletline::HISTORY_RECORD_MAX_DECODED_BYTE_COUNT ==
              ITL_STRING_MAX_LEN);

namespace {

enum class selector_outcome : u8;

struct completion_session
{
  koshka::EvalContext *context{nullptr};
  const koshka::Path *base_directory{nullptr};
  koshka::completion::completion_result *result{nullptr};
  koshka::ArrayList<const char *> candidate_pointers{koshka::heap_allocator()};
  koshka::ArrayList<const char *> description_pointers{
      koshka::heap_allocator()};
  bool is_highlight_color_enabled{false};
  bool is_highlight_styled_underlines_enabled{false};
  bool should_show_hints{true};
  bool should_show_diagnostics{true};
  bool has_pending_gather{false};
  koshka::tab_selector_mode tab_selector{
      koshka::tab_selector_mode::Interactive};

  fn attach_prompt(const koshka::Path *directory,
                   koshka::completion::completion_result *storage) -> void
  {
    base_directory = directory;
    result = storage;
    has_highlighted_spans = false;
  }

  fn detach_prompt() -> void
  {
    base_directory = nullptr;
    result = nullptr;
    has_analyzed_line = false;
    analyzed_line.clear();
    analysis_finding.clear();
  }

  pure fn is_enabled() const wontthrow -> bool { return context != nullptr; }
  fn run_selector(koshka::completion::completion_result &out_result,
                  usize token_codepoint_count) throws -> selector_outcome;
  fn select_history(const char *const *entries, size_t count,
                    const char **out_selected) -> int;
  fn complete(const char *buffer, size_t cursor, tl_completion *out,
              int for_listing) -> int;
  fn highlight(const char *buffer, tl_highlight *out) -> int;
  fn validate_ghost(const char *entry) const -> int;
  fn hint(const char *buffer, size_t cursor) -> const char *;
  fn idle(const char *buffer, size_t cursor) -> int;
  koshka::String hint_row{koshka::heap_allocator()};
  koshka::String syntax_checked_line{koshka::heap_allocator()};
  koshka::String syntax_problem{koshka::heap_allocator()};
  koshka::mimic_mood syntax_checked_mood{koshka::mimic_mood::Default};
  bool has_syntax_checked_line{false};
  bool was_syntax_checked_at_line_end{false};
  koshka::String analyzed_line{koshka::heap_allocator()};
  koshka::String analysis_finding{koshka::heap_allocator()};
  bool has_analyzed_line{false};
  koshka::String highlighted_line{koshka::heap_allocator()};
  koshka::ArrayList<koshka::highlight_span> highlighted_spans{
      koshka::heap_allocator()};
  bool has_highlighted_spans{false};
  char bracket_styles[4][48]{};
  usize bracket_style_count{0};
};

completion_session COMPLETION_SESSION{};

constexpr koshka::StringView SELECTOR_COMMAND_VARIABLE{
    "KOSH_FZF_COMPLETION_COMMAND"};
constexpr koshka::StringView SELECTOR_OPTIONS_VARIABLE{
    "KOSH_FZF_COMPLETION_OPTS"};

constexpr koshka::StringView DEFAULT_SELECTOR_COMMAND{"fzf"};
constexpr koshka::StringView DEFAULT_SELECTOR_OPTIONS_WITHOUT_RECORD_FRAMING{
    "--multi --layout=reverse --height=~40% --min-height=3"};

fn resolve_selector_program_or_report_why_it_cannot_run(
    koshka::EvalContext &context) throws -> koshka::ErrorOr<koshka::Path>
{
  let const configured = context.get_variable_value(SELECTOR_COMMAND_VARIABLE);
  if (configured.has_value() && configured->is_empty()) {
    return koshka::Error{SELECTOR_COMMAND_VARIABLE +
                         " is empty and gives no tab selector"};
  }

  let const command_name =
      configured.has_value() ? configured->view() : DEFAULT_SELECTOR_COMMAND;

  let const resolved = context.program_resolver().search(
      command_name, koshka::ProgramResolver::SearchMode::First,
      koshka::ProgramResolver::Requirement::Execution,
      koshka::ProgramResolver::CachePolicy::ReadOnly);
  if (resolved.is_empty()) {
    return koshka::Error{koshka::StringView{"The tab selector '"} +
                         command_name + "' was not found"};
  }

  return koshka::Path{resolved[0].text()};
}

fn selector_failure_note_with_variable_values(
    koshka::EvalContext &context) throws -> koshka::String
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
  do_append_variable(SELECTOR_OPTIONS_VARIABLE,
                     DEFAULT_SELECTOR_OPTIONS_WITHOUT_RECORD_FRAMING);

  note.append(". Set ");
  note.append(SELECTOR_COMMAND_VARIABLE);
  note.append(" to a program that filters the candidates, or choose another "
              "presentation with `koshconf set "
              "completion.menu_style`");

  return note;
}

fn build_nul_separated_selector_input(
    const koshka::completion::completion_result &result) throws
    -> koshka::String
{
  let input = koshka::String{koshka::heap_allocator()};
  let const has_descriptions = result.descriptions.count() > 0;
  for (let const &candidate : result.candidates) {
    input.append(candidate.view());
    if (has_descriptions) {
      if (let const description = result.descriptions.find(candidate.view());
          description.has_value() && !description->is_empty())
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

  args.push(koshka::String{"--read0"});
  args.push(koshka::String{"--print0"});

  let const options = context.get_variable_value(SELECTOR_OPTIONS_VARIABLE);
  let const option_text = options.has_value()
                              ? options->view()
                              : DEFAULT_SELECTOR_OPTIONS_WITHOUT_RECORD_FRAMING;

  for (let const &option :
       context.expand_wordlist_to_fields(option_text, false))
    args.push(koshka::String{option.view()});

  return args;
}

fn parse_selector_reply_stripping_descriptions(koshka::StringView reply,
                                               bool has_descriptions) throws
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
  NotRun,
  Selected,
  Dismissed,
};

constexpr i64 STOPPED_PROGRAM_POLL_NANOS = 100'000'000;

fn get_continue_signal() throws -> koshka::Maybe<i32>
{
  let const continue_signal = koshka::os::signal_number_from_name("CONT");
  if (!continue_signal.has_value() ||
      !koshka::os::is_process_signal_supported(*continue_signal))
  {
    return koshka::None;
  }

  return continue_signal;
}

constexpr usize STOPPED_PROGRAM_CONTINUE_LIMIT = 32;

fn signal_program_group(koshka::os::process child,
                        koshka::StringView signal_name) throws -> bool
{
  let const signal_number = koshka::os::signal_number_from_name(signal_name);
  if (!signal_number.has_value() ||
      !koshka::os::is_process_signal_supported(*signal_number))
  {
    return false;
  }

  let const group = koshka::os::process_group_of(child);
  defer { koshka::os::close_process_reference(group); };

  return koshka::os::signal_process(group, *signal_number);
}

fn is_suspension_status(i32 stop_status) throws -> bool
{
  for (let const name :
       {koshka::StringView{"TSTP"}, koshka::StringView{"STOP"}})
  {
    let const signal_number = koshka::os::signal_number_from_name(name);
    if (signal_number.has_value() && stop_status == 128 + *signal_number) {
      return true;
    }
  }

  return false;
}

fn continue_stopped_program(koshka::os::process child, i32 stop_status,
                            usize &continue_count) throws -> bool
{
  let const should_continue = !koshka::os::INTERRUPT_REQUESTED &&
                              continue_count < STOPPED_PROGRAM_CONTINUE_LIMIT &&
                              is_suspension_status(stop_status);
  if (should_continue && signal_program_group(child, "CONT")) {
    continue_count++;
    return true;
  }

  LOG(Info,
      "ending a picker or editor stopped with status %d after %zu "
      "continues",
      stop_status, continue_count);
  unused(signal_program_group(child, "KILL"));
  return false;
}

fn wait_for_continued_program(koshka::os::process child) throws -> i32
{
  usize continue_count = 0;
  loop
  {
    bool was_stopped = false;
    let const status =
        koshka::os::wait_and_monitor_process(child, &was_stopped);
    if (!was_stopped) return status;
    if (!continue_stopped_program(child, status, continue_count)) {
      unused(koshka::os::reap_process_quietly(child));
      return status;
    }
  }
}

fn read_while_continuing_program(koshka::os::descriptor fd,
                                 koshka::os::process child,
                                 koshka::Maybe<i32> &out_exit_status) throws
    -> koshka::Maybe<koshka::String>
{
  if (!get_continue_signal().has_value()) {
    return koshka::os::read_fd_to_string(fd, koshka::heap_allocator());
  }

  let captured = koshka::String{koshka::heap_allocator()};
  char buffer[4096];
  usize continue_count = 0;
  loop
  {
    let const readiness =
        koshka::os::wait_for_fd_readable(fd, STOPPED_PROGRAM_POLL_NANOS);
    if (readiness < 0) return koshka::None;

    if (readiness == 0) {
      if (out_exit_status.has_value()) continue;

      i32 status = 0;
      let const state = koshka::os::poll_process(child, status);
      if (state == koshka::os::process_state::Stopped) {
        if (!continue_stopped_program(child, status, continue_count)) {
          unused(koshka::os::reap_process_quietly(child));
          out_exit_status = status;
        }
      } else if (state == koshka::os::process_state::Exited) {
        out_exit_status = status;
      }
      continue;
    }

    let const read_count = koshka::os::read_fd(fd, buffer, sizeof(buffer));
    if (!read_count.has_value()) return koshka::None;
    if (*read_count == 0) return koshka::Maybe<koshka::String>{steal(captured)};

    captured.append(koshka::StringView{buffer, *read_count});
  }
}

fn report_selector_failure_above_repainted_prompt(
    koshka::StringView message, koshka::StringView note,
    bool should_hand_back_screen) throws -> void
{
  if (should_hand_back_screen) {
    koshka::flush();
    if (::tl_begin_external_screen() != TL_SUCCESS) return;
    if (::tl_end_external_screen() != TL_SUCCESS) return;
  }

  koshka::show_message(koshka::ErrorWithDetails{message, note}.to_string());
}

fn run_selector_program(koshka::EvalContext &context, koshka::StringView input,
                        bool has_descriptions,
                        koshka::ArrayList<koshka::String> &out_selected) throws
    -> selector_outcome
{
  if (!::itl_g_is_active) return selector_outcome::NotRun;
  if (!koshka::os::shell_has_controlling_terminal())
    return selector_outcome::NotRun;

  let const program =
      resolve_selector_program_or_report_why_it_cannot_run(context);
  if (program.is_error()) {
    let const note = selector_failure_note_with_variable_values(context);
    report_selector_failure_above_repainted_prompt(program.error().message(),
                                                   note.view(), true);
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

  bool is_child_descriptor_pair_owned = true;
  bool is_read_end_open = true;
  defer
  {
    if (is_child_descriptor_pair_owned) {
      koshka::os::close_fd(*input_fd);
      koshka::os::close_fd(output_pipe->out);
    }
    if (is_read_end_open) koshka::os::close_fd(output_pipe->in);
  };

  let arg_locations =
      koshka::ArrayList<koshka::SourceLocation>{koshka::heap_allocator()};
  let selector = koshka::ExecContext::make_from_resolved(
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

  is_child_descriptor_pair_owned = false;
  let const child = koshka::os::execute_program(
      selector, koshka::os::program_execution_options{
                    .fallback = koshka::os::script_fallback_policy::Reject,
                    .handoff = koshka::os::terminal_handoff::BeforeStart});

  bool is_terminal_lent = true;
  defer
  {
    if (is_terminal_lent) koshka::os::reclaim_controlling_terminal();
  };

  koshka::Maybe<i32> exit_status = koshka::None;
  let const captured =
      read_while_continuing_program(output_pipe->in, child, exit_status);
  koshka::os::close_fd(output_pipe->in);
  is_read_end_open = false;

  let const status = exit_status.has_value()
                         ? *exit_status
                         : wait_for_continued_program(child);
  koshka::os::reclaim_controlling_terminal();
  is_terminal_lent = false;
  koshka::os::INTERRUPT_REQUESTED = 0;

  is_editor_suspended = false;
  if (::tl_end_external_screen() != TL_SUCCESS) return selector_outcome::NotRun;

  if (status != 0) {
    if (status == 1 || status == 130) return selector_outcome::Dismissed;

    let const note = selector_failure_note_with_variable_values(context);
    report_selector_failure_above_repainted_prompt(
        koshka::StringView{"The tab selector '"} + selector_program.view() +
            "' exited with status " +
            koshka::String::from(status, koshka::heap_allocator()),
        note.view(), false);

    return selector_outcome::NotRun;
  }

  if (!captured.has_value()) return selector_outcome::NotRun;

  out_selected = parse_selector_reply_stripping_descriptions(captured->view(),
                                                             has_descriptions);
  if (out_selected.is_empty()) return selector_outcome::Dismissed;

  return selector_outcome::Selected;
}

fn completion_session::run_selector(
    koshka::completion::completion_result &result,
    usize token_codepoint_count) throws -> selector_outcome
{
  if (tab_selector != koshka::tab_selector_mode::External)
    return selector_outcome::NotRun;

  if (::tl_utf8_strlen(result.longest_common_prefix.c_str()) >
      token_codepoint_count)
  {
    return selector_outcome::NotRun;
  }

  if (result.candidates.count() < 2) return selector_outcome::NotRun;

  let selected = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  let const outcome = run_selector_program(
      *context, build_nul_separated_selector_input(result).view(),
      result.descriptions.count() > 0, selected);
  if (outcome != selector_outcome::Selected) return outcome;

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

koshka::String SELECTED_HISTORY_ENTRY{koshka::heap_allocator()};

fn completion_session::select_history(const char *const *entries, size_t count,
                                      const char **out_selected) -> int
{
  if (context == nullptr) return 0;
  if (tab_selector != koshka::tab_selector_mode::External) return 0;
  if (entries == nullptr || count == 0) return 0;

  try {
    let input = koshka::String{koshka::heap_allocator()};
    for (size_t i = 0; i < count; i++) {
      input.append(koshka::StringView{entries[i]});
      input.push('\0');
    }

    let selected = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
    let const outcome =
        run_selector_program(*context, input.view(), false, selected);
    if (outcome == selector_outcome::NotRun) return 0;
    if (outcome == selector_outcome::Dismissed) return -1;
    if (selected.is_empty()) return 0;

    SELECTED_HISTORY_ENTRY =
        koshka::String{koshka::heap_allocator(), selected[0].view()};
    *out_selected = SELECTED_HISTORY_ENTRY.c_str();
    return 1;
  } catch (...) {
    return 0;
  }
}

fn kosh_history_select_callback(const char *const *entries, size_t count,
                                const char **out_selected) -> int
{
  return COMPLETION_SESSION.select_history(entries, count, out_selected);
}

constexpr koshka::StringView LINE_EDITOR_NOTE{
    "Ctrl-X Ctrl-E runs VISUAL, then EDITOR, then vi"};
constexpr koshka::StringView LINE_EDITOR_VARIABLES[] = {"VISUAL", "EDITOR"};

fn get_line_editor_words(koshka::EvalContext &context) throws
    -> koshka::ArrayList<koshka::String>
{
  for (let const name : LINE_EDITOR_VARIABLES) {
    let const value = context.get_variable_value(name);
    if (!value.has_value() || value->is_empty()) continue;

    let words = context.expand_wordlist_to_fields(value->view(), false);
    if (!words.is_empty()) return words;
  }

  let words = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  words.push(koshka::String{"vi"});

  return words;
}

fn run_line_editor(koshka::EvalContext &context, koshka::StringView line,
                   koshka::String &out_edited) throws -> bool
{
  if (!::itl_g_is_active) return false;
  if (!koshka::os::shell_has_controlling_terminal()) return false;

  let words = get_line_editor_words(context);
  let const resolved = context.program_resolver().search(
      words[0].view(), koshka::ProgramResolver::SearchMode::First,
      koshka::ProgramResolver::Requirement::Execution,
      koshka::ProgramResolver::CachePolicy::ReadOnly);
  if (resolved.is_empty()) {
    report_selector_failure_above_repainted_prompt(
        koshka::StringView{"The editor '"} + words[0].view() +
            "' was not found",
        LINE_EDITOR_NOTE, true);
    return false;
  }

  let const temp_path = koshka::os::write_to_named_temp_file(
      koshka::Path::temp_directory(), "kosh-edit", line);
  if (!temp_path.has_value()) return false;
  defer { unused(koshka::os::remove_file(temp_path->text().view())); };

  let const program = koshka::Path{resolved[0].text()};
  words[0] = koshka::String{program.view()};
  words.push(koshka::String{temp_path->text().view()});

  let arg_locations =
      koshka::ArrayList<koshka::SourceLocation>{koshka::heap_allocator()};
  let editor = koshka::ExecContext::make_from_resolved(
      koshka::SourceLocation{},
      koshka::ResolvedCommand::from_program(koshka::Path{program.text()}),
      steal(words), steal(arg_locations));

  koshka::flush();
  if (::tl_begin_external_screen() != TL_SUCCESS) return false;
  bool is_editor_suspended = true;
  defer
  {
    if (is_editor_suspended) unused(::tl_end_external_screen());
  };

  let const child = koshka::os::execute_program(
      editor, koshka::os::program_execution_options{
                  .fallback = koshka::os::script_fallback_policy::Reject,
                  .handoff = koshka::os::terminal_handoff::BeforeStart});
  bool is_terminal_lent = true;
  defer
  {
    if (is_terminal_lent) koshka::os::reclaim_controlling_terminal();
  };
  let const status = wait_for_continued_program(child);
  koshka::os::reclaim_controlling_terminal();
  is_terminal_lent = false;
  koshka::os::INTERRUPT_REQUESTED = 0;

  is_editor_suspended = false;
  if (::tl_end_external_screen() != TL_SUCCESS) return false;

  if (status != 0) {
    report_selector_failure_above_repainted_prompt(
        koshka::StringView{"The editor '"} + program.view() +
            "' exited with status " +
            koshka::String::from(status, koshka::heap_allocator()),
        LINE_EDITOR_NOTE, false);
    return false;
  }

  let edited = temp_path->read_entire_file();
  if (!edited.has_value()) return false;

  edited->normalize_crlf_line_endings();
  while (!edited->is_empty() && edited->back() == '\n')
    edited->pop_back();

  out_edited = steal(*edited);

  return true;
}

koshka::String EDITED_LINE{koshka::heap_allocator()};

fn kosh_edit_callback(const char *buffer, const char **out_edited) -> int
{
  if (COMPLETION_SESSION.context == nullptr) return 0;

  try {
    if (!run_line_editor(*COMPLETION_SESSION.context,
                         koshka::StringView{buffer, std::strlen(buffer)},
                         EDITED_LINE))
    {
      return 0;
    }

    *out_edited = EDITED_LINE.c_str();
    return 1;
  } catch (...) {
    return 0;
  }
}

fn completion_session::complete(const char *buffer, size_t cursor,
                                tl_completion *out, int for_listing) -> int
{
  if (context == nullptr || result == nullptr) return 0;

  try {
    let const is_explicit_completion = for_listing != 0;
    has_pending_gather = false;
    let const deferral =
        koshka::completion::ScopedDocumentationDeferral{is_explicit_completion};
    if (is_explicit_completion) {
      context->program_resolver().begin_explicit_completion(
          koshka::ProgramResolver::CompletionRefresh::Listings);
    }
    defer
    {
      if (is_explicit_completion)
        context->program_resolver().end_explicit_completion();
    };

    let const are_signal_keys_lent_to_completion_spec =
        is_explicit_completion && ::tl_set_signal_keys(1) == TL_SUCCESS;
    defer
    {
      if (are_signal_keys_lent_to_completion_spec)
        unused(::tl_set_signal_keys(0));
      koshka::os::INTERRUPT_REQUESTED = 0;
    };

    const usize byte_length = std::strlen(buffer);
    let line = koshka::StringView{buffer, byte_length};

    const usize byte_cursor =
        toiletline::get_codepoint_byte_offset(buffer, byte_length, cursor);

    koshka::arm_message_leading_newline(true);
    result->candidates.clear();
    result->descriptions.clear();
    result->longest_common_prefix.clear();
    *result = koshka::completion::complete(
        line, byte_cursor, *context, *base_directory, nullptr, false,
        for_listing != 0 ? koshka::completion::completion_mode::Listing
                         : koshka::completion::completion_mode::Ghost);
    let const &completions = *result;
    koshka::arm_message_leading_newline(false);

    if (koshka::os::INTERRUPT_REQUESTED) return 0;

    let const token_start_codepoint =
        ::tl_utf8_strnlen(buffer, completions.token_start);
    let const token_codepoint_count =
        cursor >= token_start_codepoint ? cursor - token_start_codepoint : 0;
    if (is_explicit_completion &&
        run_selector(*result, token_codepoint_count) ==
            selector_outcome::Dismissed)
    {
      return 0;
    }

    if (completions.candidate_count == 0) return 0;

    candidate_pointers.clear();
    if (for_listing != 0) {
      candidate_pointers.reserve(completions.candidates.count());
      for (let const &candidate : completions.candidates)
        candidate_pointers.push(candidate.c_str());
    }

    description_pointers.clear();
    out->descriptions = nullptr;
    if (for_listing != 0 && completions.descriptions.count() > 0) {
      description_pointers.reserve(completions.candidates.count());
      for (let const &candidate : completions.candidates) {
        if (let const found_description =
                completions.descriptions.find(candidate.view());
            found_description.has_value())
          description_pointers.push(found_description->c_str());
        else
          description_pointers.push("");
      }
      out->descriptions = description_pointers.begin();
    }

    out->candidates = for_listing != 0 ? candidate_pointers.begin() : nullptr;
    out->count = completions.candidate_count;
    out->longest_common_prefix = completions.longest_common_prefix.c_str();
    out->token_start = ::tl_utf8_strnlen(buffer, completions.token_start);
    out->token_end = ::tl_utf8_strnlen(buffer, completions.token_end);
    out->is_tier_ranked = completions.is_tier_ranked ? 1 : 0;
    out->is_space_suppressed = completions.is_space_suppressed ? 1 : 0;
    return 1;
  } catch (koshka::completion::documentation_pending &) {
    koshka::arm_message_leading_newline(false);
    has_pending_gather = true;
    return TL_COMPLETE_PENDING;
  } catch (koshka::ErrorBase &error) {
    koshka::arm_message_leading_newline(false);
    LOG(Debug, "completion swallowed an error: %s", error.message().c_str());
    return 0;
  } catch (...) {
    koshka::arm_message_leading_newline(false);
    LOG(Debug, "completion swallowed an unknown throw");
    return 0;
  }
}

fn kosh_completion_callback(const char *buffer, size_t cursor,
                            tl_completion *out, int for_listing) -> int
{
  return COMPLETION_SESSION.complete(buffer, cursor, out, for_listing);
}

fn completion_session::highlight(const char *buffer, tl_highlight *out) -> int
{
  if (context == nullptr) return 0;
  if (!is_highlight_color_enabled) return 0;

  try {
    const usize byte_length = std::strlen(buffer);
    let line = koshka::StringView{buffer, byte_length};

    if (!has_highlighted_spans || highlighted_line.view() != line) {
      let const result = koshka::completion::highlight_line(line, *context);
      highlighted_line.clear();
      highlighted_line.append(line);
      highlighted_spans.clear();
      highlighted_spans.reserve(result.count());
      for (let const &span : result)
        highlighted_spans.push(span);
      has_highlighted_spans = true;
    }

    let const &theme = is_highlight_styled_underlines_enabled
                           ? koshka::colors::SHELL_HIGHLIGHT_THEME
                           : koshka::colors::NONINTERACTIVE_HIGHLIGHT_THEME;
    let const emphasis =
        theme.style_for(koshka::highlight_role::matching_bracket);
    let const bracket = koshka::completion::find_matching_bracket(
        line, highlighted_spans, out->cursor);
    usize range_starts[2] = {0, 0};
    usize range_ends[2] = {0, 0};
    usize range_count = 0;
    if (bracket.has_value() && !emphasis.is_empty()) {
      range_starts[0] = bracket->open_start;
      range_ends[0] = bracket->open_end;
      range_starts[1] = bracket->close_start;
      range_ends[1] = bracket->close_end;
      range_count = 2;
    }
    bracket_style_count = 0;

    size_t filled = 0;
    usize byte_position = 0;
    usize codepoint_position = 0;
    let const do_advance = [&](usize target) -> void {
      while (byte_position < target) {
        if ((static_cast<unsigned char>(buffer[byte_position]) & 0xC0) != 0x80)
          codepoint_position++;
        byte_position++;
      }
    };
    let const do_push = [&](usize start, usize end, const char *sgr) -> void {
      if (start >= end || sgr == nullptr || filled >= out->capacity) {
        return;
      }

      do_advance(start);
      out->spans[filled].start = codepoint_position;
      do_advance(end);
      out->spans[filled].end = codepoint_position;
      out->spans[filled].sgr = sgr;
      filled++;
    };
    let const do_emphasize = [&](koshka::StringView style) -> const char * {
      let const fallback = style.is_empty() ? nullptr : style.data;
      if (bracket_style_count == countof(bracket_styles)) return fallback;

      let &slot = bracket_styles[bracket_style_count];
      if (style.length + emphasis.length >= sizeof(slot)) return fallback;

      if (!style.is_empty()) std::memcpy(slot, style.data, style.length);
      std::memcpy(slot + style.length, emphasis.data, emphasis.length);
      slot[style.length + emphasis.length] = '\0';
      bracket_style_count++;
      return slot;
    };
    usize range_index = 0;
    let const do_push_styled = [&](usize start, usize end,
                                   koshka::StringView style) -> void {
      let const sgr = style.is_empty() ? nullptr : style.data;
      while (start < end) {
        while (range_index < range_count && range_ends[range_index] <= start) {
          range_index++;
        }

        if (range_index == range_count || range_starts[range_index] >= end) {
          do_push(start, end, sgr);
          return;
        }

        let const piece_start = range_starts[range_index] > start
                                    ? range_starts[range_index]
                                    : start;
        let const piece_end =
            range_ends[range_index] < end ? range_ends[range_index] : end;
        do_push(start, piece_start, sgr);
        do_push(piece_start, piece_end, do_emphasize(style));
        start = piece_end;
      }
    };

    usize covered_end = 0;
    for (let const &span : highlighted_spans) {
      do_push_styled(covered_end, span.start, koshka::StringView{});
      do_push_styled(span.start, span.end, theme.style_for(span.role));
      covered_end = span.end;
    }
    do_push_styled(covered_end, byte_length, koshka::StringView{});

    out->count = filled;
    return filled > 0 ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

fn kosh_highlight_callback(const char *buffer, tl_highlight *out) -> int
{
  return COMPLETION_SESSION.highlight(buffer, out);
}

koshka::EvalContext *JOB_CONTEXT = nullptr;
koshka::String WAKE_NOTIFICATION_STASH{koshka::heap_allocator()};

fn kosh_wake_callback(int phase) -> int
{
  try {
    if (phase == 0) {
      if (koshka::os::CHILD_STATE_CHANGED == 0) return 0;
      if (JOB_CONTEXT == nullptr ||
          !JOB_CONTEXT->runtime_state().option_is_enabled(
              koshka::shell_option_id::Notify))
        return 0;
      WAKE_NOTIFICATION_STASH =
          JOB_CONTEXT->job_table_store().format_done_job_notifications(
              "\r\n", JOB_CONTEXT->runtime_state().is_posix_option_on());
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

fn completion_session::validate_ghost(const char *entry) const -> int
{
  if (context == nullptr) return 1;
  try {
    const usize byte_length = std::strlen(entry);
    return koshka::completion::command_word_resolves(
               koshka::StringView{entry, byte_length}, *context)
               ? 1
               : 0;
  } catch (...) {
    return 1;
  }
}

fn kosh_ghost_validate_callback(const char *entry) -> int
{
  return COMPLETION_SESSION.validate_ghost(entry);
}

fn completion_session::hint(const char *buffer, size_t cursor) -> const char *
{
  if (context == nullptr) return nullptr;

  try {
    let const byte_length = std::strlen(buffer);
    let const line = koshka::StringView{buffer, byte_length};
    if (should_show_diagnostics) {
      let const mood = context->runtime_state().get_mood();
      let const is_cursor_at_line_end = cursor == byte_length;
      if (!has_syntax_checked_line || syntax_checked_line.view() != line ||
          syntax_checked_mood != mood ||
          was_syntax_checked_at_line_end != is_cursor_at_line_end)
      {
        has_syntax_checked_line = false;
        unused(koshka::completion::describe_syntax_problem(line, cursor, mood,
                                                           syntax_problem));
        syntax_checked_line.clear();
        syntax_checked_line.append(line);
        syntax_checked_mood = mood;
        was_syntax_checked_at_line_end = is_cursor_at_line_end;
        has_syntax_checked_line = true;
      }

      if (!syntax_problem.is_empty()) return syntax_problem.c_str();

      if (has_analyzed_line && !analysis_finding.is_empty() &&
          analyzed_line.view() == line)
      {
        return analysis_finding.c_str();
      }
    }

    if (!should_show_hints || !koshka::completion::compose_command_hint(
                                  line, cursor, *context, hint_row))
    {
      return nullptr;
    }

    return hint_row.c_str();
  } catch (...) {
    return nullptr;
  }
}

fn kosh_hint_callback(const char *buffer, size_t cursor, const char **sgr)
    -> const char *
{
  unused(sgr);
  return COMPLETION_SESSION.hint(buffer, cursor);
}

fn kosh_pair_role_callback(const char *buffer, size_t cursor, int byte) -> int
{
  try {
    let const line = koshka::StringView{buffer, std::strlen(buffer)};
    switch (koshka::completion::classify_typed_pair_byte(
        line, cursor, static_cast<char>(byte)))
    {
    case koshka::completion::typed_pair_role::opens: return TL_PAIR_OPENS;
    case koshka::completion::typed_pair_role::closes: return TL_PAIR_CLOSES;
    case koshka::completion::typed_pair_role::none: return TL_PAIR_NONE;
    }
    return TL_PAIR_NONE;
  } catch (...) {
    return TL_PAIR_NONE;
  }
}

constexpr int IDLE_DELAY_MS = 250;
constexpr int IDLE_REPEAT_MS = 20;

fn completion_session::idle(const char *buffer, size_t cursor) -> int
{
  if (context == nullptr) return 0;

  try {
    let const line = koshka::StringView{buffer, std::strlen(buffer)};
    let outcome = 0;
    koshka::completion::warm_cdpath_indexes(*context);
    let const should_analyze =
        should_show_diagnostics &&
        (!has_analyzed_line || analyzed_line.view() != line);
    if (should_analyze) {
      let const had_finding = !analysis_finding.is_empty();
      analyzed_line = koshka::String{line};
      has_analyzed_line = true;
      unused(koshka::completion::describe_analysis_finding(line, *context,
                                                           analysis_finding));
      if (had_finding || !analysis_finding.is_empty())
        outcome |= TL_IDLE_REFRESH;
    }
    if (!should_show_hints && !has_pending_gather) return outcome;

    let const progress = koshka::completion::step_idle_documentation(
        line, cursor, *context, !has_pending_gather);
    if (progress.did_finish_load) outcome |= TL_IDLE_REFRESH;
    if (progress.is_loading) outcome |= TL_IDLE_AGAIN;
    if (!progress.is_loading) has_pending_gather = false;
    return outcome;
  } catch (...) {
    return 0;
  }
}

fn kosh_idle_callback(const char *buffer, size_t cursor) -> int
{
  return COMPLETION_SESSION.idle(buffer, cursor);
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
  static let const home = koshka::os::get_home_directory();
  if (!home.has_value()) return koshka::None;
  let path = home->clone();
  path.append(default_file);
  return path;
}

static constexpr char KOSH_CALC_HISTORY_FILE[] = ".kosh_calc_history";

struct history_file_tracking
{
  os::file_status status{};
  bool has_status{false};
  bool can_rewrite{false};

  fn invalidate() -> void
  {
    has_status = false;
    can_rewrite = false;
  }

  fn refresh_can_rewrite() -> void
  {
    can_rewrite = has_status && status.has_file_identity;
  }

  fn matches_identity(const os::file_status &current) const -> bool
  {
    return can_rewrite && has_status && status.has_file_identity &&
           current.has_file_identity && status.device_id == current.device_id &&
           status.file_id == current.file_id;
  }

  fn record(const Path &path, usize expected_size) -> void
  {
    has_status = os::stat_path_following(path.view(), status);
    if (has_status && status.size != expected_size) has_status = false;
  }

  fn update_after_append(const history_file_tracking &previous,
                         const os::file_status &appended, bool was_empty)
      -> void
  {
    if (!appended.has_file_identity) {
      can_rewrite = false;
    } else if (previous.has_status && previous.status.has_file_identity) {
      can_rewrite = can_rewrite &&
                    previous.status.device_id == appended.device_id &&
                    previous.status.file_id == appended.file_id;
    } else {
      can_rewrite = was_empty;
    }
  }

  fn note_append(const Path &path, const history_file_tracking &previous,
                 bool was_empty, usize expected_size) -> void
  {
    let appended = os::file_status{};
    unused(os::stat_path_following(path.view(), appended));
    update_after_append(previous, appended, was_empty);
    record(path, expected_size);
  }
};

static history_file_tracking HISTORY_FILE{};

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
static fn replace_history_file(const Path &path, StringView name_prefix,
                               StringView contents) -> history_replacement;

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

struct history_snapshot
{
  String path{koshka::heap_allocator()};
  String encoded_contents{koshka::heap_allocator()};
  koshka::ArrayList<usize> offsets{koshka::heap_allocator()};
  koshka::ArrayList<usize> durable_offsets{koshka::heap_allocator()};
  history_file_tracking file_tracking{};
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
  bool is_valid{false};
};

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
  snapshot.file_tracking = HISTORY_FILE;
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

  HISTORY_FILE = snapshot.file_tracking;
}

class CalcHistorySwap
{
public:
  pure fn is_active() const wontthrow -> bool { return m_is_active; }

  fn enter() -> void
  {
    let const did_restore_calc = exchange(m_shell, m_calc);
    m_is_active = true;

    if (did_restore_calc) return;

    if (koshka::Maybe<koshka::Path> calc = get_calc_history_file_path();
        calc.has_value())
    {
      unused(load_history(*calc, true));
    }
  }

  fn leave() -> void
  {
    m_is_active = false;
    unused(exchange(m_calc, m_shell));
  }

private:
  static fn exchange(history_snapshot &saved, history_snapshot &incoming)
      -> bool
  {
    capture_history_snapshot(saved);
    if (!incoming.is_valid) return false;

    restore_history_snapshot(incoming);
    incoming = history_snapshot{};

    return true;
  }

  history_snapshot m_shell{};
  history_snapshot m_calc{};
  bool m_is_active{false};
};

static CalcHistorySwap CALC_HISTORY_SWAP{};

static fn get_active_history_file_path() -> koshka::Maybe<koshka::Path>
{
  if (CALC_HISTORY_SWAP.is_active()) return get_calc_history_file_path();

  return get_history_file_path();
}

fn enter_calc_history() -> void { CALC_HISTORY_SWAP.enter(); }

fn leave_calc_history() -> void { CALC_HISTORY_SWAP.leave(); }

fn get_history_path() -> koshka::Maybe<koshka::Path>
{
  return get_history_file_path();
}

static bool SHOULD_PERSIST_HISTORY = false;

static fn is_history_persistent() -> bool
{
  return SHOULD_PERSIST_HISTORY || CALC_HISTORY_SWAP.is_active();
}

fn set_history_persistent(bool should_persist) -> void
{
  SHOULD_PERSIST_HISTORY = should_persist;
}

static fn reset_private_history(const Path &path) -> void
{
  ::itl_g_history_free();
  ::itl_g_history_path =
      static_cast<char *>(::itl_malloc(path.text().count() + 1));
  std::memcpy(::itl_g_history_path, path.text().data(), path.text().count());
  ::itl_g_history_path[path.text().count()] = '\0';
  ::itl_g_history_file_is_bad = false;
  ::itl_g_history_read_buffer = ::itl_char_buf_alloc();
  ::itl_g_history_read_buffer_loaded = true;
  ::itl_g_history_read_buffer_offset = 0;
  ::itl_g_history_read_buffer_start = 0;
  HISTORY_FILE.invalidate();
}

static fn find_history_record_end(StringView contents, usize start_offset)
    -> koshka::Maybe<usize>
{
  bool is_escape_pending = false;
  for (usize position = start_offset; position < contents.length; position++) {
    let const byte = contents[position];
    if (is_escape_pending) {
      is_escape_pending = false;
    } else if (byte == '\\') {
      is_escape_pending = true;
    } else if (byte == '\n') {
      return position + 1;
    }
  }

  return koshka::None;
}

static fn
append_encoded_records_to_private_branch_as_unwritten(StringView records)
    -> bool
{
  if (!::itl_history_ensure_read_buffer() && ::itl_g_history_count != 0)
    return false;

  let const buffered = ::itl_g_history_read_buffer == nullptr
                           ? StringView{}
                           : StringView{::itl_g_history_read_buffer->data,
                                        ::itl_g_history_read_buffer->size};
  let const private_end_offset =
      ::itl_g_history_read_buffer_offset + buffered.length;
  let const has_unterminated_tail =
      !::itl_g_history_ends_with_newline && !buffered.is_empty();
  usize unterminated_offset = ::itl_g_history_read_buffer_offset;
  if (has_unterminated_tail) {
    usize position = 0;
    for (let end = find_history_record_end(buffered, position); end.has_value();
         end = find_history_record_end(buffered, position))
    {
      position = *end;
    }

    unterminated_offset += position;
  }

  let payload = String{koshka::heap_allocator()};
  payload.reserve(records.length + 1);
  if (has_unterminated_tail) payload.push('\n');
  payload.append(records);

  if (has_unterminated_tail && private_end_offset > unterminated_offset) {
    ::itl_history_push_offset(unterminated_offset, unterminated_offset);
    ::itl_g_history_total_count += 1;
  }

  let const first_record_offset =
      private_end_offset + (has_unterminated_tail ? 1 : 0);
  usize record_start_offset = 0;
  for (let end = find_history_record_end(records, record_start_offset);
       end.has_value();
       end = find_history_record_end(records, record_start_offset))
  {
    ::itl_history_push_offset(first_record_offset + record_start_offset,
                              UNWRITTEN_HISTORY_RECORD_BYTE_OFFSET);
    ::itl_g_history_total_count += 1;
    record_start_offset = *end;
  }

  ::itl_g_last_history_event_number = ::itl_g_history_total_count;
  ::itl_g_history_ends_with_newline = true;
  ::itl_history_append_read_buffer(private_end_offset, payload.data(),
                                   payload.count());

  return ::itl_g_history_read_buffer != nullptr;
}

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

  if (!write_history_file_atomically(*path, ".kosh_history_write",
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
  HISTORY_FILE.record(*path, ::itl_g_history_file_size);
  HISTORY_FILE.refresh_can_rewrite();
  return koshka::Success;
}

static fn load_history(const Path &path, bool should_allow_missing)
    -> koshka::ErrorOr<koshka::Ok>
{
  HISTORY_FILE.invalidate();
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
          HISTORY_FILE.can_rewrite = false;
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

    HISTORY_FILE.status = status_after;
    HISTORY_FILE.has_status = true;
    HISTORY_FILE.refresh_can_rewrite();
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
      !::itl_g_history_file_is_bad && HISTORY_FILE.has_status &&
      os::stat_path_following(path.view(), status) &&
      os::file_status_matches(HISTORY_FILE.status, status))
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

static fn replace_history_file(const Path &path, StringView name_prefix,
                               StringView contents) -> history_replacement
{
  if (!write_history_file_atomically(path, name_prefix, contents))
    return history_replacement::Failed;

  if (load_history(path, false).is_error())
    return history_replacement::NotReloaded;

  return history_replacement::Replaced;
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

  reset_private_history(*path);
  return koshka::Success;
}

fn import_history(StringView contents) -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};

  TRY(ensure_history_loaded(*path, true));
  if (::itl_g_history_limit == 0 || contents.is_empty()) return koshka::Success;

  let records = String{koshka::heap_allocator(), contents};
  if (contents[contents.length - 1] != '\n') records.push('\n');
  if (!append_encoded_records_to_private_branch_as_unwritten(records.view()))
    return koshka::Error{"the file contains invalid data"};

  return koshka::Success;
}

fn append_unwritten_history() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return koshka::Error{os::last_system_error_message()};
  defer { os::release_process_lock(lock.take()); };
  TRY(ensure_history_loaded(*path, true));
  if (::itl_g_history_count == 0) return koshka::Success;
  if (!::itl_history_ensure_read_buffer())
    return koshka::Error{"the file contains invalid data"};

  let const buffered = StringView{::itl_g_history_read_buffer->data,
                                  ::itl_g_history_read_buffer->size};
  let payload = String{koshka::heap_allocator()};
  let unwritten_slots = koshka::ArrayList<usize>{koshka::heap_allocator()};
  let unwritten_byte_offsets =
      koshka::ArrayList<usize>{koshka::heap_allocator()};
  for (usize index = 0; index < ::itl_g_history_count; index++) {
    let const slot = (::itl_g_history_head + index) % TL_HISTORY_MAX_SIZE;
    if (::itl_g_history_durable_offsets[slot] !=
        UNWRITTEN_HISTORY_RECORD_BYTE_OFFSET)
    {
      continue;
    }

    let const start_offset = ::itl_g_history_offsets[slot];
    if (start_offset < ::itl_g_history_read_buffer_offset)
      return koshka::Error{"the file contains invalid data"};

    let const start_offset_in_buffer =
        start_offset - ::itl_g_history_read_buffer_offset;
    let const end_offset_in_buffer =
        find_history_record_end(buffered, start_offset_in_buffer);
    if (!end_offset_in_buffer.has_value())
      return koshka::Error{"the file contains invalid data"};

    unwritten_slots.push(slot);
    unwritten_byte_offsets.push(payload.count());
    payload.append(buffered.substring_of_length(start_offset_in_buffer,
                                                *end_offset_in_buffer -
                                                    start_offset_in_buffer));
  }

  if (payload.is_empty()) return koshka::Success;

  let const file_byte_count =
      os::path_file_size(path->text().view()).value_or(0);
  bool does_file_need_separator = false;
  if (file_byte_count > 0) {
    let const readable =
        os::open_file_descriptor(path->view(), os::file_open_mode::Read);
    if (!readable.has_value())
      return koshka::Error{os::last_system_error_message()};

    let const read_fd = readable.value();
    char last_byte = '\n';
    let const was_positioned =
        os::seek_descriptor_from_start(read_fd, file_byte_count - 1);
    let const read_byte_count = was_positioned
                                    ? os::read_fd(read_fd, &last_byte, 1)
                                    : koshka::Maybe<usize>{};
    let const was_read = read_byte_count.has_value() && *read_byte_count == 1;
    if (!os::close_fd(read_fd) || !was_read)
      return koshka::Error{os::last_system_error_message()};

    does_file_need_separator = last_byte != '\n';
  }

  let written = String{koshka::heap_allocator()};
  if (does_file_need_separator) written.push('\n');
  written.append(payload.view());
  let const opened =
      os::open_file_descriptor(path->view(), os::file_open_mode::Append);
  if (!opened.has_value())
    return koshka::Error{os::last_system_error_message()};

  let const fd = opened.value();
  if (!os::write_all(fd, written.data(), written.count())) {
    let const failure_message = os::last_system_error_message();
    unused(os::close_fd(fd));
    return koshka::Error{failure_message.view()};
  }

  if (!os::close_fd(fd)) return koshka::Error{os::last_system_error_message()};

  let const first_byte_offset =
      static_cast<usize>(file_byte_count) + (does_file_need_separator ? 1 : 0);
  for (usize index = 0; index < unwritten_slots.count(); index++) {
    ::itl_g_history_durable_offsets[unwritten_slots[index]] =
        first_byte_offset + unwritten_byte_offsets[index];
  }

  let const previous_tracking = HISTORY_FILE;
  ::itl_g_history_file_size = first_byte_offset + payload.count();
  HISTORY_FILE.note_append(*path, previous_tracking, file_byte_count == 0,
                           ::itl_g_history_file_size);
  return koshka::Success;
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

fn get_newest_history_event_number() -> koshka::ErrorOr<koshka::Maybe<usize>>
{
  let const path = get_history_file_path();
  if (!path.has_value()) return koshka::Maybe<usize>{koshka::None};
  TRY(ensure_history_loaded(*path, true));
  if (::itl_g_history_count == 0) return koshka::Maybe<usize>{koshka::None};

  return koshka::Maybe<usize>{::itl_g_history_total_count};
}

fn get_history_events(koshka::Allocator allocator,
                      koshka::Maybe<usize> after_event_number)
    -> koshka::ErrorOr<koshka::ArrayList<history_event>>
{
  let events = koshka::ArrayList<history_event>{allocator};
  let const path = get_history_file_path();
  if (!path.has_value()) return events;

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

static fn append_private_history_event(const itl_string_t *entry,
                                       StringView command)
    -> koshka::Maybe<usize>
{
  ::itl_g_last_history_event_number = 0;
  if (::itl_g_history_file_is_bad || entry->length <= 1) return koshka::None;

  if (::itl_g_history_count > 0) {
    if (!::itl_history_ensure_read_buffer()) return koshka::None;

    char newest[ITL_STRING_MAX_LEN];
    usize newest_byte_count = 0;
    if (::itl_history_decode_entry_buffered(
            ::itl_history_index_to_offset(::itl_g_history_count - 1), newest,
            sizeof(newest), &newest_byte_count) &&
        ::itl_string_equal_bytes(entry, newest, newest_byte_count))
    {
      ::itl_g_last_history_event_number = ::itl_g_history_total_count;
      return ::itl_g_history_total_count;
    }
  }

  let record = String{koshka::heap_allocator()};
  encode_history_record(record, command);
  if (!append_encoded_records_to_private_branch_as_unwritten(record.view()))
    return koshka::None;

  return ::itl_g_last_history_event_number;
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

  if (!is_history_persistent())
    return append_private_history_event(entry, command);

  let const was_empty = ::itl_g_history_total_count == 0;
  let const previous_tracking = HISTORY_FILE;
  if (!::itl_history_append_to_file(entry, false, false)) return koshka::None;
  HISTORY_FILE.note_append(*path, previous_tracking, was_empty,
                           ::itl_g_history_file_size);

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
  let const should_rewrite_file =
      is_history_persistent() &&
      durable_start_offset != UNWRITTEN_HISTORY_RECORD_BYTE_OFFSET;
  let contents = String{koshka::heap_allocator()};
  usize durable_end_offset = durable_start_offset;
  if (should_rewrite_file) {
    let read_contents = path->read_entire_file();
    if (!read_contents.has_value()) return false;

    let current_status = os::file_status{};
    if (!os::stat_path_following(path->text().view(), current_status))
      return false;
    if (!HISTORY_FILE.matches_identity(current_status)) return false;

    contents = read_contents.take();
    let const end_offset =
        find_history_record_end(contents.view(), durable_start_offset);
    if (!end_offset.has_value()) return false;

    durable_end_offset = *end_offset;
  }

  if (!::itl_history_ensure_read_buffer() ||
      private_start_offset < ::itl_g_history_read_buffer_offset)
  {
    return false;
  }
  let const private_contents = StringView{::itl_g_history_read_buffer->data,
                                          ::itl_g_history_read_buffer->size};
  let const private_start_offset_in_buffer =
      private_start_offset - ::itl_g_history_read_buffer_offset;
  let const private_end_offset =
      find_history_record_end(private_contents, private_start_offset_in_buffer);
  if (!private_end_offset.has_value()) return false;

  let const private_end_offset_in_buffer = *private_end_offset;

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

  let durable_rewritten = koshka::String{koshka::heap_allocator()};
  if (should_rewrite_file) {
    let expected_encoded = koshka::String{koshka::heap_allocator()};
    encode_history_record(expected_encoded, expected);
    let const durable_record = contents.view().substring_of_length(
        durable_start_offset, durable_end_offset - durable_start_offset);
    if (durable_record != expected_encoded.view()) return false;

    durable_rewritten.reserve(contents.count() -
                              (durable_end_offset - durable_start_offset) +
                              encoded_replacements.count());
    durable_rewritten.append(
        contents.view().substring_of_length(0, durable_start_offset));
    durable_rewritten.append(encoded_replacements.view());
    durable_rewritten.append(contents.view().substring(durable_end_offset));
  }

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
    durable_offsets.push(should_rewrite_file
                             ? durable_start_offset + replacement_byte_offset
                             : UNWRITTEN_HISTORY_RECORD_BYTE_OFFSET);
  }
  for (usize old_index = index + 1; old_index < ::itl_g_history_count;
       old_index++)
  {
    let const old_slot =
        (::itl_g_history_head + old_index) % TL_HISTORY_MAX_SIZE;
    private_offsets.push(
        ::itl_g_history_offsets[old_slot] - ::itl_g_history_read_buffer_offset -
        private_removed_byte_count + encoded_replacements.count());
    let const old_durable_offset = ::itl_g_history_durable_offsets[old_slot];
    let const is_durable_offset_moved =
        should_rewrite_file &&
        old_durable_offset != UNWRITTEN_HISTORY_RECORD_BYTE_OFFSET;
    durable_offsets.push(is_durable_offset_moved
                             ? old_durable_offset - durable_removed_byte_count +
                                   encoded_replacements.count()
                             : old_durable_offset);
  }

  if (should_rewrite_file) {
    let const current_contents = path->read_entire_file();
    if (!current_contents.has_value() ||
        current_contents->view() != contents.view() ||
        !write_history_file_atomically(*path, ".kosh_history_fc",
                                       durable_rewritten.view()))
    {
      return false;
    }
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
  if (should_rewrite_file)
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
  if (!should_rewrite_file) return true;

  HISTORY_FILE.record(*path, ::itl_g_history_file_size);
  HISTORY_FILE.refresh_can_rewrite();
  return HISTORY_FILE.can_rewrite;
}

static fn strip_sgr_color_sequences_only(StringView text) throws -> String;

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
  COMPLETION_SESSION.context = &context;
  ::tl_set_complete_callback(kosh_completion_callback);
  ::tl_set_highlight_callback(kosh_highlight_callback);
  ::tl_set_highlight_follows_cursor(1);
  ::tl_set_ghost_validate_callback(kosh_ghost_validate_callback);
  ::tl_set_history_select_callback(kosh_history_select_callback);
  ::tl_set_edit_callback(kosh_edit_callback);
  ::tl_set_pair_role_callback(kosh_pair_role_callback);

  if (!context.get_variable_value(SELECTOR_COMMAND_VARIABLE).has_value())
    context.set_shell_variable(SELECTOR_COMMAND_VARIABLE,
                               DEFAULT_SELECTOR_COMMAND);

  if (!context.get_variable_value(SELECTOR_OPTIONS_VARIABLE).has_value())
    context.set_shell_variable(SELECTOR_OPTIONS_VARIABLE,
                               DEFAULT_SELECTOR_OPTIONS_WITHOUT_RECORD_FRAMING);
}

fn disable_completion() -> void
{
  COMPLETION_SESSION.context = nullptr;
  ::tl_set_complete_callback(nullptr);
  ::tl_set_highlight_callback(nullptr);
  ::tl_set_highlight_follows_cursor(0);
  ::tl_set_ghost_validate_callback(nullptr);
  ::tl_set_history_select_callback(nullptr);
  ::tl_set_edit_callback(nullptr);
  ::tl_set_pair_role_callback(nullptr);
}

fn is_completion_enabled() -> bool
{
  return COMPLETION_SESSION.context != nullptr;
}

fn enable_job_notifications(koshka::EvalContext &context) -> void
{
  JOB_CONTEXT = &context;
  ::tl_set_wake_callback(kosh_wake_callback);
}

fn set_ghost_enabled(bool enabled) -> void
{
  ::tl_set_ghost_enabled(enabled ? 1 : 0);
}

fn set_space_after_completion(u8 mode) -> void
{
  ::tl_set_space_after_completion(static_cast<tl_space_after_completion>(mode));
}

fn set_history_prefix_search(bool enabled) -> void
{
  ::tl_set_history_prefix_search(enabled ? 1 : 0);
}

fn set_hint_row(bool should_show_hints, bool should_show_diagnostics) -> void
{
  COMPLETION_SESSION.should_show_hints = should_show_hints;
  COMPLETION_SESSION.should_show_diagnostics = should_show_diagnostics;
  let const is_row_shown = should_show_hints || should_show_diagnostics;
  ::tl_set_hint_callback(is_row_shown ? kosh_hint_callback : nullptr);
  ::tl_set_idle_callback(kosh_idle_callback, IDLE_DELAY_MS, IDLE_REPEAT_MS);
  koshka::completion::set_slow_gather_notice(::tl_show_completion_loading);
}

fn set_auto_pair(bool enabled) -> void { ::tl_set_auto_pair(enabled ? 1 : 0); }

fn set_extended_keys(bool enabled) -> void
{
  ::tl_set_extended_keys(enabled ? 1 : 0);
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
  COMPLETION_SESSION.tab_selector = selector;
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

static fn compact_history_file_from_locked_reread(usize entry_limit) -> bool
{
  if (!is_history_persistent()) return true;

  let const path = get_history_file_path();
  if (!path.has_value()) return true;
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return false;
  defer { os::release_process_lock(lock.take()); };

  set_history_limit(entry_limit);
  if (load_history(*path, true).is_error()) return false;
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

  return replace_history_file(*path, ".kosh_history_compact",
                              contents.view()) == history_replacement::Replaced;
}

fn exit(usize history_size_limit) -> void
{
  if (!compact_history_file_from_locked_reread(history_size_limit)) {
    if (::itl_g_history_file_is_bad) {
      koshka::ErrorWithDetails error{
          "Toiletline: history was not saved because the history file "
          "contains invalid data",
          "Remove or repair the file to record history again"};
      koshka::show_message(error.to_string());
    } else {
      koshka::Error error{"Toiletline: Could not save history: " +
                          koshka::os::last_system_error_message()};
      koshka::show_message(error.to_string());
    }
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

fn get_input(const String &prompt, const String &right_prompt,
             const String &transient_prompt) -> input_result
{
  ::tl_set_right_prompt(right_prompt.is_empty() ? nullptr
                                                : right_prompt.c_str());
  ::tl_set_transient_prompt(
      transient_prompt.is_empty() ? nullptr : transient_prompt.c_str());

  let completion_base_directory = koshka::Maybe<Path>{};
  let completion_storage =
      koshka::Maybe<koshka::completion::completion_result>{};
  if (is_completion_enabled()) {
    COMPLETION_SESSION.is_highlight_color_enabled =
        colors::stdout_wants_color();
    COMPLETION_SESSION.is_highlight_styled_underlines_enabled =
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
    COMPLETION_SESSION.attach_prompt(&*completion_base_directory,
                                     &*completion_storage);
  }
  let const history_path = get_active_history_file_path();
  if (history_path.has_value())
    unused(ensure_history_loaded(*history_path, true));
  ::itl_g_last_history_event_number = 0;
  let const previous_history_total_count = ::itl_g_history_total_count;
  let const was_history_empty = previous_history_total_count == 0;
  let const previous_history_tracking = HISTORY_FILE;
  ::itl_g_tty_changed_size = 1;
  i32 code = ::tl_get_input(TL_BUFFER, sizeof(TL_BUFFER), prompt.c_str());
  try {
    koshka::completion::abandon_idle_documentation();
  } catch (...) {}
  ::tl_set_right_prompt(nullptr);
  ::tl_set_transient_prompt(nullptr);
  if (history_path.has_value() &&
      ::itl_g_history_total_count != previous_history_total_count)
  {
    HISTORY_FILE.note_append(*history_path, previous_history_tracking,
                             was_history_empty, ::itl_g_history_file_size);
  }
  COMPLETION_SESSION.detach_prompt();
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

fn get_input(const String &prompt) -> input_result
{
  let const no_prompt = String{koshka::heap_allocator()};
  return get_input(prompt, no_prompt, no_prompt);
}

fn set_input(const String &input) -> void
{
  ::tl_set_predefined_input(input.c_str());
}

fn enter_raw_mode() -> void
{
  if (::tl_enter_raw_mode() == TL_SUCCESS) return;
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

fn restore_terminal_for_exit() wontthrow -> void
{
  ::tl_restore_terminal_for_exit();
}

fn emit_newlines(StringView buffer) -> void
{
  if (::tl_emit_newlines(buffer.data) != TL_SUCCESS)
    throw koshka::Error{"Toiletline: could not write to the terminal: " +
                        koshka::os::last_system_error_message()};
}

static constexpr usize PROMPT_PWD_LENGTH = 24;

static fn shorten_path_with_ellipsis(StringView path, usize max_length) throws
    -> String
{
  if (path.length <= max_length) return String{path};
  if (max_length < 3) return String{path};
  usize tail_start = path.length - max_length + 3;
  while (tail_start < path.length &&
         (static_cast<unsigned char>(path[tail_start]) & 0xC0) == 0x80)
    tail_start++;
  let shortened = String{koshka::heap_allocator()};
  shortened += "...";
  shortened += StringView{path.data + tail_start, path.length - tail_start};
  return shortened;
}

static fn git_branch() throws -> String
{
  if (COMPLETION_SESSION.context == nullptr)
    return utils::current_git_branch({});

  let const ceiling_directories =
      COMPLETION_SESSION.context->get_variable_value("GIT_CEILING_DIRECTORIES");
  return utils::current_git_branch(ceiling_directories.has_value()
                                       ? ceiling_directories->view()
                                       : StringView{});
}

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
  return utils::format_duration_nanoseconds(nanos, koshka::heap_allocator());
}

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

static fn append_prompt_notation(String &out, u32 value) throws -> void
{
  static constexpr StringView HEX_DIGITS{"0123456789abcdef"};
  if (value < 0x80) {
    out.push('^');
    out.push(static_cast<char>(value ^ 0x40));
    return;
  }

  out += "\\x";
  out.push(HEX_DIGITS[(value >> 4) & 0x0f]);
  out.push(HEX_DIGITS[value & 0x0f]);
}

static fn append_prompt_value(String &out, StringView value,
                              bool should_quote) throws -> void
{
  if (!should_quote) {
    out.append(value);
    return;
  }

  for (usize i = 0; i < value.length; i++) {
    if (value[i] == '$' || value[i] == '`' || value[i] == '\\') out.push('\\');
    out.push(value[i]);
  }
}

static fn append_prompt_data(String &out, StringView data,
                             bool should_quote) throws -> void
{
  static constexpr u32 INVALID_CODEPOINT = 0xffffffffu;
  let shown = String{koshka::heap_allocator()};
  usize position = 0;
  while (position < data.length) {
    let const byte = static_cast<u8>(data[position]);
    if (byte < 0x20 || byte == 0x7f) {
      append_prompt_notation(shown, byte);
      position++;
      continue;
    }

    let const decoded = utils::decode_utf8(data, position, INVALID_CODEPOINT);
    if (decoded.value == INVALID_CODEPOINT) {
      append_prompt_notation(shown, byte);
    } else if (decoded.value >= 0x80 && decoded.value < 0xa0) {
      append_prompt_notation(shown, decoded.value);
    } else {
      shown.append(data.substring_of_length(position, decoded.length));
    }

    position += decoded.length;
  }

  append_prompt_value(out, shown.view(), should_quote);
}

static fn prompt_shell_name(EvalContext &context) throws -> String
{
  let name = String{
      Path{context.execution_store().get_shell_executable_path()}.filename()};
  let const info = koshka::os::normalize_program_name(name);

  return String{name.view().substring_of_length(0, info.stem_length)};
}

static fn prompt_terminal_name() throws -> String
{
  let const terminal = koshka::os::terminal_name(KOSH_STDIN);
  if (!terminal.has_value()) return String{"tty"};

  return String{Path{terminal->view()}.filename()};
}

static fn prompt_version(EvalContext &context, bool is_full) throws -> String
{
  let const versinfo =
      context.variable_store().indexed_arrays().find("BASH_VERSINFO");
  if (versinfo.has_value() && versinfo->count() >= 3 &&
      context.get_variable_value("BASH_VERSION").has_value())
  {
    let version = String{(*versinfo.value())[0].view()};
    version += '.';
    version.append((*versinfo.value())[1].view());
    if (is_full) {
      version += '.';
      version.append((*versinfo.value())[2].view());
    }

    return version;
  }

  if (is_full) return String{KOSH_VERSION_STRING};

  return String{
      KOSH_STRINGIFY(KOSH_VER_MAJOR) "." KOSH_STRINGIFY(KOSH_VER_MINOR)};
}

static fn prompt_history_number(EvalContext &context) throws -> usize
{
  if (!context.execution_store().shell_is_interactive()) return 1;

  let const newest = get_newest_history_event_number();
  if (newest.is_error() || !newest.value().has_value()) return 1;

  return *newest.value() + 1;
}

static fn expand_prompt_escapes(StringView prompt, StringView user,
                                StringView working_directory,
                                EvalContext &context, bool should_quote) throws
    -> String
{
  let out = String{koshka::heap_allocator()};
  let const do_append_value = [&](StringView value) throws {
    append_prompt_value(out, value, should_quote);
  };
  let const do_append_data = [&](StringView data) throws {
    append_prompt_data(out, data, should_quote);
  };

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
    case 'u': do_append_data(user); break;
    case 'h': do_append_data(prompt_hostname(false).view()); break;
    case 'H': do_append_data(prompt_hostname(true).view()); break;
    case 'w':
      do_append_data(collapse_home_prefix(working_directory).view());
      break;
    case 'W': do_append_data(Path{working_directory}.filename()); break;
    case 'P':
      do_append_data(
          shorten_path_with_ellipsis(
              collapse_home_prefix(working_directory).view(), PROMPT_PWD_LENGTH)
              .view());
      break;
    case 'g': do_append_data(git_branch().view()); break;
    case '$': do_append_value(user == "root" ? "#" : "$"); break;
    case 'n': out += '\n'; break;
    case 'r': out += '\r'; break;
    case 'e': out += '\x1b'; break;
    case 'a': out += '\a'; break;
    case '[': break;
    case ']': break;
    case 't': do_append_value(prompt_strftime("%H:%M:%S").view()); break;
    case 'T': do_append_value(prompt_strftime("%I:%M:%S").view()); break;
    case '@': do_append_value(prompt_strftime("%I:%M %p").view()); break;
    case 'A': do_append_value(prompt_strftime("%H:%M").view()); break;
    case 'd': do_append_value(prompt_strftime("%a %b %d").view()); break;
    case 's': do_append_data(prompt_shell_name(context).view()); break;
    case 'v': do_append_data(prompt_version(context, false).view()); break;
    case 'V': do_append_data(prompt_version(context, true).view()); break;
    case 'l': do_append_data(prompt_terminal_name().view()); break;
    case '?': {
      const i32 status = context.execution_store().last_exit_status();
      let const should_use_color = colors::stdout_wants_color();
      if (should_use_color)
        out += status == 0 ? colors::ansi::GREEN : colors::ansi::RED;
      out += String::from(status, koshka::heap_allocator());
      if (should_use_color) out += colors::ansi::RESET;
    } break;
    case '.': {
      const i32 status = context.execution_store().last_exit_status();
      let const should_use_color = colors::stdout_wants_color();
      if (should_use_color && status != 0) out += colors::ansi::BOLD_BRIGHT_RED;
      out += "•";
      if (should_use_color && status != 0) out += colors::ansi::RESET;
    } break;
    case 'j':
      out += String::from(
          static_cast<i64>(context.job_table_store().jobs().count()),
          koshka::heap_allocator());
      break;
    case 'L':
      out += format_prompt_duration(
          context.execution_store().last_command_duration_nanos());
      break;
    case 'D': {
      if (i + 1 >= prompt.length || prompt[i + 1] != '{') {
        out += "\\D";
        break;
      }

      let const format_start = i + 2;
      let const format_end = prompt.substring(format_start).find_character('}');
      let const format_length =
          format_end.value_or(prompt.length - format_start);
      let const format =
          format_length == 0
              ? String{"%X"}
              : String{prompt.substring_of_length(format_start, format_length)};

      do_append_value(prompt_strftime(format.c_str()).view());
      i = format_start + format_length;
    } break;
    case '!':
      out += String::from(prompt_history_number(context),
                          koshka::heap_allocator());
      break;
    case '#':
      out += String::from(context.execution_store().get_command_number(),
                          koshka::heap_allocator());
      break;
    case '\\': out += '\\'; break;
    default:
      out += '\\';
      out += static_cast<char>(escaped);
      break;
    }
  }
  return out;
}

struct prompt_cache_input
{
  String name{koshka::heap_allocator()};
  Maybe<String> value{};
};
struct prompt_cache
{
  String prompt_template{koshka::heap_allocator()};
  koshka::ArrayList<prompt_cache_input> inputs{koshka::heap_allocator()};
  String expansion{koshka::heap_allocator()};
  bool is_valid{false};

  fn invalidate() -> void { is_valid = false; }

  fn matches(StringView current_template, EvalContext &context) const -> bool
  {
    if (!is_valid || current_template != prompt_template.view()) return false;

    for (let const &input : inputs) {
      let current = context.get_variable_value(input.name.view());
      let const both_unset = !current.has_value() && !input.value.has_value();
      let const both_equal = current.has_value() && input.value.has_value() &&
                             current->view() == input.value->view();
      if (!both_unset && !both_equal) return false;
    }

    return true;
  }

  fn store(const String &current_template,
           koshka::ArrayList<prompt_cache_input> &&scanned_inputs,
           String &&rendered_expansion, EvalContext &context) -> void
  {
    for (let &input : scanned_inputs)
      input.value = context.get_variable_value(input.name.view());

    prompt_template = current_template;
    inputs = steal(scanned_inputs);
    expansion = steal(rendered_expansion);
    is_valid = true;
  }
};

static prompt_cache PROMPT_CACHE{};

static fn scan_prompt_template_inputs_or_report_impure(
    StringView text, koshka::ArrayList<prompt_cache_input> &names) throws
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

static fn strip_sgr_color_sequences_only(StringView text) throws -> String
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

static fn finish_prompt_dropping_width_markers(StringView expanded) throws
    -> String
{
  let shown = String{koshka::heap_allocator()};
  for (usize i = 0; i < expanded.length; i++) {
    if (expanded[i] != '\x01' && expanded[i] != '\x02') {
      shown.push(expanded[i]);
    }
  }

  if (!colors::stdout_wants_color())
    return strip_sgr_color_sequences_only(shown.view());

  return shown;
}

static fn get_cached_user() throws -> const String &
{
  static String CACHED_USER{koshka::heap_allocator()};
  static bool was_user_resolved = false;
  if (!was_user_resolved) {
    CACHED_USER = os::get_current_user().value_or("???");
    was_user_resolved = true;
  }

  return CACHED_USER;
}

static fn decode_prompt(StringView template_string, EvalContext &context,
                        bool should_quote) throws -> String
{
  let const working_directory = Path::current_directory().text();
  return expand_prompt_escapes(template_string, get_cached_user().view(),
                               working_directory.view(), context, should_quote);
}

static fn expand_decoded_prompt(EvalContext &context, StringView name,
                                StringView decoded) throws -> Maybe<String>
{
  const i32 saved_status = context.execution_store().last_exit_status();
  Maybe<String> expanded = koshka::None;
  try {
    let source_text = String{"$"};
    source_text.append(name);
    let const source_name = koshka::intern_source_name(source_text.view());
    let const source_location =
        koshka::SourceLocation{0, decoded.length, source_name};
    expanded =
        String{context.expand_heredoc_body(decoded, &source_location).view()};
  } catch (const koshka::ErrorBase &error) {
    koshka::show_message(error.to_string(decoded, &context));
    if (let const definition =
            context.special_variable_definition_location(name);
        definition.has_value())
      context.print_source_backtrace(definition);
  }
  context.execution_store().set_last_exit_status(saved_status);

  return expanded;
}

static fn expand_prompt_variable(EvalContext &context, StringView name,
                                 StringView template_string,
                                 StringView decoded) throws -> String
{
  if (let expanded = expand_decoded_prompt(context, name, decoded);
      expanded.has_value())
    return steal(*expanded);

  return decode_prompt(template_string, context, false);
}

fn expand_prompt_template(StringView prompt, EvalContext &context) throws
    -> String
{
  let const decoded = decode_prompt(prompt, context, true);
  return String{context.expand_heredoc_body(decoded.view(), nullptr).view()};
}

fn build_prompt(EvalContext &context) -> String
{
  String ps1_template{koshka::heap_allocator()};
  if (Maybe<String> ps1 = context.get_variable_value("PS1");
      ps1.has_value() && !ps1->is_empty())
    ps1_template = steal(*ps1);
  else
    ps1_template = get_default_prompt_template();

  let const decoded = decode_prompt(ps1_template.view(), context, true);
  let scanned_inputs =
      koshka::ArrayList<prompt_cache_input>{koshka::heap_allocator()};
  let const is_cacheable = scan_prompt_template_inputs_or_report_impure(
      decoded.view(), scanned_inputs);
  if (is_cacheable && PROMPT_CACHE.matches(decoded.view(), context))
    return finish_prompt_dropping_width_markers(PROMPT_CACHE.expansion.view());

  String expanded = expand_prompt_variable(context, "PS1", ps1_template.view(),
                                           decoded.view());
  String rendered = finish_prompt_dropping_width_markers(expanded.view());

  PROMPT_CACHE.invalidate();
  if (is_cacheable)
    PROMPT_CACHE.store(decoded, steal(scanned_inputs), steal(expanded),
                       context);

  return rendered;
}

static fn render_prompt_variable(EvalContext &context, StringView name,
                                 StringView template_string) throws -> String
{
  let const decoded = decode_prompt(template_string, context, true);
  return finish_prompt_dropping_width_markers(
      expand_prompt_variable(context, name, template_string, decoded.view())
          .view());
}

fn build_right_prompt(EvalContext &context) -> String
{
  for (let const name : {StringView{"RPS1"}, StringView{"RPROMPT"}}) {
    Maybe<String> right_template = context.get_variable_value(name);
    if (right_template.has_value() && !right_template->is_empty()) {
      return render_prompt_variable(context, name, right_template->view());
    }
  }

  return String{koshka::heap_allocator()};
}

fn build_transient_prompt(EvalContext &context) -> String
{
  Maybe<String> transient_template =
      context.get_variable_value("PS1_TRANSIENT");
  if (transient_template.has_value() && !transient_template->is_empty()) {
    return render_prompt_variable(context, "PS1_TRANSIENT",
                                  transient_template->view());
  }

  return finish_prompt_dropping_width_markers(
      decode_prompt("\\$ ", context, false).view());
}

fn render_ps0(EvalContext &context) -> String
{
  Maybe<String> ps0 = context.get_variable_value("PS0");
  if (!ps0.has_value() || ps0->is_empty()) {
    return String{koshka::heap_allocator()};
  }

  let const decoded = decode_prompt(ps0->view(), context, true);
  Maybe<String> expanded =
      expand_decoded_prompt(context, "PS0", decoded.view());
  if (!expanded.has_value()) return String{koshka::heap_allocator()};

  return finish_prompt_dropping_width_markers(expanded->view());
}

static constexpr StringView SHELL_INTEGRATION_VARIABLE{
    "KOSH_SHELL_INTEGRATION"};
static constexpr StringView OSC_END{"\a"};

static fn is_shell_integration_enabled(EvalContext &context) throws -> bool
{
  if (!colors::stdout_is_a_terminal()) return false;

  if (let const term = os::get_environment_variable("TERM");
      term.has_value() && term->view() == StringView{"dumb"})
  {
    return false;
  }

  if (let const value = context.get_variable_value(SHELL_INTEGRATION_VARIABLE);
      value.has_value() && value->view() == StringView{"0"})
  {
    return false;
  }

  return true;
}

static fn is_vscode_terminal() throws -> bool
{
  let const program = os::get_environment_variable("TERM_PROGRAM");
  return program.has_value() && program->view() == StringView{"vscode"};
}

static fn append_hex_byte(String &output, unsigned char byte) throws -> void
{
  static constexpr StringView DIGITS{"0123456789abcdef"};
  output.push(DIGITS[byte >> 4]);
  output.push(DIGITS[byte & 0x0f]);
}

static fn append_file_uri_path(String &output, StringView directory) throws
    -> void
{
  let const is_drive_path = directory.count() >= 2 && directory[1] == ':';
  if (is_drive_path || (directory.count() > 0 && directory[0] == '\\')) {
    output.push('/');
  }

  for (usize index = 0; index < directory.count(); index++) {
    let const byte = static_cast<unsigned char>(directory[index]);
    let const is_plain =
        (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
        byte == '_' || byte == '~' || byte == '/' || byte == ':';
    if (is_plain) {
      output.push(static_cast<char>(byte));
    } else if (byte == '\\' && is_drive_path) {
      output.push('/');
    } else {
      output.push('%');
      append_hex_byte(output, byte);
    }
  }
}

static fn append_uri_host(String &output, StringView host) throws -> void
{
  for (usize index = 0; index < host.count(); index++) {
    let const byte = static_cast<unsigned char>(host[index]);
    let const is_plain =
        (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.';
    if (is_plain) {
      output.push(static_cast<char>(byte));
    } else {
      output.push('%');
      append_hex_byte(output, byte);
    }
  }
}

static fn append_vscode_property(String &output, StringView value) throws
    -> void
{
  for (usize index = 0; index < value.count(); index++) {
    let const byte = static_cast<unsigned char>(value[index]);
    let const next_byte = static_cast<unsigned char>(
        index + 1 < value.count() ? value[index + 1] : '\0');
    let const is_c1_control =
        byte == 0xc2 && next_byte >= 0x80 && next_byte < 0xa0;
    if (byte == '\\') {
      output += "\\\\";
    } else if (is_c1_control) {
      output += "\\x";
      append_hex_byte(output, next_byte);
      index++;
    } else if (byte < 0x20 || byte == ';' || byte == 0x7f) {
      output += "\\x";
      append_hex_byte(output, byte);
    } else {
      output.push(static_cast<char>(byte));
    }
  }
}

fn emit_command_end_mark(EvalContext &context, i32 exit_status) -> void
{
  if (!is_shell_integration_enabled(context)) return;

  let sequence = String{koshka::heap_allocator()};
  sequence += "\x1b]133;D;";
  sequence += String::from(exit_status, koshka::heap_allocator());
  sequence += OSC_END;
  koshka::print(sequence);
  koshka::flush();
}

fn emit_prompt_start_marks(EvalContext &context) -> void
{
  if (!is_shell_integration_enabled(context)) return;

  let const directory = Path::current_directory().text();
  let const host = os::get_hostname().value_or(String{""});

  let sequence = String{koshka::heap_allocator()};
  sequence += "\x1b]7;file://";
  append_uri_host(sequence, host.view());
  append_file_uri_path(sequence, directory.view());
  sequence += OSC_END;
  if (is_vscode_terminal()) {
    sequence += "\x1b]633;P;Cwd=";
    append_vscode_property(sequence, directory.view());
    sequence += OSC_END;
  }
  sequence += "\x1b]133;A";
  sequence += OSC_END;
  koshka::print(sequence);
  koshka::flush();
}

fn append_prompt_end_mark(EvalContext &context, String &prompt) -> void
{
  if (!is_shell_integration_enabled(context)) return;

  prompt += "\x1b]133;B";
  prompt += OSC_END;
}

fn emit_command_start_marks(EvalContext &context, StringView command_line)
    -> void
{
  if (!is_shell_integration_enabled(context)) return;

  let sequence = String{koshka::heap_allocator()};
  if (is_vscode_terminal()) {
    sequence += "\x1b]633;E;";
    append_vscode_property(sequence, command_line);
    sequence += OSC_END;
  }
  sequence += "\x1b]133;C";
  sequence += OSC_END;
  koshka::print(sequence);
  koshka::flush();
}

} /* namespace toiletline */

#endif
