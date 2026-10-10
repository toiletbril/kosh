/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the helper interface implemented across the Utils
 * sources. It covers word decoding, command execution, stream input, glob
 * matching, numeric conversion, source positions, directory caches, and
 * command resolution.
 */

#pragma once

#include "Builtin.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "base/Common.hpp"
#include "base/ErrorOr.hpp"
#include "base/Path.hpp"

namespace koshka {

namespace utils {

using extglob_mode = koshka::extglob_mode;
using glob_charset = koshka::glob_charset;

struct opaque_shell_word_range
{
  usize decoded_start;
  usize decoded_length;
  usize raw_start;
  usize raw_length;
};

struct leading_expansion
{
  bool is_tilde_active{false};
  bool is_variable_active{false};
  usize variable_end{0};
};

struct decoded_shell_word
{
  String text;
  Bitset glob_active;
  leading_expansion leading;
  ArrayList<usize> raw_positions;
  ArrayList<opaque_shell_word_range> opaque_ranges;
  usize raw_directory_end{0};
  usize open_quote_content_start{0};
  usize open_quote_decoded_start{0};
  usize last_quote_content_start{0};
  usize last_quote_decoded_start{0};
  char quote_character{0};
  char last_quote_character{0};
  bool is_last_quote_ansi_c{false};
  bool has_shell_syntax{false};

  explicit decoded_shell_word(Allocator allocator)
      : text(allocator), glob_active(allocator), raw_positions(allocator),
        opaque_ranges(allocator)
  {}
};

enum class shell_word_source_mapping : u8
{
  Omit,
  Record,
};

fn decode_shell_word(
    StringView word, Allocator allocator,
    shell_word_source_mapping mapping = shell_word_source_mapping::Omit) throws
    -> decoded_shell_word;

struct unavailable_path_source_component
{
  Path prefix;
  SourceLocation location;
  String reported_prefix;
  String typed_prefix;
  usize typed_component_start;
  bool is_not_directory;
  bool has_single_raw_component;
  bool is_final_component;
};

fn locate_first_unavailable_path_component(const Path &target,
                                           StringView expanded_operand,
                                           StringView raw_operand,
                                           SourceLocation operand_location,
                                           Allocator allocator) throws
    -> Maybe<unavailable_path_source_component>;

fn merge_tokens_to_string(const ArrayList<const Token *> &tokens) throws
    -> String;

inline fn merge_args_to_string(const ArrayList<String> &args) throws -> String
{
  let result = String{heap_allocator()};
  for (usize i = 0; i < args.count(); i++) {
    result.append(args[i].view());
    if (i + 1 < args.count()) {
      result.push(' ');
    }
  }
  return result;
}

template <class GetName>
fn append_name_columns(String &output, usize name_count,
                       GetName do_get_name) throws -> void
{
  usize longest_length = 0;
  for (usize index = 0; index < name_count; index++) {
    let const name = do_get_name(index);
    if (name.length > longest_length) longest_length = name.length;
  }
  let const column_width = longest_length + 2;
  let const column_count = column_width >= 78 ? usize{1} : 78 / column_width;

  for (usize index = 0; index < name_count; index++) {
    let const name = do_get_name(index);
    if (index % column_count == 0) output += "  ";
    output += name;
    let const is_last_in_row =
        index % column_count == column_count - 1 || index + 1 == name_count;
    if (is_last_in_row) {
      output += "\n";
    } else {
      for (usize pad = name.length; pad < column_width; pad++)
        output += " ";
    }
  }
}

fn expand_leading_tilde_path(StringView name) throws -> Maybe<String>;

fn should_ansi_c_quote(StringView text, bool is_utf8_locale) throws -> bool;

fn append_ansi_c_quoted(String &out, StringView text,
                        bool is_utf8_locale) throws -> void;

fn append_ansi_c_quote_if_needed(String &out, StringView arg,
                                 bool is_utf8_locale) throws -> bool;

fn append_shell_quoted(String &out, StringView arg, bool is_utf8_locale) throws
    -> void;

fn decode_ansi_c_escapes(String &out, StringView body) throws -> void;

fn set_foreground_program_title(const ArrayList<String> &arguments,
                                EvalContext &cxt) throws -> void;

fn execute_context(ExecContext &&ec, EvalContext &cxt,
                   execution_mode mode) throws -> i32;

fn execute_contexts_with_pipes(ArrayList<ExecContext> &&ecs, EvalContext &cxt,
                               execution_mode mode) throws -> i32;
fn terminate_and_reap_processes(const ArrayList<os::process> &processes,
                                usize first_process_position = 0) wontthrow
    -> void;

pure fn strip_sig_prefix(StringView name) wontthrow -> StringView;

struct signal_pair
{
  i32 number;
  StringView name;
};

fn find_signal_number(const signal_pair *pairs, usize pair_count,
                      StringView name) throws -> Maybe<i32>;
fn find_signal_name(const signal_pair *pairs, usize pair_count,
                    i32 number) throws -> Maybe<String>;
fn collect_signal_names(const signal_pair *pairs, usize pair_count) throws
    -> ArrayList<StringView>;

pure alwaysinline fn ascii_to_lower(char ch) wontthrow -> char
{
  if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
  return ch;
}

pure fn contains_case_insensitive_ascii(StringView value,
                                        StringView folded_pattern) wontthrow
    -> bool;

struct text_position_range
{
  usize first;
  usize last;

  pure fn operator<(const text_position_range &other) const wontthrow->bool
  {
    if (first != other.first) return first < other.first;
    return last < other.last;
  }
};

fn parse_text_position_ranges(StringView text, Allocator allocator) throws
    -> Maybe<SortedArrayList<text_position_range,
                             order_comparator<text_position_range>>>;
pure fn text_position_is_selected(
    usize one_based_position,
    const SortedArrayList<text_position_range,
                          order_comparator<text_position_range>> &ranges)
    wontthrow -> bool;

fn parse_tab_stop_list(StringView text, Allocator allocator) throws
    -> Maybe<ArrayList<usize>>;
pure fn get_next_tab_column(usize column,
                            const ArrayList<usize> &tab_stops) wontthrow
    -> usize;

pure alwaysinline fn environment_name_is_path(StringView name) wontthrow -> bool
{
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) return name == "PATH";

  return name.length == 4 && ascii_to_lower(name[0]) == 'p' &&
         ascii_to_lower(name[1]) == 'a' && ascii_to_lower(name[2]) == 't' &&
         ascii_to_lower(name[3]) == 'h';
}

pure alwaysinline fn hex_digit_value(char byte) wontthrow -> Maybe<u8>
{
  if (byte >= '0' && byte <= '9') return static_cast<u8>(byte - '0');
  if (byte >= 'a' && byte <= 'f') return static_cast<u8>(byte - 'a' + 10);
  if (byte >= 'A' && byte <= 'F') return static_cast<u8>(byte - 'A' + 10);

  return None;
}

pure fn token_has_uppercase(StringView token) wontthrow -> bool;
pure fn smart_case_prefix_matches(StringView candidate, StringView prefix,
                                  bool is_prefix_case_sensitive) wontthrow
    -> bool;
pure fn smart_case_prefix_matches(StringView candidate,
                                  StringView prefix) wontthrow -> bool;

struct decoded_codepoint
{
  u32 value;
  usize length;
};

pure fn decode_utf8(StringView source, usize position,
                    u32 invalid_codepoint) wontthrow -> decoded_codepoint;
fn append_utf8(String &output, u32 codepoint) throws -> void;

enum class line_terminator_mode : u8
{
  Discard,
  Preserve,
};

fn split_lines(StringView text, Allocator allocator = heap_allocator(),
               line_terminator_mode terminators = line_terminator_mode::Discard)
    throws -> ArrayList<StringView>;

fn encode_base64(StringView bytes) throws -> String;
fn decode_base64(StringView text) throws -> Maybe<String>;

fn format_unix_timestamp(i64 unix_time, const char *format) throws -> String;

pure fn is_posix_reserved_word(StringView word) wontthrow -> bool;

fn parse_decimal_i64(StringView text, bool *out_of_range = nullptr) throws
    -> ErrorOr<i64>;
fn parse_decimal_u64(StringView text) throws -> ErrorOr<u64>;

fn parse_decimal_f64(const String &text) throws -> ErrorOr<f64>;

fn format_f64(f64 value, Allocator allocator) throws -> String;

fn parse_timeout_seconds_to_nanos(StringView text) throws -> ErrorOr<i64>;

fn int_to_text_into(i64 value, char *buffer, usize buffer_size) wontthrow
    -> StringView;
fn uint_to_text_into(u64 value, char *buffer, usize buffer_size) wontthrow
    -> StringView;

fn format_minutes_seconds(double seconds, i32 decimal_count = 3) throws
    -> String;
fn format_duration_nanoseconds(u64 nanoseconds, Allocator allocator) throws
    -> String;

enum class time_report_layout : u8
{
  Rich,
  Bash,
  Posix,
};

enum class time_report_rss : u8
{
  Omit,
  Include,
};

fn format_time_report(const Maybe<String> &time_format, double real_seconds,
                      double user_seconds, double system_seconds,
                      u64 peak_rss_bytes, time_report_layout layout,
                      time_report_rss rss) throws -> String;

struct source_line_position
{
  usize line_number;
  usize line_start;
  usize line_end;
};

fn source_line_position_at(StringView source, usize position) throws
    -> source_line_position;
fn line_number_at(StringView source, usize position) throws -> usize;

fn invalidate_line_number_cache_for(StringView source) wontthrow -> void;
fn parse_integer_in_base(StringView text, bool *out_of_range,
                         int_base base) throws -> ErrorOr<i64>;
fn parse_integer_in_base_u64(StringView text, int_base base) throws
    -> ErrorOr<u64>;

pure fn bounded_osa_distance(StringView a, StringView b,
                             usize max_distance) wontthrow -> usize;

pure fn suggestion_distance_budget(usize name_length) wontthrow -> usize;

class NameSuggestion
{
public:
  explicit NameSuggestion(StringView name)
      : m_name(name), m_max_distance(suggestion_distance_budget(name.length)),
        m_best_distance(m_max_distance + 1)
  {}

  static pure fn is_correction(usize distance, usize name_length) wontthrow
      -> bool
  {
    return distance <= suggestion_distance_budget(name_length) &&
           distance < name_length;
  }

  fn consider(StringView candidate) throws -> void
  {
    if (candidate.is_empty() || candidate == m_name) return;

    const usize distance =
        bounded_osa_distance(m_name, candidate, m_max_distance);
    if (distance > m_best_distance) return;
    if (!is_correction(distance, m_name.length)) return;

    const bool is_candidate_anagram = is_anagram(m_name, candidate);
    if (distance < m_best_distance ||
        (is_candidate_anagram && !m_best_is_anagram))
    {
      m_best_distance = distance;
      m_best_is_anagram = is_candidate_anagram;
      m_best = String{candidate};
    }
  }

  fn take_suggestion() throws -> Maybe<String>
  {
    if (m_best_distance > m_max_distance) return None;

    return steal(m_best);
  }

private:
  static fn is_anagram(StringView a, StringView b) wontthrow -> bool
  {
    if (a.length != b.length) return false;

    i32 counts[256] = {0};
    for (usize i = 0; i < a.length; i++) {
      counts[static_cast<u8>(a[i])]++;
      counts[static_cast<u8>(b[i])]--;
    }

    for (let const count : counts)
      if (count != 0) return false;

    return true;
  }

  StringView m_name;
  usize m_max_distance;
  usize m_best_distance;
  bool m_best_is_anagram{false};
  String m_best{heap_allocator()};
};

fn suggest_command(StringView name, const ArrayList<String> &local_names,
                   const ProgramResolver *resolver = nullptr) throws
    -> Maybe<String>;
fn suggest_directory_entry(const Path &directory, StringView name) throws
    -> Maybe<String>;

fn current_git_branch(StringView ceiling_directories) throws -> String;

fn resolve_git_directory(StringView ceiling_directories) throws -> Path;

fn read_git_ref_sha(const Path &git_dir, StringView ref_name) throws -> String;

fn git_upstream_ref(const Path &git_dir, StringView branch_name) throws
    -> String;

struct git_status_result
{
  explicit git_status_result(Allocator allocator) : branch(allocator) {}

  String branch;
  i32 ahead_count{0};
  i32 behind_count{0};
};

fn git_status(StringView ceiling_directories,
              Allocator allocator = heap_allocator()) throws
    -> git_status_result;

fn read_entire_standard_input() throws -> String;

struct read_line_result
{
  Maybe<String> line;
  bool was_delimiter_terminated{false};
  bool was_timed_out{false};
  bool did_read_fail{false};
};

fn read_line_from_fd(os::descriptor fd, char delimiter = '\n',
                     u64 deadline_nanos = 0,
                     Allocator allocator = heap_allocator()) throws
    -> read_line_result;

class BufferedLineReader
{
public:
  enum class Result : u8
  {
    Line,
    End,
    Error,
  };

  explicit BufferedLineReader(os::descriptor descriptor);

  fn next() throws -> Result;
  pure fn get_line() const wontthrow -> StringView;
  pure fn was_line_terminated() const wontthrow -> bool
  {
    return m_was_line_terminated;
  }

private:
  os::descriptor m_descriptor;
  String m_line{heap_allocator()};
  usize m_buffer_position{0};
  usize m_buffer_length{0};
  bool m_is_at_end{false};
  bool m_was_line_terminated{false};
  char m_buffer[65536];
};

enum class directory_validation : u8
{
  IndexOnly,
  Cached,
  Validate,
};

enum class directory_listing_order : u8
{
  Unsorted,
  FoldedName,
};

fn read_directory_cached(
    const Path &directory,
    directory_validation validation = directory_validation::Validate,
    directory_listing_order order = directory_listing_order::Unsorted) throws
    -> const ArrayList<Path::directory_child> *;
fn warm_directory_index(const Path &directory) throws -> void;
pure fn directory_listing_generation(const Path &directory) wontthrow -> u64;
pure fn directory_entry_name_lower_bound(
    const ArrayList<Path::directory_child> &entries, StringView name) wontthrow
    -> usize;
pure fn directory_entry_name_has_casefold_prefix(StringView name,
                                                 StringView prefix) wontthrow
    -> bool;
fn directory_entry_kind(const Path &directory,
                        const Path::directory_child &entry) throws
    -> Path::entry_kind;

#if !defined NDEBUG
pure fn debug_directory_stat_count() wontthrow -> usize;
pure fn debug_directory_read_count() wontthrow -> usize;
#endif

fn file_content_identity(const Path &path, Allocator allocator) throws
    -> Maybe<String>;

fn kosh_identity(StringView fallback_path) throws -> Maybe<StringView>;

fn glob_matches(StringView glob, StringView str, const Bitset &glob_active,
                usize mask_offset, extglob_mode mode = extglob_mode::Disabled,
                glob_charset charset = glob_charset::Bytes) throws -> bool;

fn lowercase_for_glob(StringView text, glob_charset charset,
                      Allocator allocator) throws -> String;

fn locale_name_is_utf8(StringView locale_name) wontthrow -> bool;

pure fn utf8_character_length(StringView text, usize position) wontthrow
    -> usize;

pure fn utf8_character_count(StringView text) wontthrow -> usize;

pure alwaysinline fn charset_character_length(StringView text, usize position,
                                              glob_charset charset) wontthrow
    -> usize
{
  if (charset == glob_charset::Utf8 && position < text.length) {
    return utf8_character_length(text, position);
  }

  return 1;
}

fn set_quit_context(const EvalContext *context) wontthrow -> void;
fn print_memory_report() wontthrow -> void;

enum class farewell_policy : u8
{
  Silent,
  Goodbye,
};

wontreturn fn quit(i32 code,
                   farewell_policy farewell = farewell_policy::Silent) throws
    -> void;

enum class file_kind_mode : u8
{
  Regular,
  Directory,
};

fn parse_file_mode(StringView expression, u32 current_mode, u32 creation_mask,
                   file_kind_mode kind) wontthrow -> Maybe<u32>;

} /* namespace utils */

} /* namespace koshka */
