/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements program resolver execution helpers. It provides
 * shared low-level operations for commands, streams, numbers, globbing, and
 * resolution without duplicating policy owners.
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

struct cached_directory_listing
{
  u64 device_id{0};
  u64 file_id{0};
  i64 modification_time{0};
  u32 modification_nanoseconds{0};
  i64 change_time{0};
  u32 change_nanoseconds{0};
  u64 size{0};
  u64 generation{0};
  usize alias_count{0};
  bool has_file_identity{false};
  bool is_sorted{false};
  ArrayList<Path::directory_child> entries{heap_allocator()};
};

struct cached_directory_alias
{
  usize listing_position{0};
  u64 validation_epoch{0};
  u64 observed_generation{0};
};

static StringMap<cached_directory_alias> DIR_LISTING_ALIASES{heap_allocator()};
static StringMap<usize> DIR_LISTING_IDENTITIES{heap_allocator()};
static ArrayList<cached_directory_listing> DIR_LISTINGS{heap_allocator()};
static ArrayList<usize> FREE_DIR_LISTING_POSITIONS{heap_allocator()};
static u64 DIRECTORY_VALIDATION_EPOCH = 0;
static u64 DIRECTORY_LISTING_GENERATION = 0;
#if !defined NDEBUG
static usize DEBUG_DIRECTORY_STAT_COUNT = 0;
static usize DEBUG_DIRECTORY_READ_COUNT = 0;
static usize DEBUG_DIRECTORY_SORT_COUNT = 0;
static usize DEBUG_EXECUTABLE_PROBE_COUNT = 0;
static usize DEBUG_PROGRAM_PATH_CANDIDATE_COUNT = 0;
#endif

static fn clear_directory_listing_cache() throws -> void
{
  DIR_LISTING_ALIASES.clear();
  DIR_LISTING_IDENTITIES.clear();
  DIR_LISTINGS.clear();
  FREE_DIR_LISTING_POSITIONS.clear();
}

static fn begin_directory_validation_epoch() wontthrow -> void;

fn ProgramResolver::cache_resolved_path(StringView name, const Path &full_path,
                                        os::program_extension extension,
                                        bool is_bare_result) throws -> void
{
  let &entry = m_execution_cache.get_or_create(name, CacheEntry{});
  for (usize position = 0; position < entry.paths.count(); position++) {
    let &cached = entry.paths[position];
    if (cached.extension != extension) continue;
    cached.path = full_path;
    if (is_bare_result) entry.bare_path_position = position;
    return;
  }

  if (is_bare_result) {
    entry.bare_path_position = entry.paths.count();
  }
  entry.paths.push({full_path, extension});
}

pure fn ProgramResolver::find_cached_program_path(
    const CacheEntry &entry,
    os::program_extension wanted_extension) const wontthrow -> const Path *
{
  if (wanted_extension == os::program_extension::None) {
    if (!entry.bare_path_position.has_value()) return nullptr;
    return &entry.paths[*entry.bare_path_position].path;
  }

  for (let const &cached : entry.paths) {
    if (cached.extension != wanted_extension) continue;
    return &cached.path;
  }

  return nullptr;
}

static fn directory_identity_key(u64 device_id, u64 file_id) throws -> String
{
  let key = String{heap_allocator()};
  key.append(StringView{reinterpret_cast<const char *>(&device_id),
                        sizeof(device_id)});
  key.append(
      StringView{reinterpret_cast<const char *>(&file_id), sizeof(file_id)});
  return key;
}

static fn release_directory_listing_alias(usize position) throws -> void
{
  let &listing = DIR_LISTINGS[position];
  ASSERT(listing.alias_count > 0);
  listing.alias_count--;
  if (listing.alias_count != 0) return;

  if (listing.has_file_identity) {
    let const key = directory_identity_key(listing.device_id, listing.file_id);
    DIR_LISTING_IDENTITIES.erase(key.view());
  }
  listing = cached_directory_listing{};
  FREE_DIR_LISTING_POSITIONS.push(position);
}

static fn set_directory_listing_alias(StringView key, usize position) throws
    -> void
{
  let *existing = DIR_LISTING_ALIASES.find(key);
  if (existing != nullptr && existing->listing_position == position) {
    existing->validation_epoch = DIRECTORY_VALIDATION_EPOCH;
    existing->observed_generation = DIR_LISTINGS[position].generation;
    return;
  }

  if (existing != nullptr)
    release_directory_listing_alias(existing->listing_position);
  DIR_LISTINGS[position].alias_count++;
  DIR_LISTING_ALIASES.set(
      key, cached_directory_alias{position, DIRECTORY_VALIDATION_EPOCH,
                                  DIR_LISTINGS[position].generation});
}

static fn allocate_directory_listing_position() throws -> usize
{
  if (!FREE_DIR_LISTING_POSITIONS.is_empty()) {
    let const position = FREE_DIR_LISTING_POSITIONS.back();
    FREE_DIR_LISTING_POSITIONS.pop_back();
    return position;
  }

  let const position = DIR_LISTINGS.count();
  DIR_LISTINGS.push({});
  return position;
}

static pure fn directory_entry_folded_name_is_less(StringView left,
                                                   StringView right) wontthrow
    -> bool
{
  let const shared_length =
      left.length < right.length ? left.length : right.length;
  for (usize position = 0; position < shared_length; position++) {
    let const left_byte = ascii_to_lower(left[position]);
    let const right_byte = ascii_to_lower(right[position]);
    if (left_byte != right_byte) return left_byte < right_byte;
  }
  if (left.length != right.length) return left.length < right.length;

  return false;
}

static pure fn directory_entry_name_is_less(StringView left,
                                            StringView right) wontthrow -> bool
{
  if (directory_entry_folded_name_is_less(left, right)) return true;
  if (directory_entry_folded_name_is_less(right, left)) return false;

  return left < right;
}

pure fn directory_entry_name_lower_bound(
    const ArrayList<Path::directory_child> &entries, StringView name) wontthrow
    -> usize
{
  usize lower = 0;
  usize upper = entries.count();
  while (lower < upper) {
    let const middle = lower + (upper - lower) / 2;
    if (directory_entry_folded_name_is_less(entries[middle].name.view(), name))
      lower = middle + 1;
    else
      upper = middle;
  }

  return lower;
}

pure fn directory_entry_name_has_casefold_prefix(StringView name,
                                                 StringView prefix) wontthrow
    -> bool
{
  if (name.length < prefix.length) return false;

  for (usize position = 0; position < prefix.length; position++)
    if (ascii_to_lower(name[position]) != ascii_to_lower(prefix[position]))
      return false;

  return true;
}

fn read_directory_cached(const Path &directory, directory_validation validation,
                         directory_listing_order order) throws
    -> const ArrayList<Path::directory_child> *
{
  let const do_apply_order =
      [&](cached_directory_listing &listing)
          throws -> const ArrayList<Path::directory_child> * {
    if (order == directory_listing_order::FoldedName && !listing.is_sorted) {
#if !defined NDEBUG
      DEBUG_DIRECTORY_SORT_COUNT++;
#endif
      listing.entries.sort([](const Path::directory_child &left,
                              const Path::directory_child &right) {
        return directory_entry_name_is_less(left.name.view(),
                                            right.name.view());
      });
      listing.is_sorted = true;
    }

    return &listing.entries;
  };

  let const key = directory.text().view();
  let *alias = DIR_LISTING_ALIASES.find(key);
  if (validation == directory_validation::Cached && alias != nullptr &&
      alias->validation_epoch == DIRECTORY_VALIDATION_EPOCH &&
      alias->observed_generation ==
          DIR_LISTINGS[alias->listing_position].generation)
  {
    return do_apply_order(DIR_LISTINGS[alias->listing_position]);
  }

#if !defined NDEBUG
  DEBUG_DIRECTORY_STAT_COUNT++;
#endif
  os::file_status status{};
  let const has_status = os::stat_path_following(key, status);
  let physical_position = Maybe<usize>{};
  if (has_status && status.has_file_identity) {
    let const identity_key =
        directory_identity_key(status.device_id, status.file_id);
    if (let const *position = DIR_LISTING_IDENTITIES.find(identity_key.view());
        position != nullptr)
      physical_position = *position;
  }

  if (physical_position.has_value()) {
    let &cached = DIR_LISTINGS[*physical_position];
    if (cached.modification_time == status.modification_time &&
        cached.modification_nanoseconds == status.modification_nanoseconds &&
        cached.change_time == status.change_time &&
        cached.change_nanoseconds == status.change_nanoseconds &&
        cached.size == status.size)
    {
      set_directory_listing_alias(key, *physical_position);
      return do_apply_order(cached);
    }
  }

#if !defined NDEBUG
  DEBUG_DIRECTORY_READ_COUNT++;
#endif
  let entries = Path::read_directory_typed(directory);
  if (!entries.has_value()) return nullptr;

  for (let &child : *entries) {
    if (child.kind != Path::entry_kind::Unknown) continue;

    let full_path = directory.clone();
    full_path.push_component(child.name.view());
    if (full_path.is_directory())
      child.kind = Path::entry_kind::Directory;
    else if (full_path.is_regular_file())
      child.kind = Path::entry_kind::Regular;
    else
      child.kind = Path::entry_kind::Other;
  }

  cached_directory_listing fresh{};
  fresh.device_id = has_status ? status.device_id : 0;
  fresh.file_id = has_status ? status.file_id : 0;
  fresh.modification_time = has_status ? status.modification_time : 0;
  fresh.modification_nanoseconds =
      has_status ? status.modification_nanoseconds : 0;
  fresh.change_time = has_status ? status.change_time : 0;
  fresh.change_nanoseconds = has_status ? status.change_nanoseconds : 0;
  fresh.size = has_status ? status.size : 0;
  fresh.generation = ++DIRECTORY_LISTING_GENERATION;
  fresh.has_file_identity = has_status && status.has_file_identity;
  fresh.entries = steal(*entries);
  if (physical_position.has_value()) {
    fresh.alias_count = DIR_LISTINGS[*physical_position].alias_count;
    DIR_LISTINGS[*physical_position] = steal(fresh);
  } else {
    physical_position = allocate_directory_listing_position();
    DIR_LISTINGS[*physical_position] = steal(fresh);
    if (DIR_LISTINGS[*physical_position].has_file_identity) {
      let const identity_key =
          directory_identity_key(DIR_LISTINGS[*physical_position].device_id,
                                 DIR_LISTINGS[*physical_position].file_id);
      DIR_LISTING_IDENTITIES.set(identity_key.view(), *physical_position);
    }
  }
  set_directory_listing_alias(key, *physical_position);

  return do_apply_order(DIR_LISTINGS[*physical_position]);
}

fn directory_entry_kind(const Path &directory,
                        const Path::directory_child &entry) throws
    -> Path::entry_kind
{
  if (entry.kind != Path::entry_kind::Symlink) return entry.kind;

  let full_path = directory.clone();
  full_path.push_component(entry.name.view());
  if (full_path.is_directory()) return Path::entry_kind::Directory;
  if (full_path.is_regular_file()) return Path::entry_kind::Regular;
  return Path::entry_kind::Other;
}

pure fn directory_listing_generation(const Path &directory) wontthrow -> u64
{
  let const *alias = DIR_LISTING_ALIASES.find(directory.text().view());
  if (alias == nullptr) return 0;
  return DIR_LISTINGS[alias->listing_position].generation;
}

static fn sort_and_deduplicate_names(ArrayList<String> &names) throws -> void
{
  let positions = ArrayList<usize>{names.allocator()};
  positions.reserve(names.count());
  for (usize position = 0; position < names.count(); position++)
    positions.push(position);
  positions.sort([&](usize left, usize right) {
    return names[left].view() < names[right].view();
  });

  let sorted_names = ArrayList<String>{names.allocator()};
  sorted_names.reserve(names.count());
  for (let const position : positions)
    if (sorted_names.is_empty() ||
        sorted_names.back().view() != names[position].view())
      sorted_names.push(steal(names[position]));
  names = steal(sorted_names);
}

static fn begin_directory_validation_epoch() wontthrow -> void
{
  DIRECTORY_VALIDATION_EPOCH++;
}

ProgramResolver::ProgramResolver()
    : m_path(os::get_environment_variable("PATH"))
{}

ProgramResolver::ProgramResolver(Maybe<String> path) : m_path(steal(path)) {}

fn ProgramResolver::mark_command_name_indexes_stale() wontthrow -> void
{
  m_validated_prefix.clear();
  m_command_names_are_valid = false;
  m_command_names_validation_epoch = 0;
  m_prefix_validation_epoch = 0;
}

fn ProgramResolver::clear_command_name_indexes() wontthrow -> void
{
  m_command_names.clear();
  m_regular_names.clear();
  mark_command_name_indexes_stale();
}

fn ProgramResolver::mark_derived_indexes_stale() wontthrow -> void
{
  mark_command_name_indexes_stale();
  m_path_directory_generations.clear();
  m_path_directory_generations_are_valid = false;
  m_path_directories_validation_epoch = 0;
}

fn ProgramResolver::clear_derived_indexes() wontthrow -> void
{
  m_command_names.clear();
  m_regular_names.clear();
  mark_derived_indexes_stale();
}

fn ProgramResolver::assign_path(Maybe<String> path) throws -> void
{
  if (m_path.has_value() == path.has_value() &&
      (!m_path.has_value() || m_path->view() == path->view()))
  {
    m_execution_cache.clear();
    return;
  }

  let path_dirs = path.has_value() ? split_path_dirs(path->view())
                                   : ArrayList<String>{heap_allocator()};
  let index_path_dirs = deduplicate_path_dirs(path_dirs);
  let const path_search_changed = get_index_path_dirs() != index_path_dirs;
  m_path = steal(path);
  m_path_dirs = steal(path_dirs);
  m_index_path_dirs = steal(index_path_dirs);
  m_path_dirs_are_valid = true;
  m_execution_cache.clear();
  if (!path_search_changed) return;

  clear_derived_indexes();
}

fn ProgramResolver::restore_path(Maybe<String> path) throws -> void
{
  assign_path(steal(path));
}

fn ProgramResolver::invalidate() throws -> void
{
  m_execution_cache.clear();
  clear_derived_indexes();
  clear_directory_listing_cache();
}

fn ProgramResolver::remember_path(StringView name, const Path &path) throws
    -> void
{
  cache_resolved_path(name, path, os::program_extension::None, true);
}

fn ProgramResolver::split_path_dirs(StringView path) throws -> ArrayList<String>
{
  let directories = ArrayList<String>{heap_allocator()};
  let current = String{heap_allocator()};

  for (usize position = 0; position < path.length; position++) {
    let const byte = path[position];
    if (byte == os::PATH_DELIMITER) {
      directories.push(current.is_empty() ? String{"."}
                                          : String{current.view()});
      current.clear();
    } else {
      current.push(byte);
    }
  }
  directories.push(current.is_empty() ? String{"."} : String{current.view()});

  return directories;
}

fn ProgramResolver::deduplicate_path_dirs(
    const ArrayList<String> &directories) throws -> ArrayList<String>
{
  let unique_directories = ArrayList<String>{heap_allocator()};
  for (let const &directory : directories)
    if (!unique_directories.find(directory.view()).has_value())
      unique_directories.push(String{directory.view()});

  return unique_directories;
}

fn ProgramResolver::get_path_dirs() throws -> const ArrayList<String> &
{
  if (m_path_dirs_are_valid) return m_path_dirs;

  m_path_dirs = m_path.has_value() ? split_path_dirs(m_path->view())
                                   : ArrayList<String>{heap_allocator()};
  m_index_path_dirs = deduplicate_path_dirs(m_path_dirs);
  m_path_dirs_are_valid = true;

  return m_path_dirs;
}

fn ProgramResolver::get_index_path_dirs() throws -> const ArrayList<String> &
{
  unused(get_path_dirs());

  return m_index_path_dirs;
}

fn ProgramResolver::working_directory_changed() throws -> void
{
  begin_directory_validation_epoch();

  for (let const &directory : get_index_path_dirs())
    if (!Path{directory.view()}.is_absolute()) {
      m_execution_cache.clear();
      mark_derived_indexes_stale();
      return;
    }
}

fn ProgramResolver::refresh_path_directory_generations() throws -> void
{
  m_path_directory_generations.clear();
  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Validate);
    m_path_directory_generations.push(
        entries == nullptr ? 0 : directory_listing_generation(directory));
  }
  m_path_directory_generations_are_valid = true;
  m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
}

fn ProgramResolver::rebuild_path_command_index(CompletionRefresh refresh) throws
    -> void
{
  if (refresh == CompletionRefresh::Fresh) {
    clear_derived_indexes();
  } else {
    ASSERT(m_path_directory_generations_are_valid);
    clear_command_name_indexes();
  }
  if (!m_path.has_value()) {
    m_command_names_are_valid = true;
    m_command_names_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
    m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
    m_prefix_validation_epoch = 0;
    m_validated_prefix.clear();
    return;
  }

  if (refresh == CompletionRefresh::Fresh) refresh_path_directory_generations();

  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Cached);
    if (entries == nullptr) continue;

    for (let const &entry : *entries) {
      let full_path = directory.clone();
      full_path.push_component(entry.name.view());
      if (entry.kind == Path::entry_kind::Symlink && !full_path.exists()) {
        continue;
      }
      if (directory_entry_kind(directory, entry) != Path::entry_kind::Regular)
        continue;

      let normalized_name = entry.name.clone();
      let const name_info = os::normalize_program_name(normalized_name);
      let const stem =
          normalized_name.substring_of_length(0, name_info.stem_length);
      m_regular_names.push(String{normalized_name.view()});
      if (stem.length != normalized_name.length())
        m_regular_names.push(String{stem});

#if !defined NDEBUG
      DEBUG_EXECUTABLE_PROBE_COUNT++;
#endif
      if (!full_path.is_executable()) continue;
      if (stem.length != entry.name.length())
        m_command_names.push(String{stem});
      m_command_names.push(steal(normalized_name));
    }
  }

  sort_and_deduplicate_names(m_command_names);
  sort_and_deduplicate_names(m_regular_names);
  m_command_names_are_valid = true;
  m_command_names_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
  m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
  m_prefix_validation_epoch = 0;
  m_validated_prefix.clear();
}

fn ProgramResolver::initialize_path_map() throws -> void
{
  LOG(Info, "scanning %zu unique PATH directories to seed the program cache",
      get_index_path_dirs().count());
  rebuild_path_command_index(CompletionRefresh::Fresh);
}

fn ProgramResolver::begin_explicit_completion(CompletionRefresh refresh) throws
    -> void
{
  if (m_explicit_completion_depth == 0) {
    switch (refresh) {
    case CompletionRefresh::Cached:
      m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
      m_command_names_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
      m_prefix_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
      break;
    case CompletionRefresh::Fresh: begin_directory_validation_epoch(); break;
    }
  }
  m_explicit_completion_depth++;
}

fn ProgramResolver::end_explicit_completion() wontthrow -> void
{
  ASSERT(m_explicit_completion_depth > 0);
  m_explicit_completion_depth--;
}

#if !defined NDEBUG
pure fn debug_directory_stat_count() wontthrow -> usize
{
  return DEBUG_DIRECTORY_STAT_COUNT;
}

pure fn debug_directory_read_count() wontthrow -> usize
{
  return DEBUG_DIRECTORY_READ_COUNT;
}

pure fn debug_directory_sort_count() wontthrow -> usize
{
  return DEBUG_DIRECTORY_SORT_COUNT;
}

pure fn debug_executable_probe_count() wontthrow -> usize
{
  return DEBUG_EXECUTABLE_PROBE_COUNT;
}

pure fn debug_program_path_candidate_count() wontthrow -> usize
{
  return DEBUG_PROGRAM_PATH_CANDIDATE_COUNT;
}
#endif

fn ProgramResolver::validate_path_directory_generations() throws -> bool
{
  if (m_path_directory_generations_are_valid &&
      m_path_directories_validation_epoch == DIRECTORY_VALIDATION_EPOCH)
    return false;

  bool did_change =
      !m_path_directory_generations_are_valid ||
      m_path_directory_generations.count() != get_index_path_dirs().count();
  let observed_generations = ArrayList<u64>{heap_allocator()};
  observed_generations.reserve(get_index_path_dirs().count());
  usize directory_position = 0;
  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Validate);
    let const generation =
        entries == nullptr ? 0 : directory_listing_generation(directory);
    if (directory_position >= m_path_directory_generations.count() ||
        m_path_directory_generations[directory_position] != generation)
    {
      did_change = true;
    }
    observed_generations.push(generation);
    directory_position++;
  }
  m_path_directory_generations = steal(observed_generations);
  m_path_directory_generations_are_valid = true;
  m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;

  return did_change;
}

fn ProgramResolver::revalidate_command_prefix(StringView prefix) throws -> void
{
  clear_command_name_indexes();

  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Cached);
    if (entries == nullptr) continue;

    for (let const &entry : *entries) {
      let normalized_name = entry.name.clone();
      let const name_info = os::normalize_program_name(normalized_name);
      let const stem =
          normalized_name.substring_of_length(0, name_info.stem_length);
      let const full_name_matches =
          smart_case_prefix_matches(normalized_name.view(), prefix);
      let const stem_matches = stem.length != normalized_name.length() &&
                               smart_case_prefix_matches(stem, prefix);
      if (!full_name_matches && !stem_matches) continue;

      let full_path = directory.clone();
      full_path.push_component(entry.name.view());
      if (entry.kind == Path::entry_kind::Symlink && !full_path.exists()) {
        continue;
      }
      if (directory_entry_kind(directory, entry) != Path::entry_kind::Regular)
        continue;

      if (stem_matches) m_regular_names.push(String{stem});
      if (full_name_matches)
        m_regular_names.push(String{normalized_name.view()});

#if !defined NDEBUG
      DEBUG_EXECUTABLE_PROBE_COUNT++;
#endif
      if (!full_path.is_executable()) continue;
      if (stem_matches) m_command_names.push(String{stem});
      if (full_name_matches) m_command_names.push(steal(normalized_name));
    }
  }

  sort_and_deduplicate_names(m_command_names);
  sort_and_deduplicate_names(m_regular_names);
  m_validated_prefix = String{prefix};
  m_prefix_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
}

fn ProgramResolver::prepare_complete_path_cache(
    StringView validation_prefix, ValidationScope validation_scope) throws
    -> void
{
  if (!m_command_names_are_valid && !m_path_directory_generations_are_valid &&
      m_explicit_completion_depth == 0)
    return;

  if (!m_command_names_are_valid) {
    if (validation_scope == ValidationScope::All ||
        validation_prefix.is_empty())
    {
      if (m_path_directory_generations_are_valid &&
          m_path_directories_validation_epoch == DIRECTORY_VALIDATION_EPOCH)
        rebuild_path_command_index(CompletionRefresh::Cached);
      else
        initialize_path_map();
      return;
    }

    if (!m_path_directory_generations_are_valid) {
      refresh_path_directory_generations();
      m_execution_cache.clear();
    } else if (m_path_directories_validation_epoch !=
               DIRECTORY_VALIDATION_EPOCH)
    {
      if (validate_path_directory_generations()) {
        m_execution_cache.clear();
        clear_command_name_indexes();
      }
    }

    if (!m_validated_prefix.is_empty() &&
        m_prefix_validation_epoch == DIRECTORY_VALIDATION_EPOCH &&
        validation_prefix.starts_with(m_validated_prefix.view()))
      return;

    revalidate_command_prefix(validation_prefix);
    return;
  }
  if (m_explicit_completion_depth == 0) return;
  if (m_command_names_validation_epoch == DIRECTORY_VALIDATION_EPOCH) return;

  if (validate_path_directory_generations()) m_execution_cache.clear();
  if (validation_scope == ValidationScope::Prefix &&
      !validation_prefix.is_empty())
  {
    m_command_names_are_valid = false;
    revalidate_command_prefix(validation_prefix);
    return;
  }
  rebuild_path_command_index(CompletionRefresh::Cached);
}

fn ProgramResolver::get_command_names(StringView validation_prefix,
                                      ValidationScope validation_scope) throws
    -> const ArrayList<String> &
{
  prepare_complete_path_cache(validation_prefix, validation_scope);

  return m_command_names;
}

pure fn ProgramResolver::command_name_lower_bound_in(
    const ArrayList<String> &names, StringView name) const wontthrow -> usize
{
  usize lower = 0;
  usize upper = names.count();
  while (lower < upper) {
    let const middle = lower + (upper - lower) / 2;
    if (names[middle].view() < name)
      lower = middle + 1;
    else
      upper = middle;
  }

  return lower;
}

pure fn ProgramResolver::get_command_name_lower_bound(
    StringView name) const wontthrow -> usize
{
  return command_name_lower_bound_in(m_command_names, name);
}

fn ProgramResolver::command_name_has_prefix(StringView prefix) throws -> bool
{
  let normalized_prefix = String{prefix};
  unused(os::normalize_program_name(normalized_prefix));
  prepare_complete_path_cache(normalized_prefix.view(),
                              ValidationScope::Prefix);
  for (let const &name : m_command_names)
    if (smart_case_prefix_matches(name.view(), normalized_prefix.view()))
      return true;

  return false;
}

pure fn ProgramResolver::has_valid_command_names() const wontthrow -> bool
{
  return m_command_names_are_valid;
}

fn ProgramResolver::get_status(StringView name, StatusLookup lookup) throws
    -> Status
{
  if (lookup == StatusLookup::Authoritative) {
    let const paths = search(name, SearchMode::First, Requirement::Execution,
                             CachePolicy::Bypass);
    if (paths.is_empty()) return Status::Missing;
    if (paths[0].is_executable()) return Status::Runnable;
    return Status::Blocked;
  }

  let normalized_name = String{name};
  unused(os::normalize_program_name(normalized_name));
  if (!m_command_names_are_valid && !normalized_name.is_empty()) {
    prepare_complete_path_cache(normalized_name.substring_of_length(0, 1),
                                ValidationScope::Prefix);
  }

  let const runnable_position =
      command_name_lower_bound_in(m_command_names, normalized_name.view());
  if (runnable_position < m_command_names.count() &&
      m_command_names[runnable_position].view() == normalized_name.view())
    return Status::Runnable;
  let const regular_position =
      command_name_lower_bound_in(m_regular_names, normalized_name.view());
  if (regular_position < m_regular_names.count() &&
      m_regular_names[regular_position].view() == normalized_name.view())
    return Status::Blocked;

  return Status::Missing;
}

fn ProgramResolver::resolve_along_path(StringView program_name,
                                       SearchMode search_mode,
                                       Requirement requirement,
                                       CachePolicy cache_policy,
                                       Maybe<StringView> path_override) throws
    -> ArrayList<Path>
{
  if (!path_override.has_value() && !m_path.has_value()) {
    return ArrayList<Path>{heap_allocator()};
  }

  LOG(Debug, "statting candidates for '%.*s' along PATH%s",
      static_cast<int>(program_name.length), program_name.data,
      search_mode == SearchMode::All ? ", collecting every match" : "");

  let result = ArrayList<Path>{heap_allocator()};

  let normalized_name = String{program_name};
  let const name_info = os::normalize_program_name(normalized_name);
  let const key = normalized_name.substring_of_length(0, name_info.stem_length);
  let blocked = Maybe<CachedPath>{};
  let override_directories = ArrayList<String>{heap_allocator()};
  const ArrayList<String> *directories;
  if (path_override.has_value()) {
    override_directories = split_path_dirs(*path_override);
    directories = &override_directories;
  } else {
    directories = &get_path_dirs();
  }

  for (let const &dir_string : *directories) {
    let const directory = Path{dir_string.view()};

    let full_path = directory.clone();
    full_path.push_component(program_name);

    if (name_info.extension == os::program_extension::None) {
      for (let const &suffix : os::PROGRAM_SUFFIXES) {
        let suffixed_path = Maybe<Path>{};
        if (!suffix.text.is_empty())
          suffixed_path = Path{(full_path.text() + suffix.text).view()};
        let const &try_path =
            suffixed_path.has_value() ? *suffixed_path : full_path;

#if !defined NDEBUG
        DEBUG_PROGRAM_PATH_CANDIDATE_COUNT++;
#endif
        if (!try_path.is_regular_file()) continue;
        let const is_runnable = try_path.is_executable();
        let const is_match = requirement == Requirement::Regular || is_runnable;
        if (search_mode == SearchMode::All) {
          if (is_match) result.push(try_path);
          continue;
        }
        if (is_match) {
          result.push(try_path);
          if ((cache_policy == CachePolicy::Remember ||
               cache_policy == CachePolicy::RememberUnchecked) &&
              is_runnable)
          {
            cache_resolved_path(key, try_path, suffix.extension, true);
          }
          return result;
        }
        if (requirement == Requirement::Execution && !blocked.has_value()) {
          blocked = CachedPath{try_path, suffix.extension};
        }
      }
    } else {
#if !defined NDEBUG
      DEBUG_PROGRAM_PATH_CANDIDATE_COUNT++;
#endif
      if (!full_path.is_regular_file()) continue;
      let const is_runnable = full_path.is_executable();
      let const is_match = requirement == Requirement::Regular || is_runnable;
      if (search_mode == SearchMode::All) {
        if (is_match) result.push(full_path);
      } else if (is_match) {
        result.push(full_path);
        if ((cache_policy == CachePolicy::Remember ||
             cache_policy == CachePolicy::RememberUnchecked) &&
            is_runnable)
        {
          cache_resolved_path(key, full_path, name_info.extension, false);
        }
        return result;
      } else if (requirement == Requirement::Execution && !blocked.has_value())
        blocked = CachedPath{full_path, name_info.extension};
    }
  }

  if (search_mode == SearchMode::First && blocked.has_value()) {
    result.push(blocked->path);
  }

  return result;
}

hot fn ProgramResolver::search(StringView program_name, SearchMode search_mode,
                               Requirement requirement,
                               CachePolicy cache_policy,
                               const Maybe<StringView> &path_override) throws
    -> ArrayList<Path>
{
  if (os::has_directory_separator(program_name)) {
    let result = ArrayList<Path>{heap_allocator()};
    let const candidate = Path{program_name};
    if (!candidate.is_regular_file()) return result;
    if (requirement != Requirement::Regular && !candidate.is_executable()) {
      return result;
    }
    result.push(candidate);
    return result;
  }

  if (search_mode == SearchMode::All || cache_policy == CachePolicy::Bypass ||
      path_override.has_value())
    return resolve_along_path(program_name, search_mode, requirement,
                              CachePolicy::Bypass, path_override);

  let normalized_name = String{program_name};
  let const name_info = os::normalize_program_name(normalized_name);
  let const stem =
      normalized_name.substring_of_length(0, name_info.stem_length);

  if (const CacheEntry *const cached = m_execution_cache.find(stem);
      cached != nullptr)
  {
    let result = ArrayList<Path>{heap_allocator()};
    let const path = find_cached_program_path(*cached, name_info.extension);
    if (path != nullptr) {
      if (cache_policy != CachePolicy::RememberUnchecked &&
          (!path->is_regular_file() || !path->is_executable()))
      {
        m_execution_cache.erase(stem);
        return resolve_along_path(program_name, SearchMode::First, requirement,
                                  cache_policy, path_override);
      }
      result.push(*path);
      return result;
    }
  }

  return resolve_along_path(program_name, SearchMode::First, requirement,
                            cache_policy, path_override);
}

} /* namespace utils */

} /* namespace koshka */
