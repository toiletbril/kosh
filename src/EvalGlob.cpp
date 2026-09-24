/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements pathname expansion, recursive globstar traversal,
 * tolerant glob expansion, and tilde expansion. Segment masks distinguish
 * active pattern bytes from quoted or literal text. The split confines
 * filesystem traversal and pattern expansion outside general word expansion.
 */

#include "base/Arena.hpp"
#include "base/Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "base/Path.hpp"
#include "Platform.hpp"
#include "base/Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace {

fn name_matches_glob(StringView glob, StringView filename,
                     const Bitset &glob_active, usize mask_offset, bool extglob,
                     bool should_ignore_case, Allocator allocator) throws
    -> bool
{
  if (!should_ignore_case)
    return utils::glob_matches(glob, filename, glob_active, mask_offset,
                               extglob);

  /* The glob arrives already lowered from the caller, so only the per-entry
     filename is lowered here. Lowering preserves length, so the active mask
     stays aligned. */
  let const lowered_name = filename.to_lower_ascii(allocator);

  return utils::glob_matches(glob, lowered_name.view(), glob_active,
                             mask_offset, extglob);
}

} /* namespace */

fn EvalContext::expand_path_once(const glob_field &field,
                                 bool should_expand_files) throws
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

  /* A missing or unreadable parent directory yields no match, so the caller
     applies the failglob policy. */
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

  /* The typed prefix is preserved on each match so a dot-slash glob yields a
     dot-slash result. */
  let const typed_prefix =
      has_slashes ? path.substring_of_length(0, stem_start) : StringView{};

  ASSERT(has_glob);
  ASSERT(!glob.is_empty());

  /* The directory read omits . and .. , so a dotted pattern that should reach
     them has them fed back in unless globskipdots keeps them out. */
  let const pattern_leads_with_dot = glob[0] == '.';
  if (pattern_leads_with_dot && !is_shopt_enabled("globskipdots")) {
    entries->push(
        Path::directory_child{String{"."}, Path::entry_kind::Directory});
    entries->push(
        Path::directory_child{String{".."}, Path::entry_kind::Directory});
  }

  let const dotglob_is_on = is_shopt_enabled("dotglob");
  let const nocaseglob_is_on = is_shopt_enabled("nocaseglob");
  let const is_extglob_enabled = extglob_enabled();

  let lowered_glob = String{scratch};
  if (nocaseglob_is_on) lowered_glob = glob.to_lower_ascii(scratch);
  let const match_glob = nocaseglob_is_on ? lowered_glob.view() : glob;

  let const do_entry_matches = [&](const Path::directory_child &entry)
                                   throws -> bool {
    let const filename = entry.name.view();

    /* A leading-dot-less pattern skips a dotfile unless dotglob is on. */
    if (filename == "." || filename == "..") {
      if (!pattern_leads_with_dot) return false;
    } else if (!pattern_leads_with_dot && !filename.is_empty() &&
               filename[0] == '.' && !dotglob_is_on)
    {
      return false;
    }

    return name_matches_glob(match_glob, filename, field.glob_active,
                             stem_start, is_extglob_enabled, nocaseglob_is_on,
                             scratch);
  };
  let const do_append_entry = [&](StringView filename) throws -> void {
    add_expansion();

    let result_field = glob_field{scratch};
    result_field.text.append(typed_prefix);
    result_field.text.append(filename);
    expanded.push(steal(result_field));
  };

  if (should_expand_files) {
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
    full_path.push_component(entry.name.view());
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

/* The index of the first active metacharacter that actually forms a glob. A '['
   without a later ']' is a literal bracket. None when the field is all
   literal. */
hot pure fn first_active_glob(StringView text, const Bitset &mask,
                              bool extglob) wontthrow -> Maybe<usize>
{
  let open_bracket = Maybe<usize>{};
  /* An absent tail mask entry counts as inert, so an empty mask names a fully
     quoted or literal word with no glob. */
  for (usize i = 0; i < text.length; i++) {
    if (i >= mask.count() || !mask[i]) {
      continue;
    }

    let const ch = text.data[i];
    if (extglob && i + 1 < text.length && lexer::is_extglob_operator(ch) &&
        text.data[i + 1] == '(')
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

/* The recursion depth cap for a ** walk, stopping a symlink cycle from looping
   forever. */
constexpr usize GLOBSTAR_MAX_DEPTH = 256;

/* In directory position only subdirectories are collected and the base is added
   as the empty path so ** can match zero levels. As a trailing component every
   entry is collected. */
fn collect_globstar_paths(const Path &dir, StringView relative,
                          bool directories_only, bool should_match_dotfiles,
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
    child_path.push_component(name);
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
    child_dir.push_component(name);

    let child_relative = String{allocator};
    child_relative.reserve(relative.length + name.length + 1);
    if (!relative.is_empty()) {
      child_relative.append(relative);
      child_relative += '/';
    }
    child_relative.append(name);

    if (!directories_only || is_directory[index]) {
      out.push(String{allocator, child_relative.view()});
    }
    /* A directory symlink is a match but is not descended into, so a self or
       parent symlink does not spin the walk to the depth cap. */
    if (is_directory[index] && !is_symbolic_link[index]) {
      collect_globstar_paths(child_dir, child_relative.view(), directories_only,
                             should_match_dotfiles, false, depth + 1, allocator,
                             out);
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
        field.text.view(), field.glob_active, extglob_enabled());
    if (glob_index.has_value()) should_batch_literals = false;
    glob_indices.push(glob_index);
  }

  if (should_batch_literals) {
    let literal_paths = ArrayList<Path>{scratch};
    literal_paths.reserve(fields.count());
    for (let const &field : fields)
      literal_paths.push(Path{field.text.view()});

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
      /* This field is a literal suffix appended after an earlier glob, so keep
         it only when it exists. */
      if (Path{field.text.view()}.exists()) result.push(steal(field));
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
      let relatives = ArrayList<String>{scratch};
      collect_globstar_paths(base, StringView{""}, directory_position,
                             is_shopt_enabled("dotglob"), true, 0, scratch,
                             relatives);

      /* The base directory is the zero-level match, emitted as the bare prefix,
         skipped when the prefix is empty so a bare ** does not yield the
         current directory. */
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

      /* In a directory position the globstar stands in for zero or more levels,
         so the suffix is matched in the base and every descendant directory. */
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
      let expanded_files = expand_path_once(field, true);
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

    let expanded_directories = expand_path_once(directory_component, false);

    /* Each match came back all-literal, so its false mask entries are restored
       before the suffix mask to keep the mask aligned with the text. */
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

  /* A tilde prefix that runs to the segment's end while the word continues in a
     later segment carries a quoted character, so bash leaves the whole word
     literal. */
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
  /* ~+ is PWD and ~- is OLDPWD. */
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

fn EvalContext::expand_colon_tildes(WordSegment &segment,
                                    bool word_continues) const throws -> void
{
  if (!segment.is_tilde_candidate()) return;
  let const view = segment.text.view();
  let rewritten = String{heap_allocator()};
  let was_changed = false;
  usize i = 0;
  while (i < view.length) {
    if (view[i] == ':' && i + 1 < view.length && view[i + 1] == '~') {
      usize prefix_end = i + 2;
      while (prefix_end < view.length && view[prefix_end] != '/' &&
             view[prefix_end] != ':')
        prefix_end++;
      if (!(prefix_end == view.length && word_continues)) {
        let const name = view.substring_of_length(i + 2, prefix_end - i - 2);
        if (let const directory = resolve_tilde_prefix(name)) {
          rewritten += ':';
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

hot fn EvalContext::expand_path(glob_field field,
                                const SourceLocation &location) throws
    -> ArrayList<String>
{
  let const scratch = scratch_allocator();

  /* Fast path. A field with no glob is its own single result. */
  let const has_glob =
      !no_glob() &&
      first_active_glob(field.text.view(), field.glob_active, extglob_enabled())
          .has_value();

  if (!has_glob) {
    let single_result = ArrayList<String>{scratch};
    single_result.push(steal(field.text));
    return single_result;
  }

  /* The pattern is kept so a glob that matches None falls back to it. */
  let pattern = String{scratch};
  pattern.append(field.text.view());

  let input = ArrayList<glob_field>{scratch};
  input.push(steal(field));
  let fields = expand_path_recurse(steal(input));

  let values = ArrayList<String>{scratch};
  values.reserve(fields.count());
  for (let &f : fields)
    values.push(steal(f.text));

  values.sort();

  LOG(All, "the glob pattern '%s' matched %zu paths", pattern.c_str(),
      values.count());

  /* A glob that matches no file is a hard error by default, or the POSIX
     literal fallback with failglob off. A test or [ command is exempt so a glob
     probing for a file keeps its literal text. */
  if (values.count() == 0) {
    let const failglob_is_on = failglob() || is_shopt_enabled("failglob");
    let const failglob_is_explicit =
        m_runtime.was_failglob_set_explicitly() || is_shopt_enabled("failglob");
    if (!m_glob_exempt_for_test)
      warn_or_throw(failglob_is_on, failglob_is_explicit, location,
                    "The glob pattern '" + pattern +
                        "' matched no file, it expands to its literal text, "
                        "which is rarely intended",
                    "Probe for matches with compgen -G '" + pattern +
                        "' or relax with set +o failglob");
    /* nullglob drops a no-match glob entirely, while the default and a test
       probe keep its literal text. */
    if (m_glob_exempt_for_test || !is_shopt_enabled("nullglob")) {
      values.push(steal(pattern));
    }
  }

  return values;
}

/* The compgen -G probe, a glob expansion that never trips failglob. */
fn EvalContext::expand_glob_lenient(StringView pattern) throws
    -> ArrayList<String>
{
  let const scratch = scratch_allocator();
  let values = ArrayList<String>{scratch};

  let field = glob_field{scratch};
  field.text.append(pattern);
  field.glob_active.reserve(pattern.length);
  for (usize i = 0; i < pattern.length; i++)
    field.glob_active.push(true);

  if (!first_active_glob(field.text.view(), field.glob_active,
                         extglob_enabled())
           .has_value())
  {
    LOG(Debug, "compgen -G probe of '%.*s' has no glob, checking existence",
        static_cast<int>(pattern.length), pattern.data);
    if (Path{pattern}.exists()) values.push(String{scratch, pattern});
    return values;
  }

  let input = ArrayList<glob_field>{scratch};
  input.push(steal(field));
  for (let &f : expand_path_recurse(steal(input)))
    values.push(steal(f.text));
  values.sort();
  LOG(Debug, "compgen -G probe matched %zu paths", values.count());
  return values;
}

} /* namespace koshka */
