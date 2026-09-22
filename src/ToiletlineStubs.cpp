/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file supplies KOSH_NO_TOILETLINE builds with file-backed noninteractive
 * history and inert implementations of terminal-dependent editor operations.
 */

#include "CLIColors.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"

#if defined KOSH_NO_TOILETLINE

namespace koshka::internal {

static constexpr usize NO_EDITOR_HISTORY_ENTRY_MAX_BYTE_COUNT = 2048;
static constexpr usize NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT =
    toiletline::HISTORY_RECORD_MAX_DECODED_BYTE_COUNT;
static constexpr u64 HISTORY_HASH_OFFSET_BASIS = 14695981039346656037ull;
static constexpr u64 HISTORY_HASH_PRIME = 1099511628211ull;
static constexpr char NO_EDITOR_HISTORY_FILE[] = ".kosh_history";

struct no_editor_history_state
{
  String loaded_path{heap_allocator()};
  String branch_contents{heap_allocator()};
  ArrayList<usize> record_byte_offsets{heap_allocator()};
  ArrayList<usize> durable_record_byte_offsets{heap_allocator()};
  usize total_count{0};
  usize file_byte_count{0};
  usize trailing_record_start_byte_offset{0};
  usize first_record_index{0};
  os::file_status file_status{};
  u64 file_contents_hash{HISTORY_HASH_OFFSET_BASIS};
  u16 entry_limit{TL_HISTORY_MAX_SIZE};
  bool is_loaded{false};
  bool is_file_bad{false};
  bool has_file_status{false};
  bool can_rewrite{false};
};

struct history_record_span
{
  usize start_byte_offset;
  usize end_byte_offset;
};

/* Rereading the same bytes cannot repair a malformed record, and it can repair
   a file that grew between the read and the stat. The outcomes are kept apart
   so a retry is spent only on the second case. */
enum class history_scan_outcome : u8
{
  Loaded,
  Invalid,
  Stale,
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

static fn update_history_file_status(no_editor_history_state &state,
                                     const Path &path) -> bool
{
  state.has_file_status =
      os::stat_path_following(path.view(), state.file_status);
  return state.has_file_status;
}

static fn resolve_no_editor_history_path() -> Maybe<Path>
{
  if (let const override_path =
          os::get_environment_variable("KOSH_HISTORY_FILE");
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
  ASSERT(!state.record_byte_offsets.is_empty());
  return state.record_byte_offsets[(state.first_record_index + record_index) %
                                   state.record_byte_offsets.count()];
}

static fn
get_history_durable_record_byte_offset(const no_editor_history_state &state,
                                       usize record_index) -> usize
{
  ASSERT(!state.durable_record_byte_offsets.is_empty());
  return state
      .durable_record_byte_offsets[(state.first_record_index + record_index) %
                                   state.durable_record_byte_offsets.count()];
}

static fn get_history_record_span(const no_editor_history_state &state,
                                  usize record_index) -> history_record_span
{
  ASSERT(record_index < state.record_byte_offsets.count());
  let const start_byte_offset =
      get_history_record_byte_offset(state, record_index);
  let const end_byte_offset =
      record_index + 1 < state.record_byte_offsets.count()
          ? get_history_record_byte_offset(state, record_index + 1)
          : state.trailing_record_start_byte_offset;
  ASSERT(start_byte_offset < end_byte_offset);
  return {start_byte_offset, end_byte_offset};
}

static fn push_history_record_byte_offset(no_editor_history_state &state,
                                          usize byte_offset,
                                          usize durable_byte_offset) -> void
{
  if (state.entry_limit == 0) return;

  if (state.record_byte_offsets.count() < state.entry_limit) {
    state.record_byte_offsets.push(byte_offset);
    state.durable_record_byte_offsets.push(durable_byte_offset);
    return;
  }

  state.record_byte_offsets[state.first_record_index] = byte_offset;
  state.durable_record_byte_offsets[state.first_record_index] =
      durable_byte_offset;
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
    } else if (byte == '\r' && byte_offset + 2 == span.end_byte_offset) {
      /* The record was written with CRLF endings. A carriage return anywhere
         else is entry data. */
      continue;
    }

    if (decoded.count() == NO_EDITOR_HISTORY_DECODED_MAX_BYTE_COUNT)
      return false;
    decoded.push(byte);
  }

  return true;
}

static fn scan_no_editor_history(const Path &path, StringView contents,
                                 Maybe<u64> contents_hash = None,
                                 bool should_allow_missing = false)
    -> history_scan_outcome
{
  let &state = get_no_editor_history_state();
  if (state.loaded_path.view() != path.view())
    state.loaded_path = String{heap_allocator(), path.view()};
  state.branch_contents = String{heap_allocator(), contents};
  state.record_byte_offsets.clear();
  state.durable_record_byte_offsets.clear();
  state.total_count = 0;
  state.file_byte_count = contents.length;
  state.trailing_record_start_byte_offset = 0;
  state.first_record_index = 0;
  state.file_contents_hash =
      contents_hash.has_value()
          ? *contents_hash
          : extend_history_contents_hash(HISTORY_HASH_OFFSET_BASIS, contents);
  state.is_loaded = false;
  state.has_file_status = false;
  state.can_rewrite = false;

  usize byte_offset = 0;
  bool is_valid = true;
  history_record_span span{};
  while (next_history_record(contents, byte_offset, span, is_valid)) {
    push_history_record_byte_offset(state, span.start_byte_offset,
                                    span.start_byte_offset);
    state.total_count++;
  }
  if (!is_valid) return history_scan_outcome::Invalid;

  state.trailing_record_start_byte_offset = span.start_byte_offset;
  if (!update_history_file_status(state, path)) {
    if (!should_allow_missing || path.exists())
      return history_scan_outcome::Stale;
  } else if (state.file_status.size != contents.length) {
    return history_scan_outcome::Stale;
  }
  state.can_rewrite =
      state.has_file_status && state.file_status.has_file_identity;
  state.is_file_bad = false;
  state.is_loaded = true;
  return history_scan_outcome::Loaded;
}

static fn load_no_editor_history(const Path &path, bool should_allow_missing)
    -> ErrorOr<Ok>
{
  let &state = get_no_editor_history_state();
  state.is_loaded = false;

  for (int attempt_index = 0;
       attempt_index < toiletline::HISTORY_RACE_ATTEMPT_COUNT; attempt_index++)
  {
    let const contents = path.read_entire_file();
    if (!contents.has_value()) {
      let const was_missing = os::last_system_error_is_missing_file();
      if (!should_allow_missing || !was_missing)
        return Error{os::last_system_error_message()};

      if (scan_no_editor_history(path, {}, None, true) ==
          history_scan_outcome::Loaded)
      {
        return Success;
      }

      continue;
    }

    let const outcome = scan_no_editor_history(path, contents->view());
    if (outcome == history_scan_outcome::Loaded) return Success;
    if (outcome == history_scan_outcome::Invalid)
      return Error{"the file contains invalid data"};
  }

  return Error{"the file kept changing"};
}

static fn ensure_no_editor_history_loaded(const Path &path,
                                          bool should_allow_missing)
    -> ErrorOr<Ok>
{
  let &state = get_no_editor_history_state();
  if (!state.is_loaded || state.loaded_path.view() != path.view())
    return load_no_editor_history(path, should_allow_missing);

  return Success;
}

template <class Match>
static fn find_no_editor_history_event(Allocator allocator,
                                       Maybe<usize> before_event_number,
                                       Match do_match)
    -> Maybe<toiletline::history_event>
{
  let const path = resolve_no_editor_history_path();
  if (!path.has_value()) return None;
  if (ensure_no_editor_history_loaded(*path, false).is_error()) return None;
  let &state = get_no_editor_history_state();
  let const retained_record_count = state.record_byte_offsets.count();
  let const first_number = state.total_count - retained_record_count + 1;
  let decoded = String{heap_allocator()};
  for (usize index = retained_record_count; index > 0; index--) {
    let const number = first_number + index - 1;
    if (before_event_number.has_value() && number >= *before_event_number)
      continue;
    let const span = get_history_record_span(state, index - 1);
    if (!decode_history_record(decoded, state.branch_contents.view(), span))
      return None;
    if (do_match(number, decoded.view())) {
      return toiletline::history_event{
          number, String{allocator, decoded.view()}
      };
    }
  }

  return None;
}

static fn rewrite_no_editor_history_event(usize wanted_number,
                                          StringView expected,
                                          const ArrayList<String> &replacements)
    -> bool
{
  let const path = resolve_no_editor_history_path();
  if (!path.has_value()) return false;
  let const parent = path->parent_or_current();
  let lock = os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return false;
  defer { os::release_process_lock(lock.take()); };
  let &state = get_no_editor_history_state();
  if (ensure_no_editor_history_loaded(*path, false).is_error()) return false;
  let const retained_record_count = state.record_byte_offsets.count();
  if (retained_record_count == 0) return false;
  let const first_number = state.total_count - retained_record_count + 1;
  if (wanted_number < first_number ||
      wanted_number >= first_number + retained_record_count)
  {
    return false;
  }

  let const retained_index = wanted_number - first_number;
  let const private_span = get_history_record_span(state, retained_index);
  let decoded = String{heap_allocator()};
  if (!decode_history_record(decoded, state.branch_contents.view(),
                             private_span) ||
      decoded.view() != expected)
  {
    return false;
  }

  let const durable_contents = path->read_entire_file();
  if (!durable_contents.has_value()) return false;
  let current_status = os::file_status{};
  if (!os::stat_path_following(path->text().view(), current_status))
    return false;
  if (!state.can_rewrite || !state.has_file_status ||
      !state.file_status.has_file_identity ||
      !current_status.has_file_identity ||
      state.file_status.device_id != current_status.device_id ||
      state.file_status.file_id != current_status.file_id)
  {
    return false;
  }

  usize durable_byte_offset =
      get_history_durable_record_byte_offset(state, retained_index);
  history_record_span durable_span{};
  bool is_valid = true;
  if (!next_history_record(durable_contents->view(), durable_byte_offset,
                           durable_span, is_valid) ||
      !is_valid)
  {
    return false;
  }
  let const durable_record = durable_contents->substring_of_length(
      durable_span.start_byte_offset,
      durable_span.end_byte_offset - durable_span.start_byte_offset);

  let expected_encoded = String{heap_allocator()};
  toiletline::encode_history_record(expected_encoded, expected);
  if (durable_record != expected_encoded.view()) {
    return false;
  }

  let encoded_replacements = String{heap_allocator()};
  let replacement_byte_offsets = ArrayList<usize>{heap_allocator()};
  for (let const &replacement : replacements) {
    if (replacement.count() > NO_EDITOR_HISTORY_ENTRY_MAX_BYTE_COUNT ||
        !toiletline::is_history_contents_valid(replacement.view()))
    {
      return false;
    }
    replacement_byte_offsets.push(encoded_replacements.count());
    toiletline::encode_history_record(encoded_replacements, replacement.view());
  }

  let durable_rewritten = String{heap_allocator()};
  durable_rewritten.append(
      durable_contents->substring_of_length(0, durable_span.start_byte_offset));
  durable_rewritten.append(encoded_replacements.view());
  durable_rewritten.append(
      durable_contents->substring(durable_span.end_byte_offset));

  let private_rewritten = String{heap_allocator()};
  private_rewritten.append(state.branch_contents.substring_of_length(
      0, private_span.start_byte_offset));
  private_rewritten.append(encoded_replacements.view());
  private_rewritten.append(
      state.branch_contents.substring(private_span.end_byte_offset));

  let private_offsets = ArrayList<usize>{heap_allocator()};
  let durable_offsets = ArrayList<usize>{heap_allocator()};
  let const private_removed_byte_count =
      private_span.end_byte_offset - private_span.start_byte_offset;
  let const durable_removed_byte_count =
      durable_span.end_byte_offset - durable_span.start_byte_offset;
  for (usize old_index = 0; old_index < retained_index; old_index++) {
    private_offsets.push(get_history_record_byte_offset(state, old_index));
    durable_offsets.push(
        get_history_durable_record_byte_offset(state, old_index));
  }
  for (let const replacement_byte_offset : replacement_byte_offsets) {
    private_offsets.push(private_span.start_byte_offset +
                         replacement_byte_offset);
    durable_offsets.push(durable_span.start_byte_offset +
                         replacement_byte_offset);
  }
  for (usize old_index = retained_index + 1; old_index < retained_record_count;
       old_index++)
  {
    private_offsets.push(get_history_record_byte_offset(state, old_index) -
                         private_removed_byte_count +
                         encoded_replacements.count());
    durable_offsets.push(
        get_history_durable_record_byte_offset(state, old_index) -
        durable_removed_byte_count + encoded_replacements.count());
  }

  let replacement_path = os::write_to_named_temp_file(
      parent, ".kosh_history_fc", durable_rewritten.view());
  if (!replacement_path.has_value()) return false;
  defer { unused(os::remove_file(replacement_path->text().view())); };
  let const current_contents = path->read_entire_file();
  if (!current_contents.has_value() ||
      current_contents->view() != durable_contents->view() ||
      !os::rename_path(replacement_path->text().view(), path->text().view()))
  {
    return false;
  }

  state.total_count = state.total_count - 1 + replacement_byte_offsets.count();
  let const retained_count = private_offsets.count() < state.entry_limit
                                 ? private_offsets.count()
                                 : state.entry_limit;
  let const first_retained_index = private_offsets.count() - retained_count;
  let retained_private_offsets = ArrayList<usize>{heap_allocator()};
  let retained_durable_offsets = ArrayList<usize>{heap_allocator()};
  retained_private_offsets.reserve(retained_count);
  retained_durable_offsets.reserve(retained_count);
  for (usize new_index = first_retained_index;
       new_index < private_offsets.count(); new_index++)
  {
    retained_private_offsets.push(private_offsets[new_index]);
    retained_durable_offsets.push(durable_offsets[new_index]);
  }
  state.branch_contents = steal(private_rewritten);
  state.record_byte_offsets = steal(retained_private_offsets);
  state.durable_record_byte_offsets = steal(retained_durable_offsets);
  state.first_record_index = 0;
  state.file_byte_count = state.branch_contents.count();
  usize trailing_byte_offset = 0;
  history_record_span trailing_span{};
  bool is_trailing_valid = true;
  while (next_history_record(state.branch_contents.view(), trailing_byte_offset,
                             trailing_span, is_trailing_valid))
  {}
  ASSERT(is_trailing_valid);
  state.trailing_record_start_byte_offset = trailing_span.start_byte_offset;
  state.file_contents_hash = extend_history_contents_hash(
      HISTORY_HASH_OFFSET_BASIS, state.branch_contents.view());
  state.is_file_bad = false;
  state.is_loaded = true;
  state.has_file_status = update_history_file_status(state, *path);
  state.can_rewrite =
      state.has_file_status && state.file_status.has_file_identity;
  return state.can_rewrite;
}

} /* namespace koshka::internal */

namespace toiletline {

using koshka::String;
using koshka::StringView;

fn enable_completion(koshka::EvalContext &context) -> void { unused(context); }

fn disable_completion() -> void {}

fn completion_is_enabled() -> bool { return false; }

fn set_space_after_completion(bool enabled) -> void { unused(enabled); }

fn enter_calc_history() -> void {}

fn leave_calc_history() -> void {}

fn get_history_path() -> koshka::Maybe<koshka::Path>
{
  return koshka::internal::resolve_no_editor_history_path();
}

/* Every event is appended to the file as it is stored. A write only has to
   drop the leading records the bounded list no longer reaches. */
fn history_write() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};

  let const parent = path->parent_or_current();
  let lock = koshka::os::acquire_process_lock(parent.view());
  if (!lock.has_value())
    return koshka::Error{koshka::os::last_system_error_message()};
  defer { koshka::os::release_process_lock(lock.take()); };

  let &state = koshka::internal::get_no_editor_history_state();
  TRY(koshka::internal::ensure_no_editor_history_loaded(*path, true));
  let written = String{koshka::heap_allocator()};
  let durable_offsets = koshka::ArrayList<usize>{koshka::heap_allocator()};
  durable_offsets.reserve(state.record_byte_offsets.count());
  for (usize index = 0; index < state.record_byte_offsets.count(); index++) {
    let const span = koshka::internal::get_history_record_span(state, index);
    durable_offsets.push(written.count());
    written.append(state.branch_contents.substring_of_length(
        span.start_byte_offset, span.end_byte_offset - span.start_byte_offset));
  }

  let const replacement_path = koshka::os::write_to_named_temp_file(
      parent, ".kosh_history_write", written.view());
  if (!replacement_path.has_value())
    return koshka::Error{koshka::os::last_system_error_message()};
  defer { unused(koshka::os::remove_file(replacement_path->text().view())); };
  if (!koshka::os::rename_path(replacement_path->text().view(),
                               path->text().view()))
  {
    return koshka::Error{koshka::os::last_system_error_message()};
  }

  for (usize index = 0; index < durable_offsets.count(); index++) {
    let const slot = (state.first_record_index + index) %
                     state.durable_record_byte_offsets.count();
    state.durable_record_byte_offsets[slot] = durable_offsets[index];
  }
  state.is_file_bad = false;
  state.has_file_status =
      koshka::internal::update_history_file_status(state, *path);
  state.can_rewrite =
      state.has_file_status && state.file_status.has_file_identity;
  if (!state.can_rewrite)
    return koshka::Error{"the file contains invalid data"};
  return koshka::Success;
}

fn history_read() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};

  return koshka::internal::load_no_editor_history(*path, false);
}

fn sync_history() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};
  let const parent = path->parent_or_current();
  let lock = koshka::os::acquire_process_lock(parent.view());
  if (!lock.has_value())
    return koshka::Error{koshka::os::last_system_error_message()};
  defer { koshka::os::release_process_lock(lock.take()); };

  return koshka::internal::load_no_editor_history(*path, true);
}

fn history_clear() -> koshka::ErrorOr<koshka::Ok>
{
  let const path = get_history_path();
  if (!path.has_value()) return koshka::Error{"the path is unavailable"};

  let const parent = path->parent_or_current();
  let lock = koshka::os::acquire_process_lock(parent.view());
  if (!lock.has_value())
    return koshka::Error{koshka::os::last_system_error_message()};
  defer { koshka::os::release_process_lock(lock.take()); };
  let opened = koshka::os::open_file_descriptor(
      path->text().view(), koshka::os::file_open_mode::Truncate);
  if (!opened.has_value())
    return koshka::Error{koshka::os::last_system_error_message()};
  if (!koshka::os::close_fd(opened.take()))
    return koshka::Error{koshka::os::last_system_error_message()};

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
  let const retained_record_count = state.record_byte_offsets.count();
  if (retained_record_count <= retained_limit && state.first_record_index == 0)
  {
    return;
  }

  let retained_offsets = koshka::ArrayList<usize>{koshka::heap_allocator()};
  let retained_durable_offsets =
      koshka::ArrayList<usize>{koshka::heap_allocator()};
  let const new_record_count = retained_record_count < retained_limit
                                   ? retained_record_count
                                   : retained_limit;
  retained_offsets.reserve(new_record_count);
  retained_durable_offsets.reserve(new_record_count);
  let const first_retained_index = retained_record_count - new_record_count;
  for (usize index = first_retained_index; index < retained_record_count;
       index++)
  {
    retained_offsets.push(
        koshka::internal::get_history_record_byte_offset(state, index));
    retained_durable_offsets.push(
        koshka::internal::get_history_durable_record_byte_offset(state, index));
  }

  state.record_byte_offsets = steal(retained_offsets);
  state.durable_record_byte_offsets = steal(retained_durable_offsets);
  state.first_record_index = 0;
}

fn get_newest_history_event_number() -> koshka::Maybe<usize>
{
  let const path = get_history_path();
  if (!path.has_value()) return koshka::None;
  if (koshka::internal::ensure_no_editor_history_loaded(*path, false)
          .is_error())
    return koshka::None;

  let const &state = koshka::internal::get_no_editor_history_state();
  if (state.record_byte_offsets.is_empty()) return koshka::None;

  return state.total_count;
}

fn get_history_events(koshka::Allocator allocator,
                      koshka::Maybe<usize> after_event_number)
    -> koshka::ErrorOr<koshka::ArrayList<history_event>>
{
  let events = koshka::ArrayList<history_event>{allocator};
  let const path = get_history_path();
  if (!path.has_value()) return steal(events);

  TRY(koshka::internal::ensure_no_editor_history_loaded(*path, true));
  let &state = koshka::internal::get_no_editor_history_state();
  let const retained_record_count = state.record_byte_offsets.count();
  let const first_number = state.total_count - retained_record_count + 1;
  usize first_index = 0;
  if (after_event_number.has_value() && *after_event_number >= first_number) {
    first_index = *after_event_number - first_number + 1;
  }

  events.reserve(retained_record_count - (first_index < retained_record_count
                                              ? first_index
                                              : retained_record_count));
  for (usize index = first_index; index < retained_record_count; index++) {
    let const span = koshka::internal::get_history_record_span(state, index);
    let command = String{allocator};
    if (!koshka::internal::decode_history_record(
            command, state.branch_contents.view(), span))
    {
      return koshka::Error{"the file contains invalid data"};
    }
    events.push(history_event{first_number + index, steal(command)});
  }

  return steal(events);
}

fn get_relative_history_event(koshka::Allocator allocator, usize distance,
                              koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  if (distance == 0) return koshka::None;
  usize remaining_event_count = distance;
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number,
      [&](usize, StringView) { return --remaining_event_count == 0; });
}

fn get_numbered_history_event(koshka::Allocator allocator, usize number,
                              koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number, [&](usize candidate_number, StringView) {
        return candidate_number == number;
      });
}

fn get_prefixed_history_event(koshka::Allocator allocator, StringView prefix,
                              koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number,
      [&](usize, StringView command) { return command.starts_with(prefix); });
}

fn get_containing_history_event(koshka::Allocator allocator, StringView text,
                                koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  return koshka::internal::find_no_editor_history_event(
      allocator, before_event_number, [&](usize, StringView command) {
        return command.find_substring(text).has_value();
      });
}

fn history_append_event(StringView command) -> koshka::Maybe<usize>
{
  if (command.is_empty() ||
      command.length > koshka::internal::NO_EDITOR_HISTORY_ENTRY_MAX_BYTE_COUNT)
  {
    return koshka::None;
  }
  let const path = get_history_path();
  if (!path.has_value()) return koshka::None;
  let const parent = path->parent_or_current();
  let lock = koshka::os::acquire_process_lock(parent.view());
  if (!lock.has_value()) return koshka::None;
  defer { koshka::os::release_process_lock(lock.take()); };
  if (koshka::internal::ensure_no_editor_history_loaded(*path, true).is_error())
    return koshka::None;

  let &state = koshka::internal::get_no_editor_history_state();
  if (state.is_file_bad) return koshka::None;
  if (state.entry_limit == 0) return state.total_count;
  let const first_rune = koshka::utils::decode_utf8(command, 0, 0xfffd);
  if (first_rune.length >= command.length ||
      !toiletline::is_history_contents_valid(command))
  {
    return koshka::None;
  }
  if (!state.record_byte_offsets.is_empty()) {
    let const newest_span = koshka::internal::get_history_record_span(
        state, state.record_byte_offsets.count() - 1);
    let newest = String{koshka::heap_allocator()};
    if (!koshka::internal::decode_history_record(
            newest, state.branch_contents.view(), newest_span))
    {
      return koshka::None;
    }

    if (newest.view() == command) return state.total_count;
  }

  let const previous_branch_byte_count = state.branch_contents.count();
  let const previous_file_status = state.file_status;
  let const had_previous_file_status = state.has_file_status;
  let const had_private_unterminated_record =
      state.trailing_record_start_byte_offset != previous_branch_byte_count;
  let record = String{koshka::heap_allocator()};
  encode_history_record(record, command);

  let private_payload = String{koshka::heap_allocator()};
  if (had_private_unterminated_record) private_payload.push('\n');
  private_payload.append(record.view());

  bool does_durable_file_need_separator = false;
  let const durable_byte_count =
      koshka::os::path_file_size(path->text().view());
  if (durable_byte_count.has_value() && *durable_byte_count > 0) {
    let readable = koshka::os::open_file_descriptor(
        path->text().view(), koshka::os::file_open_mode::Read);
    if (!readable.has_value()) return koshka::None;
    let const read_fd = readable.value();
    char last_byte = '\0';
    let const was_positioned = koshka::os::seek_descriptor_from_start(
        read_fd, *durable_byte_count - 1);
    let const read_byte_count =
        was_positioned ? koshka::os::read_fd(read_fd, &last_byte, 1)
                       : koshka::Maybe<usize>{};
    let const was_read = read_byte_count.has_value() && *read_byte_count == 1;
    let const was_closed = koshka::os::close_fd(read_fd);
    if (!was_read || !was_closed) return koshka::None;
    does_durable_file_need_separator = last_byte != '\n';
  } else if (!durable_byte_count.has_value() && path->exists()) {
    return koshka::None;
  }

  let durable_payload = String{koshka::heap_allocator()};
  if (does_durable_file_need_separator) durable_payload.push('\n');
  durable_payload.append(record.view());
  let opened = koshka::os::open_file_descriptor(
      path->text().view(), koshka::os::file_open_mode::Append);
  if (!opened.has_value()) return koshka::None;
  let const fd = opened.value();
  let const was_written = koshka::os::write_all(fd, durable_payload.data(),
                                                durable_payload.count());
  let const was_closed = koshka::os::close_fd(fd);
  if (!was_written || !was_closed) {
    state.is_file_bad = true;
    state.has_file_status = false;
    state.can_rewrite = false;
    return koshka::None;
  }
  if (had_private_unterminated_record) {
    koshka::internal::push_history_record_byte_offset(
        state, state.trailing_record_start_byte_offset,
        state.trailing_record_start_byte_offset);
    state.total_count++;
  }

  let const record_start_byte_offset =
      previous_branch_byte_count + (had_private_unterminated_record ? 1 : 0);
  koshka::internal::push_history_record_byte_offset(
      state, record_start_byte_offset,
      durable_byte_count.value_or(0) +
          (does_durable_file_need_separator ? 1 : 0));
  state.total_count++;
  let const event_number = state.total_count;
  state.branch_contents.append(private_payload.view());
  state.file_contents_hash = koshka::internal::extend_history_contents_hash(
      state.file_contents_hash, private_payload.view());
  state.file_byte_count = state.branch_contents.count();
  state.trailing_record_start_byte_offset = state.file_byte_count;
  let const has_appended_file_status =
      koshka::internal::update_history_file_status(state, *path);
  if (!has_appended_file_status || !state.file_status.has_file_identity) {
    state.can_rewrite = false;
  } else if (had_previous_file_status && previous_file_status.has_file_identity)
  {
    state.can_rewrite =
        state.can_rewrite &&
        previous_file_status.device_id == state.file_status.device_id &&
        previous_file_status.file_id == state.file_status.file_id;
  } else {
    state.can_rewrite = previous_branch_byte_count == 0;
  }

  return event_number;
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

fn set_colors_enabled(bool enabled) -> void { unused(enabled); }

fn set_edit_mode(edit_mode mode) -> void { unused(mode); }

fn set_tab_selector(koshka::tab_selector_mode selector) -> void
{
  unused(selector);
}

fn is_active() -> bool { return false; }

fn initialize() -> void
{
  throw koshka::Error{
      "This build has no line editor, use '-c', '-s', or a file argument"};
}

fn exit(usize history_size_limit) -> void { unused(history_size_limit); }

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
