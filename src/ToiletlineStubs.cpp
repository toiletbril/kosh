/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file supplies KOSH_NO_TOILETLINE builds with file-backed noninteractive
 * history and inert implementations of terminal-dependent editor operations.
 */

#include "CliColors.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"

#if defined KOSH_NO_TOILETLINE

namespace koshka::internal {

static constexpr usize NO_EDITOR_HISTORY_ENTRY_MAX_BYTE_COUNT = 2048;
static constexpr usize NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT = 4095;
static constexpr u64 HISTORY_HASH_OFFSET_BASIS = 14695981039346656037ull;
static constexpr u64 HISTORY_HASH_PRIME = 1099511628211ull;
static constexpr char NO_EDITOR_HISTORY_FILE[] = ".kosh_history";

struct no_editor_history_state
{
  String loaded_path{heap_allocator()};
  ArrayList<usize> record_byte_offsets{heap_allocator()};
  usize total_count{0};
  usize file_byte_count{0};
  usize trailing_record_start_byte_offset{0};
  usize first_record_index{0};
  u64 file_contents_hash{HISTORY_HASH_OFFSET_BASIS};
  u16 entry_limit{TL_HISTORY_MAX_SIZE};
  bool is_loaded{false};
};

struct history_record_span
{
  usize start_byte_offset;
  usize end_byte_offset;
};

static fn get_no_editor_history_state() -> no_editor_history_state &
{
  static no_editor_history_state state;
  return state;
}

static fn extend_history_contents_hash(u64 hash, StringView contents) -> u64
{
  for (usize byte_offset = 0; byte_offset < contents.length; byte_offset++) {
    hash ^= static_cast<u8>(contents[byte_offset]);
    hash *= HISTORY_HASH_PRIME;
  }

  return hash;
}

static fn resolve_no_editor_history_path() -> Maybe<Path>
{
  if (let const override_path = os::get_environment_variable("KOSH_HISTORY");
      override_path.has_value() && !override_path->is_empty())
  {
    return Path{override_path->view()};
  }

  let home = os::get_home_directory();
  if (!home.has_value()) return None;
  let path = home->clone();
  path.push_component(NO_EDITOR_HISTORY_FILE);
  return path;
}

static fn get_history_record_byte_offset(const no_editor_history_state &state,
                                         usize record_index) -> usize
{
  return state.record_byte_offsets[(state.first_record_index + record_index) %
                                   state.record_byte_offsets.count()];
}

static fn push_history_record_byte_offset(no_editor_history_state &state,
                                          usize byte_offset) -> void
{
  if (state.entry_limit == 0) return;

  if (state.record_byte_offsets.count() < state.entry_limit) {
    state.record_byte_offsets.push(byte_offset);
    return;
  }

  state.record_byte_offsets[state.first_record_index] = byte_offset;
  state.first_record_index =
      (state.first_record_index + 1) % state.record_byte_offsets.count();
}

static fn next_history_record(StringView contents, usize &byte_offset,
                              history_record_span &span, bool &is_valid) -> bool
{
  span.start_byte_offset = byte_offset;
  bool is_escape_pending = false;
  while (byte_offset < contents.length) {
    let const byte = static_cast<u8>(contents[byte_offset]);
    byte_offset++;

    if (is_escape_pending) {
      is_escape_pending = false;
      continue;
    }
    if (byte == '\\') {
      is_escape_pending = true;
      continue;
    }
    if (byte == '\n') {
      span.end_byte_offset = byte_offset;
      return true;
    }
    if (byte == '\r' || byte == '\t' || byte == '\v' || byte == '\f') continue;
    if (byte < 0x20 || byte == 0x7f) {
      is_valid = false;
      return false;
    }
  }

  return false;
}

static fn measure_history_text(StringView text, usize &accepted_byte_count,
                               usize *codepoint_count) -> bool;

static fn decode_history_record(String &decoded, StringView contents,
                                history_record_span span) -> bool
{
  decoded.clear();
  let const encoded_byte_count = span.end_byte_offset - span.start_byte_offset;
  decoded.reserve(encoded_byte_count < NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT
                      ? encoded_byte_count
                      : NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT);
  bool is_escape_pending = false;
  for (usize byte_offset = span.start_byte_offset;
       byte_offset + 1 < span.end_byte_offset; byte_offset++)
  {
    let byte = contents[byte_offset];
    if (is_escape_pending) {
      is_escape_pending = false;
      if (byte == 'n')
        byte = '\n';
      else if (byte != '\\') {
        if (decoded.count() == NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT)
          return false;
        decoded.push('\\');
      }
    } else if (byte == '\\') {
      is_escape_pending = true;
      continue;
    } else if (byte == '\r') {
      continue;
    }

    if (decoded.count() == NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT)
      return false;
    decoded.push(byte);
  }

  return true;
}

static fn encode_history_record(String &output, StringView command) -> void
{
  output.reserve(output.count() + command.length * 2 + 1);
  for (usize byte_offset = 0; byte_offset < command.length; byte_offset++) {
    let const byte = command[byte_offset];
    if (byte == '\\')
      output += "\\\\";
    else if (byte == '\n')
      output += "\\n";
    else
      output.push(byte);
  }
  output.push('\n');
}

static fn measure_history_text(StringView text, usize &accepted_byte_count,
                               usize *codepoint_count) -> bool
{
  accepted_byte_count = 0;
  if (codepoint_count != nullptr) *codepoint_count = 0;
  while (accepted_byte_count < text.length) {
    let const first_byte = static_cast<u8>(text[accepted_byte_count]);
    usize codepoint_byte_count = 0;
    if ((first_byte & 0x80) == 0)
      codepoint_byte_count = 1;
    else if ((first_byte & 0xe0) == 0xc0)
      codepoint_byte_count = 2;
    else if ((first_byte & 0xf0) == 0xe0)
      codepoint_byte_count = 3;
    else if ((first_byte & 0xf8) == 0xf0)
      codepoint_byte_count = 4;
    else
      return false;

    if (accepted_byte_count + codepoint_byte_count > text.length) break;
    accepted_byte_count += codepoint_byte_count;
    if (codepoint_count != nullptr) (*codepoint_count)++;
  }

  return true;
}

static fn scan_no_editor_history(const Path &path, StringView contents,
                                 Maybe<u64> contents_hash = None) -> bool
{
  let &state = get_no_editor_history_state();
  state.loaded_path = String{heap_allocator(), path.text().view()};
  state.record_byte_offsets.clear();
  state.total_count = 0;
  state.file_byte_count = contents.length;
  state.trailing_record_start_byte_offset = 0;
  state.first_record_index = 0;
  state.file_contents_hash =
      contents_hash.has_value()
          ? *contents_hash
          : extend_history_contents_hash(HISTORY_HASH_OFFSET_BASIS, contents);
  state.is_loaded = false;

  usize byte_offset = 0;
  bool is_valid = true;
  history_record_span span{};
  while (next_history_record(contents, byte_offset, span, is_valid)) {
    push_history_record_byte_offset(state, span.start_byte_offset);
    state.total_count++;
  }
  if (!is_valid) return false;

  state.trailing_record_start_byte_offset = span.start_byte_offset;
  state.is_loaded = true;
  return true;
}

static fn load_no_editor_history(const Path &path, bool should_allow_missing)
    -> bool
{
  let &state = get_no_editor_history_state();
  state.is_loaded = false;

  if (!path.exists()) {
    if (!should_allow_missing) return false;
    return scan_no_editor_history(path, {});
  }

  let const contents = path.read_entire_file();
  if (!contents.has_value()) return false;
  return scan_no_editor_history(path, contents->view());
}

static fn ensure_no_editor_history_loaded(const Path &path,
                                          bool should_allow_missing) -> bool
{
  let &state = get_no_editor_history_state();
  if (!state.is_loaded || state.loaded_path.view() != path.text().view())
    return load_no_editor_history(path, should_allow_missing);

  let const file_byte_count = path.file_size();
  if (!file_byte_count.has_value()) {
    if (!should_allow_missing || path.exists()) return false;
    return load_no_editor_history(path, true);
  }
  if (*file_byte_count == state.file_byte_count) return true;
  return load_no_editor_history(path, should_allow_missing);
}

static fn read_no_editor_history_contents(const Path &path,
                                          bool should_allow_missing)
    -> Maybe<String>
{
  if (!path.exists()) {
    if (!should_allow_missing || !scan_no_editor_history(path, {})) return None;
    return String{heap_allocator()};
  }

  let contents = path.read_entire_file();
  if (!contents.has_value()) {
    let &state = get_no_editor_history_state();
    state.record_byte_offsets.clear();
    state.total_count = 0;
    state.is_loaded = false;
    return None;
  }
  let &state = get_no_editor_history_state();
  let const contents_hash =
      extend_history_contents_hash(HISTORY_HASH_OFFSET_BASIS, contents->view());
  if (!state.is_loaded || state.loaded_path.view() != path.text().view() ||
      state.file_byte_count != contents->count() ||
      state.file_contents_hash != contents_hash)
  {
    if (!scan_no_editor_history(path, contents->view(), contents_hash))
      return None;
  }

  return contents.take();
}

static fn replace_history_file(const Path &path, StringView original,
                               StringView replacement, StringView prefix)
    -> bool
{
  let parent = path.parent();
  if (parent.text().is_empty()) parent = Path{"."};
  let replacement_path =
      os::write_to_named_temp_file(parent, prefix, replacement);
  if (!replacement_path.has_value()) return false;
  defer { unused(os::remove_file(replacement_path->text().view())); };

  {
    let const current_contents = path.read_entire_file();
    if (!current_contents.has_value() || current_contents->view() != original)
      return false;
  }
  if (!os::rename_path(replacement_path->text().view(), path.text().view()))
    return false;

  return scan_no_editor_history(path, replacement);
}

static fn erase_previous_no_editor_history_events(StringView command) -> bool
{
  let const path = resolve_no_editor_history_path();
  if (!path.has_value()) return false;
  let contents = read_no_editor_history_contents(*path, true);
  if (!contents.has_value() || contents->is_empty())
    return contents.has_value();
  let &state = get_no_editor_history_state();
  if (state.record_byte_offsets.is_empty()) return true;

  let rewritten = String{heap_allocator()};
  let const first_retained_number =
      state.total_count - state.record_byte_offsets.count() + 1;
  usize byte_offset = 0;
  usize copied_offset = 0;
  usize number = 0;
  bool is_valid = true;
  bool did_remove = false;
  history_record_span span{};
  let decoded = String{heap_allocator()};
  while (next_history_record(contents->view(), byte_offset, span, is_valid)) {
    number++;
    if (number < first_retained_number) continue;
    if (!decode_history_record(decoded, contents->view(), span)) return false;
    if (decoded.view() != command) continue;

    if (!did_remove) rewritten.reserve(contents->count());
    rewritten.append(contents->substring_of_length(
        copied_offset, span.start_byte_offset - copied_offset));
    copied_offset = span.end_byte_offset;
    did_remove = true;
  }
  if (!is_valid) return false;
  if (!did_remove) return true;
  rewritten.append(contents->substring(copied_offset));
  return replace_history_file(*path, contents->view(), rewritten.view(),
                              ".kosh_history_erasedups");
}

template <class Match>
static fn find_no_editor_history_event(Allocator allocator,
                                       Maybe<usize> before_event_number,
                                       Match do_match)
    -> Maybe<toiletline::history_event>
{
  let const path = resolve_no_editor_history_path();
  if (!path.has_value()) return None;
  let contents = read_no_editor_history_contents(*path, false);
  if (!contents.has_value()) return None;
  let &state = get_no_editor_history_state();
  let const retained_record_count = state.record_byte_offsets.count();
  let const first_number = state.total_count - retained_record_count + 1;
  let decoded = String{heap_allocator()};
  for (usize index = retained_record_count; index > 0; index--) {
    let const number = first_number + index - 1;
    if (before_event_number.has_value() && number >= *before_event_number)
      continue;
    usize byte_offset = get_history_record_byte_offset(state, index - 1);
    bool is_valid = true;
    history_record_span span{};
    if (!next_history_record(contents->view(), byte_offset, span, is_valid))
      return None;
    if (!decode_history_record(decoded, contents->view(), span)) return None;
    if (do_match(number, decoded.view())) {
      return toiletline::history_event{
          number, String{allocator, decoded.view()}
      };
    }
  }

  return None;
}

static fn newest_no_editor_history_event_equals(const Path &path,
                                                StringView command)
    -> Maybe<bool>
{
  let contents = read_no_editor_history_contents(path, true);
  if (!contents.has_value()) return None;
  let &state = get_no_editor_history_state();
  if (state.record_byte_offsets.is_empty()) return false;

  usize byte_offset = get_history_record_byte_offset(
      state, state.record_byte_offsets.count() - 1);
  bool is_valid = true;
  history_record_span span{};
  if (!next_history_record(contents->view(), byte_offset, span, is_valid))
    return None;
  let decoded = String{heap_allocator()};
  if (!decode_history_record(decoded, contents->view(), span)) return None;
  return decoded.view() == command;
}

static fn rewrite_no_editor_history_event(usize wanted_number,
                                          StringView expected,
                                          const ArrayList<String> &replacements)
    -> bool
{
  let const path = resolve_no_editor_history_path();
  if (!path.has_value()) return false;
  let contents = read_no_editor_history_contents(*path, false);
  if (!contents.has_value()) return false;
  let &state = get_no_editor_history_state();
  let const retained_record_count = state.record_byte_offsets.count();
  if (retained_record_count == 0) return false;
  let const first_number = state.total_count - retained_record_count + 1;
  if (wanted_number < first_number ||
      wanted_number >= first_number + retained_record_count)
  {
    return false;
  }

  let const retained_index = wanted_number - first_number;
  usize byte_offset = get_history_record_byte_offset(state, retained_index);
  bool is_valid = true;
  history_record_span span{};
  if (!next_history_record(contents->view(), byte_offset, span, is_valid))
    return false;
  let decoded = String{heap_allocator()};
  if (!decode_history_record(decoded, contents->view(), span)) return false;
  if (decoded.view() != expected) return false;

  let rewritten = String{heap_allocator()};
  rewritten.reserve(contents->count());
  rewritten.append(contents->substring_of_length(0, span.start_byte_offset));
  for (let const &replacement : replacements) {
    usize accepted_byte_count = 0;
    if (replacement.count() > NO_EDITOR_HISTORY_ENTRY_MAX_BYTE_COUNT ||
        !measure_history_text(replacement.view(), accepted_byte_count, nullptr))
    {
      return false;
    }
    encode_history_record(rewritten, replacement.view().substring_of_length(
                                         0, accepted_byte_count));
  }
  rewritten.append(contents->substring(span.end_byte_offset));
  return replace_history_file(*path, contents->view(), rewritten.view(),
                              ".kosh_history_fc");
}

} /* namespace koshka::internal */

namespace toiletline {

using koshka::String;
using koshka::StringView;

fn enable_completion(koshka::EvalContext &context) -> void { unused(context); }

fn disable_completion() -> void {}

fn completion_is_enabled() -> bool { return false; }

fn enter_calc_history() -> void {}

fn leave_calc_history() -> void {}

fn history_path() -> koshka::Maybe<koshka::Path>
{
  return koshka::internal::resolve_no_editor_history_path();
}

fn history_write() -> bool { return true; }

fn history_read() -> bool
{
  let const path = history_path();
  if (!path.has_value()) return false;
  return koshka::internal::load_no_editor_history(*path, false);
}

fn history_clear() -> bool
{
  let const path = history_path();
  if (!path.has_value()) return false;
  let opened = koshka::os::open_file_descriptor(
      path->text().view(), koshka::os::file_open_mode::Truncate);
  if (!opened.has_value()) return false;
  koshka::os::close_fd(opened.take());
  return koshka::internal::load_no_editor_history(*path, false);
}

fn set_history_enabled(bool is_enabled) -> void { unused(is_enabled); }

fn set_history_limit(usize entry_count) -> void
{
  let &state = koshka::internal::get_no_editor_history_state();
  let const retained_limit =
      entry_count < TL_HISTORY_MAX_SIZE ? entry_count : TL_HISTORY_MAX_SIZE;
  if (retained_limit == state.entry_limit) return;
  state.entry_limit = static_cast<u16>(retained_limit);
  if (state.record_byte_offsets.count() <= retained_limit) return;

  let retained_offsets = koshka::ArrayList<usize>{koshka::heap_allocator()};
  retained_offsets.reserve(retained_limit);
  let const first_retained_index =
      state.record_byte_offsets.count() - retained_limit;
  for (usize index = first_retained_index;
       index < state.record_byte_offsets.count(); index++)
  {
    retained_offsets.push(
        koshka::internal::get_history_record_byte_offset(state, index));
  }

  state.record_byte_offsets = steal(retained_offsets);
  state.first_record_index = 0;
}

fn history_events(koshka::Allocator allocator)
    -> koshka::ArrayList<history_event>
{
  let events = koshka::ArrayList<history_event>{allocator};
  let const path = history_path();
  if (!path.has_value()) return events;
  let contents =
      koshka::internal::read_no_editor_history_contents(*path, false);
  if (!contents.has_value()) return events;
  let &state = koshka::internal::get_no_editor_history_state();
  let const retained_record_count = state.record_byte_offsets.count();
  let const first_number = state.total_count - retained_record_count + 1;
  events.reserve(retained_record_count);
  for (usize index = 0; index < retained_record_count; index++) {
    usize byte_offset =
        koshka::internal::get_history_record_byte_offset(state, index);
    bool is_valid = true;
    koshka::internal::history_record_span span{};
    if (!koshka::internal::next_history_record(contents->view(), byte_offset,
                                               span, is_valid))
    {
      events.clear();
      return events;
    }
    let command = String{allocator};
    if (!koshka::internal::decode_history_record(command, contents->view(),
                                                 span))
    {
      events.clear();
      return events;
    }
    events.push(history_event{first_number + index, steal(command)});
  }
  return events;
}

fn relative_history_event(koshka::Allocator allocator, usize distance,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  if (distance == 0) return koshka::None;
  usize remaining_event_count = distance;
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number,
      [&](usize, StringView) { return --remaining_event_count == 0; });
}

fn numbered_history_event(koshka::Allocator allocator, usize number,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number, [&](usize candidate_number, StringView) {
        return candidate_number == number;
      });
}

fn prefixed_history_event(koshka::Allocator allocator, StringView prefix,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number,
      [&](usize, StringView command) { return command.starts_with(prefix); });
}

fn containing_history_event(koshka::Allocator allocator, StringView text,
                            koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number, [&](usize, StringView command) {
        return command.find_substring(text).has_value();
      });
}

fn history_append_event(StringView command,
                        history_duplicate_policy duplicate_policy)
    -> koshka::Maybe<usize>
{
  if (command.is_empty() ||
      command.length > koshka::internal::NO_EDITOR_HISTORY_ENTRY_MAX_BYTE_COUNT)
  {
    return koshka::None;
  }
  usize accepted_byte_count = 0;
  usize codepoint_count = 0;
  if (!koshka::internal::measure_history_text(command, accepted_byte_count,
                                              &codepoint_count) ||
      codepoint_count <= 1)
  {
    return koshka::None;
  }
  command = command.substring_of_length(0, accepted_byte_count);

  let const path = history_path();
  if (!path.has_value()) return koshka::None;
  let const should_erase_previous =
      duplicate_policy == history_duplicate_policy::ErasePrevious ||
      duplicate_policy ==
          history_duplicate_policy::IgnoreConsecutiveAndErasePrevious;
  let const should_ignore_consecutive =
      duplicate_policy == history_duplicate_policy::IgnoreConsecutive ||
      duplicate_policy ==
          history_duplicate_policy::IgnoreConsecutiveAndErasePrevious;
  if (should_ignore_consecutive) {
    let const is_newest_equal =
        koshka::internal::newest_no_editor_history_event_equals(*path, command);
    if (!is_newest_equal.has_value() || *is_newest_equal) return koshka::None;
  }
  if (should_erase_previous &&
      !koshka::internal::erase_previous_no_editor_history_events(command))
  {
    return koshka::None;
  }
  if (!should_ignore_consecutive && !should_erase_previous &&
      !koshka::internal::ensure_no_editor_history_loaded(*path, true))
  {
    return koshka::None;
  }

  let &state = koshka::internal::get_no_editor_history_state();
  let const had_unterminated_record =
      state.trailing_record_start_byte_offset != state.file_byte_count;
  let payload = String{koshka::heap_allocator()};
  if (had_unterminated_record) payload.push('\n');
  koshka::internal::encode_history_record(payload, command);
  let opened = koshka::os::open_file_descriptor(
      path->text().view(), koshka::os::file_open_mode::Append);
  if (!opened.has_value()) return koshka::None;
  let const fd = opened.value();
  let const was_written =
      koshka::os::write_all(fd, payload.data(), payload.count());
  koshka::os::close_fd(fd);
  if (!was_written) {
    state.is_loaded = false;
    return koshka::None;
  }
  if (!koshka::internal::load_no_editor_history(*path, false))
    return koshka::None;

  return state.total_count;
}

fn history_rewrite_event(usize number, StringView expected,
                         StringView replacement) -> bool
{
  let replacements = koshka::ArrayList<String>{koshka::heap_allocator()};
  if (!replacement.is_empty())
    replacements.push(String{koshka::heap_allocator(), replacement});
  return history_rewrite_event(number, expected, replacements);
}

fn history_rewrite_event(usize number, StringView expected,
                         const koshka::ArrayList<koshka::String> &replacements)
    -> bool
{
  return koshka::internal::rewrite_no_editor_history_event(number, expected,
                                                           replacements);
}

fn enable_job_notifications(koshka::EvalContext &context) -> void
{
  unused(context);
}

fn set_ghost_enabled(bool enabled) -> void { unused(enabled); }

fn set_highlight_enabled(bool enabled) -> void { unused(enabled); }

fn set_edit_mode(edit_mode mode) -> void { unused(mode); }

fn is_active() -> bool { return false; }

fn initialize() -> void
{
  throw koshka::Error{
      "This build has no line editor, use '-c', '-s', or a file argument"};
}

fn exit(bool should_append_history, usize history_size_limit,
        usize history_file_size_limit) -> void
{
  unused(should_append_history);
  unused(history_size_limit);
  unused(history_file_size_limit);
}

fn set_title(StringView title) -> void { unused(title); }

fn set_idle_title() -> void {}

fn get_input(const String &prompt) -> input_result
{
  unused(prompt);
  throw koshka::Error{"This build has no line editor"};
}

fn set_input(const String &input) -> void { unused(input); }

fn enter_raw_mode() -> void {}

fn exit_raw_mode() -> void {}

fn emit_newlines(StringView buffer) -> void { unused(buffer); }

fn debug_allocation_failure() -> bool { return true; }

fn default_prompt_template() -> String
{
  let template_string = String{koshka::heap_allocator()};
  let const should_use_color = koshka::colors::stdout_wants_color();

  if (should_use_color) {
    template_string += "[\\u@\\h${KOSH_GIT_BRANCH:+ (";
    template_string += koshka::colors::ansi::CYAN;
    template_string += "$KOSH_GIT_BRANCH";
    template_string += koshka::colors::ansi::RESET;
    template_string += ")} ";
    template_string += koshka::colors::ansi::GREEN;
    template_string += "\\P";
    template_string += koshka::colors::ansi::RESET;
  } else {
    template_string += "[\\u@\\h${KOSH_GIT_BRANCH:+ ($KOSH_GIT_BRANCH)} \\P";
  }
  template_string += "] ";
  return template_string;
}

fn build_prompt(koshka::EvalContext &context) -> String
{
  unused(context);
  throw koshka::Error{"This build has no line editor"};
}

fn expand_prompt_template(StringView prompt, koshka::EvalContext &context)
    -> String
{
  unused(context);
  return String{prompt};
}

fn render_ps0(koshka::EvalContext &context) -> String
{
  unused(context);
  return String{koshka::heap_allocator()};
}

} /* namespace toiletline */

#endif /* KOSH_NO_TOILETLINE */
