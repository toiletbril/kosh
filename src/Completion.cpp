#include "Completion.hpp"

#include "Arena.hpp"
#include "Builtin.hpp"
#include "Colors.hpp"
#include "CompletionInternal.hpp"
#include "CompletionPolicy.hpp"
#include "Debug.hpp"
#include "HashSet.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace completion {

BumpArena COMPLETION_ARENA{};

static pure fn is_word_separator(char c) wontthrow -> bool
{
  return lexer::is_whitespace(c) || c == '\n';
}

static pure fn is_command_separator(char c) wontthrow -> bool
{
  return c == ';' || c == '|' || c == '&' || c == '(' || c == '\n';
}

static pure fn is_unmatched_closing_paren(StringView line,
                                          usize position) wontthrow -> bool
{
  usize depth = 0;
  for (usize k = 0; k < position; k++) {
    if (line[k] == '(') {
      depth++;
    } else if (line[k] == ')' && depth > 0) {
      depth--;
    }
  }
  return depth == 0;
}

static pure fn quoted_run_end(StringView line, usize position) wontthrow
    -> usize
{
  let const opener = line[position];
  if (opener == '\\')
    return position + 1 < line.length ? position + 1 : position;
  if (opener != '\'' && opener != '"') return position;

  usize k = position + 1;
  while (k < line.length && line[k] != opener) {
    if (opener == '"' && line[k] == '\\' && k + 1 < line.length) k++;
    k++;
  }
  return k < line.length ? k : line.length - 1;
}

pure fn token_has_glob_metacharacter(StringView token) wontthrow -> bool
{
  for (usize i = 0; i < token.length; i++) {
    let const c = token[i];
    if (c == '*' || c == '?' || c == '[') return true;
  }
  return false;
}

static pure fn is_token_boundary(char c) wontthrow -> bool
{
  return is_word_separator(c) || is_command_separator(c);
}

static pure fn is_active_token_boundary(StringView line,
                                        usize position) wontthrow -> bool
{
  let const c = line[position];
  if (!is_token_boundary(c)) return false;

  if ((c == '(' || c == ')') && position > 0 &&
      !is_token_boundary(line[position - 1]))
  {
    return false;
  }

  return true;
}

struct token_bounds
{
  usize start;
  usize end;
};

/* A forward scan honors single and double quotes and a backslash escape. A
   quoted or escaped separator stays part of the word. A paren glued to the
   preceding byte is literal. A name like burner (3).log completes without
   opening a subshell. */
static pure fn find_token_bounds(StringView line, usize cursor) wontthrow
    -> token_bounds
{
  usize start = 0;
  usize i = 0;
  while (i < cursor && i < line.length) {
    let const c = line[i];
    if (c == '\\' || c == '\'' || c == '"') {
      i = quoted_run_end(line, i) + 1;
      continue;
    }

    if (is_active_token_boundary(line, i)) start = i + 1;
    i++;
  }

  usize end = cursor;
  while (end < line.length) {
    let const c = line[end];
    if (c == '\\' || c == '\'' || c == '"') {
      end = quoted_run_end(line, end) + 1;
      continue;
    }

    if (is_active_token_boundary(line, end)) break;
    end++;
  }

  return token_bounds{start, end};
}

static pure fn is_transparent_command_prefix(StringView word) wontthrow -> bool
{
  if (word.is_empty()) return false;
  if (word[0] == '-') return true;
  if (lexer::is_variable_name_start(word[0]) &&
      word.find_character('=').has_value())
  {
    return true;
  }
  return TRANSPARENT_PREFIXES.contains(word);
}

static pure fn next_completion_prefix_word(StringView line,
                                           usize &position) wontthrow
    -> Maybe<StringView>
{
  position = skip_blanks(line, position);
  if (position >= line.length) return None;

  let const start = position;
  while (position < line.length && line[position] != ' ' &&
         line[position] != '\t')
  {
    if (line[position] == '\'' || line[position] == '"' ||
        line[position] == '\\')
    {
      position = quoted_run_end(line, position) + 1;
    } else {
      position++;
    }
  }

  return line.substring_of_length(start, position - start);
}

static fn timeout_flag_takes_value(char short_name,
                                   StringView long_name) wontthrow -> bool
{
  let const flags =
      koshkit::koshkit_util_flag_list(koshkit::Utility::Kind::Timeout);
  if (flags == nullptr) return false;

  for (let const flag : *flags) {
    if (short_name != '\0' && flag->short_name() != short_name) continue;
    if (!long_name.is_empty() && flag->long_name() != long_name) continue;
    return flag->kind() == Flag::Kind::String ||
           flag->kind() == Flag::Kind::ManyStrings;
  }
  return false;
}

static fn timeout_option_takes_next_word(StringView word) wontthrow -> bool
{
  if (word.length < 2 || word[0] != '-') return false;

  if (word[1] == '-') {
    let const separator = word.find_character('=');
    let const name_length =
        separator.has_value() ? *separator - 2 : word.length - 2;
    let const name = word.substring_of_length(2, name_length);
    return !separator.has_value() && timeout_flag_takes_value('\0', name);
  }

  for (usize flag_index = 1; flag_index < word.length; flag_index++) {
    if (timeout_flag_takes_value(word[flag_index], {}))
      return flag_index + 1 == word.length;
  }
  return false;
}

static pure fn timeout_managed_command_start(StringView line,
                                             usize position) wontthrow
    -> Maybe<usize>
{
  let should_skip_value = false;
  loop
  {
    let const word = next_completion_prefix_word(line, position);
    if (!word.has_value()) return None;
    let const decoded_word =
        utils::decode_shell_word(*word, completion_allocator());

    if (should_skip_value) {
      should_skip_value = false;
      continue;
    }

    if (decoded_word.text == "-") return None;

    if (decoded_word.text == "--") {
      let const duration = next_completion_prefix_word(line, position);
      if (!duration.has_value()) return None;
      return skip_blanks(line, position);
    }

    if (decoded_word.text.length() > 1 && decoded_word.text[0] == '-') {
      should_skip_value =
          timeout_option_takes_next_word(decoded_word.text.view());
      continue;
    }

    return skip_blanks(line, position);
  }
}

static pure fn timeout_command_start(StringView line) wontthrow -> Maybe<usize>
{
  usize position = 0;
  loop
  {
    let const word = next_completion_prefix_word(line, position);
    if (!word.has_value()) return None;
    let const decoded_word =
        utils::decode_shell_word(*word, completion_allocator());

    if (decoded_word.text == "timeout")
      return timeout_managed_command_start(line, position);

    if (decoded_word.text == "koshkit") {
      let const utility = next_completion_prefix_word(line, position);
      if (!utility.has_value()) return None;
      let const decoded_utility =
          utils::decode_shell_word(*utility, completion_allocator());
      if (decoded_utility.text == "timeout")
        return timeout_managed_command_start(line, position);
      return None;
    }

    if (!is_transparent_command_prefix(decoded_word.text.view())) return None;
  }
}

static pure fn is_in_command_position(StringView line,
                                      usize token_start) wontthrow -> bool
{
  if (let const managed_start = timeout_command_start(line);
      managed_start.has_value())
  {
    return token_start == *managed_start;
  }

  let i = token_start;
  loop
  {
    while (i > 0 && is_word_separator(line[i - 1]))
      i--;
    if (i == 0) return true;
    if (is_command_separator(line[i - 1])) return true;
    if (line[i - 1] == ')' && is_unmatched_closing_paren(line, i - 1)) {
      return true;
    }

    let word_start = i;
    while (word_start > 0 && !is_word_separator(line[word_start - 1]) &&
           !is_command_separator(line[word_start - 1]))
      word_start--;
    if (!is_transparent_command_prefix(
            line.substring_of_length(word_start, i - word_start)))
      return false;
    i = word_start;
  }
}

static fn all_active_glob_mask(usize length) throws -> Bitset
{
  let mask = Bitset{completion_allocator()};
  mask.reserve(length);
  for (usize i = 0; i < length; i++)
    mask.push(true);
  return mask;
}

/* The three match strengths a typed token has against a candidate, best first.
   An exact prefix always wins, then a smart-case prefix, then a subsequence
   such as fbb inside foo_bar_baz. */
enum class match_tier : u8
{
  exact_prefix = 0,
  prefix = 1,
  subsequence = 2,
};

static constexpr usize MATCH_TIER_COUNT = 3;

/* Smart case means a token with an uppercase byte matches case sensitively,
   while an all-lowercase token matches either case. The caller passes the smart
   verdict so it is computed once per token, not once per candidate. */
static pure fn candidate_match(StringView token, StringView candidate,
                               bool is_case_sensitive) wontthrow
    -> Maybe<match_tier>
{
  if (candidate.starts_with(token)) return match_tier::exact_prefix;

  if (!is_case_sensitive && candidate.length >= token.length) {
    bool is_prefix = true;
    for (usize i = 0; i < token.length; i++)
      if (utils::ascii_to_lower(candidate[i]) !=
          utils::ascii_to_lower(token[i]))
      {
        is_prefix = false;
        break;
      }
    if (is_prefix) return match_tier::prefix;
  }

  /* A subsequence match is far looser than a prefix, so it is limited to a
     name-like token of at least two bytes. A single byte or an option dash
     would otherwise match almost every entry. */
  if (token.length < 2 || !lexer::is_variable_name(token[0])) return None;

  usize matched_count = 0;
  for (usize i = 0; i < candidate.length && matched_count < token.length; i++) {
    let const is_equal = is_case_sensitive
                             ? candidate[i] == token[matched_count]
                             : utils::ascii_to_lower(candidate[i]) ==
                                   utils::ascii_to_lower(token[matched_count]);
    if (is_equal) matched_count++;
  }
  if (matched_count == token.length) return match_tier::subsequence;

  return None;
}

class TieredCandidates
{
public:
  TieredCandidates()
      : by_tier{ArrayList<String>{completion_allocator()},
                ArrayList<String>{completion_allocator()},
                ArrayList<String>{completion_allocator()}}
  {}

  fn add(match_tier tier, String candidate) throws -> void
  {
    by_tier[static_cast<usize>(tier)].push(steal(candidate));
  }

  pure fn has(match_tier tier) const wontthrow -> bool
  {
    return !by_tier[static_cast<usize>(tier)].is_empty();
  }

  mustuse fn best() throws -> ArrayList<String>
  {
    for (usize tier = 0; tier < MATCH_TIER_COUNT; tier++)
      if (!by_tier[tier].is_empty()) return steal(by_tier[tier]);
    return ArrayList<String>{completion_allocator()};
  }

private:
  ArrayList<String> by_tier[MATCH_TIER_COUNT];
};

class BorrowedStringSet
{
public:
  fn add(StringView value) throws -> bool
  {
    if (slots.is_empty() || (entry_count + 1) * 4 >= slots.count() * 3) grow();

    return place(slots, value, hash_bytes(value));
  }

private:
  struct slot
  {
    StringView value{};
    u64 hash{0};
    bool is_occupied{false};
  };

  fn place(ArrayList<slot> &destination, StringView value, u64 hash) wontthrow
      -> bool
  {
    let const mask = destination.count() - 1;
    let position = static_cast<usize>(hash) & mask;

    while (destination[position].is_occupied) {
      if (destination[position].hash == hash &&
          destination[position].value == value)
      {
        return false;
      }
      position = (position + 1) & mask;
    }

    destination[position] = {value, hash, true};
    entry_count++;
    return true;
  }

  fn grow() throws -> void
  {
    let fresh = ArrayList<slot>{completion_allocator()};
    let const capacity = slots.is_empty() ? 16 : slots.count() * 2;
    fresh.reserve(capacity);
    for (usize position = 0; position < capacity; position++)
      fresh.push({});

    entry_count = 0;
    for (let const &entry : slots)
      if (entry.is_occupied) place(fresh, entry.value, entry.hash);

    slots = steal(fresh);
  }

  ArrayList<slot> slots{completion_allocator()};
  usize entry_count{0};
};

static fn command_name_match(StringView name, StringView token,
                             bool token_is_glob, bool is_case_sensitive,
                             const Bitset &glob_active) throws
    -> Maybe<match_tier>
{
  if (token_is_glob) {
    if (utils::glob_matches(token, name, glob_active, 0))
      return match_tier::exact_prefix;
    return None;
  }
  return candidate_match(token, name, is_case_sensitive);
}

class CommandListCollector
{
public:
  fn add(StringView name, match_tier tier) throws -> void
  {
    candidates.add(tier, String{completion_allocator(), name});
    materialized_count++;
  }

  fn note_source_candidate() wontthrow -> void { source_scan_count++; }

  pure fn has_exact() const wontthrow -> bool
  {
    return candidates.has(match_tier::exact_prefix);
  }

  pure fn has_prefix() const wontthrow -> bool
  {
    return has_exact() || candidates.has(match_tier::prefix);
  }

  pure fn allows_fuzzy_fallback() const wontthrow -> bool { return true; }

  fn take() throws -> ArrayList<String> { return candidates.best(); }
  pure fn source_scans() const wontthrow -> usize { return source_scan_count; }
  pure fn materialized() const wontthrow -> usize { return materialized_count; }

private:
  TieredCandidates candidates{};
  usize source_scan_count{0};
  usize materialized_count{0};
};

static pure fn common_prefix_length(StringView left, StringView right,
                                    usize limit,
                                    bool should_ignore_ascii_case) wontthrow
    -> usize
{
  usize shared_length = 0;
  while (shared_length < limit && shared_length < left.length &&
         shared_length < right.length)
  {
    let const left_byte = left[shared_length];
    let const right_byte = right[shared_length];
    let const bytes_match = should_ignore_ascii_case
                                ? utils::ascii_to_lower(left_byte) ==
                                      utils::ascii_to_lower(right_byte)
                                : left_byte == right_byte;
    if (!bytes_match) break;
    shared_length++;
  }
  while (shared_length > 0 && shared_length < left.length &&
         (static_cast<unsigned char>(left[shared_length]) & 0xC0) == 0x80)
  {
    shared_length--;
  }

  return shared_length;
}

class GhostPrefixCollector
{
public:
  fn add(StringView name, match_tier tier) throws -> void
  {
    let const tier_index = static_cast<usize>(tier);
    if (tier_index > best_tier) return;
    if (tier_index < best_tier) {
      best_tier = tier_index;
      prefix = String{completion_allocator(), name};
      match_count = 1;
      return;
    }

    let const shared_length = common_prefix_length(
        prefix.view(), name, prefix.count(), tier == match_tier::prefix);
    while (prefix.count() > shared_length)
      prefix.pop_back();
    match_count++;
  }

  fn note_source_candidate() wontthrow -> void { source_scan_count++; }

  pure fn has_exact() const wontthrow -> bool { return best_tier == 0; }
  pure fn has_prefix() const wontthrow -> bool { return best_tier <= 1; }
  pure fn allows_fuzzy_fallback() const wontthrow -> bool { return false; }
  pure fn count() const wontthrow -> usize { return match_count; }
  pure fn source_scans() const wontthrow -> usize { return source_scan_count; }
  pure fn materialized() const wontthrow -> usize { return 0; }
  fn take_prefix() wontthrow -> String { return steal(prefix); }

private:
  usize best_tier{MATCH_TIER_COUNT};
  usize match_count{0};
  usize source_scan_count{0};
  String prefix{completion_allocator()};
};

template <typename Collector>
static fn collect_command_names(StringView token, command_match_mode match_mode,
                                EvalContext &context,
                                Collector &collector) throws -> void
{
  let const token_is_glob = match_mode == command_match_mode::Glob;
  let const is_case_sensitive = utils::token_has_uppercase(token);
  let const glob_active = token_is_glob ? all_active_glob_mask(token.length)
                                        : Bitset{completion_allocator()};
  let normalized_path_token = String{completion_allocator(), token};
  unused(os::normalize_program_name(normalized_path_token));
  let const path_is_case_sensitive =
      utils::token_has_uppercase(normalized_path_token.view());
  let seen = BorrowedStringSet{};

  let const do_add = [&](StringView name) throws {
    collector.note_source_candidate();
    let const tier = command_name_match(name, token, token_is_glob,
                                        is_case_sensitive, glob_active);
    if (tier.has_value() && seen.add(name)) collector.add(name, *tier);
  };

  let const do_add_path = [&](StringView name) throws {
    collector.note_source_candidate();
    let const tier =
        command_name_match(name, normalized_path_token.view(), token_is_glob,
                           path_is_case_sensitive, glob_active);
    if (tier.has_value() && seen.add(name)) collector.add(name, *tier);
  };

  for (let const &builtin_name : builtin_names())
    do_add(builtin_name.view());

  if (context.koshkit() || context.mood() == mimic_mood::Default) {
    for (const String &util_name : koshkit::util_names())
      do_add(util_name.view());
  }

  context.for_each_function_name(do_add);
  context.for_each_alias_name(do_add);

  let const &path_names = context.get_program_resolver().get_command_names(
      token_is_glob ? StringView{} : normalized_path_token.view(),
      token_is_glob || token.is_empty()
          ? ProgramResolver::ValidationScope::All
          : ProgramResolver::ValidationScope::Prefix);
  if (!token_is_glob &&
      (!token.is_empty() || collector.allows_fuzzy_fallback()))
  {
    for (let const &path_name : path_names)
      if (utils::smart_case_prefix_matches(path_name.view(),
                                           normalized_path_token.view()))
        do_add_path(path_name.view());
  }

  if (collector.allows_fuzzy_fallback() && !collector.has_prefix()) {
    let const &fallback_path_names =
        context.get_program_resolver().get_command_names(
            {}, ProgramResolver::ValidationScope::All);
    for (let const &entry : fallback_path_names)
      do_add_path(entry.view());
  }
}

static fn complete_command_name_prefix(StringView token,
                                       command_match_mode match_mode,
                                       EvalContext &context) throws
    -> GhostPrefixCollector
{
  let collector = GhostPrefixCollector{};
  collect_command_names(token, match_mode, context, collector);
  return collector;
}

static fn compute_longest_common_prefix(const ArrayList<String> &candidates,
                                        bool should_ignore_ascii_case) throws
    -> String
{
  if (candidates.is_empty()) return String{candidates.allocator()};
  let const first = candidates[0].view();
  usize prefix_length = first.length;
  for (usize i = 1; i < candidates.count(); i++) {
    let const candidate = candidates[i].view();
    prefix_length = common_prefix_length(first, candidate, prefix_length,
                                         should_ignore_ascii_case);
  }
  return String{candidates.allocator(),
                first.substring_of_length(0, prefix_length)};
}

fn complete_command_names(StringView token, command_match_mode match_mode,
                          EvalContext &context) throws -> ArrayList<String>
{
  let collector = CommandListCollector{};

  LOG(Debug, "completing command position for token '%.*s'",
      static_cast<int>(token.length), token.data);

  collect_command_names(token, match_mode, context, collector);
  return collector.take();
}

/* The directory part keeps its trailing separator so the basename joins back
   on. */
struct path_token
{
  StringView directory_part;
  StringView basename_part;
};

static pure fn split_path_token(StringView token) wontthrow -> path_token
{
  let last_separator = token.length;
  for (usize i = 0; i < token.length; i++) {
    if (os::is_directory_separator(token[i])) last_separator = i;
  }
  if (last_separator == token.length) {
    return path_token{StringView{}, token};
  }
  return path_token{
      token.substring_of_length(0, last_separator + 1),
      token.substring(last_separator + 1),
  };
}

/* The tilde is excluded since it expands a home the user wants. */
static pure fn byte_needs_quoting(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '*':
  case '?':
  case '[':
  case ']':
  case '(':
  case ')':
  case '{':
  case '}':
  case '\'':
  case '"':
  case '`':
  case '$':
  case '&':
  case '|':
  case ';':
  case '<':
  case '>':
  case '\\':
  case '!':
  case '#': return true;
  default: return false;
  }
}

static pure fn path_candidate_needs_quoting(StringView candidate) wontthrow
    -> bool
{
  for (usize i = 0; i < candidate.length; i++)
    if (byte_needs_quoting(candidate[i])) return true;

  return false;
}

static pure fn byte_needs_double_quote_escape(char byte) wontthrow -> bool
{
  return byte == '"' || byte == '\\' || byte == '$' || byte == '`';
}

static fn quote_path_candidate(StringView candidate) throws -> String
{
  let quoted = String{completion_allocator()};

  let const has_single_quote = candidate.find_character('\'').has_value();
  let const has_bang = candidate.find_character('!').has_value();

  if (!has_single_quote) {
    quoted.push('\'');
    quoted += candidate;
    quoted.push('\'');
    return quoted;
  }

  if (!has_bang) {
    quoted.push('"');
    for (usize i = 0; i < candidate.length; i++) {
      let const byte = candidate[i];
      if (byte_needs_double_quote_escape(byte)) quoted.push('\\');
      quoted.push(byte);
    }
    quoted.push('"');
    return quoted;
  }

  for (usize i = 0; i < candidate.length; i++) {
    let const byte = candidate[i];
    if (byte_needs_quoting(byte)) quoted.push('\\');
    quoted.push(byte);
  }

  return quoted;
}

static fn escape_path_candidate(StringView candidate) throws -> String
{
  let escaped = String{completion_allocator()};

  for (usize position = 0; position < candidate.length; position++) {
    let const byte = candidate[position];
    if (byte_needs_quoting(byte)) escaped.push('\\');
    escaped.push(byte);
  }

  return escaped;
}

static fn append_open_quote_candidate(String &candidate, StringView text,
                                      char quote_character) throws -> void
{
  for (usize position = 0; position < text.length; position++) {
    let const byte = text[position];
    if (quote_character == '\'' && byte == '\'') {
      candidate.push('\'');
      candidate.push('\\');
      candidate.push('\'');
      candidate.push('\'');
      continue;
    }
    if (quote_character == '"' && byte_needs_double_quote_escape(byte)) {
      candidate.push('\\');
    }
    candidate.push(byte);
  }
}

static fn open_quote_candidate_boundary(StringView typed, usize typed_boundary,
                                        StringView candidate) wontthrow -> usize
{
  let const is_case_sensitive = utils::token_has_uppercase(typed);
  usize typed_position = 0;
  usize candidate_position = 0;
  while (candidate_position < candidate.length &&
         typed_position < typed_boundary)
  {
    let const is_equal =
        is_case_sensitive
            ? candidate[candidate_position] == typed[typed_position]
            : utils::ascii_to_lower(candidate[candidate_position]) ==
                  utils::ascii_to_lower(typed[typed_position]);
    candidate_position++;
    if (is_equal) typed_position++;
  }
  if (typed_position == typed_boundary) return candidate_position;
  return typed_boundary < candidate.length ? typed_boundary : candidate.length;
}

static fn
append_candidate_suffix(String &candidate, StringView suffix,
                        const utils::decoded_shell_word &decoded_word) throws
    -> void
{
  if (suffix.is_empty()) return;

  if (decoded_word.is_leading_variable_active) {
    candidate += escape_path_candidate(suffix);
    return;
  }

  if (path_candidate_needs_quoting(suffix))
    candidate += quote_path_candidate(suffix);
  else
    candidate += suffix;
}

static fn
rebuild_shell_syntax_candidate(StringView raw_token,
                               const utils::decoded_shell_word &decoded_word,
                               StringView decoded_candidate) throws -> String
{
  let candidate = String{completion_allocator()};
  if (decoded_candidate.starts_with(decoded_word.text.view())) {
    let const suffix = decoded_candidate.substring(decoded_word.text.length());
    let const can_extend_closed_quote =
        decoded_word.quote_character == 0 &&
        decoded_word.last_quote_character != 0 && !raw_token.is_empty() &&
        raw_token[raw_token.length - 1] == decoded_word.last_quote_character;
    if (can_extend_closed_quote) {
      candidate.append(raw_token.substring_of_length(0, raw_token.length - 1));
      append_open_quote_candidate(candidate, suffix,
                                  decoded_word.last_quote_character);
      candidate.push(decoded_word.last_quote_character);
    } else if (decoded_word.quote_character != 0) {
      candidate.append(raw_token);
      append_open_quote_candidate(candidate, suffix,
                                  decoded_word.quote_character);
    } else {
      candidate.append(raw_token);
      append_candidate_suffix(candidate, suffix, decoded_word);
    }
    return candidate;
  }

  if (decoded_word.last_quote_character == 0) {
    if (decoded_word.is_leading_tilde_active &&
        decoded_candidate.starts_with("~"))
    {
      candidate.push('~');
      append_candidate_suffix(candidate, decoded_candidate.substring(1),
                              decoded_word);
      return candidate;
    }

    if (decoded_word.is_leading_variable_active &&
        decoded_word.leading_variable_expansion_end <= decoded_candidate.length)
    {
      candidate.append(raw_token.substring_of_length(
          0, decoded_word.leading_variable_expansion_end));
      candidate += escape_path_candidate(decoded_candidate.substring(
          decoded_word.leading_variable_expansion_end));
      return candidate;
    }

    append_candidate_suffix(candidate, decoded_candidate, decoded_word);
    return candidate;
  }

  let const candidate_boundary = open_quote_candidate_boundary(
      decoded_word.text.view(), decoded_word.last_quote_decoded_start,
      decoded_candidate);
  let const decoded_prefix = decoded_word.text.view().substring_of_length(
      0, decoded_word.last_quote_decoded_start);
  let const candidate_prefix =
      decoded_candidate.substring_of_length(0, candidate_boundary);
  if (decoded_prefix == candidate_prefix) {
    candidate.append(raw_token.substring_of_length(
        0, decoded_word.last_quote_content_start));
  } else {
    if (!candidate_prefix.is_empty()) {
      if (path_candidate_needs_quoting(candidate_prefix))
        candidate.append(quote_path_candidate(candidate_prefix).view());
      else
        candidate.append(candidate_prefix);
    }
    candidate.push(decoded_word.last_quote_character);
  }
  append_open_quote_candidate(candidate,
                              decoded_candidate.substring(candidate_boundary),
                              decoded_word.last_quote_character);
  if (decoded_word.quote_character == 0)
    candidate.push(decoded_word.last_quote_character);
  return candidate;
}

/* A leading $NAME or ${NAME} in the directory prefix is expanded to its value
   so the listing reads the real directory, while the offered candidate keeps
   the unexpanded prefix. None means no leading variable, so the caller falls
   back to the literal path. */
static fn expand_leading_variable_path(StringView directory_part,
                                       usize expansion_end,
                                       EvalContext &context) throws
    -> Maybe<String>
{
  if (directory_part.is_empty() || directory_part[0] != '$' ||
      expansion_end > directory_part.length)
  {
    return None;
  }

  let const expansion = directory_part.substring_of_length(0, expansion_end);

  usize name_start = 1;
  let const is_braced =
      name_start < expansion.length && expansion[name_start] == '{';
  if (is_braced) name_start++;

  let name_end = expansion.length;
  if (is_braced) {
    if (name_end <= name_start || expansion[name_end - 1] != '}') return None;
    name_end--;
  }

  let const name =
      expansion.substring_of_length(name_start, name_end - name_start);
  if (name.is_empty()) return None;

  let const value = context.get_variable_value(name);
  if (!value.has_value()) return None;

  let expanded = String{completion_allocator(), value->view()};
  expanded.append(directory_part.substring(expansion_end));
  return expanded;
}

static fn
resolve_listing_directory(StringView directory_part, const Path &base_directory,
                          EvalContext &context, bool is_leading_tilde_active,
                          bool is_leading_variable_active,
                          usize leading_variable_expansion_end) throws -> Path
{
  if (directory_part.is_empty()) return base_directory;

  if (is_leading_tilde_active)
    if (Maybe<String> expanded =
            utils::expand_leading_tilde_path(directory_part);
        expanded.has_value())
      return Path{expanded->view()};

  if (is_leading_variable_active)
    if (Maybe<String> expanded = expand_leading_variable_path(
            directory_part, leading_variable_expansion_end, context);
        expanded.has_value())
    {
      let const directory = Path{expanded->view()};
      if (directory.is_absolute()) return directory;
      let resolved_path = base_directory.clone();
      resolved_path.push_component(expanded->view());
      return resolved_path;
    }

  let const directory = Path{directory_part};
  if (directory.is_absolute()) return directory;

  let resolved_path = base_directory.clone();
  resolved_path.push_component(directory_part);
  return resolved_path;
}

static fn entry_is_executable(const Path &directory, StringView name) throws
    -> bool
{
  let full = directory.clone();
  full.push_component(name);
  return full.is_executable();
}

enum class filesystem_entry_filter : u8
{
  All,
  DirectoriesOnly,
  RunnableOrDirectories,
};

enum class path_text_mode : u8
{
  ShellSyntax,
  Literal,
};

static fn build_filesystem_candidate(
    StringView directory_part, StringView raw_directory_part, StringView name,
    bool is_directory, path_text_mode text_mode, StringView raw_token,
    const utils::decoded_shell_word &decoded_word) throws -> String
{
  let const inside_quote = text_mode == path_text_mode::Literal;
  let const preserve_directory_spelling = raw_directory_part != directory_part;
  let entry_name = String{completion_allocator(), name};
  if (is_directory) {
    let separator = '/';
    if (!directory_part.is_empty() &&
        os::is_directory_separator(directory_part[directory_part.length - 1]))
    {
      separator = directory_part[directory_part.length - 1];
    }
    entry_name.push(separator);
  }

  let const token_ends_with_closed_quote =
      decoded_word.quote_character == 0 &&
      decoded_word.last_quote_character != 0 && !raw_token.is_empty() &&
      raw_token[raw_token.length - 1] == decoded_word.last_quote_character;
  if (decoded_word.quote_character != 0 || token_ends_with_closed_quote) {
    let decoded_candidate =
        String{completion_allocator(), directory_part} + entry_name;
    return rebuild_shell_syntax_candidate(raw_token, decoded_word,
                                          decoded_candidate.view());
  }

  if (preserve_directory_spelling) {
    if (!inside_quote && path_candidate_needs_quoting(entry_name.view())) {
      if (decoded_word.is_leading_variable_active)
        entry_name = escape_path_candidate(entry_name.view());
      else
        entry_name = quote_path_candidate(entry_name.view());
    }
    return String{completion_allocator(), raw_directory_part} + entry_name;
  }

  let candidate = String{completion_allocator(), directory_part};
  let const is_variable_prefixed =
      !directory_part.is_empty() && directory_part[0] == '$';
  if (is_variable_prefixed && !inside_quote) {
    if (path_candidate_needs_quoting(entry_name.view()))
      entry_name = escape_path_candidate(entry_name.view());
    candidate += entry_name;
  } else if (decoded_word.is_leading_tilde_active && !inside_quote) {
    candidate = String{completion_allocator(), raw_directory_part};
    if (path_candidate_needs_quoting(entry_name.view()))
      entry_name = quote_path_candidate(entry_name.view());
    candidate += entry_name;
  } else {
    candidate += entry_name;
    if (!inside_quote && path_candidate_needs_quoting(candidate.view())) {
      candidate = quote_path_candidate(candidate.view());
    }
  }

  return candidate;
}

template <typename Collector>
static fn
collect_filesystem_matches(StringView token,
                           const utils::decoded_shell_word &decoded_word,
                           const Path &base_directory, path_text_mode text_mode,
                           filesystem_entry_filter filter, EvalContext &context,
                           Collector &collector) throws -> void
{
  let const inside_quote = text_mode == path_text_mode::Literal;
  let parts = split_path_token(decoded_word.text.view());
  let raw_directory_part = parts.directory_part;
  if (!inside_quote && decoded_word.raw_directory_end > 0) {
    raw_directory_part =
        token.substring_of_length(0, decoded_word.raw_directory_end);
  }
  let const is_case_sensitive = os::FILESYSTEM_IS_CASE_SENSITIVE &&
                                utils::token_has_uppercase(parts.basename_part);

  LOG(Debug, "completing filesystem token '%.*s', dir '%.*s', base '%.*s'",
      static_cast<int>(token.length), token.data,
      static_cast<int>(parts.directory_part.length), parts.directory_part.data,
      static_cast<int>(parts.basename_part.length), parts.basename_part.data);

  let listing_directory =
      resolve_listing_directory(parts.directory_part, base_directory, context,
                                decoded_word.is_leading_tilde_active,
                                decoded_word.is_leading_variable_active,
                                decoded_word.leading_variable_expansion_end);

  let const entries = utils::read_directory_cached(
      listing_directory, utils::directory_validation::Cached,
      utils::directory_listing_order::FoldedName);
  if (entries == nullptr) return;

  let const do_add_entry = [&](const Path::directory_child &entry) throws {
    let const name = entry.name.view();
    collector.note_source_candidate();
    let const tier =
        candidate_match(parts.basename_part, name, is_case_sensitive);
    if (!tier.has_value()) return;

    let const is_directory =
        utils::directory_entry_kind(listing_directory, entry) ==
        Path::entry_kind::Directory;
    if (filter == filesystem_entry_filter::DirectoriesOnly && !is_directory) {
      return;
    }

    /* A command-position path completes only runnable files, so a plain data
       file is dropped, while directories stay for navigation. */
    if (filter == filesystem_entry_filter::RunnableOrDirectories &&
        !is_directory && !entry_is_executable(listing_directory, name))
    {
      return;
    }

    /* A dotfile stays hidden unless the user typed a leading dot. */
    if (name.length > 0 && name[0] == '.' &&
        (parts.basename_part.is_empty() || parts.basename_part[0] != '.'))
    {
      return;
    }

    let candidate = build_filesystem_candidate(
        parts.directory_part, raw_directory_part, name, is_directory, text_mode,
        token, decoded_word);
    collector.add(candidate.view(), *tier);
  };

  let entry_position =
      utils::directory_entry_name_lower_bound(*entries, parts.basename_part);
  while (entry_position < entries->count() &&
         utils::directory_entry_name_has_casefold_prefix(
             (*entries)[entry_position].name.view(), parts.basename_part))
  {
    do_add_entry((*entries)[entry_position]);
    entry_position++;
  }
  if (collector.has_prefix() || !collector.allows_fuzzy_fallback()) return;

  for (let const &entry : *entries)
    if (!utils::directory_entry_name_has_casefold_prefix(entry.name.view(),
                                                         parts.basename_part))
      do_add_entry(entry);
}

static fn
complete_filesystem(StringView token, const Path &base_directory,
                    path_text_mode text_mode, filesystem_entry_filter filter,
                    EvalContext &context,
                    const utils::decoded_shell_word *decoded = nullptr) throws
    -> ArrayList<String>
{
  let decoded_storage = utils::decoded_shell_word{completion_allocator()};
  if (decoded == nullptr) {
    if (text_mode == path_text_mode::Literal)
      decoded_storage.text.append(token);
    else
      decoded_storage = utils::decode_shell_word(token, completion_allocator());
    decoded = &decoded_storage;
  }
  let collector = CommandListCollector{};
  collect_filesystem_matches(token, *decoded, base_directory, text_mode, filter,
                             context, collector);

  return collector.take();
}

fn complete_filesystem_names(StringView token, EvalContext &context,
                             const Path &base_directory) throws
    -> ArrayList<String>
{
  return complete_filesystem(token, base_directory, path_text_mode::Literal,
                             filesystem_entry_filter::All, context);
}

static fn complete_filesystem_prefix(
    StringView token, const Path &base_directory, path_text_mode text_mode,
    filesystem_entry_filter filter, EvalContext &context,
    const utils::decoded_shell_word *decoded = nullptr) throws
    -> GhostPrefixCollector
{
  let decoded_storage = utils::decoded_shell_word{completion_allocator()};
  if (decoded == nullptr) {
    if (text_mode == path_text_mode::Literal)
      decoded_storage.text.append(token);
    else
      decoded_storage = utils::decode_shell_word(token, completion_allocator());
    decoded = &decoded_storage;
  }
  let collector = GhostPrefixCollector{};
  collect_filesystem_matches(token, *decoded, base_directory, text_mode, filter,
                             context, collector);

  return collector;
}

/* Only the trailing component is globbed. */
static fn complete_glob(StringView token, const Path &base_directory,
                        filesystem_entry_filter filter, EvalContext &context,
                        const utils::decoded_shell_word &decoded_word) throws
    -> ArrayList<String>
{
  let candidates = ArrayList<String>{completion_allocator()};
  let parts = split_path_token(decoded_word.text.view());

  LOG(Debug, "resolving glob token '%.*s'", static_cast<int>(token.length),
      token.data);

  let listing_directory =
      resolve_listing_directory(parts.directory_part, base_directory, context,
                                decoded_word.is_leading_tilde_active,
                                decoded_word.is_leading_variable_active,
                                decoded_word.leading_variable_expansion_end);

  let const entries = utils::read_directory_cached(
      listing_directory, utils::directory_validation::Cached,
      utils::directory_listing_order::FoldedName);
  if (entries == nullptr) return candidates;

  let glob_active = Bitset{completion_allocator()};
  glob_active.reserve(parts.basename_part.length);
  for (usize position = parts.directory_part.length;
       position < decoded_word.glob_active.count(); position++)
    glob_active.push(decoded_word.glob_active[position]);
  let normalized_pattern = String{completion_allocator()};
  let match_pattern = parts.basename_part;
  if (!os::FILESYSTEM_IS_CASE_SENSITIVE) {
    normalized_pattern.reserve(match_pattern.length);
    for (usize position = 0; position < match_pattern.length; position++)
      normalized_pattern.push(utils::ascii_to_lower(match_pattern[position]));
    match_pattern = normalized_pattern.view();
  }
  let candidate_name = String{completion_allocator()};

  for (let const &entry : *entries) {
    let const name = entry.name.view();
    if (!name.is_empty() && name[0] == '.' &&
        (parts.basename_part.is_empty() || parts.basename_part[0] != '.'))
    {
      continue;
    }

    let match_name = name;
    if (!os::FILESYSTEM_IS_CASE_SENSITIVE) {
      candidate_name.clear();
      candidate_name.reserve(match_name.length);
      for (usize position = 0; position < match_name.length; position++)
        candidate_name.push(utils::ascii_to_lower(match_name[position]));
      match_name = candidate_name.view();
    }

    if (!utils::glob_matches(match_pattern, match_name, glob_active, 0)) {
      continue;
    }

    let const is_directory =
        utils::directory_entry_kind(listing_directory, entry) ==
        Path::entry_kind::Directory;
    if (filter == filesystem_entry_filter::DirectoriesOnly && !is_directory) {
      continue;
    }

    if (filter == filesystem_entry_filter::RunnableOrDirectories &&
        !is_directory && !entry_is_executable(listing_directory, name))
    {
      continue;
    }

    let const raw_directory_part =
        decoded_word.raw_directory_end > 0
            ? token.substring_of_length(0, decoded_word.raw_directory_end)
            : parts.directory_part;
    let candidate = build_filesystem_candidate(
        parts.directory_part, raw_directory_part, name, is_directory,
        path_text_mode::ShellSyntax, token, decoded_word);

    candidates.push(steal(candidate));
  }

  LOG(All, "glob pattern '%.*s' matched %zu entries",
      static_cast<int>(token.length), token.data, candidates.count());

  return candidates;
}

static pure fn token_is_variable(StringView token) wontthrow -> bool
{
  /* A slash after the reference makes it a variable-prefixed path, which the
     filesystem completion expands to list while keeping the literal prefix. */
  return !token.is_empty() && token[0] == '$' &&
         !os::has_directory_separator(token);
}

static fn complete_variable(StringView token, EvalContext &context) throws
    -> ArrayList<String>
{
  let candidates = ArrayList<String>{completion_allocator()};

  let has_brace = token.length >= 2 && token[1] == '{';
  usize name_start = has_brace ? 2 : 1;
  let const prefix = token.substring(name_start);

  LOG(Debug, "completing variable token '%.*s', prefix '%.*s', brace %d",
      static_cast<int>(token.length), token.data,
      static_cast<int>(prefix.length), prefix.data, has_brace ? 1 : 0);

  let seen = HashSet{completion_allocator()};

  let const do_add_name = [&](StringView name) throws -> void {
    if (!name.starts_with(prefix)) return;
    if (seen.contains(name)) return;
    seen.add(name);

    let candidate = String{completion_allocator()};
    candidate += has_brace ? "${" : "$";
    candidate.append(name);
    if (has_brace) candidate.push('}');
    candidates.push(steal(candidate));
  };

  context.variable_names().for_each(
      [&](StringView name) { do_add_name(name); });

  for (let const &name : os::environment_names())
    do_add_name(name.view());

  let dynamic_names = ArrayList<StringView>{completion_allocator()};
  context.append_dynamic_variable_names(dynamic_names);
  for (let const &name : dynamic_names)
    do_add_name(name);

  LOG(All, "%zu variable names match prefix '%.*s'", candidates.count(),
      static_cast<int>(prefix.length), prefix.data);

  return candidates;
}

static fn token_is_tilde_user_prefix(StringView token) wontthrow -> bool
{
  return !token.is_empty() && token[0] == '~' &&
         !os::has_directory_separator(token);
}

static fn complete_tilde_user(StringView token) throws -> ArrayList<String>
{
  let candidates = ArrayList<String>{completion_allocator()};
  let const prefix = token.substring(1);
  for (let const &user : os::enumerate_users()) {
    if (!user.view().starts_with(prefix)) continue;
    let candidate = String{completion_allocator()};
    candidate.push('~');
    candidate.append(user.view());
    candidate.push('/');
    candidates.push(steal(candidate));
  }
  LOG(All, "%zu user names match tilde prefix '%.*s'", candidates.count(),
      static_cast<int>(prefix.length), prefix.data);
  return candidates;
}

fn command_word_of(StringView line) wontthrow -> StringView
{
  if (let const managed_start = timeout_command_start(line);
      managed_start.has_value())
  {
    usize position = *managed_start;
    let const command = next_completion_prefix_word(line, position);
    return command.has_value() ? *command : StringView{};
  }

  usize i = 0;
  usize open_paren_depth = 0;
  for (usize k = 0; k < line.length; k++) {
    let const c = line[k];
    if (c == '\'' || c == '"' || c == '\\') {
      k = quoted_run_end(line, k);
      continue;
    }

    if (c == '(') {
      open_paren_depth++;
    } else if (c == ')') {
      /* An unmatched paren closes a case pattern and starts the arm's body. */
      if (open_paren_depth > 0)
        open_paren_depth--;
      else
        i = k + 1;
    } else if (c == ';' || c == '|' || c == '&') {
      i = k + 1;
    }
  }
  loop
  {
    i = skip_blanks(line, i);
    let const start = i;
    while (i < line.length && line[i] != ' ' && line[i] != '\t')
      i++;
    let const word = line.substring_of_length(start, i - start);
    if (word.is_empty() || !is_transparent_command_prefix(word)) return word;
  }
}

static pure fn command_segment_start(StringView line, usize cursor) wontthrow
    -> usize
{
  usize start = 0;
  usize open_paren_depth = 0;
  let const limit = cursor < line.length ? cursor : line.length;
  for (usize k = 0; k < limit; k++) {
    let const c = line[k];
    if (c == '\'' || c == '"' || c == '\\') {
      k = quoted_run_end(line, k);
      continue;
    }

    if (c == '(') {
      open_paren_depth++;
    } else if (c == ')') {
      if (open_paren_depth > 0)
        open_paren_depth--;
      else
        start = k + 1;
    } else if (c == ';' || c == '|' || c == '&' || c == '\n') {
      start = k + 1;
    }
  }
  return start;
}

/* Symlinks are left alone so a name that dispatches on its argv[0], such as a
   busybox or rustup link, keeps the surface name the user typed. */
fn resolve_completion_alias(StringView command, EvalContext &context) throws
    -> String
{
  let name = String{command};
  for (usize depth = 0; depth < 8; depth++) {
    let const expansion = context.get_alias(name.view());
    if (!expansion.has_value()) break;
    let const expanded = expansion->view();
    usize i = 0;
    i = skip_blanks(expanded, i);
    let const start = i;
    while (i < expanded.length && expanded[i] != ' ' && expanded[i] != '\t')
      i++;
    let const first = expanded.substring_of_length(start, i - start);
    if (first.is_empty() || first == name.view()) break;
    name = String{first};
  }
  return name;
}

fn resolve_completion_command(StringView command, EvalContext &context) throws
    -> String
{
  let name = resolve_completion_alias(command, context);
  let const located = context.get_program_resolver().search(
      name.view(), ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (!located.is_empty()) {
    if (let const canonical = os::canonical_path(located.front());
        canonical.has_value())
    {
      let resolved_name = String{canonical->filename()};
      let const name_info = os::normalize_program_name(resolved_name);
      return String{
          resolved_name.substring_of_length(0, name_info.stem_length)};
    }
  }
  return name;
}

fn split_completion_words(StringView line, usize cursor, usize &cword) throws
    -> ArrayList<String>
{
  let words = ArrayList<String>{completion_allocator()};
  usize i = 0;
  let is_found = false;
  while (i < line.length) {
    i = skip_blanks(line, i);
    if (i >= line.length) break;
    let const start = i;
    while (i < line.length && line[i] != ' ' && line[i] != '\t')
      i++;
    if (cursor >= start && cursor <= i) {
      cword = words.count();
      is_found = true;
    }
    words.push(String{completion_allocator(),
                      line.substring_of_length(start, i - start)});
  }
  if (!is_found) {
    cword = words.count();
    words.push(String{completion_allocator()});
  }
  return words;
}

static pure fn file_extension_hint(StringView command) wontthrow -> const char *
{
  if (let const hint = FILE_EXTENSION_HINTS.find(command); hint.has_value())
    return *hint;
  return nullptr;
}

static pure fn candidate_extension_is_hinted(
    StringView candidate,
    const ArrayList<StringView> &hinted_extensions) wontthrow -> bool
{
  if (!candidate.is_empty() &&
      os::is_directory_separator(candidate[candidate.length - 1]))
    return false;

  usize dot = candidate.length;
  for (usize k = candidate.length; k > 0; k--) {
    if (os::is_directory_separator(candidate[k - 1])) break;
    if (candidate[k - 1] == '.') {
      dot = k - 1;
      break;
    }
  }
  if (dot >= candidate.length) return false;

  let const extension = candidate.substring(dot + 1);
  for (let const wanted : hinted_extensions) {
    if (wanted.length != extension.length) continue;

    bool is_equal = true;
    for (usize i = 0; i < wanted.length; i++)
      if (utils::ascii_to_lower(extension[i]) !=
          utils::ascii_to_lower(wanted[i]))
      {
        is_equal = false;
        break;
      }
    if (is_equal) return true;
  }
  return false;
}

static fn split_hint_extensions(StringView hint_list,
                                Allocator allocator) throws
    -> ArrayList<StringView>
{
  let extensions = ArrayList<StringView>{allocator};
  usize start = 0;
  while (start < hint_list.length) {
    usize end = start;
    while (end < hint_list.length && hint_list[end] != ' ')
      end++;
    if (end > start)
      extensions.push(hint_list.substring_of_length(start, end - start));
    start = end + 1;
  }
  return extensions;
}

static fn partition_by_extension(ArrayList<String> candidates,
                                 StringView hint_list) throws
    -> ArrayList<String>
{
  let const hinted_extensions =
      split_hint_extensions(hint_list, candidates.allocator());

  let ordered = ArrayList<String>{candidates.allocator()};
  ordered.reserve(candidates.count());
  let rest = ArrayList<String>{candidates.allocator()};

  for (usize i = 0; i < candidates.count(); i++) {
    if (candidate_extension_is_hinted(candidates[i].view(), hinted_extensions))
      ordered.push(steal(candidates[i]));
    else
      rest.push(steal(candidates[i]));
  }

  for (usize i = 0; i < rest.count(); i++)
    ordered.push(steal(rest[i]));

  return ordered;
}

static fn keep_hinted_extension(ArrayList<String> candidates,
                                StringView hint_list) throws
    -> ArrayList<String>
{
  let const hinted_extensions =
      split_hint_extensions(hint_list, candidates.allocator());

  let kept = ArrayList<String>{candidates.allocator()};
  for (usize i = 0; i < candidates.count(); i++) {
    let const candidate = candidates[i].view();
    let const is_directory =
        !candidate.is_empty() &&
        os::is_directory_separator(candidate[candidate.length - 1]);
    if (is_directory ||
        candidate_extension_is_hinted(candidate, hinted_extensions))
    {
      kept.push(steal(candidates[i]));
    }
  }
  return kept;
}

flatten fn complete(StringView line, usize cursor, EvalContext &context,
                    const Path &base_directory, completion_mode mode) throws
    -> completion_result
{
  let const for_listing = mode == completion_mode::Listing;
  COMPLETION_ARENA.reset();
  let const arena = completion_allocator();

  if (cursor > line.length) cursor = line.length;

  /* When the cursor sits inside a command substitution, completion re-roots to
     the substitution's own command line. The offset maps the replaced token
     span back to the full line for the caller. */
  let const command_range = command_substitution_range(line, cursor);
  let completion_offset = command_range.start;
  line = line.substring_of_length(command_range.start,
                                  command_range.end - command_range.start);
  cursor -= completion_offset;

  let const segment_start = command_segment_start(line, cursor);
  completion_offset += segment_start;
  line = line.substring(segment_start);
  cursor -= segment_start;

  let const bounds = find_token_bounds(line, cursor);
  let token_start = bounds.start;
  let token_end = bounds.end;
  let replacement_token_end = bounds.end;
  let token = line.substring_of_length(token_start, token_end - token_start);
  let const is_command = is_in_command_position(line, token_start);

  let const token_prefix =
      line.substring_of_length(token_start, cursor - token_start);
  let decoded_prefix =
      utils::decode_shell_word(token_prefix, completion_allocator());
  if (decoded_prefix.quote_character != 0) {
    token_end = cursor;
    token = line.substring_of_length(token_start, token_end - token_start);
    replacement_token_end = cursor;
    let const quote_start =
        token_start + decoded_prefix.open_quote_content_start - 1;
    let const quote_end = quoted_run_end(line, quote_start);
    if (quote_end >= cursor && quote_end < line.length &&
        line[quote_end] == decoded_prefix.quote_character)
    {
      replacement_token_end = quote_end;
    }
  } else if (cursor < token_end) {
    token_end = cursor;
    token = token_prefix;
  }

  /* An option-value word such as --exit-node=host completes only the value
     after the equals sign, the way bash splits on the equals through
     COMP_WORDBREAKS. A command-position word is left whole, since an assignment
     such as name=value is its own token there. */
  if (!is_command && token.length >= 2 && token[0] == '-') {
    if (let const equals = token.find_character('='); equals.has_value()) {
      token_start = token_start + *equals + 1;
      token = line.substring_of_length(token_start, token_end - token_start);
    }
  }

  let const decoded_token =
      token.data == token_prefix.data && token.length == token_prefix.length
          ? steal(decoded_prefix)
          : utils::decode_shell_word(token, completion_allocator());
  let const has_open_quote = decoded_token.quote_character != 0;
  let const open_quote_content_token =
      has_open_quote ? decoded_token.text.view().substring(
                           decoded_token.open_quote_decoded_start)
                     : decoded_token.text.view();
  let const stage_token =
      decoded_token.has_shell_syntax ? decoded_token.text.view() : token;
  let const token_is_glob = !has_open_quote && decoded_token.glob_active.any();
  let const is_leading_variable_active =
      has_open_quote ? decoded_token.quote_character == '"' &&
                           !open_quote_content_token.is_empty() &&
                           open_quote_content_token[0] == '$'
                     : decoded_token.is_leading_variable_active;
  let const is_leading_tilde_active =
      !has_open_quote && decoded_token.is_leading_tilde_active;

  /* A command-position token holding a path separator completes against the
     filesystem rather than the command sets. */
  let const token_has_path_separator =
      os::has_directory_separator(decoded_token.text.view());
  LOG(Debug, "complete line '%.*s' cursor %zu token '%.*s' command %d",
      static_cast<int>(line.length), line.data, cursor,
      static_cast<int>(stage_token.length), stage_token.data,
      is_command ? 1 : 0);

  /* A glob word with the cursor right after it expands inline to its file
     matches, even in command position. */
  let const inline_glob = token_is_glob && cursor == token_end;

  let const command_word =
      is_command ? StringView{}
                 : command_word_of(line.substring_of_length(0, cursor));
  let const filesystem_filter =
      is_command             ? filesystem_entry_filter::RunnableOrDirectories
      : command_word == "cd" ? filesystem_entry_filter::DirectoriesOnly
                             : filesystem_entry_filter::All;
  const char *const extension_hint =
      is_command ? nullptr : file_extension_hint(command_word);

  let candidates = ArrayList<String>{arena};
  let descriptions = StringMap<String>{arena};
  let ghost_prefix = String{arena};
  usize ghost_candidate_count = 0;
  usize source_candidate_scan_count = 0;
  usize materialized_candidate_count = 0;
  let should_rebuild_shell_syntax_candidates = false;
  let should_ignore_common_prefix_case = false;

  let const is_posix_completion = context.mood() == mimic_mood::Posix;

  if (token_is_variable(open_quote_content_token) && is_leading_variable_active)
  {
    candidates = complete_variable(open_quote_content_token, context);
    if (has_open_quote) {
      token_start += decoded_token.open_quote_content_start;
      token_end = cursor;
    }
  } else if (token_is_tilde_user_prefix(stage_token) &&
             is_leading_tilde_active && !is_posix_completion)
  {
    candidates = complete_tilde_user(stage_token);
  } else if (inline_glob) {
    candidates = complete_glob(token, base_directory, filesystem_filter,
                               context, decoded_token);
    if (!candidates.is_empty()) {
      let joined = String{arena};
      for (usize i = 0; i < candidates.count(); i++) {
        if (i > 0) joined += ' ';
        let match = candidates[i].view();
        if (!match.is_empty() &&
            os::is_directory_separator(match[match.length - 1]))
        {
          match = match.substring_of_length(0, match.length - 1);
        }
        joined.append(match);
      }
      candidates.clear();
      candidates.push(steal(joined));
    } else if (is_command && !token_has_path_separator) {
      candidates = complete_command_names(
          stage_token,
          token_is_glob ? command_match_mode::Glob : command_match_mode::Prefix,
          context);
    }
  } else if (is_command && !token_has_path_separator) {
    /* An empty command token would enumerate every PATH command on each
       keystroke for the ghost, so command completion runs only once a prefix
       is typed. An explicit tab still lists them all. */
    if (!stage_token.is_empty() || for_listing) {
      if (for_listing) {
        should_ignore_common_prefix_case =
            !stage_token.is_empty() && !token_is_glob &&
            !utils::token_has_uppercase(stage_token);
        candidates =
            complete_command_names(stage_token,
                                   token_is_glob ? command_match_mode::Glob
                                                 : command_match_mode::Prefix,
                                   context);
      } else {
        let collector = complete_command_name_prefix(
            stage_token,
            token_is_glob ? command_match_mode::Glob
                          : command_match_mode::Prefix,
            context);
        ghost_candidate_count = collector.count();
        source_candidate_scan_count = collector.source_scans();
        materialized_candidate_count = collector.materialized();
        ghost_prefix = collector.take_prefix();
      }
      should_rebuild_shell_syntax_candidates = decoded_token.has_shell_syntax;
    }
  } else if (token_is_glob) {
    candidates = complete_glob(token, base_directory, filesystem_filter,
                               context, decoded_token);
  } else {
    /* The argument cascade runs in the bash and the default moods, the POSIX
       mood goes straight to files. The build tools answer before the man
       sources, so a recognized build tool in the current directory offers its
       targets even when a like-named subcommand man page exists. */
    Maybe<ArrayList<String>> from_stage = None;
    if (!is_posix_completion) {
      from_stage = complete_from_process_arguments(line, stage_token, mode);
      if (!from_stage.has_value())
        from_stage = complete_from_builtin_flags(line, stage_token, token_start,
                                                 context);
      if (!from_stage.has_value())
        from_stage = complete_from_spec(line, stage_token, cursor, mode,
                                        context, descriptions);
      if (!from_stage.has_value())
        from_stage = complete_from_tools_with_targets(
            line, stage_token, token_start, mode, context);
      if (!from_stage.has_value())
        from_stage = complete_from_man_subcommands(line, stage_token,
                                                   token_start, mode, context);
      if (!from_stage.has_value())
        from_stage = complete_from_manpage(line, stage_token, mode, context,
                                           descriptions);
      if (!from_stage.has_value())
        from_stage = complete_from_help_subcommands(
            line, stage_token, token_start, mode, context, descriptions);
      if (!from_stage.has_value())
        from_stage = complete_from_help(line, stage_token, token_start, mode,
                                        context, descriptions);
    }
    if (from_stage.has_value()) {
      candidates = steal(*from_stage);
      should_rebuild_shell_syntax_candidates = decoded_token.has_shell_syntax;
    } else if (for_listing || !split_path_token(decoded_token.text.view())
                                   .basename_part.is_empty())
    {
      /* A token ending in a slash has an empty basename, so the ghost listing
         runs only once a basename is typed. An explicit tab still lists. */
      if (for_listing) {
        let const basename =
            split_path_token(decoded_token.text.view()).basename_part;
        should_ignore_common_prefix_case =
            !basename.is_empty() && (!os::FILESYSTEM_IS_CASE_SENSITIVE ||
                                     !utils::token_has_uppercase(basename));
        candidates = complete_filesystem(
            token, base_directory, path_text_mode::ShellSyntax,
            filesystem_filter, context, &decoded_token);
      } else {
        let collector = complete_filesystem_prefix(
            token, base_directory, path_text_mode::ShellSyntax,
            filesystem_filter, context, &decoded_token);
        ghost_candidate_count = collector.count();
        source_candidate_scan_count = collector.source_scans();
        materialized_candidate_count = collector.materialized();
        ghost_prefix = collector.take_prefix();
      }
    }
  }

  if (should_rebuild_shell_syntax_candidates) {
    let rebuilt_descriptions = StringMap<String>{arena};
    if (descriptions.count() > 0)
      rebuilt_descriptions.reserve(descriptions.count());
    for (let &candidate : candidates) {
      let const description = descriptions.find(candidate.view());
      let rebuilt = rebuild_shell_syntax_candidate(token, decoded_token,
                                                   candidate.view());
      if (description != nullptr)
        rebuilt_descriptions.set(rebuilt.view(), description->view());
      candidate = steal(rebuilt);
    }
    descriptions = steal(rebuilt_descriptions);
    if (ghost_candidate_count > 0)
      ghost_prefix = rebuild_shell_syntax_candidate(token, decoded_token,
                                                    ghost_prefix.view());
  }

  let longest_common_prefix = String{arena};
  if (ghost_candidate_count > 0) {
    longest_common_prefix = steal(ghost_prefix);
  } else if (!candidates.is_empty()) {
    if (for_listing) {
      candidates.sort();

      let unique_candidates = ArrayList<String>{candidates.allocator()};
      unique_candidates.reserve(candidates.count());

      for (usize i = 0; i < candidates.count(); i++) {
        if (unique_candidates.is_empty() ||
            unique_candidates.back().view() != candidates[i].view())
          unique_candidates.push(steal(candidates[i]));
      }
      candidates = steal(unique_candidates);

      if (extension_hint != nullptr && stage_token.is_empty()) {
        candidates = keep_hinted_extension(steal(candidates),
                                           StringView{extension_hint});
      }
    }

    longest_common_prefix = compute_longest_common_prefix(
        candidates, should_ignore_common_prefix_case);

    if (for_listing && extension_hint != nullptr && !stage_token.is_empty()) {
      candidates =
          partition_by_extension(steal(candidates), StringView{extension_hint});
    }
  }

  let const candidate_count =
      ghost_candidate_count > 0 ? ghost_candidate_count : candidates.count();
  return completion_result{
      steal(candidates),
      steal(descriptions),
      steal(longest_common_prefix),
      candidate_count,
      source_candidate_scan_count,
      materialized_candidate_count,
      token_start + completion_offset,
      replacement_token_end + completion_offset,
      is_command,
  };
}

} /* namespace completion */

} /* namespace koshka */
