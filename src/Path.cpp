#include "Path.hpp"

#include "MimicMood.hpp"
#include "PackedStringKey.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Trace.hpp"

namespace koshka {

Path::Path(StringView text) : m_text(text) {}

hot fn Path::text() const wontthrow -> const String & { return m_text; }

hot fn Path::c_str() const wontthrow -> const char * { return m_text.c_str(); }

hot fn Path::count() const wontthrow -> usize { return m_text.count(); }

hot fn Path::is_empty() const wontthrow -> bool { return m_text.is_empty(); }

hot fn Path::has_trailing_separator() const wontthrow -> bool
{
  return !m_text.is_empty() && os::is_directory_separator(m_text.back());
}

fn Path::is_absolute() const wontthrow -> bool
{
  return os::path_is_absolute(m_text.view());
}

fn Path::is_relative() const wontthrow -> bool { return !is_absolute(); }

static pure fn filename_offset(const String &text) wontthrow -> usize
{
  for (usize i = text.count(); i > 0; i--)
    if (os::is_directory_separator(text[i - 1])) return i;
  return 0;
}

fn Path::filename() const wontthrow -> StringView
{
  let const start = filename_offset(m_text);
  ASSERT(start <= m_text.count());
  return m_text.substring(start);
}

fn Path::extension() const wontthrow -> StringView
{
  let const name = filename();
  if (name == StringView{"."} || name == StringView{".."}) {
    return StringView{name.data + name.length, 0};
  }
  /* A leading dot names a hidden file, so the scan stops before the first byte.
   */
  for (usize i = name.length; i > 1; i--)
    if (name.data[i - 1] == '.') return name.substring(i - 1);
  return StringView{name.data + name.length, 0};
}

fn Path::parent() const throws -> Path
{
  let const end = filename_offset(m_text);
  if (end == 0) return Path{};
  if (end == 1) return Path{m_text.substring_of_length(0, 1)};
  return Path{m_text.substring_of_length(0, end - 1)};
}

static fn append_path_component(String &text, StringView component) throws
    -> void
{
  if (component.length == 0) return;
  if (!text.is_empty() && !os::is_directory_separator(text.back()) &&
      !os::is_directory_separator(component.data[0]))
  {
    text.push(os::DIRECTORY_SEPARATOR);
  }
  text.append(component);
}

fn Path::push_component(StringView component) throws -> Path &
{
  append_path_component(m_text, component);
  return *this;
}

fn Path::with_extension(StringView new_extension) const throws -> Path
{
  let const current_extension = extension();

  ASSERT(current_extension.length <= m_text.count(),
         "extension is a suffix of the path text");
  let const prefix_length = m_text.count() - current_extension.length;

  let result = Path{m_text.substring_of_length(0, prefix_length)};
  if (new_extension.length > 0 && new_extension.data[0] != '.') {
    result.m_text.push('.');
  }
  result.m_text.append(new_extension);

  return result;
}

cold fn Path::normalized() const throws -> Path
{
  LOG(Debug, "normalizing the path '%s'", m_text.c_str());

  let const root_length = os::path_root_length(m_text.view());
  let const is_absolute_path = root_length > 0;

  let components = ArrayList<StringView>{heap_allocator()};
  usize i = root_length;
  while (i < m_text.count()) {
    if (os::is_directory_separator(m_text[i])) {
      i++;
      continue;
    }
    let const component_start = i;
    while (i < m_text.count() && !os::is_directory_separator(m_text[i]))
      i++;
    let const component =
        m_text.substring_of_length(component_start, i - component_start);
    if (component == StringView{"."}) continue;
    if (component == StringView{".."}) {
      /* A relative path keeps a leading .. because it cannot climb past its own
         start. */
      if (components.count() > 0 && !(components.back() == StringView{".."})) {
        components.pop_back();
      } else if (!is_absolute_path) {
        components.push(component);
      }
      continue;
    }
    components.push(component);
  }

  let normalized_text = String{heap_allocator()};
  normalized_text.append(m_text.substring_of_length(0, root_length));
  if (!normalized_text.is_empty() && !components.is_empty() &&
      !os::is_directory_separator(normalized_text.back()))
  {
    normalized_text.push(os::DIRECTORY_SEPARATOR);
  }
  for (usize i = 0; i < components.count(); i++) {
    if (i > 0) normalized_text.push(os::DIRECTORY_SEPARATOR);
    normalized_text.append(components[i]);
  }
  if (normalized_text.is_empty())
    normalized_text.append(is_absolute_path
                               ? StringView{&os::DIRECTORY_SEPARATOR, 1}
                               : StringView{"."});
  return Path{normalized_text};
}

cold fn Path::first_unavailable_component() const throws
    -> Maybe<unavailable_path_component>
{
  const usize root_length = os::path_root_length(m_text.view());
  usize component_index = 0;
  usize position = root_length;
  let has_dot_component = false;
  while (position < m_text.count()) {
    while (position < m_text.count() &&
           os::is_directory_separator(m_text[position]))
      position++;
    if (position >= m_text.count()) break;

    const usize component_start = position;
    while (position < m_text.count() &&
           !os::is_directory_separator(m_text[position]))
      position++;

    let const component =
        m_text.substring_of_length(component_start, position - component_start);
    if (component == StringView{"."} || component == StringView{".."}) {
      has_dot_component = true;
    }

    let const prefix = Path{m_text.substring_of_length(0, position)};
    let is_available = false;
    let is_directory = false;
    if (has_dot_component) {
      let status = os::file_status{};
      is_available = os::stat_path_following(prefix.text().view(), status);
      is_directory = is_available && os::file_type_letter(status.mode) == 'd';
    } else {
      is_available = prefix.exists();
      is_directory = is_available && prefix.is_directory();
    }
    if (!is_available || !is_directory) {
      usize remaining_component_count = 0;
      usize remaining_position = position;
      while (remaining_position < m_text.count()) {
        while (remaining_position < m_text.count() &&
               os::is_directory_separator(m_text[remaining_position]))
          remaining_position++;
        if (remaining_position >= m_text.count()) break;
        remaining_component_count++;
        while (remaining_position < m_text.count() &&
               !os::is_directory_separator(m_text[remaining_position]))
          remaining_position++;
      }
      let const component_count =
          component_index + remaining_component_count + 1;
      if (!is_available)
        return unavailable_path_component{
            component_start, position, component_index, component_count, false};
      return unavailable_path_component{component_start, position,
                                        component_index, component_count, true};
    }
    component_index++;
  }

  return None;
}

fn Path::to_absolute_without_normalizing() const throws -> Path
{
  let result = Path{};
  if (is_absolute()) {
    result = clone();
  } else if (let native = os::resolve_drive_relative_path(m_text.view())) {
    result = native.take();
  } else {
    result = current_directory();
    let relative = m_text.view();
    while (relative.length >= 2 && relative[0] == '.' &&
           os::is_directory_separator(relative[1]))
      relative = relative.substring(2);
    if (!relative.is_empty()) result.push_component(relative);
  }

  let const root_length = os::path_root_length(result.m_text.view());
  while (result.m_text.count() > root_length &&
         os::is_directory_separator(result.m_text.back()))
    result.m_text.pop_back();
  return result;
}

fn Path::to_absolute() const throws -> Path
{
  return to_absolute_without_normalizing().normalized();
}

hot fn Path::operator==(const Path &other) const wontthrow -> bool
{
  return m_text == other.m_text;
}

cold fn Path::exists() const wontthrow -> bool
{
  return os::path_exists(m_text.view());
}

cold fn Path::is_directory() const wontthrow -> bool
{
  return os::path_is_directory(m_text.view());
}

fn Path::is_regular_file() const wontthrow -> bool
{
  return os::path_is_regular_file(m_text.view());
}

fn Path::is_symbolic_link() const wontthrow -> bool
{
  return os::path_is_symbolic_link(m_text.view());
}

fn Path::is_block_device() const wontthrow -> bool
{
  return os::path_is_block_device(m_text.view());
}

fn Path::is_character_device() const wontthrow -> bool
{
  return os::path_is_character_device(m_text.view());
}

fn Path::is_fifo() const wontthrow -> bool
{
  return os::path_is_fifo(m_text.view());
}

fn Path::is_socket() const wontthrow -> bool
{
  return os::path_is_socket(m_text.view());
}

fn Path::has_setuid_bit() const wontthrow -> bool
{
  return os::path_has_setuid_bit(m_text.view());
}

fn Path::has_setgid_bit() const wontthrow -> bool
{
  return os::path_has_setgid_bit(m_text.view());
}

fn Path::has_sticky_bit() const wontthrow -> bool
{
  return os::path_has_sticky_bit(m_text.view());
}

fn Path::is_owned_by_effective_user() const wontthrow -> bool
{
  return os::path_is_owned_by_effective_user(m_text.view());
}

fn Path::is_owned_by_effective_group() const wontthrow -> bool
{
  return os::path_is_owned_by_effective_group(m_text.view());
}

fn Path::file_size() const wontthrow -> Maybe<u64>
{
  return os::path_file_size(m_text.view());
}

fn Path::modification_time() const wontthrow -> Maybe<i64>
{
  return os::path_modification_time(m_text.view());
}

fn Path::is_same_file_as(const Path &other) const wontthrow -> bool
{
  return os::paths_are_same_file(m_text.view(), other.m_text.view());
}

fn Path::is_newer_than(const Path &other) const wontthrow -> bool
{
  return os::path_is_newer_than(m_text.view(), other.m_text.view());
}

fn Path::is_older_than(const Path &other) const wontthrow -> bool
{
  return os::path_is_older_than(m_text.view(), other.m_text.view());
}

fn Path::is_readable() const wontthrow -> bool
{
  return os::path_is_readable(m_text.view());
}

fn Path::is_writable() const wontthrow -> bool
{
  return os::path_is_writable(m_text.view());
}

fn Path::is_executable() const wontthrow -> bool
{
  return os::path_is_executable(m_text.view());
}

cold fn Path::current_directory() throws -> Path
{
  return os::read_current_directory();
}

fn Path::set_current_directory(const Path &path) throws -> ErrorOr<Ok>
{
  return os::change_current_directory(path.text().view());
}

cold fn Path::read_directory(const Path &dir) throws -> Maybe<ArrayList<String>>
{
  return os::list_directory(dir.text().view());
}

cold fn Path::read_directory_typed(const Path &dir) throws
    -> Maybe<ArrayList<directory_child>>
{
  return os::list_directory_typed(dir.text().view());
}

fn Path::temp_directory() throws -> Path
{
  return Path{os::temp_directory_path().view()};
}

fn Path::read_entire_file() const throws -> Maybe<String>
{
  LOG(Debug, "reading the entire file '%s'", c_str());

  let const file =
      os::open_file_descriptor(text().view(), os::file_open_mode::Read);
  if (!file) return None;
  defer { os::close_fd(*file); };

  return os::read_fd_to_string(*file, heap_allocator());
}

fn Path::canonicalize(StringView path) throws -> Maybe<Path>
{
  LOG(Debug, "canonicalizing the path '%.*s'", static_cast<int>(path.length),
      path.data);

  let candidate = Path{path};

  if (candidate.is_relative() && os::has_directory_separator(path)) {
    candidate = candidate.to_absolute_without_normalizing();
  }

  /* A name written with a trailing dot gets no suffix added. */
  let const ends_with_dot =
      path.length > 0 && path.data[path.length - 1] == '.';
  let status = os::file_status{};
  let was_resolved = os::stat_path_following(candidate.text().view(), status);
  if (candidate.extension().is_empty() && !ends_with_dot) {
    usize suffix_index = 0;
    while (!was_resolved && suffix_index < os::PROGRAM_SUFFIXES.count()) {
      let const &suffix = os::PROGRAM_SUFFIXES[suffix_index++];
      candidate = candidate.with_extension(suffix.text);
      was_resolved = os::stat_path_following(candidate.text().view(), status);
    }
  }

  if (!was_resolved) return koshka::None;
  return candidate;
}

fn Path::detect_mimic_shell() const throws -> Maybe<mimic_mood>
{
  LOG(Debug, "probing '%s' for a shell shebang to mimic", c_str());

  let const file =
      os::open_file_descriptor(text().view(), os::file_open_mode::Read);
  if (!file) return None;
  char buffer[256];
  let const read_count = os::read_fd(*file, buffer, sizeof(buffer));
  os::close_fd(*file);
  if (!read_count || *read_count < 3) return None;

  return detect_mimic_shell_from_source(StringView{buffer, *read_count});
}

fn Path::is_shell_source(StringView source) const throws -> bool
{
  static constexpr static_string_entry<bool> EXTENSION_ENTRIES[] = {
      {SSK(".bash"), true},
      {SSK(".dash"), true},
      {SSK(".sh"),   true},
      {SSK(".kosh"), true},
  };
  static constexpr StaticStringMap EXTENSIONS{EXTENSION_ENTRIES};
  return EXTENSIONS.find(extension()).has_value() ||
         detect_mimic_shell_from_source(source).has_value();
}

PathBuilder::PathBuilder(StringView root) : m_text(root) {}

fn PathBuilder::append(StringView component) throws -> PathBuilder &
{
  append_path_component(m_text, component);
  return *this;
}

fn PathBuilder::append_raw(StringView bytes) throws -> PathBuilder &
{
  m_text.append(bytes);
  return *this;
}

fn PathBuilder::build() const throws -> Path { return Path{m_text}; }

} /* namespace koshka */
