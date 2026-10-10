/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements helpers that depend on stream or filesystem input. It
 * reads complete inputs and delimited lines, computes command and directory
 * suggestions, and derives Git branch and upstream status. It also parses the
 * octal and symbolic file modes that chmod, mkdir, mkfifo, and mknod apply,
 * with creation-mask filtering, copied permission classes, and special bits.
 */

#include "Builtin.hpp"
#include "CLI.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"
#include "base/Containers.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace utils {

constexpr usize OSA_ROW_WIDTH = 256;

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
  let const is_beyond_row_width = b_length + 1 > OSA_ROW_WIDTH;
  if (is_beyond_row_width) return max_distance + 1;

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

  let directory_names_unsorted = ArrayList<StringView>{heap_allocator()};
  for (let const &entry : *entries)
    if (directory_entry_kind(directory, entry) == Path::entry_kind::Directory)
      directory_names_unsorted.push(entry.name.view());
  let const directory_names =
      steal(directory_names_unsorted).make_sorted(sort_order::ascending);

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

fn read_line_from_fd(os::descriptor fd, char delimiter, u64 deadline_nanos,
                     Allocator allocator) throws -> read_line_result
{
  let result = read_line_result{};
  let line = String{allocator};
  bool has_read_any_byte = false;
  let const should_read_chunks = os::descriptor_is_seekable(fd);
  u8 buffer[65536];
  loop
  {
    if (deadline_nanos != 0) {
      let const now_nanos = os::monotonic_nanos();
      if (now_nanos >= deadline_nanos) {
        result.was_timed_out = true;
        break;
      }
      let const remaining_nanos_unsigned = deadline_nanos - now_nanos;
      let const remaining_nanos = static_cast<i64>(
          remaining_nanos_unsigned > static_cast<u64>(INT64_MAX)
              ? INT64_MAX
              : remaining_nanos_unsigned);
      let const readable = os::wait_for_fd_readable(fd, remaining_nanos);
      if (readable != 1) {
        if (readable == 0) result.was_timed_out = true;
        break;
      }
    }

    let const requested_count = should_read_chunks ? sizeof(buffer) : 1;
    let const read_count = os::read_fd(fd, buffer, requested_count);
    if (!read_count.has_value()) {
      result.did_read_fail = true;
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
        result.did_read_fail = true;
        return result;
      }
      result.was_delimiter_terminated = true;
      result.line = steal(line);
      return result;
    }
  }

  if (!has_read_any_byte) return result;

  result.line = steal(line);
  return result;
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
      m_was_line_terminated = true;
      return Result::Line;
    }
    if (m_is_at_end) {
      m_was_line_terminated = false;
      return m_line.is_empty() ? Result::End : Result::Line;
    }

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

static fn ceiling_directory_list(StringView ceiling_directories) throws
    -> ArrayList<String>
{
  let ceilings = ArrayList<String>{heap_allocator()};
  usize entry_start = 0;
  for (usize position = 0; position <= ceiling_directories.length; position++) {
    if (position < ceiling_directories.length &&
        ceiling_directories[position] != os::PATH_DELIMITER)
    {
      continue;
    }

    let const entry = ceiling_directories.substring_of_length(
        entry_start, position - entry_start);
    entry_start = position + 1;
    if (entry.is_empty()) continue;

    let const ceiling = Path{entry};
    if (!ceiling.is_absolute()) continue;

    ceilings.push(String{ceiling.normalized().text()});
  }

  return ceilings;
}

static pure fn is_same_directory_text(StringView left,
                                      StringView right) wontthrow -> bool
{
  if (left.length != right.length) return false;

  for (usize index = 0; index < left.length; index++) {
    let left_byte = left[index];
    let right_byte = right[index];
    if (os::is_directory_separator(left_byte) &&
        os::is_directory_separator(right_byte))
    {
      continue;
    }
    if (!os::FILESYSTEM_IS_CASE_SENSITIVE) {
      left_byte = ascii_to_lower(left_byte);
      right_byte = ascii_to_lower(right_byte);
    }
    if (left_byte != right_byte) return false;
  }

  return true;
}

fn resolve_git_directory(StringView ceiling_directories) throws -> Path
{
  let const ceilings = ceiling_directory_list(ceiling_directories);
  let dir = Path::current_directory();
  loop
  {
    let head = dir.clone();
    head.append(".git");
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
          resolved_gitdir.append(line);
        }
        git_dir = steal(resolved_gitdir);
      }
    }
    if (git_dir.is_directory()) return git_dir;

    let parent = dir.clone();
    parent.append("..");
    let normalized = parent.to_absolute().normalized();
    if (normalized.text() == dir.text()) break;

    let is_below_ceiling = false;
    for (let const &ceiling : ceilings) {
      if (is_same_directory_text(ceiling.view(), normalized.text().view())) {
        is_below_ceiling = true;
        break;
      }
    }

    if (is_below_ceiling) {
      LOG(Debug, "the git directory walk stops below the ceiling '%.*s'",
          static_cast<int>(normalized.text().view().length),
          normalized.text().view().data);
      break;
    }

    dir = steal(normalized);
  }
  return Path{StringView{}};
}

fn current_git_branch(StringView ceiling_directories) throws -> String
{
  let const git_dir = resolve_git_directory(ceiling_directories);
  if (git_dir.text().is_empty()) return String{heap_allocator()};

  let git_head = git_dir.clone();
  git_head.append("HEAD");
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
  ref_path.append(ref_name);
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
  packed_path.append("packed-refs");
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
  config_path.append("config");
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

fn git_status(StringView ceiling_directories, Allocator allocator) throws
    -> git_status_result
{
  let result = git_status_result{allocator};

  let const git_dir = resolve_git_directory(ceiling_directories);
  if (git_dir.text().is_empty()) return result;

  let git_head = git_dir.clone();
  git_head.append("HEAD");
  let const head_content = git_head.read_entire_file();
  if (!head_content.has_value()) return result;

  let head_text = head_content->view();
  while (!head_text.is_empty() && (head_text[head_text.length - 1] == '\n' ||
                                   head_text[head_text.length - 1] == '\r'))
  {
    head_text = head_text.substring_of_length(0, head_text.length - 1);
  }
  let const ref_prefix = StringView{"ref: refs/heads/"};
  result.branch = head_text.starts_with(ref_prefix)
                      ? String{head_text.substring(ref_prefix.length)}
                      : String{head_text.substring_of_length(
                            0, head_text.length < 7 ? head_text.length : 7)};
  if (result.branch.is_empty()) return result;

  let const local_ref = StringView{"refs/heads/"} + result.branch.view();
  let const local_sha = read_git_ref_sha(git_dir, local_ref.view());
  if (local_sha.is_empty()) return result;

  let const upstream = git_upstream_ref(git_dir, result.branch.view());
  if (upstream.is_empty()) return result;

  let const upstream_sha = read_git_ref_sha(git_dir, upstream.view());
  if (upstream_sha.is_empty()) return result;

  if (local_sha == upstream_sha) return result;

  let const path_env = os::get_environment_variable("PATH");
  if (!path_env.has_value()) return result;
  let path_resolver = ProgramResolver{*path_env};
  let const git_results =
      path_resolver.search("git", ProgramResolver::SearchMode::First,
                           ProgramResolver::Requirement::Regular,
                           ProgramResolver::CachePolicy::Bypass);
  if (git_results.is_empty()) return result;
  let const git_path = String{heap_allocator(), git_results[0].view()};

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
      result.ahead_count = do_parse_count(
          count_output->view().substring_of_length(0, *separator));
      result.behind_count =
          do_parse_count(count_output->view().substring(*separator + 1));
    }
  }

  return result;
}

fn parse_file_mode(StringView expression, u32 current_mode, u32 creation_mask,
                   file_kind_mode kind) wontthrow -> Maybe<u32>
{
  if (expression.is_empty()) return None;

  bool is_octal = expression.length <= 4;
  u32 octal_mode = 0;
  for (usize position = 0; position < expression.length; position++) {
    let const byte = expression[position];
    if (byte < '0' || byte > '7') {
      is_octal = false;
      break;
    }
    octal_mode = octal_mode * 8 + static_cast<u32>(byte - '0');
  }
  if (is_octal) return (current_mode & ~07777u) | octal_mode;

  u32 mode = current_mode;
  usize position = 0;
  while (position < expression.length) {
    u32 selected_classes = 0;
    bool has_explicit_class = false;
    while (position < expression.length) {
      let const byte = expression[position];
      if (byte == 'u')
        selected_classes |= 0700;
      else if (byte == 'g')
        selected_classes |= 0070;
      else if (byte == 'o')
        selected_classes |= 0007;
      else if (byte == 'a')
        selected_classes |= 0777;
      else
        break;
      has_explicit_class = true;
      position++;
    }
    if (!has_explicit_class) selected_classes = 0777;
    if (position == expression.length) return None;

    let const operation = expression[position++];
    if (operation != '+' && operation != '-' && operation != '=') return None;

    u32 requested_bits = 0;
    u32 requested_special_bits = 0;
    while (position < expression.length && expression[position] != ',') {
      let const byte = expression[position++];
      switch (byte) {
      case 'r': requested_bits |= 0444; break;
      case 'w': requested_bits |= 0222; break;
      case 'x': requested_bits |= 0111; break;
      case 'X':
        if (kind == file_kind_mode::Directory || (mode & 0111) != 0)
          requested_bits |= 0111;
        break;
      case 's':
        if ((selected_classes & 0700) != 0) requested_special_bits |= 04000;
        if ((selected_classes & 0070) != 0) requested_special_bits |= 02000;
        break;
      case 't': requested_special_bits |= 01000; break;
      case 'u': {
        let const source = (mode >> 6) & 7;
        if ((selected_classes & 0700) != 0) requested_bits |= source << 6;
        if ((selected_classes & 0070) != 0) requested_bits |= source << 3;
        if ((selected_classes & 0007) != 0) requested_bits |= source;
        break;
      }
      case 'g': {
        let const source = (mode >> 3) & 7;
        if ((selected_classes & 0700) != 0) requested_bits |= source << 6;
        if ((selected_classes & 0070) != 0) requested_bits |= source << 3;
        if ((selected_classes & 0007) != 0) requested_bits |= source;
        break;
      }
      case 'o': {
        let const source = mode & 7;
        if ((selected_classes & 0700) != 0) requested_bits |= source << 6;
        if ((selected_classes & 0070) != 0) requested_bits |= source << 3;
        if ((selected_classes & 0007) != 0) requested_bits |= source;
        break;
      }
      default: return None;
      }
    }

    u32 affected_bits = selected_classes;
    if (!has_explicit_class) affected_bits &= ~creation_mask;
    requested_bits &= affected_bits;
    u32 affected_special_bits = 0;
    if ((selected_classes & 0700) != 0) affected_special_bits |= 04000;
    if ((selected_classes & 0070) != 0) affected_special_bits |= 02000;
    if ((selected_classes & 0007) != 0) affected_special_bits |= 01000;

    if (operation == '=') mode &= ~(affected_bits | affected_special_bits);
    if (operation == '-')
      mode &= ~(requested_bits | requested_special_bits);
    else
      mode |= requested_bits | requested_special_bits;

    if (position == expression.length) break;
    position++;
    if (position == expression.length) return None;
  }

  return mode;
}

} /* namespace utils */

} /* namespace koshka */
