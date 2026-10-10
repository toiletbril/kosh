/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements pathname expansion, recursive globstar traversal,
 * tolerant glob expansion, and tilde expansion. Segment masks distinguish
 * active pattern bytes from quoted or literal text. The split confines
 * filesystem traversal and pattern expansion outside general word expansion.
 */

#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Debug.hpp"
#include "base/Path.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace {

fn name_matches_glob(StringView glob, StringView filename,
                     const Bitset &glob_active, usize mask_offset,
                     utils::extglob_mode mode, utils::glob_charset charset,
                     Allocator allocator,
                     os::case_sensitivity sensitivity) throws -> bool
{
  if (sensitivity == os::case_sensitivity::Sensitive)
    return utils::glob_matches(glob, filename, glob_active, mask_offset, mode,
                               charset);

  let const lowered_name =
      utils::lowercase_for_glob(filename, charset, allocator);

  return utils::glob_matches(glob, lowered_name.view(), glob_active,
                             mask_offset, mode, charset);
}

} /* namespace */

fn EvalContext::get_glob_charset() const throws -> glob_charset
{
  for (let const name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
    let const value = get_variable_value(name);
    if (!value.has_value() || value->is_empty()) {
      continue;
    }

    return utils::locale_name_is_utf8(value->view()) ? glob_charset::Utf8
                                                     : glob_charset::Bytes;
  }

  return os::DEFAULT_LOCALE_IS_UTF8 ? glob_charset::Utf8 : glob_charset::Bytes;
}

hot fn EvalContext::get_glob_charset_for(StringView subject) const throws
    -> glob_charset
{
  usize position = 0;
  using byte_vector = u64 __attribute__((vector_size(16)));
  while (position + 4 * sizeof(byte_vector) <= subject.length) {
    byte_vector lanes[4];
    __builtin_memcpy(lanes, subject.data + position, sizeof(lanes));
    let const merged = lanes[0] | lanes[1] | lanes[2] | lanes[3];
    if (((merged[0] | merged[1]) & byte_scan::HIGH_BITS) != 0) {
      return get_glob_charset();
    }

    position += sizeof(lanes);
  }

  while (position + sizeof(u64) <= subject.length) {
    if ((byte_scan::load_word(subject.data + position) &
         byte_scan::HIGH_BITS) != 0)
    {
      return get_glob_charset();
    }

    position += sizeof(u64);
  }

  for (; position < subject.length; position++) {
    if (static_cast<u8>(subject[position]) >= 0x80) return get_glob_charset();
  }

  return glob_charset::Bytes;
}

fn EvalContext::first_field_separator() const throws -> StringView
{
  let const separators = variable_store().field_separators();
  if (separators.is_empty()) return separators;

  return separators.substring_of_length(
      0, utils::charset_character_length(separators, 0,
                                         get_glob_charset_for(separators)));
}

fn EvalContext::expand_path_once(const glob_field &field,
                                 glob_expansion_mode expansion_mode) throws
    -> ArrayList<glob_field>
{
  let const scratch = scratch_allocator();
  let expanded = ArrayList<glob_field>{scratch};

  let const path = field.text.view();
  LOG(All, "scanning a directory for the glob component '%.*s'",
      static_cast<int>(path.length), path.data);

  let const last_slash = field.text.find_last_character('/');
  let const has_slashes = last_slash.has_value();

  let parent_dir = Path{};
  if (has_slashes)
    parent_dir =
        Path{*last_slash != 0 ? path.substring_of_length(0, *last_slash)
                              : path.substring_of_length(0, 1)};
  else
    parent_dir = Path{StringView{"."}};

  let const stem_start = has_slashes ? *last_slash + 1 : 0;
  let const has_glob = stem_start < path.length;
  let glob = StringView{};
  if (has_glob) glob = path.substring(stem_start);

  let entries = Path::read_directory_typed(parent_dir);
  if (!entries.has_value()) {
    LOG(Debug,
        "the parent directory is unreadable, the glob '%.*s' yields no match",
        static_cast<int>(path.length), path.data);
    return expanded;
  }

  if (!has_glob) {
    let literal_field = glob_field{scratch};
    literal_field.text.append(field.text.view());
    literal_field.glob_active = field.glob_active;
    expanded.push(steal(literal_field));
    return expanded;
  }

  let const typed_prefix =
      has_slashes ? path.substring_of_length(0, stem_start) : StringView{};

  ASSERT(has_glob);
  ASSERT(!glob.is_empty());

  let const pattern_leads_with_dot = glob[0] == '.';
  if (pattern_leads_with_dot && !is_shopt_enabled("globskipdots")) {
    entries->push(
        Path::directory_child{String{"."}, Path::entry_kind::Directory});
    entries->push(
        Path::directory_child{String{".."}, Path::entry_kind::Directory});
  }

  let const dotglob_is_on = is_shopt_enabled("dotglob");
  let const nocaseglob_is_on = is_shopt_enabled("nocaseglob");
  let const extglob = get_extglob_mode();
  Maybe<glob_charset> locale_charset = None;
  let const do_get_entry_charset = [&](StringView filename)
                                       throws -> glob_charset {
    if (get_glob_charset_for(filename) == glob_charset::Bytes) {
      return glob_charset::Bytes;
    }
    if (!locale_charset.has_value()) locale_charset = get_glob_charset();

    return *locale_charset;
  };

  let lowered_glob = String{scratch};
  if (nocaseglob_is_on) {
    lowered_glob =
        utils::lowercase_for_glob(glob, get_glob_charset_for(glob), scratch);
  }
  let const match_glob = nocaseglob_is_on ? lowered_glob.view() : glob;

  let const do_entry_matches = [&](const Path::directory_child &entry)
                                   throws -> bool {
    let const filename = entry.name.view();

    if (filename == "." || filename == "..") {
      if (!pattern_leads_with_dot) return false;
    } else if (!pattern_leads_with_dot && !filename.is_empty() &&
               filename[0] == '.' && !dotglob_is_on)
    {
      return false;
    }

    return name_matches_glob(
        match_glob, filename, field.glob_active, stem_start, extglob,
        do_get_entry_charset(filename), scratch,
        nocaseglob_is_on ? os::case_sensitivity::Insensitive
                         : os::case_sensitivity::Sensitive);
  };
  let const do_append_entry = [&](StringView filename) throws -> void {
    evaluation_metrics_store().add_expansion(runtime_state().stats_enabled());

    let result_field = glob_field{scratch};
    result_field.text.append(typed_prefix);
    result_field.text.append(filename);
    expanded.push(steal(result_field));
  };

  if (expansion_mode == glob_expansion_mode::Files) {
    for (let const &entry : *entries)
      if (do_entry_matches(entry)) do_append_entry(entry.name.view());

    return expanded;
  }

  let should_include = ArrayList<bool>{scratch};
  let uncertain_positions = ArrayList<usize>{scratch};
  let uncertain_paths = ArrayList<Path>{scratch};
  let uncertain_statuses = ArrayList<os::file_status>{scratch};
  should_include.reserve(entries->count());
  uncertain_positions.reserve(entries->count());
  uncertain_paths.reserve(entries->count());
  uncertain_statuses.reserve(entries->count());
  for (usize index = 0; index < entries->count(); index++) {
    let const &entry = (*entries)[index];
    should_include.push(false);
    if (!do_entry_matches(entry)) continue;

    if (entry.kind == Path::entry_kind::Directory) {
      should_include[index] = true;
      continue;
    }
    if (entry.kind == Path::entry_kind::Regular ||
        entry.kind == Path::entry_kind::Other)
    {
      continue;
    }

    let full_path = parent_dir.clone();
    full_path.append(entry.name.view());
    uncertain_positions.push(index);
    uncertain_paths.push(steal(full_path));
    uncertain_statuses.push({});
  }

  let directory_batch = os::Batch{scratch};
  directory_batch.reserve(uncertain_paths.count());
  for (usize index = 0; index < uncertain_paths.count(); index++)
    directory_batch.add(os::batch_operation::stat(uncertain_paths[index],
                                                  uncertain_statuses[index]));
  let const directory_results = directory_batch.execute();
  for (usize index = 0; index < uncertain_paths.count(); index++)
    should_include[uncertain_positions[index]] =
        directory_results[index].error_number == 0 &&
        os::file_type_letter(uncertain_statuses[index].mode) == 'd';

  for (usize index = 0; index < entries->count(); index++)
    if (should_include[index]) do_append_entry((*entries)[index].name.view());

  return expanded;
}

hot pure fn first_active_glob(StringView text, const Bitset &mask,
                              extglob_mode mode) wontthrow -> Maybe<usize>
{
  let open_bracket = Maybe<usize>{};
  for (usize i = 0; i < text.length; i++) {
    if (i >= mask.count() || !mask[i]) {
      continue;
    }

    let const ch = text.data[i];
    if (mode == extglob_mode::Enabled && i + 1 < text.length &&
        lexer::is_extglob_operator(ch) && text.data[i + 1] == '(')
    {
      return i;
    }
    if (ch == '*' || ch == '?') {
      return i;
    }
    if (ch == '[') {
      if (!open_bracket) open_bracket = i;
    } else if (ch == ']' && open_bracket) {
      return open_bracket;
    }
  }
  return koshka::None;
}

namespace {

constexpr usize GLOBSTAR_MAX_DEPTH = 256;

fn collect_globstar_paths(const Path &dir, StringView relative,
                          bool directories_only, bool should_match_dotfiles,
                          bool should_list_symlinked_directories,
                          bool include_base, usize depth, Allocator allocator,
                          ArrayList<String> &out) throws -> void
{
  LOG(All,
      "collecting globstar paths under the relative path '%.*s', depth %zu",
      static_cast<int>(relative.length), relative.data, depth);
  if (directories_only && include_base) {
    out.push(String{allocator, relative});
  }
  if (depth >= GLOBSTAR_MAX_DEPTH) return;

  let const entries = Path::read_directory_typed(dir);
  if (!entries.has_value()) return;

  let is_directory = Bitset{allocator};
  let is_symbolic_link = Bitset{allocator};
  let uncertain_positions = ArrayList<usize>{allocator};
  let uncertain_paths = ArrayList<Path>{allocator};
  let uncertain_statuses = ArrayList<os::file_status>{allocator};
  is_directory.reset(entries->count());
  is_symbolic_link.reset(entries->count());

  usize uncertain_count = 0;
  usize unknown_count = 0;
  for (usize index = 0; index < entries->count(); index++) {
    let const &entry = (*entries)[index];
    let const name = entry.name.view();
    let const should_skip =
        !should_match_dotfiles && !name.is_empty() && name[0] == '.';
    switch (entry.kind) {
    case Path::entry_kind::Directory: is_directory.set(index); break;
    case Path::entry_kind::Symlink:
      is_symbolic_link.set(index);
      if (!should_skip) uncertain_count++;
      break;
    case Path::entry_kind::Unknown:
      if (!should_skip) {
        uncertain_count++;
        unknown_count++;
      }
      break;
    case Path::entry_kind::Regular:
    case Path::entry_kind::Other: break;
    }
  }

  uncertain_positions.reserve(uncertain_count);
  uncertain_paths.reserve(uncertain_count);
  uncertain_statuses.reserve(uncertain_count);

  for (usize index = 0; index < entries->count(); index++) {
    let const &entry = (*entries)[index];
    let const name = entry.name.view();
    if (!should_match_dotfiles && !name.is_empty() && name[0] == '.') {
      continue;
    }
    switch (entry.kind) {
    case Path::entry_kind::Symlink:
    case Path::entry_kind::Unknown: break;
    case Path::entry_kind::Directory:
    case Path::entry_kind::Regular:
    case Path::entry_kind::Other: continue;
    }

    let child_path = dir;
    child_path.append(name);
    uncertain_positions.push(index);
    uncertain_paths.push(steal(child_path));
    uncertain_statuses.push({});
  }

  let lstat_batch = os::Batch{allocator};
  lstat_batch.reserve(unknown_count);
  for (usize index = 0; index < uncertain_paths.count(); index++)
    if ((*entries)[uncertain_positions[index]].kind ==
        Path::entry_kind::Unknown)
    {
      lstat_batch.add(os::batch_operation::lstat(uncertain_paths[index],
                                                 uncertain_statuses[index]));
    }
  let const lstat_results = lstat_batch.execute();
  usize lstat_result_index = 0;
  for (usize index = 0; index < uncertain_paths.count(); index++) {
    if ((*entries)[uncertain_positions[index]].kind !=
        Path::entry_kind::Unknown)
    {
      continue;
    }

    is_symbolic_link.set(
        uncertain_positions[index],
        lstat_results[lstat_result_index].error_number == 0 &&
            os::file_type_letter(uncertain_statuses[index].mode) == 'l');
    lstat_result_index++;
  }

  let stat_batch = os::Batch{allocator};
  stat_batch.reserve(uncertain_paths.count());
  for (usize index = 0; index < uncertain_paths.count(); index++)
    stat_batch.add(os::batch_operation::stat(uncertain_paths[index],
                                             uncertain_statuses[index]));
  let const stat_results = stat_batch.execute();
  for (usize index = 0; index < uncertain_paths.count(); index++)
    is_directory.set(uncertain_positions[index],
                     stat_results[index].error_number == 0 &&
                         os::file_type_letter(uncertain_statuses[index].mode) ==
                             'd');

  for (usize index = 0; index < entries->count(); index++) {
    let const &entry = (*entries)[index];
    let const name = entry.name.view();
    if (!should_match_dotfiles && !name.is_empty() && name[0] == '.') {
      continue;
    }

    let child_dir = dir;
    child_dir.append(name);

    let child_relative = String{allocator};
    child_relative.reserve(relative.length + name.length + 1);
    if (!relative.is_empty()) {
      child_relative.append(relative);
      child_relative += '/';
    }
    child_relative.append(name);

    let const is_listed =
        !directories_only ||
        (is_directory[index] &&
         (should_list_symlinked_directories || !is_symbolic_link[index]));
    if (is_listed) out.push(String{allocator, child_relative.view()});
    if (is_directory[index] && !is_symbolic_link[index]) {
      collect_globstar_paths(child_dir, child_relative.view(), directories_only,
                             should_match_dotfiles,
                             should_list_symlinked_directories, false,
                             depth + 1, allocator, out);
    }
  }
}

} /* namespace */

fn EvalContext::expand_path_recurse(ArrayList<glob_field> fields) throws
    -> ArrayList<glob_field>
{
  let const scratch = scratch_allocator();
  let result = ArrayList<glob_field>{scratch};

  let glob_indices = ArrayList<Maybe<usize>>{scratch};
  glob_indices.reserve(fields.count());
  let should_batch_literals = !fields.is_empty();
  for (let const &field : fields) {
    let const glob_index = first_active_glob(
        field.text.view(), field.glob_active, get_extglob_mode());
    if (glob_index.has_value()) should_batch_literals = false;
    glob_indices.push(glob_index);
  }

  if (should_batch_literals) {
    let literal_paths = ArrayList<Path>{scratch};
    literal_paths.reserve(fields.count());
    for (let const &field : fields)
      literal_paths.push(Path{field.text.view(), scratch});

    let literal_batch = os::Batch{scratch};
    literal_batch.reserve(fields.count());
    for (usize index = 0; index < fields.count(); index++)
      literal_batch.add(os::batch_operation::exists(literal_paths[index]));
    let const literal_results = literal_batch.execute();

    for (usize index = 0; index < fields.count(); index++)
      if (literal_results[index].error_number == 0 &&
          literal_results[index].is_existing)
      {
        result.push(steal(fields[index]));
      }

    return result;
  }

  for (usize field_index = 0; field_index < fields.count(); field_index++) {
    let &field = fields[field_index];
    let const text = field.text.view();
    let const glob_index = glob_indices[field_index];

    if (!glob_index) {
      if (Path{field.text.view(), scratch}.exists()) result.push(steal(field));
      continue;
    }

    ASSERT(*glob_index < text.length);
    ASSERT(field.glob_active.count() == text.length);

    let slash_after = Maybe<usize>{};
    for (usize k = *glob_index; k < text.length; k++) {
      if (text.data[k] == '/') {
        slash_after = k;
        break;
      }
    }

    usize component_start = 0;
    for (usize k = *glob_index; k > 0; k--)
      if (text.data[k - 1] == '/') {
        component_start = k;
        break;
      }
    let const component_end = slash_after.value_or(text.length);
    let const is_globstar_component = component_end - component_start == 2 &&
                                      text.data[component_start] == '*' &&
                                      text.data[component_start + 1] == '*' &&
                                      field.glob_active[component_start] &&
                                      field.glob_active[component_start + 1] &&
                                      is_shopt_enabled("globstar");

    if (is_globstar_component) {
      LOG(All, "expanding a globstar component across directory levels");
      let const prefix = text.substring_of_length(0, component_start);
      let base = Path{StringView{"."}};
      if (component_start == 1)
        base = Path{StringView{"/"}};
      else if (component_start > 1)
        base = Path{text.substring_of_length(0, component_start - 1)};

      let const directory_position = slash_after.has_value();
      let const has_pattern_after =
          directory_position && *slash_after + 1 < text.length;
      let const should_list_symlinked_directories =
          !has_pattern_after || component_start != 0;
      let relatives = ArrayList<String>{scratch};
      collect_globstar_paths(base, StringView{""}, directory_position,
                             is_shopt_enabled("dotglob"),
                             should_list_symlinked_directories, true, 0,
                             scratch, relatives);

      if (!directory_position) {
        if (!prefix.is_empty()) {
          let base_field = glob_field{scratch};
          base_field.text.append(prefix);
          result.push(steal(base_field));
        }
        for (let const &relative : relatives) {
          let match_field = glob_field{scratch};
          match_field.text.append(prefix);
          match_field.text.append(relative.view());
          result.push(steal(match_field));
        }
        continue;
      }

      let const suffix = text.substring(*slash_after + 1);
      let rebuilt = ArrayList<glob_field>{scratch};
      for (let const &relative : relatives) {
        let candidate = glob_field{scratch};
        candidate.text.append(prefix);
        if (!relative.view().is_empty()) {
          candidate.text.append(relative.view());
          candidate.text += '/';
        }
        let const literal_length = candidate.text.count();
        candidate.text.append(suffix);
        if (candidate.text.is_empty()) continue;
        for (usize k = 0; k < literal_length; k++)
          candidate.glob_active.push(false);
        for (usize k = *slash_after + 1; k < field.glob_active.count(); k++)
          candidate.glob_active.push(field.glob_active[k]);
        rebuilt.push(steal(candidate));
      }
      let recursed = expand_path_recurse(steal(rebuilt));
      for (let &f : recursed)
        result.push(steal(f));
      continue;
    }

    if (!slash_after) {
      let expanded_files = expand_path_once(field, glob_expansion_mode::Files);
      for (let &f : expanded_files)
        result.push(steal(f));
      continue;
    }

    let const slash_offset = static_cast<std::ptrdiff_t>(*slash_after);
    let directory_component = glob_field{scratch};
    directory_component.text.append(StringView{text.data, *slash_after});
    for (std::ptrdiff_t k = 0; k < slash_offset; k++)
      directory_component.glob_active.push(
          field.glob_active[static_cast<usize>(k)]);
    let removed_suffix = glob_field{scratch};
    removed_suffix.text.append(
        StringView{text.data + *slash_after, text.length - *slash_after});
    for (usize k = static_cast<usize>(slash_offset);
         k < field.glob_active.count(); k++)
      removed_suffix.glob_active.push(field.glob_active[k]);

    let expanded_directories =
        expand_path_once(directory_component, glob_expansion_mode::Directories);

    for (let &f : expanded_directories) {
      let const matched_length = f.text.count();
      f.text.append(removed_suffix.text.view());
      f.glob_active.clear();
      for (usize k = 0; k < matched_length; k++)
        f.glob_active.push(false);
      for (usize k = 0; k < removed_suffix.glob_active.count(); k++)
        f.glob_active.push(removed_suffix.glob_active[k]);
    }

    let recursed_matches = expand_path_recurse(steal(expanded_directories));
    for (let &f : recursed_matches)
      result.push(steal(f));
  }

  return result;
}

fn EvalContext::expand_tilde(WordSegment &leading_segment, bool word_continues,
                             bool stop_at_colon) const throws -> void
{
  if (!leading_segment.is_tilde_candidate()) return;

  let &text = leading_segment.text;
  if (text.is_empty() || text[0] != '~') {
    return;
  }

  usize name_end = 1;
  while (name_end < text.length() && text[name_end] != '/' &&
         !(stop_at_colon && text[name_end] == ':'))
    name_end++;
  let const name = text.view().substring_of_length(1, name_end - 1);

  if (name_end == text.length() && word_continues) {
    return;
  }

  let const directory = resolve_tilde_prefix(name);
  if (name.is_empty() && !directory.has_value()) {
    throw Error{"Could not figure out home directory"};
  }
  if (!directory.has_value()) return;

  LOG(All, "the tilde prefix '~%.*s' expands to '%.*s'",
      static_cast<int>(name.length), name.data,
      static_cast<int>(directory->view().length), directory->view().data);
  let expanded = String{heap_allocator()};
  expanded.append(directory->view());
  expanded.append(text.view().substring(name_end));
  text.assign_copy(heap_allocator(), expanded.view());
}

fn EvalContext::resolve_tilde_prefix(StringView name) const throws
    -> Maybe<String>
{
  if (name == "+" || name == "-")
    return get_variable_value(name == "+" ? StringView{"PWD"}
                                          : StringView{"OLDPWD"});
  if (name.is_empty()) {
    if (let home = get_variable_value("HOME"); home.has_value()) return home;
  }
  let const home =
      name.is_empty() ? os::get_home_directory() : os::get_home_for_user(name);
  if (!home) return koshka::None;
  return String{heap_allocator(), home->text().view()};
}

fn EvalContext::expand_colon_tildes(WordSegment &segment, bool word_continues,
                                    Maybe<usize> equals_position) const throws
    -> void
{
  if (!segment.is_tilde_candidate()) return;
  let const view = segment.text.view();
  let rewritten = String{heap_allocator()};
  let was_changed = false;
  usize i = 0;
  while (i < view.length) {
    let const is_separator =
        view[i] == ':' ||
        (equals_position.has_value() && *equals_position == i);
    if (is_separator && i + 1 < view.length && view[i + 1] == '~') {
      usize prefix_end = i + 2;
      while (prefix_end < view.length && view[prefix_end] != '/' &&
             view[prefix_end] != ':')
        prefix_end++;
      if (!(prefix_end == view.length && word_continues)) {
        let const name = view.substring_of_length(i + 2, prefix_end - i - 2);
        if (let const directory = resolve_tilde_prefix(name)) {
          rewritten += view[i];
          rewritten.append(directory->view());
          i = prefix_end;
          was_changed = true;
          continue;
        }
      }
    }
    rewritten += view[i];
    i++;
  }
  if (was_changed) {
    LOG(All, "rewrote colon tilde prefixes in an assignment value");
    segment.text.assign_copy(heap_allocator(), rewritten.view());
  }
}

namespace {

struct glob_ignore_pattern
{
  String text;
  Bitset active;
};

fn parse_glob_ignore_patterns(StringView list, bool should_fold_case,
                              Allocator allocator) throws
    -> ArrayList<glob_ignore_pattern>
{
  let patterns = ArrayList<glob_ignore_pattern>{allocator};
  usize entry_start = 0;
  while (entry_start <= list.length) {
    let const rest = list.substring(entry_start);
    let const colon = rest.find_character(':');
    let const entry =
        colon.has_value() ? rest.substring_of_length(0, *colon) : rest;
    entry_start += entry.length + 1;
    if (entry.is_empty()) continue;

    let pattern = glob_ignore_pattern{String{allocator}, Bitset{allocator}};
    for (usize index = 0; index < entry.length; index++) {
      let const is_escaped = entry[index] == '\\' && index + 1 < entry.length;
      if (is_escaped) index++;
      pattern.text.push(should_fold_case ? utils::ascii_to_lower(entry[index])
                                         : entry[index]);
      pattern.active.push(!is_escaped);
    }
    patterns.push(steal(pattern));
  }

  return patterns;
}

fn glob_ignore_segments_match(const glob_ignore_pattern &pattern,
                              usize pattern_length, StringView text,
                              extglob_mode mode, glob_charset charset) throws
    -> bool
{
  let const pattern_text = pattern.text.view();
  usize pattern_start = 0;
  usize text_start = 0;
  while (true) {
    let const pattern_rest = pattern_text.substring_of_length(
        pattern_start, pattern_length - pattern_start);
    let const text_rest = text.substring(text_start);
    let const pattern_slash = pattern_rest.find_character('/');
    let const text_slash = text_rest.find_character('/');
    let const pattern_segment =
        pattern_slash.has_value()
            ? pattern_rest.substring_of_length(0, *pattern_slash)
            : pattern_rest;
    let const text_segment = text_slash.has_value()
                                 ? text_rest.substring_of_length(0, *text_slash)
                                 : text_rest;
    if (!utils::glob_matches(pattern_segment, text_segment, pattern.active,
                             pattern_start, mode, charset))
    {
      return false;
    }
    if (!pattern_slash.has_value() || !text_slash.has_value()) {
      return !pattern_slash.has_value() && !text_slash.has_value();
    }

    pattern_start += *pattern_slash + 1;
    text_start += *text_slash + 1;
  }
}

fn glob_ignore_pattern_matches(const glob_ignore_pattern &pattern,
                               StringView path, extglob_mode mode,
                               glob_charset charset) throws -> bool
{
  let const length = pattern.text.length();
  let const has_trailing_star = length > 0 && pattern.text[length - 1] == '*' &&
                                pattern.active[length - 1];
  if (!has_trailing_star) {
    return glob_ignore_segments_match(pattern, length, path, mode, charset);
  }

  for (usize prefix_length = 0; prefix_length <= path.length; prefix_length++) {
    if (glob_ignore_segments_match(pattern, length - 1,
                                   path.substring_of_length(0, prefix_length),
                                   mode, charset))
    {
      return true;
    }
  }

  return false;
}

} /* namespace */

hot fn EvalContext::expand_path(glob_field field,
                                const SourceLocation &location) throws
    -> SortedArrayList<String, order_comparator<String>>
{
  let const scratch = scratch_allocator();

  let const has_glob = !runtime_state().no_glob() &&
                       first_active_glob(field.text.view(), field.glob_active,
                                         get_extglob_mode())
                           .has_value();

  if (!has_glob) {
    let single_result = ArrayList<String>{scratch};
    single_result.push(steal(field.text));
    return steal(single_result).make_sorted(sort_order::ascending);
  }

  let pattern = String{scratch};
  pattern.append(field.text.view());
  let const has_literal_glob = field.has_literal_glob;

  let input = ArrayList<glob_field>{scratch};
  input.push(steal(field));
  let fields = expand_path_recurse(steal(input));

  let values = ArrayList<String>{scratch};
  values.reserve(fields.count());

  let const glob_ignore_value = runtime_state().was_glob_ignore_assigned()
                                    ? get_variable_value("GLOBIGNORE")
                                    : Maybe<String>{};
  let const has_glob_ignore =
      glob_ignore_value.has_value() && !glob_ignore_value->is_empty();
  let const should_fold_case = is_shopt_enabled("nocaseglob");
  let const ignored_patterns =
      has_glob_ignore ? parse_glob_ignore_patterns(glob_ignore_value->view(),
                                                   should_fold_case, scratch)
                      : ArrayList<glob_ignore_pattern>{scratch};
  let const extglob = get_extglob_mode();
  let const do_is_ignored = [&](StringView path) throws -> bool {
    let const last_slash = path.find_last_character('/');
    let const name =
        last_slash.has_value() ? path.substring(*last_slash + 1) : path;
    if (name == "." || name == "..") {
      return true;
    }

    let const folded_path =
        should_fold_case ? path.to_lower_ascii(scratch) : String{scratch};
    let const subject = should_fold_case ? folded_path.view() : path;
    let const charset = get_glob_charset_for(subject);
    for (let const &pattern : ignored_patterns) {
      if (glob_ignore_pattern_matches(pattern, subject, extglob, charset)) {
        return true;
      }
    }

    return false;
  };
  for (let &f : fields) {
    if (has_glob_ignore && do_is_ignored(f.text.view())) {
      continue;
    }

    values.push(steal(f.text));
  }

  LOG(All, "the glob pattern '%s' matched %zu paths", pattern.c_str(),
      values.count());

  if (values.count() == 0) {
    let const failglob_is_on = runtime_state().failglob();
    let const failglob_is_explicit =
        runtime_state().was_failglob_set_explicitly();
    let const is_failglob_fatal =
        failglob_is_on &&
        (failglob_is_explicit || !strict_diagnostics_are_warnings());
    if (!expansion_store().glob_exempt_for_test() &&
        (is_failglob_fatal || has_literal_glob))
    {
      try {
        warn_or_throw(failglob_is_on, failglob_is_explicit, location,
                      "The glob pattern '" + pattern +
                          "' matched no file, it expands to its literal text, "
                          "which is rarely intended",
                      "Probe for matches with compgen -G '" + pattern +
                          "' or relax it with shopt -u failglob outside the "
                          "kosh mood");
      } catch (ErrorBase &error) {
        mark_expansion_error(error, expansion_error_reach::Line);
        throw;
      }
    }

    if (expansion_store().glob_exempt_for_test() ||
        !is_shopt_enabled("nullglob"))
    {
      values.push(steal(pattern));
    }
  }

  return steal(values).make_sorted(sort_order::ascending);
}

fn EvalContext::expand_glob_lenient(StringView pattern) throws
    -> SortedArrayList<String, order_comparator<String>>
{
  let const scratch = scratch_allocator();
  let values = ArrayList<String>{scratch};

  let field = glob_field{scratch};
  field.text.append(pattern);
  field.glob_active.reserve(pattern.length);
  for (usize i = 0; i < pattern.length; i++)
    field.glob_active.push(true);

  if (!first_active_glob(field.text.view(), field.glob_active,
                         get_extglob_mode())
           .has_value())
  {
    LOG(Debug, "compgen -G probe of '%.*s' has no glob, checking existence",
        static_cast<int>(pattern.length), pattern.data);
    if (Path{pattern, scratch}.exists()) values.push(String{scratch, pattern});
    return steal(values).make_sorted(sort_order::ascending);
  }

  let input = ArrayList<glob_field>{scratch};
  input.push(steal(field));
  for (let &f : expand_path_recurse(steal(input)))
    values.push(steal(f.text));
  LOG(Debug, "compgen -G probe matched %zu paths", values.count());
  return steal(values).make_sorted(sort_order::ascending);
}

} /* namespace koshka */
