#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
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
  let lowered_name = String{allocator};
  lowered_name.reserve(filename.length);
  for (usize i = 0; i < filename.length; i++)
    lowered_name += utils::ascii_to_lower(filename[i]);

  return utils::glob_matches(glob, lowered_name.view(), glob_active,
                             mask_offset, extglob);
}

} // namespace

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
  let entries = Path::read_directory(parent_dir);
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
    entries->push(String{"."});
    entries->push(String{".."});
  }

  let const dotglob_is_on = is_shopt_enabled("dotglob");
  let const nocaseglob_is_on = is_shopt_enabled("nocaseglob");

  let lowered_glob = String{scratch};
  if (nocaseglob_is_on) {
    lowered_glob.reserve(glob.length);
    for (usize i = 0; i < glob.length; i++)
      lowered_glob += utils::ascii_to_lower(glob[i]);
  }
  let const match_glob = nocaseglob_is_on ? lowered_glob.view() : glob;

  for (let const &entry_name : *entries) {
    let const filename = entry_name.view();

    if (!should_expand_files) {
      let full_path = parent_dir;
      full_path.push_component(filename);
      if (!full_path.is_directory()) continue;
    }

    /* A leading-dot-less pattern skips a dotfile unless dotglob is on. */
    if (filename == "." || filename == "..") {
      if (!pattern_leads_with_dot) continue;
    } else if (!pattern_leads_with_dot && !filename.is_empty() &&
               filename[0] == '.' && !dotglob_is_on)
    {
      continue;
    }

    if (name_matches_glob(match_glob, filename, field.glob_active, stem_start,
                          extglob_enabled(), nocaseglob_is_on, scratch))
    {
      add_expansion();

      /* A real filename is literal, so the result field never globs again. */
      let result_field = glob_field{scratch};
      result_field.text.append(typed_prefix);
      result_field.text.append(filename);
      expanded.push(steal(result_field));
    }
  }

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
    if (i >= mask.count() || !mask[i]) continue;

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
  if (directories_only && include_base) out.push(String{allocator, relative});
  if (depth >= GLOBSTAR_MAX_DEPTH) return;

  let const entries = Path::read_directory_typed(dir);
  if (!entries.has_value()) return;

  for (let const &entry : *entries) {
    let const name = entry.name.view();
    if (!should_match_dotfiles && !name.is_empty() && name[0] == '.') {
      continue;
    }

    let child_dir = dir;
    child_dir.push_component(name);

    bool is_dir = false;
    bool is_link = false;
    switch (entry.kind) {
    case Path::entry_kind::Directory: is_dir = true; break;
    case Path::entry_kind::Symlink:
      is_link = true;
      is_dir = child_dir.is_directory();
      break;
    case Path::entry_kind::Regular:
    case Path::entry_kind::Other: break;
    case Path::entry_kind::Unknown:
      is_link = child_dir.is_symbolic_link();
      is_dir = child_dir.is_directory();
      break;
    }

    let child_relative = String{allocator};
    child_relative.reserve(relative.length + name.length + 1);
    if (!relative.is_empty()) {
      child_relative.append(relative);
      child_relative += '/';
    }
    child_relative.append(name);

    if (!directories_only || is_dir) {
      out.push(String{allocator, child_relative.view()});
    }
    /* A directory symlink is a match but is not descended into, so a self or
       parent symlink does not spin the walk to the depth cap. */
    if (is_dir && !is_link) {
      collect_globstar_paths(child_dir, child_relative.view(), directories_only,
                             should_match_dotfiles, false, depth + 1, allocator,
                             out);
    }
  }
}

} // namespace

fn EvalContext::expand_path_recurse(ArrayList<glob_field> fields) throws
    -> ArrayList<glob_field>
{
  let const scratch = scratch_allocator();
  let result = ArrayList<glob_field>{scratch};

  for (let &field : fields) {
    let const text = field.text.view();

    let const glob_index =
        first_active_glob(text, field.glob_active, extglob_enabled());

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
  if (text.is_empty() || text[0] != '~') return;

  usize name_end = 1;
  while (name_end < text.length() && text[name_end] != '/' &&
         !(stop_at_colon && text[name_end] == ':'))
    name_end++;
  let const name = text.view().substring_of_length(1, name_end - 1);

  /* A tilde prefix that runs to the segment's end while the word continues in a
     later segment carries a quoted character, so bash leaves the whole word
     literal. */
  if (name_end == text.length() && word_continues) return;

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
  text = steal(expanded);
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
    segment.text = steal(rewritten);
  }
}

hot fn EvalContext::expand_path(glob_field field,
                                SourceLocation location) throws
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
        m_runtime.failglob_explicit || is_shopt_enabled("failglob");
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

} // namespace koshka
