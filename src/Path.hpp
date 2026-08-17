#pragma once

#include "ArrayList.hpp"
#include "Common.hpp"
#include "ErrorOr.hpp"
#include "Maybe.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

enum class mimic_mood : u8;

struct unavailable_path_component
{
  usize component_start;
  usize component_end;
  usize component_index;
  usize component_count;
  bool is_not_directory;
};

/* A forward slash is always accepted on input, even on Windows, so a script
   written for POSIX keeps working. */
class Path
{
public:
  struct component
  {
    StringView text;
    usize start;
    usize end;
  };

  Path() = default;
  explicit Path(StringView text);

  mustuse fn clone() const throws -> Path { return Path{*this}; }

  hot mustuse pure fn text() const wontthrow -> const String &;
  hot mustuse pure fn c_str() const wontthrow -> const char *;
  hot mustuse pure fn count() const wontthrow -> usize;
  hot mustuse pure fn is_empty() const wontthrow -> bool;
  hot mustuse pure fn has_trailing_separator() const wontthrow -> bool;

  mustuse fn parent() const throws -> Path;
  mustuse pure fn filename() const wontthrow -> StringView;
  mustuse pure fn extension() const wontthrow -> StringView;
  mustuse pure fn next_component(usize &position) const wontthrow -> component;
  mustuse static pure fn next_component(StringView path,
                                        usize &position) wontthrow -> component;

  mustuse pure fn is_absolute() const wontthrow -> bool;
  mustuse pure fn is_relative() const wontthrow -> bool;

  cold mustuse fn normalized() const throws -> Path;
  cold mustuse fn first_unavailable_component() const throws
      -> Maybe<unavailable_path_component>;

  mustuse fn to_absolute_without_normalizing() const throws -> Path;
  mustuse fn to_absolute() const throws -> Path;

  fn push_component(StringView component) throws -> Path &;

  mustuse pure fn with_extension(StringView new_extension) const throws -> Path;

  cold mustuse fn exists() const wontthrow -> bool;
  cold mustuse fn is_directory() const wontthrow -> bool;
  mustuse fn is_regular_file() const wontthrow -> bool;
  /* Tested without following the link. */
  mustuse fn is_symbolic_link() const wontthrow -> bool;
  /* Each is always false on Windows where the type has no equivalent. */
  mustuse fn is_block_device() const wontthrow -> bool;
  mustuse fn is_character_device() const wontthrow -> bool;
  mustuse fn is_fifo() const wontthrow -> bool;
  mustuse fn is_socket() const wontthrow -> bool;
  /* Each is always false on Windows where the bit has no equivalent. */
  mustuse fn has_setuid_bit() const wontthrow -> bool;
  mustuse fn has_setgid_bit() const wontthrow -> bool;
  mustuse fn has_sticky_bit() const wontthrow -> bool;
  mustuse fn is_owned_by_effective_user() const wontthrow -> bool;
  mustuse fn is_owned_by_effective_group() const wontthrow -> bool;
  mustuse fn file_size() const wontthrow -> Maybe<u64>;
  mustuse fn modification_time() const wontthrow -> Maybe<i64>;

  mustuse fn is_readable() const wontthrow -> bool;
  mustuse fn is_writable() const wontthrow -> bool;
  mustuse fn is_executable() const wontthrow -> bool;

  mustuse fn is_same_file_as(const Path &other) const wontthrow -> bool;
  mustuse fn is_newer_than(const Path &other) const wontthrow -> bool;
  mustuse fn is_older_than(const Path &other) const wontthrow -> bool;

  hot mustuse pure fn operator==(const Path &other) const wontthrow->bool;

  cold mustuse static fn current_directory() throws -> Path;
  static fn set_current_directory(const Path &path) throws -> ErrorOr<Ok>;
  mustuse static fn temp_directory() throws -> Path;

  cold mustuse static fn read_directory(const Path &dir) throws
      -> Maybe<ArrayList<String>>;

  /* Unknown means the caller must stat to learn the type. */
  enum class entry_kind : u8
  {
    Unknown,
    Directory,
    Regular,
    Symlink,
    Other,
  };
  struct directory_child
  {
    String name;
    entry_kind kind;
  };
  cold mustuse static fn read_directory_typed(const Path &dir) throws
      -> Maybe<ArrayList<directory_child>>;

  mustuse fn read_entire_file() const throws -> Maybe<String>;

  mustuse static fn canonicalize(StringView path) throws -> Maybe<Path>;

  mustuse fn detect_mimic_shell() const throws -> Maybe<mimic_mood>;
  mustuse fn is_shell_source(StringView source) const throws -> bool;

private:
  String m_text{heap_allocator()};
};

class PathBuilder
{
public:
  PathBuilder() = default;
  explicit PathBuilder(StringView root);

  /* A separator is inserted unless the builder is empty or the component starts
     at a root. */
  fn append(StringView component) throws -> PathBuilder &;
  fn append_raw(StringView bytes) throws -> PathBuilder &;

  mustuse fn build() const throws -> Path;

private:
  String m_text{heap_allocator()};
};

} /* namespace koshka */
