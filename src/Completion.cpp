/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file ranks completion candidates and coordinates command, filesystem,
 * glob, variable, user, and extension-aware completion. It owns common
 * matching and candidate construction used by contextual providers.
 */

#include "Completion.hpp"

#include "Arena.hpp"
#include "Builtin.hpp"
#include "CliColors.hpp"
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

using namespace internal;

BumpArena internal::COMPLETION_ARENA{};

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
  TieredCandidates() = default;

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
  ArrayList<String> by_tier[MATCH_TIER_COUNT]{
      ArrayList<String>{completion_allocator()},
      ArrayList<String>{completion_allocator()},
      ArrayList<String>{completion_allocator()}};
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
  enum class Selection : u8
  {
    CommonPrefix,
    FirstMatch,
  };

  explicit GhostPrefixCollector(Selection selection) : selection(selection) {}

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

    if (selection == Selection::FirstMatch) {
      match_count++;
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
  Selection selection;
};

template <typename Collector>
static fn
collect_command_names(StringView token, command_match_mode match_mode,
                      EvalContext &context, Collector &collector,
                      const ArrayList<StringView> *extra_command_names) throws
    -> void
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

  /* A keyword is resolved before a builtin of the same name, so it claims the
     name first and the builtin loop then skips it. */
  for (let const &keyword_name : keyword_names())
    do_add(keyword_name.view());

  for (let const &builtin_name : builtin_names())
    do_add(builtin_name.view());

  if (context.koshkit_utilities_are_reachable()) {
    for (const String &util_name : koshkit::util_names()) {
      if (!token_is_glob && !collector.allows_fuzzy_fallback() &&
          !utils::smart_case_prefix_matches(util_name.view(), token))
        continue;
      do_add(util_name.view());
    }
  }

  if (extra_command_names != nullptr) {
    for (let const name : *extra_command_names)
      do_add(name);
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

static fn complete_command_name_prefix(
    StringView token, command_match_mode match_mode, EvalContext &context,
    const ArrayList<StringView> *extra_command_names) throws
    -> GhostPrefixCollector
{
  let collector =
      GhostPrefixCollector{GhostPrefixCollector::Selection::FirstMatch};
  collect_command_names(token, match_mode, context, collector,
                        extra_command_names);
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
    if (prefix_length == 0) break;
    let const candidate = candidates[i].view();
    prefix_length = common_prefix_length(first, candidate, prefix_length,
                                         should_ignore_ascii_case);
  }
  return String{candidates.allocator(),
                first.substring_of_length(0, prefix_length)};
}

fn complete_command_names(
    StringView token, command_match_mode match_mode, EvalContext &context,
    const ArrayList<StringView> *extra_command_names) throws
    -> ArrayList<String>
{
  let collector = CommandListCollector{};

  LOG(Debug, "completing command position for token '%.*s'",
      static_cast<int>(token.length), token.data);

  collect_command_names(token, match_mode, context, collector,
                        extra_command_names);
  return collector.take();
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

struct filesystem_listing
{
  path_token parts;
  Path directory;
  const ArrayList<Path::directory_child> *entries;
};

struct eligible_filesystem_entry
{
  bool is_directory;
};

static fn open_filesystem_listing(const utils::decoded_shell_word &decoded_word,
                                  const Path &base_directory,
                                  EvalContext &context) throws
    -> Maybe<filesystem_listing>
{
  let const parts = split_path_token(decoded_word.text.view());
  let directory =
      resolve_listing_directory(parts.directory_part, base_directory, context,
                                decoded_word.is_leading_tilde_active,
                                decoded_word.is_leading_variable_active,
                                decoded_word.leading_variable_expansion_end);
  let const entries = utils::read_directory_cached(
      directory, utils::directory_validation::Cached,
      utils::directory_listing_order::FoldedName);
  if (entries == nullptr) return None;

  return filesystem_listing{parts, steal(directory), entries};
}

static fn check_filesystem_entry(const filesystem_listing &listing,
                                 const Path::directory_child &entry,
                                 filesystem_entry_filter filter) throws
    -> Maybe<eligible_filesystem_entry>
{
  let const name = entry.name.view();
  if (!name.is_empty() && name[0] == '.' &&
      (listing.parts.basename_part.is_empty() ||
       listing.parts.basename_part[0] != '.'))
  {
    return None;
  }

  let const is_directory =
      utils::directory_entry_kind(listing.directory, entry) ==
      Path::entry_kind::Directory;
  if (filter == filesystem_entry_filter::DirectoriesOnly && !is_directory)
    return None;
  if (filter == filesystem_entry_filter::RunnableOrDirectories &&
      !is_directory && !entry_is_executable(listing.directory, name))
  {
    return None;
  }

  return eligible_filesystem_entry{is_directory};
}

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
  let listing = open_filesystem_listing(decoded_word, base_directory, context);
  if (!listing.has_value()) return;
  let const &parts = listing->parts;
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

  let const do_add_entry = [&](const Path::directory_child &entry) throws {
    let const name = entry.name.view();
    collector.note_source_candidate();
    let const tier =
        candidate_match(parts.basename_part, name, is_case_sensitive);
    if (!tier.has_value()) return;

    let const eligible_entry = check_filesystem_entry(*listing, entry, filter);
    if (!eligible_entry.has_value()) return;

    let candidate = build_filesystem_candidate(
        parts.directory_part, raw_directory_part, name,
        eligible_entry->is_directory, text_mode, token, decoded_word);
    collector.add(candidate.view(), *tier);
  };

  let entry_position = utils::directory_entry_name_lower_bound(
      *listing->entries, parts.basename_part);
  while (
      entry_position < listing->entries->count() &&
      utils::directory_entry_name_has_casefold_prefix(
          (*listing->entries)[entry_position].name.view(), parts.basename_part))
  {
    do_add_entry((*listing->entries)[entry_position]);
    entry_position++;
  }
  if (collector.has_prefix() || !collector.allows_fuzzy_fallback()) return;

  for (let const &entry : *listing->entries)
    if (!utils::directory_entry_name_has_casefold_prefix(entry.name.view(),
                                                         parts.basename_part))
      do_add_entry(entry);
}

template <typename Collector>
static fn complete_filesystem_with(
    StringView token, const Path &base_directory, path_text_mode text_mode,
    filesystem_entry_filter filter, EvalContext &context, Collector collector,
    const utils::decoded_shell_word *decoded = nullptr) throws -> Collector
{
  let decoded_storage = utils::decoded_shell_word{completion_allocator()};
  if (decoded == nullptr) {
    if (text_mode == path_text_mode::Literal)
      decoded_storage.text.append(token);
    else
      decoded_storage = utils::decode_shell_word(token, completion_allocator());
    decoded = &decoded_storage;
  }
  collect_filesystem_matches(token, *decoded, base_directory, text_mode, filter,
                             context, collector);

  return collector;
}

static fn
complete_filesystem(StringView token, const Path &base_directory,
                    path_text_mode text_mode, filesystem_entry_filter filter,
                    EvalContext &context,
                    const utils::decoded_shell_word *decoded = nullptr) throws
    -> ArrayList<String>
{
  let collector = complete_filesystem_with<CommandListCollector>(
      token, base_directory, text_mode, filter, context, CommandListCollector{},
      decoded);
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
  return complete_filesystem_with<GhostPrefixCollector>(
      token, base_directory, text_mode, filter, context,
      GhostPrefixCollector{GhostPrefixCollector::Selection::FirstMatch},
      decoded);
}

/* Only the trailing component is globbed. */
static fn complete_glob(StringView token, const Path &base_directory,
                        filesystem_entry_filter filter, EvalContext &context,
                        const utils::decoded_shell_word &decoded_word) throws
    -> ArrayList<String>
{
  let candidates = ArrayList<String>{completion_allocator()};
  let listing = open_filesystem_listing(decoded_word, base_directory, context);
  if (!listing.has_value()) return candidates;
  let const &parts = listing->parts;

  LOG(Debug, "resolving glob token '%.*s'", static_cast<int>(token.length),
      token.data);

  let glob_active = Bitset{completion_allocator()};
  glob_active.reserve(parts.basename_part.length);
  for (usize position = parts.directory_part.length;
       position < decoded_word.glob_active.count(); position++)
    glob_active.push(decoded_word.glob_active[position]);
  let normalized_pattern = String{completion_allocator()};
  let match_pattern = parts.basename_part;
  if (!os::FILESYSTEM_IS_CASE_SENSITIVE) {
    normalized_pattern.assign_lowercase_ascii(match_pattern);
    match_pattern = normalized_pattern.view();
  }
  let candidate_name = String{completion_allocator()};

  for (let const &entry : *listing->entries) {
    let const name = entry.name.view();

    let match_name = name;
    if (!os::FILESYSTEM_IS_CASE_SENSITIVE) {
      candidate_name.assign_lowercase_ascii(match_name);
      match_name = candidate_name.view();
    }

    if (!utils::glob_matches(match_pattern, match_name, glob_active, 0)) {
      continue;
    }

    let const eligible_entry = check_filesystem_entry(*listing, entry, filter);
    if (!eligible_entry.has_value()) continue;

    let const raw_directory_part =
        decoded_word.raw_directory_end > 0
            ? token.substring_of_length(0, decoded_word.raw_directory_end)
            : parts.directory_part;
    let candidate = build_filesystem_candidate(
        parts.directory_part, raw_directory_part, name,
        eligible_entry->is_directory, path_text_mode::ShellSyntax, token,
        decoded_word);

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
    if (!seen.add(name)) return;

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
  hint_list.for_each_ascii_whitespace_word(
      [&](StringView extension) throws { extensions.push(extension); });
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

fn complete(StringView line, usize cursor, EvalContext &context,
            const Path &base_directory, completion_mode mode,
            const ArrayList<StringView> *extra_command_names,
            bool should_complete_external_arguments_in_posix) throws
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
  line = line.substring_of_length(0, replacement_token_end);
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
    if (has_open_quote) token_start += decoded_token.open_quote_content_start;
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
          context, extra_command_names);
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
                                   context, extra_command_names);
      } else {
        let collector = complete_command_name_prefix(
            stage_token,
            token_is_glob ? command_match_mode::Glob
                          : command_match_mode::Prefix,
            context, extra_command_names);
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
      from_stage =
          complete_from_process_arguments(line, stage_token, token_start, mode);
      if (!from_stage.has_value())
        from_stage = complete_from_builtin_flags(line, stage_token, token_start,
                                                 context);
      if (!from_stage.has_value())
        from_stage = complete_from_spec(line, stage_token, cursor, mode,
                                        context, descriptions);
      if (!from_stage.has_value())
        from_stage = complete_from_tools_with_targets(
            line, stage_token, token_start, mode, context);
    }
    if (!from_stage.has_value() &&
        (!is_posix_completion || should_complete_external_arguments_in_posix))
    {
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
