#pragma once

#include "Builtin.hpp"
#include "Common.hpp"
#include "ErrorOr.hpp"
#include "Eval.hpp"
#include "NameValueArg.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"

namespace koshka {

namespace utils {

struct opaque_shell_word_range
{
  usize decoded_start;
  usize decoded_length;
  usize raw_start;
  usize raw_length;
};

struct decoded_shell_word
{
  String text;
  Bitset glob_active;
  ArrayList<usize> raw_positions;
  ArrayList<opaque_shell_word_range> opaque_ranges;
  usize raw_directory_end{0};
  usize open_quote_content_start{0};
  usize open_quote_decoded_start{0};
  usize last_quote_content_start{0};
  usize last_quote_decoded_start{0};
  usize leading_variable_expansion_end{0};
  char quote_character{0};
  char last_quote_character{0};
  bool leading_tilde_is_active{false};
  bool leading_variable_is_active{false};
  bool has_shell_syntax{false};

  explicit decoded_shell_word(Allocator allocator)
      : text(allocator), glob_active(allocator), raw_positions(allocator),
        opaque_ranges(allocator)
  {}
};

fn decode_shell_word(StringView word, Allocator allocator,
                     bool should_map_source = false) throws
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

fn expand_leading_tilde_path(StringView name) throws -> Maybe<String>;

/* Returns false when the value has no control byte, so the caller applies its
   own non-control quoting. */
fn append_ansi_c_quote_if_needed(String &out, StringView arg) throws -> bool;

fn decode_ansi_c_escapes(String &out, StringView body) throws -> void;

fn execute_context(ExecContext &&ec, EvalContext &cxt,
                   execution_mode mode) throws -> i32;

fn execute_contexts_with_pipes(ArrayList<ExecContext> &&ecs, EvalContext &cxt,
                               execution_mode mode) throws -> i32;
fn terminate_and_reap_processes(const ArrayList<os::process> &processes,
                                usize first_process_position = 0) wontthrow
    -> void;

pure fn strip_sig_prefix(StringView name) wontthrow -> StringView;

pure alwaysinline fn ascii_to_lower(char ch) wontthrow -> char
{
  if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
  return ch;
}

pure fn token_has_uppercase(StringView token) wontthrow -> bool;
pure fn smart_case_prefix_matches(StringView candidate,
                                  StringView prefix) wontthrow -> bool;

fn split_lines(StringView text) throws -> ArrayList<StringView>;

fn format_unix_timestamp(i64 unix_time, const char *format) throws -> String;

/* It matches dash's set, not the lexer's, so a shell-specific token such as
   time is excluded. */
pure fn is_posix_reserved_word(StringView word) wontthrow -> bool;

/* The value saturates to the i64 range on overflow, and any other content
   yields an Error. */
fn parse_decimal_i64(StringView text, bool *out_of_range = nullptr) throws
    -> ErrorOr<i64>;
fn parse_decimal_u64(StringView text) throws -> ErrorOr<u64>;

fn parse_decimal_f64(const String &text) throws -> ErrorOr<f64>;

fn format_f64(f64 value, Allocator allocator) throws -> String;

fn parse_timeout_seconds_to_nanos(StringView text) throws -> ErrorOr<i64>;

/* The caller's buffer must hold at least twenty-one bytes. */
fn int_to_text_into(i64 value, char *buffer, usize buffer_size) wontthrow
    -> StringView;

fn format_minutes_seconds(double seconds) throws -> String;

fn format_time_report_posix(double real_seconds, double user_seconds,
                            double system_seconds) throws -> String;
fn format_time_report_pretty(double real_seconds, double user_seconds,
                             double system_seconds, u64 peak_rss_bytes) throws
    -> String;

/* The bash conversions are honored, %%, a literal percent, %[p][l]R, %[p][l]U,
   and %[p][l]S for the real, user, and system seconds, and %P for the cpu busy
   percent, where p is a precision from zero to six and l selects the minutes
   form. */
fn format_time_report_custom(StringView format, double real_seconds,
                             double user_seconds, double system_seconds) throws
    -> String;

/* The zero-based line number the byte at position falls on. The newline table
   is cached on the source pointer and length, holding one source at a time. */
struct source_line_position
{
  usize line_number;
  usize line_start;
  usize line_end;
};

fn source_line_position_at(StringView source, usize position) throws
    -> source_line_position;
fn line_number_at(StringView source, usize position) throws -> usize;

/* Dropped when the host frees a retained source, so a later source at the same
   address with the same length does not read a stale table. */
fn invalidate_line_number_cache() wontthrow -> void;
fn parse_integer_in_base(StringView text, int_base base,
                         bool *out_of_range = nullptr) throws -> ErrorOr<i64>;
fn parse_integer_in_base_u64(StringView text, int_base base) throws
    -> ErrorOr<u64>;

/* The optimal-string-alignment distance, the edit distance that also counts an
   adjacent transposition as one edit. The result saturates at max_distance + 1
   once the rows can no longer reach the bound. */
pure fn bounded_osa_distance(StringView a, StringView b,
                             usize max_distance) wontthrow -> usize;

/* The edit budget a name of this length is allowed, which is one for a name too
   short to survive two edits. */
pure fn suggestion_distance_budget(usize name_length) wontthrow -> usize;

/* The closest candidate to a name, kept as candidates are offered one at a
   time. A candidate that is an anagram of the name wins a tie, since a
   transposed pair is the likelier typo. */
class NameSuggestion
{
public:
  explicit NameSuggestion(StringView name)
      : m_name(name), m_max_distance(suggestion_distance_budget(name.length)),
        m_best_distance(m_max_distance + 1)
  {}

  /* Whether an edit distance reads as a correction of a name of this length. A
     correction keeps at least one byte of the name it corrects, so a distance
     that covers the whole name names something else. */
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

/* The current git branch read from .git/HEAD without forking git, walking up
   from the working directory to the filesystem root. Empty outside a
   repository. A detached HEAD reads as the short commit hash. */
fn current_git_branch() throws -> String;

fn resolve_git_directory() throws -> Path;

fn read_git_ref_sha(const Path &git_dir, StringView ref_name) throws -> String;

fn git_upstream_ref(const Path &git_dir, StringView branch_name) throws
    -> String;

fn git_ahead_behind_counts(i32 &ahead_count, i32 &behind_count) throws -> void;
fn git_status(String &branch, i32 &ahead_count, i32 &behind_count) throws
    -> void;

fn read_entire_standard_input() throws -> String;

/* Returns None at end of input with no bytes read. The delimiter defaults to a
   newline, and read -d passes the first byte of its argument, or a NUL for an
   empty argument. */
fn read_line_from_fd(os::descriptor fd, bool &was_delimiter_terminated,
                     char delimiter = '\n', u64 deadline_nanos = 0,
                     bool *was_timed_out = nullptr,
                     Allocator allocator = heap_allocator(),
                     bool *did_read_fail = nullptr) throws -> Maybe<String>;

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

private:
  os::descriptor m_descriptor;
  String m_line{heap_allocator()};
  usize m_buffer_position{0};
  usize m_buffer_length{0};
  bool m_is_at_end{false};
  char m_buffer[65536]{};
};

enum class directory_validation : u8
{
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
pure fn debug_directory_sort_count() wontthrow -> usize;
pure fn debug_executable_probe_count() wontthrow -> usize;
pure fn debug_program_path_candidate_count() wontthrow -> usize;
#endif

fn file_content_identity(const Path &path, Allocator allocator) throws
    -> Maybe<String>;

fn kosh_identity(StringView fallback_path) throws -> Maybe<StringView>;

/* glob_active reads which bytes act as metacharacters. With extglob set the
   bash extended-glob groups ?(..), *(..), +(..), @(..), and !(..) are
   recognized, otherwise they are plain bytes. */
fn glob_matches(StringView glob, StringView str, const Bitset &glob_active,
                usize mask_offset, bool extglob = false) throws -> bool;

fn set_quit_context(const EvalContext *context) wontthrow -> void;

enum class farewell_policy : u8
{
  Silent,
  Goodbye,
};

[[noreturn]] fn quit(i32 code,
                     farewell_policy farewell = farewell_policy::Silent) throws
    -> void;

} /* namespace utils */

} /* namespace koshka */
