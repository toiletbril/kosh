#pragma once

#include "Arena.hpp"
#include "Bitset.hpp"
#include "Builtin.hpp"
#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ResolvedCommand.hpp"
#include "RuntimeState.hpp"

namespace koshka {

namespace utils {

class ProgramResolver
{
public:
  enum class Requirement : u8
  {
    Regular,
    Runnable,
    Execution,
  };

  enum class CachePolicy : u8
  {
    Bypass,
    ReadOnly,
    Remember,
  };

  enum class Status : u8
  {
    Missing,
    Blocked,
    Runnable,
  };

  enum class StatusLookup : u8
  {
    Cached,
    Authoritative,
  };

  enum class SearchMode : u8
  {
    First,
    All,
  };

  enum class ValidationScope : u8
  {
    Prefix,
    All,
  };

  enum class CompletionRefresh : u8
  {
    Cached,
    Fresh,
  };

  ProgramResolver();
  explicit ProgramResolver(Maybe<String> path);

  fn assign_path(Maybe<String> path) throws -> void;
  fn restore_path(Maybe<String> path) throws -> void;
  fn invalidate() throws -> void;
  fn remember_path(StringView name, const Path &path) throws -> void;
  fn working_directory_changed() throws -> void;
  fn initialize_path_map() throws -> void;
  fn begin_explicit_completion(CompletionRefresh refresh) throws -> void;
  fn end_explicit_completion() wontthrow -> void;
  fn search(StringView program_name, SearchMode search_mode = SearchMode::First,
            Requirement requirement = Requirement::Runnable,
            CachePolicy cache_policy = CachePolicy::Bypass,
            Maybe<StringView> path_override = None) throws -> ArrayList<Path>;
  fn get_status(StringView name,
                StatusLookup lookup = StatusLookup::Cached) throws -> Status;
  fn get_command_names(
      StringView validation_prefix = {},
      ValidationScope validation_scope = ValidationScope::Prefix) throws
      -> const ArrayList<String> &;
  pure fn get_command_name_lower_bound(StringView name) const wontthrow
      -> usize;
  fn command_name_has_prefix(StringView prefix) throws -> bool;
  pure fn has_valid_command_names() const wontthrow -> bool;
  fn for_each_command_name(auto callback) const throws -> void
  {
    for (let const &name : m_command_names)
      callback(name);
  }

private:
  struct CachedPath
  {
    Path path;
    os::program_extension extension{os::program_extension::None};
  };

  struct CacheEntry
  {
    ArrayList<CachedPath> paths{heap_allocator()};
    Maybe<usize> bare_path_position{};
  };

  fn mark_command_name_indexes_stale() wontthrow -> void;
  fn clear_command_name_indexes() wontthrow -> void;
  fn mark_derived_indexes_stale() wontthrow -> void;
  fn clear_derived_indexes() wontthrow -> void;
  fn split_path_dirs(StringView path) throws -> ArrayList<String>;
  fn deduplicate_path_dirs(const ArrayList<String> &directories) throws
      -> ArrayList<String>;
  fn get_path_dirs() throws -> const ArrayList<String> &;
  fn get_index_path_dirs() throws -> const ArrayList<String> &;
  fn refresh_path_directory_generations() throws -> void;
  fn rebuild_path_command_index(CompletionRefresh refresh) throws -> void;
  fn prepare_complete_path_cache(StringView validation_prefix,
                                 ValidationScope validation_scope) throws
      -> void;
  fn validate_path_directory_generations() throws -> bool;
  fn revalidate_command_prefix(StringView prefix) throws -> void;
  fn resolve_along_path(StringView program_name, SearchMode search_mode,
                        Requirement requirement, CachePolicy cache_policy,
                        Maybe<StringView> path_override) throws
      -> ArrayList<Path>;
  fn cache_resolved_path(StringView name, const Path &full_path,
                         os::program_extension extension,
                         bool is_bare_result) throws -> void;
  pure fn find_cached_program_path(
      const CacheEntry &entry,
      os::program_extension wanted_extension) const wontthrow -> const Path *;
  pure fn command_name_lower_bound_in(const ArrayList<String> &names,
                                      StringView name) const wontthrow -> usize;

  StringMap<CacheEntry> m_execution_cache{heap_allocator()};
  ArrayList<String> m_command_names{heap_allocator()};
  ArrayList<String> m_regular_names{heap_allocator()};
  Maybe<String> m_path;
  ArrayList<String> m_path_dirs{heap_allocator()};
  ArrayList<String> m_index_path_dirs{heap_allocator()};
  ArrayList<u64> m_path_directory_generations{heap_allocator()};
  String m_validated_prefix{heap_allocator()};
  bool m_path_dirs_are_valid{false};
  bool m_path_directory_generations_are_valid{false};
  bool m_command_names_are_valid{false};
  u64 m_path_directories_validation_epoch{0};
  u64 m_command_names_validation_epoch{0};
  u64 m_prefix_validation_epoch{0};
  usize m_explicit_completion_depth{0};
};

} /* namespace utils */

using ProgramResolver = utils::ProgramResolver;
} /* namespace koshka */
