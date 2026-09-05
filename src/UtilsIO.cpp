/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements io execution helpers. It provides shared low-level
 * operations for commands, streams, numbers, globbing, and resolution
 * without duplicating policy owners.
 */

#include "Builtin.hpp"
#include "Cli.hpp"
#include "Containers.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace utils {

/* The rolling distance rows are fixed-width stack arrays, so a candidate name
   longer than this is treated as too far rather than indexed past the row. */
constexpr usize OSA_ROW_WIDTH = 256;

/* The optimal-string-alignment distance, the edit distance that also counts an
   adjacent transposition as one edit, so a typo such as gti for git scores one
   rather than two. Bounded by max_distance, returning max_distance + 1 once the
   best possible result on the current row already exceeds it, so a far-off
   candidate costs little. */
pure fn bounded_osa_distance(StringView a, StringView b,
                             usize max_distance) wontthrow -> usize
{
  const usize a_length = a.length;
  const usize b_length = b.length;
  if (a_length > b_length ? a_length - b_length > max_distance
                          : b_length - a_length > max_distance)
    return max_distance + 1;
  if (a_length == 0) return b_length;
  if (b_length == 0) return a_length;
  /* The rolling rows are indexed up to b_length, so a candidate longer than the
     row width is rejected before the rows are reserved. */
  if (b_length + 1 > OSA_ROW_WIDTH) return max_distance + 1;

  usize rows[3][OSA_ROW_WIDTH];
  let previous_previous = rows[0];
  let previous = rows[1];
  let current = rows[2];

  for (usize j = 0; j <= b_length; j++)
    previous[j] = j;
  for (usize i = 1; i <= a_length; i++) {
    current[0] = i;
    usize row_best = current[0];
    for (usize j = 1; j <= b_length; j++) {
      const usize cost = a[i - 1] == b[j - 1] ? 0 : 1;
      usize value = previous[j] + 1;
      if (current[j - 1] + 1 < value) value = current[j - 1] + 1;
      if (previous[j - 1] + cost < value) value = previous[j - 1] + cost;
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
          previous_previous[j - 2] + 1 < value)
      {
        value = previous_previous[j - 2] + 1;
      }
      current[j] = value;
      if (value < row_best) row_best = value;
    }
    if (row_best > max_distance) return max_distance + 1;
    let old_previous_previous = previous_previous;
    previous_previous = previous;
    previous = current;
    current = old_previous_previous;
  }
  return previous[b_length];
}

pure fn suggestion_distance_budget(usize name_length) wontthrow -> usize
{
  return name_length <= 3 ? 1 : 2;
}

fn suggest_command(StringView name, const ArrayList<String> &local_names,
                   const ProgramResolver *resolver) throws -> Maybe<String>
{
  if (name.is_empty()) return None;

  let suggestion = NameSuggestion{name};

  for (let const &local : local_names)
    suggestion.consider(local.view());
  for (let const &builtin : builtin_names())
    suggestion.consider(builtin.view());
  if (resolver != nullptr && resolver->has_valid_command_names()) {
    resolver->for_each_command_name(
        [&](const String &entry) { suggestion.consider(entry.view()); });
  }

  return suggestion.take_suggestion();
}

fn suggest_directory_entry(const Path &directory, StringView name) throws
    -> Maybe<String>
{
  if (name.is_empty()) return None;

  let const entries = read_directory_cached(directory);
  if (entries == nullptr) return None;

  let directory_names = ArrayList<StringView>{heap_allocator()};
  for (let const &entry : *entries)
    if (directory_entry_kind(directory, entry) == Path::entry_kind::Directory)
      directory_names.push(entry.name.view());
  directory_names.sort();

  let suggestion = NameSuggestion{name};
  for (let const directory_name : directory_names)
    suggestion.consider(directory_name);
  return suggestion.take_suggestion();
}

fn read_entire_standard_input() throws -> String
{
  let contents = os::read_fd_to_string(KOSH_STDIN, heap_allocator());
  if (!contents.has_value())
    throw Error{"Unable to read standard input: " +
                os::last_system_error_message()};
  return steal(*contents);
}

fn read_line_from_fd(os::descriptor fd, bool &was_delimiter_terminated,
                     char delimiter, u64 deadline_nanos, bool *was_timed_out,
                     Allocator allocator, bool *did_read_fail) throws
    -> Maybe<String>
{
  if (did_read_fail != nullptr) *did_read_fail = false;
  let line = String{allocator};
  bool has_read_any_byte = false;
  let const should_read_chunks = os::descriptor_is_seekable(fd);
  u8 buffer[65536];
  loop
  {
    if (deadline_nanos != 0) {
      let const now_nanos = os::monotonic_nanos();
      if (now_nanos >= deadline_nanos) {
        if (was_timed_out != nullptr) *was_timed_out = true;
        break;
      }
      let const remaining_nanos_unsigned = deadline_nanos - now_nanos;
      let const remaining_nanos = static_cast<i64>(
          remaining_nanos_unsigned > static_cast<u64>(INT64_MAX)
              ? INT64_MAX
              : remaining_nanos_unsigned);
      let const readable = os::wait_for_fd_readable(fd, remaining_nanos);
      if (readable != 1) {
        if (readable == 0 && was_timed_out != nullptr) *was_timed_out = true;
        break;
      }
    }

    let const requested_count = should_read_chunks ? sizeof(buffer) : 1;
    let const read_count = os::read_fd(fd, buffer, requested_count);
    if (!read_count.has_value()) {
      if (did_read_fail != nullptr) *did_read_fail = true;
      break;
    }
    if (*read_count == 0) break;
    has_read_any_byte = true;

    usize delimiter_position = 0;
    while (delimiter_position < *read_count &&
           buffer[delimiter_position] != static_cast<u8>(delimiter))
    {
      delimiter_position++;
    }
    line.append(
        StringView{reinterpret_cast<const char *>(buffer), delimiter_position});
    if (delimiter_position < *read_count) {
      let const unread_count = *read_count - delimiter_position - 1;
      if (unread_count > 0 && !os::rewind_descriptor(fd, unread_count)) {
        if (did_read_fail != nullptr) *did_read_fail = true;
        was_delimiter_terminated = false;
        return None;
      }
      was_delimiter_terminated = true;
      return line;
    }
  }

  /* The loop fell out at end of input, so no delimiter ended the line. The read
     builtin maps an unterminated final line to a non-zero status while still
     assigning the bytes it read, the way dash does. */
  was_delimiter_terminated = false;

  if (!has_read_any_byte) return None;

  return line;
}

BufferedLineReader::BufferedLineReader(os::descriptor descriptor)
    : m_descriptor(descriptor)
{}

hot flatten fn BufferedLineReader::next() throws -> Result
{
  m_line.clear();

  loop
  {
    usize delimiter_position = m_buffer_position;
    while (delimiter_position < m_buffer_length &&
           m_buffer[delimiter_position] != '\n')
    {
      delimiter_position++;
    }

    m_line.append(StringView{m_buffer + m_buffer_position,
                             delimiter_position - m_buffer_position});
    m_buffer_position = delimiter_position;
    if (m_buffer_position < m_buffer_length) {
      m_buffer_position++;
      return Result::Line;
    }
    if (m_is_at_end) return m_line.is_empty() ? Result::End : Result::Line;

    let const read_size = os::read_fd(m_descriptor, m_buffer, sizeof(m_buffer));
    if (!read_size.has_value()) return Result::Error;

    m_buffer_position = 0;
    m_buffer_length = *read_size;
    m_is_at_end = m_buffer_length == 0;
  }
}

pure fn BufferedLineReader::get_line() const wontthrow -> StringView
{
  return m_line.view();
}

fn resolve_git_directory() throws -> Path
{
  let dir = Path::current_directory();
  loop
  {
    let head = dir.clone();
    head.push_component(".git");
    let git_dir = head.clone();
    if (let const dot_git = head.read_entire_file()) {
      let const pointer = dot_git->view();
      let const gitdir_prefix = StringView{"gitdir: "};
      if (pointer.starts_with(gitdir_prefix)) {
        let line = pointer.substring(gitdir_prefix.length);
        while (!line.is_empty() &&
               (line[line.length - 1] == '\n' || line[line.length - 1] == '\r'))
        {
          line = line.substring_of_length(0, line.length - 1);
        }
        let resolved_gitdir = Path{line};
        if (!resolved_gitdir.is_absolute()) {
          resolved_gitdir = dir;
          resolved_gitdir.push_component(line);
        }
        git_dir = steal(resolved_gitdir);
      }
    }
    if (git_dir.is_directory()) return git_dir;

    let parent = dir.clone();
    parent.push_component("..");
    let normalized = parent.to_absolute().normalized();
    if (normalized.text() == dir.text()) break;
    dir = steal(normalized);
  }
  return Path{StringView{}};
}

fn current_git_branch() throws -> String
{
  let const git_dir = resolve_git_directory();
  if (git_dir.text().is_empty()) return String{heap_allocator()};

  let git_head = git_dir.clone();
  git_head.push_component("HEAD");
  if (let const content = git_head.read_entire_file()) {
    let text = content->view();
    while (!text.is_empty() &&
           (text[text.length - 1] == '\n' || text[text.length - 1] == '\r'))
    {
      text = text.substring_of_length(0, text.length - 1);
    }
    let const ref_prefix = StringView{"ref: refs/heads/"};
    if (text.starts_with(ref_prefix))
      return String{text.substring(ref_prefix.length)};
    return String{
        text.substring_of_length(0, text.length < 7 ? text.length : 7)};
  }
  return String{heap_allocator()};
}

fn read_git_ref_sha(const Path &git_dir, StringView ref_name) throws -> String
{
  let ref_path = git_dir.clone();
  ref_path.push_component(ref_name);
  if (let const content = ref_path.read_entire_file()) {
    let text = content->view();
    while (!text.is_empty() &&
           (text[text.length - 1] == '\n' || text[text.length - 1] == '\r'))
    {
      text = text.substring_of_length(0, text.length - 1);
    }
    if (!text.is_empty()) return String{text};
  }

  let packed_path = git_dir.clone();
  packed_path.push_component("packed-refs");
  if (let const packed = packed_path.read_entire_file()) {
    let remainder = packed->view();
    while (!remainder.is_empty()) {
      let const newline_pos = remainder.find_character('\n');
      let line = newline_pos.has_value()
                     ? remainder.substring_of_length(0, *newline_pos)
                     : remainder;
      if (!line.is_empty() && line[line.length - 1] == '\r')
        line = line.substring_of_length(0, line.length - 1);
      if (!line.is_empty() && line[0] != '#' && line[0] != '^') {
        if (let const separator = line.find_character(' ');
            separator.has_value() && line.substring(*separator + 1) == ref_name)
          return String{line.substring_of_length(0, *separator)};
      }
      if (!newline_pos.has_value()) break;
      remainder = remainder.substring(*newline_pos + 1);
    }
  }
  return String{heap_allocator()};
}

fn git_upstream_ref(const Path &git_dir, StringView branch_name) throws
    -> String
{
  let config_path = git_dir.clone();
  config_path.push_component("config");
  if (!config_path.exists()) return String{heap_allocator()};

  let const content = config_path.read_entire_file();
  if (!content.has_value()) return String{heap_allocator()};

  let const section_header =
      StringView{"[branch \""} + branch_name + StringView{"\"]"};
  let remote_name = String{heap_allocator()};
  let merge_ref = String{heap_allocator()};
  let in_section = false;

  let remainder = content->view();
  while (!remainder.is_empty()) {
    let const newline_pos = remainder.find_character('\n');
    let line = newline_pos.has_value()
                   ? remainder.substring_of_length(0, *newline_pos)
                   : remainder;

    while (!line.is_empty() && (line[0] == ' ' || line[0] == '\t'))
      line = line.substring(1);
    while (!line.is_empty() &&
           (line[line.length - 1] == ' ' || line[line.length - 1] == '\r'))
    {
      line = line.substring_of_length(0, line.length - 1);
    }

    if (line.starts_with("[")) {
      in_section = line == section_header;
    } else if (in_section) {
      let const eq_pos = line.find_character('=');
      if (!eq_pos.has_value()) {
        if (!newline_pos.has_value()) break;
        remainder = remainder.substring(*newline_pos + 1);
        continue;
      }
      let key = line.substring_of_length(0, *eq_pos);
      while (!key.is_empty() && key[key.length - 1] == ' ')
        key = key.substring_of_length(0, key.length - 1);
      let value = line.substring(*eq_pos + 1);
      while (!value.is_empty() && (value[0] == ' ' || value[0] == '\t'))
        value = value.substring(1);

      if (key == "remote") remote_name = String{value};
      if (key == "merge") merge_ref = String{value};
    }

    if (!newline_pos.has_value()) break;
    remainder = remainder.substring(*newline_pos + 1);
  }

  if (remote_name.is_empty() || merge_ref.is_empty()) {
    return String{heap_allocator()};
  }
  if (remote_name == ".") return merge_ref;

  let const refs_prefix = StringView{"refs/"};
  let const remotes_prefix = StringView{"refs/remotes/"};
  if (merge_ref.starts_with(refs_prefix)) {
    let short_ref = merge_ref.view().substring(refs_prefix.length);
    if (short_ref.starts_with("heads/")) short_ref = short_ref.substring(6);
    let result = String{heap_allocator()};
    result += remotes_prefix;
    result += remote_name.view();
    result += "/";
    result += short_ref;
    return result;
  }

  let result = String{heap_allocator()};
  result += remotes_prefix;
  result += remote_name.view();
  result += "/";
  result += merge_ref.view();
  return result;
}

fn git_ahead_behind_counts(i32 &ahead_count, i32 &behind_count) throws -> void
{
  let branch = String{heap_allocator()};
  git_status(branch, ahead_count, behind_count);
}

fn git_status(String &branch, i32 &ahead_count, i32 &behind_count) throws
    -> void
{
  branch.clear();
  ahead_count = 0;
  behind_count = 0;

  let const git_dir = resolve_git_directory();
  if (git_dir.text().is_empty()) return;

  let git_head = git_dir.clone();
  git_head.push_component("HEAD");
  let const head_content = git_head.read_entire_file();
  if (!head_content.has_value()) return;

  let head_text = head_content->view();
  while (!head_text.is_empty() && (head_text[head_text.length - 1] == '\n' ||
                                   head_text[head_text.length - 1] == '\r'))
  {
    head_text = head_text.substring_of_length(0, head_text.length - 1);
  }
  let const ref_prefix = StringView{"ref: refs/heads/"};
  branch = head_text.starts_with(ref_prefix)
               ? String{head_text.substring(ref_prefix.length)}
               : String{head_text.substring_of_length(
                     0, head_text.length < 7 ? head_text.length : 7)};
  if (branch.is_empty()) return;

  let const local_ref = StringView{"refs/heads/"} + branch.view();
  let const local_sha = read_git_ref_sha(git_dir, local_ref.view());
  if (local_sha.is_empty()) return;

  let const upstream = git_upstream_ref(git_dir, branch.view());
  if (upstream.is_empty()) return;

  let const upstream_sha = read_git_ref_sha(git_dir, upstream.view());
  if (upstream_sha.is_empty()) return;

  ahead_count = 0;
  behind_count = 0;

  if (local_sha == upstream_sha) return;

  let const path_env = os::get_environment_variable("PATH");
  if (!path_env.has_value()) return;
  let path_resolver = ProgramResolver{*path_env};
  let const git_results =
      path_resolver.search("git", ProgramResolver::SearchMode::First,
                           ProgramResolver::Requirement::Regular,
                           ProgramResolver::CachePolicy::Bypass);
  if (git_results.is_empty()) return;
  let const git_path = String{heap_allocator(), git_results[0].text().view()};

  let count_argv = ArrayList<String>{heap_allocator()};
  count_argv.push(String{heap_allocator(), git_path.view()});
  count_argv.push(String{heap_allocator(), "rev-list"});
  count_argv.push(String{heap_allocator(), "--left-right"});
  count_argv.push(String{heap_allocator(), "--count"});
  count_argv.push(local_sha.view() + "..." + upstream_sha.view());

  let const count_output =
      os::capture_program_output(count_argv, 5'000'000'000);

  let const do_parse_count = [](StringView text) throws -> i32 {
    usize position = 0;
    let const word = text.next_ascii_whitespace_word(position);
    let const parsed = parse_decimal_u64(word);
    if (parsed.is_error() || parsed.value() > INT32_MAX) return 0;

    return static_cast<i32>(parsed.value());
  };

  if (count_output.has_value()) {
    let const separator = count_output->view().find_character('\t');
    if (separator.has_value()) {
      ahead_count = do_parse_count(
          count_output->view().substring_of_length(0, *separator));
      behind_count =
          do_parse_count(count_output->view().substring(*separator + 1));
    }
  }
}

} /* namespace utils */

} /* namespace koshka */
