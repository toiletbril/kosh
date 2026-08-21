#include "Arena.hpp"
#include "Builtin.hpp"
#include "Colors.hpp"
#include "Completion.hpp"
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

struct help_entry
{
  String name;
  String description;
};

static fn parse_help_subcommands(StringView text, StringView command) throws
    -> ArrayList<help_entry>;
static fn manpage_subcommands_for(StringView page_name, StringView command,
                                  EvalContext &context) throws
    -> const ArrayList<help_entry> &;
static fn is_known_help_subcommand(ProgramResolver &resolver,
                                   StringView command,
                                   StringView subcommand_prefix,
                                   StringView word) throws -> bool;

static fn matches_from_help_entries(const ArrayList<help_entry> &entries,
                                    StringView token,
                                    StringMap<String> &descriptions) throws
    -> ArrayList<String>
{
  let matches = ArrayList<String>{heap_allocator()};
  for (let const &entry : entries)
    if (entry.name.view().starts_with(token)) {
      matches.push(String{entry.name.view()});
      if (!entry.description.is_empty())
        descriptions.set(entry.name.view(), String{entry.description.view()});
    }
  return matches;
}

/* An empty list is cached too, so a command with no manpage is not retried. */
static StringMap<ArrayList<help_entry>> MANPAGE_OPTION_CACHE{heap_allocator()};
static StringMap<ArrayList<help_entry>> MANPAGE_SUBCOMMAND_CACHE{
    heap_allocator()};

static fn manpage_name_for(StringView command) throws -> String
{
  if (Maybe<const char *> alias = MANPAGE_ALIASES.find(command);
      alias.has_value())
    return String{alias.value()};

  return String{command};
}

static StringMap<ArrayList<String>> MAN_SUBCOMMAND_INDEX{heap_allocator()};
/* Every stripped section-1 page name mapped to its full file path, the
   existence gate for the subcommand split. */
static StringMap<String> MAN_PAGE_FILE_PATHS{heap_allocator()};
static StringMap<bool> MAN_SUBCOMMAND_PAGE_VALID{heap_allocator()};
static bool is_man_subcommand_index_built = false;

/* A fork that runs past this budget is killed and caches the empty string, so
   the prompt never freezes and the command is not forked again this session. */
static constexpr u64 HELP_FORK_TIMEOUT_NANOS = 1'000'000'000;
static constexpr usize HELP_OUTPUT_LIMIT = 4 * 1024 * 1024;

static fn
capture_completion_program_output(const ArrayList<String> &arguments) wontthrow
    -> Maybe<String>
{
  return os::capture_program_output(arguments, HELP_FORK_TIMEOUT_NANOS,
                                    HELP_OUTPUT_LIMIT);
}

/* An empty $MANPATH segment stands for the system defaults at that position,
   the manpath(1) reading. */
static fn manpage_section1_directories(ProgramResolver &resolver) throws
    -> ArrayList<Path>;

/* A trusted `manpath` or `man --path` run reports every man root the system
   resolves, including the macOS CommandLineTools root a bare $MANPATH leaves
   out. A result line is colon-separated. The fork happens once and the output
   is cached for the session. */
static fn manpath_command_output(ProgramResolver &resolver) throws -> StringView
{
  static bool was_tried = false;
  static String cached{heap_allocator()};
  if (was_tried) return cached.view();
  was_tried = true;

  let const man_paths =
      resolver.search("manpath", ProgramResolver::SearchMode::First,
                      ProgramResolver::Requirement::Runnable,
                      ProgramResolver::CachePolicy::Bypass);
  let const manbin_paths =
      resolver.search("man", ProgramResolver::SearchMode::First,
                      ProgramResolver::Requirement::Runnable,
                      ProgramResolver::CachePolicy::Bypass);
  let const manpath_present =
      !man_paths.is_empty() &&
      os::directory_is_trusted_for_exec(man_paths[0].parent());
  let const man_present =
      !manbin_paths.is_empty() &&
      os::directory_is_trusted_for_exec(manbin_paths[0].parent());
  if (manpath_present) {
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{man_paths[0].text().view()});
    if (Maybe<String> output = capture_completion_program_output(argv);
        output.has_value())
      cached = steal(*output);
  } else if (man_present) {
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{manbin_paths[0].text().view()});
    argv.push(String{"--path"});
    if (Maybe<String> output = capture_completion_program_output(argv);
        output.has_value())
      cached = steal(*output);
  }
  return cached.view();
}

static fn manpage_section1_directories(ProgramResolver &resolver) throws
    -> ArrayList<Path>
{
  let directories = ArrayList<Path>{heap_allocator()};
  let seen_roots = HashSet{heap_allocator()};

  let const do_push_man1_of_root = [&](StringView root) {
    if (!seen_roots.add(root)) return;
    let directory = Path{root};
    directory.push_component("man1");
    directories.push(steal(directory));
  };
  let const do_push_default_roots = [&]() {
    do_push_man1_of_root("/usr/local/share/man");
    do_push_man1_of_root("/usr/share/man");
  };
  let const do_push_command_roots = [&]() {
    let const view = manpath_command_output(resolver);
    usize segment_start = 0;
    for (usize i = 0; i <= view.length; i++) {
      if (i != view.length && view[i] != os::PATH_DELIMITER &&
          view[i] != '\n' && view[i] != '\r')
        continue;
      let const segment =
          view.substring_of_length(segment_start, i - segment_start)
              .trim_blanks();
      segment_start = i + 1;
      if (!segment.is_empty()) do_push_man1_of_root(segment);
    }
  };

  let const manpath = os::get_environment_variable("MANPATH");
  if (!manpath.has_value() || manpath->is_empty()) {
    do_push_default_roots();
    do_push_command_roots();
    return directories;
  }

  let const value = manpath->view();
  usize segment_start = 0;
  for (usize i = 0; i <= value.length; i++) {
    if (i != value.length && value[i] != os::PATH_DELIMITER) continue;
    let const segment =
        value.substring_of_length(segment_start, i - segment_start);
    segment_start = i + 1;
    if (segment.is_empty())
      do_push_default_roots();
    else
      do_push_man1_of_root(segment);
  }
  do_push_command_roots();
  return directories;
}

static const StringView COMPRESSION_SUFFIXES[] = {
    StringView{".gz"}, StringView{".xz"}, StringView{".zst"},
    StringView{".bz2"}};

static pure fn has_compression_suffix(StringView name) wontthrow -> bool
{
  for (let const tail : COMPRESSION_SUFFIXES) {
    if (name.length > tail.length &&
        name.substring(name.length - tail.length) == tail)
    {
      return true;
    }
  }
  return false;
}

static pure fn double_space_gap(StringView row, usize from) wontthrow -> usize
{
  for (usize j = from; j + 1 < row.length; j++) {
    if (row[j] == ' ' && row[j + 1] == ' ') {
      return j;
    }
  }
  return row.length;
}

static pure fn strip_man1_suffix(StringView entry) wontthrow
    -> Maybe<StringView>
{
  let name = entry;
  for (let const tail : COMPRESSION_SUFFIXES) {
    if (name.length > tail.length &&
        name.substring(name.length - tail.length) == tail)
    {
      name = name.substring_of_length(0, name.length - tail.length);
      break;
    }
  }
  if (name.length > 2 && name.substring(name.length - 2) == ".1") {
    return name.substring_of_length(0, name.length - 2);
  }
  return None;
}

/* The tail is a subcommand only when the head page exists too, so xdg-open
   invents no xdg, and a digit-leading version tail is none. */
static fn build_man_subcommand_index(ProgramResolver &resolver) throws -> void
{
  is_man_subcommand_index_built = true;
  for (let const &directory : manpage_section1_directories(resolver)) {
    LOG(Info, "scanning man1 directory '%s'", directory.text().c_str());
    let entries = Path::read_directory(directory);
    if (!entries.has_value()) {
      LOG(Debug, "directory '%s' is unreadable, skipping",
          directory.text().c_str());
      continue;
    }
    MAN_PAGE_FILE_PATHS.reserve(MAN_PAGE_FILE_PATHS.count() + entries->count());
    for (let const &entry : *entries) {
      let const stripped = strip_man1_suffix(entry.view());
      if (!stripped.has_value() || stripped->is_empty()) continue;
      if (MAN_PAGE_FILE_PATHS.find(*stripped) != nullptr) continue;
      let file_path = directory.clone();
      file_path.push_component(entry.view());
      MAN_PAGE_FILE_PATHS.set(*stripped, String{file_path.text().view()});
    }
  }
  MAN_PAGE_FILE_PATHS.for_each([&](StringView name, const String &) {
    let const dash = name.find_character('-');
    if (!dash.has_value() || *dash == 0) return;
    let const head = name.substring_of_length(0, *dash);
    let const tail = name.substring(*dash + 1);
    if (tail.is_empty() || (tail[0] >= '0' && tail[0] <= '9')) return;
    if (MAN_PAGE_FILE_PATHS.find(head) == nullptr) return;
    MAN_SUBCOMMAND_INDEX
        .get_or_create(head, ArrayList<String>{heap_allocator()})
        .push(String{tail});
  });
  LOG(Info, "indexed %zu section-1 pages", MAN_PAGE_FILE_PATHS.count());
}

/* Empty when the page has no synopsis. */
static fn cleaned_synopsis_of_page(StringView source) throws -> String
{
  let synopsis = String{heap_allocator()};
  let is_inside_synopsis = false;
  usize line_start = 0;
  for (usize i = 0; i <= source.length; i++) {
    if (i != source.length && source[i] != '\n') continue;
    let const line = source.substring_of_length(line_start, i - line_start);
    line_start = i + 1;
    if (line.starts_with(".SH") || line.starts_with(".Sh")) {
      let is_synopsis_heading = false;
      for (usize j = 0; j + 8 <= line.length && !is_synopsis_heading; j++)
        is_synopsis_heading = line.substring(j).starts_with("SYNOPSIS");
      if (is_inside_synopsis && !is_synopsis_heading) break;
      is_inside_synopsis = is_synopsis_heading;
      continue;
    }
    if (!is_inside_synopsis) continue;
    for (usize j = 0; j < line.length; j++) {
      let const byte = line[j];
      if (byte == '\\' && j + 1 < line.length) {
        let const escaped = line[j + 1];
        if (escaped == 'f') {
          j += 2;
        } else if (escaped == '-') {
          synopsis.push('-');
          j++;
        } else if (escaped == '&') {
          j++;
        } else {
          synopsis.push(escaped);
          j++;
        }
        continue;
      }
      /* A CR at a CRLF line end folds like whitespace. */
      if (byte == ' ' || byte == '\t' || byte == '\r') {
        if (!synopsis.is_empty() &&
            synopsis.view()[synopsis.length() - 1] != ' ')
          synopsis.push(' ');
        continue;
      }
      synopsis.push(byte);
    }
    synopsis.push(' ');
  }
  return synopsis;
}

/* is_read_allowed is false on the ghost path, which trusts a cached verdict
   rather than scan a page on a keystroke. */
static fn man_subcommand_page_is_valid(StringView command,
                                       StringView subcommand,
                                       bool is_read_allowed) throws -> bool
{
  let page_name = String{command};
  page_name.push('-');
  page_name.append(subcommand);
  if (let const cached = MAN_SUBCOMMAND_PAGE_VALID.find(page_name.view());
      cached != nullptr)
    return *cached;
  if (!is_read_allowed) return false;

  let const file_path = MAN_PAGE_FILE_PATHS.find(page_name.view());
  if (file_path == nullptr) {
    MAN_SUBCOMMAND_PAGE_VALID.set(page_name.view(), false);
    return false;
  }

  /* A compressed page cannot be scanned without a decompressor. */
  let const path_view = file_path->view();
  if (has_compression_suffix(path_view)) {
    MAN_SUBCOMMAND_PAGE_VALID.set(page_name.view(), true);
    return true;
  }

  let source = Path{file_path->view()}.read_entire_file();
  if (!source.has_value()) {
    MAN_SUBCOMMAND_PAGE_VALID.set(page_name.view(), true);
    return true;
  }
  /* A page that is one .so redirect reads its target relative to the man root
     above the section directory. */
  if (source->view().starts_with(".so ")) {
    let const rest = source->view().substring(4);
    usize target_end = 0;
    while (target_end < rest.length && rest[target_end] != '\n' &&
           rest[target_end] != ' ')
      target_end++;
    let target = Path{file_path->view()}.parent().parent();
    target.push_component(rest.substring_of_length(0, target_end));
    source = target.read_entire_file();
    if (!source.has_value()) {
      MAN_SUBCOMMAND_PAGE_VALID.set(page_name.view(), true);
      return true;
    }
  }

  let const synopsis = cleaned_synopsis_of_page(source->view());
  let needle = String{command};
  needle.push(' ');
  needle.append(subcommand);
  let const valid = synopsis.find_substring(needle.view()).has_value();
  MAN_SUBCOMMAND_PAGE_VALID.set(page_name.view(), valid);
  return valid;
}

static fn is_first_argument_token(StringView line, usize token_start) wontthrow
    -> bool
{
  let const command = command_word_of(line);
  if (command.is_empty()) return false;
  let const command_end =
      static_cast<usize>(command.data - line.data) + command.length;
  if (token_start <= command_end) return false;
  for (usize i = command_end; i < token_start; i++)
    if (line[i] != ' ' && line[i] != '\t') return false;
  return true;
}

/* None when the line has no completed second word or it opens with a dash. */
fn second_word_of(StringView line) wontthrow -> Maybe<StringView>
{
  let const command = command_word_of(line);
  if (command.is_empty()) return None;
  let const command_end =
      static_cast<usize>(command.data - line.data) + command.length;
  let position = command_end;
  let const word = line.next_ascii_whitespace_word(position);
  /* A word the cursor still sits in is the token under completion, not a
     settled subcommand. */
  if (position >= line.length) return None;
  if (word.is_empty() || word[0] == '-') return None;
  return word;
}

/* The ghost path reads only an already built and validated entry, so a
   keystroke never scans a directory or reads a page. */
fn complete_from_man_subcommands(StringView line, StringView token,
                                 usize token_start, completion_mode mode,
                                 EvalContext &context,
                                 StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>
{
  let const for_listing = mode == completion_mode::Listing;
  if (!token.is_empty() && token[0] == '-') return None;
  if (os::has_directory_separator(token)) return None;
  if (!for_listing && token.is_empty()) return None;
  if (!is_first_argument_token(line, token_start)) return None;
  let const surface_command = command_word_of(line);
  if (surface_command.is_empty() ||
      os::has_directory_separator(surface_command))
    return None;

  let const resolved_name =
      resolve_completion_command(surface_command, context);
  let const command = resolved_name.view();

  if (!is_man_subcommand_index_built) {
    if (!for_listing) return None;
    build_man_subcommand_index(context.get_program_resolver());
  }

  if (!for_listing) {
    let const subcommands = MAN_SUBCOMMAND_INDEX.find(command);
    if (subcommands == nullptr) return None;
    let matches = ArrayList<String>{heap_allocator()};
    for (let const &subcommand : *subcommands)
      if (subcommand.view().starts_with(token) &&
          man_subcommand_page_is_valid(command, subcommand.view(), false))
        matches.push(String{subcommand.view()});
    if (matches.is_empty()) return None;
    return matches;
  }

  let matches = ArrayList<String>{heap_allocator()};
  let seen = HashSet{heap_allocator()};
  if (let const subcommands = MAN_SUBCOMMAND_INDEX.find(command);
      subcommands != nullptr)
    for (let const &subcommand : *subcommands)
      if (subcommand.view().starts_with(token) &&
          man_subcommand_page_is_valid(command, subcommand.view(), for_listing))
      {
        matches.push(String{subcommand.view()});
        seen.add(subcommand.view());
      }

  let const page_name = manpage_name_for(command);
  if (MAN_PAGE_FILE_PATHS.find(page_name.view()) != nullptr) {
    let const &subcommands =
        manpage_subcommands_for(page_name.view(), command, context);
    for (let const &subcommand : subcommands)
      if (subcommand.name.view().starts_with(token)) {
        if (seen.add(subcommand.name.view()))
          matches.push(String{subcommand.name.view()});
        if (!subcommand.description.is_empty())
          descriptions.set(subcommand.name.view(),
                           String{subcommand.description.view()});
      }
  }
  LOG(Debug, "%zu subcommands of '%.*s' match token '%.*s'", matches.count(),
      static_cast<int>(command.length), command.data,
      static_cast<int>(token.length), token.data);
  if (matches.is_empty()) return None;
  return matches;
}

static fn extract_dash_flags(StringView option_part) throws -> ArrayList<String>
{
  let flags = ArrayList<String>{heap_allocator()};
  usize k = 0;
  while (k < option_part.length) {
    while (k < option_part.length &&
           (option_part[k] == ' ' || option_part[k] == ',' ||
            option_part[k] == '\t'))
      k++;
    let const token_start = k;
    while (k < option_part.length && option_part[k] != ' ' &&
           option_part[k] != ',' && option_part[k] != '\t')
      k++;
    let flag = option_part.substring_of_length(token_start, k - token_start);
    if (let const equals = flag.find_character('='); equals.has_value())
      flag = flag.substring_of_length(0, *equals);
    if (flag.length >= 2 && flag[0] == '-') flags.push(String{flag});
  }
  return flags;
}

/* man's overstrike formatting, a byte backspace byte for bold and an underscore
   backspace char for an underline, is stripped first. */
static fn parse_manpage_option_entries(StringView text) throws

    -> ArrayList<help_entry>
{
  let clean = String{heap_allocator()};
  clean.reserve(text.length);
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\b') {
      if (!clean.is_empty()) clean.pop_back();
      continue;
    }
    clean += text[i];
  }
  let const view = clean.view();

  let descriptions = StringMap<String>{heap_allocator()};
  let pending_flags = ArrayList<String>{heap_allocator()};
  usize pending_indent = 0;
  let pending_description = String{heap_allocator()};

  let const do_finalize_pending = [&]() throws -> void {
    if (pending_flags.is_empty()) return;
    let const desc = pending_description.view().trim_blanks();
    for (let const &flag : pending_flags)
      if (!desc.is_empty() && descriptions.find(flag.view()) == nullptr) {
        descriptions.set(flag.view(), String{desc});
      }
    pending_flags.clear();
    pending_description.clear();
  };

  usize i = 0;
  while (i < view.length) {
    let const raw = view.next_line(i);

    let const indent = skip_blanks(raw, 0);
    if (indent >= raw.length) {
      do_finalize_pending();
      continue;
    }

    /* A dashless line at or below the option's indent continues the wrapped
       description. */
    if (!pending_flags.is_empty() && raw[indent] != '-' &&
        indent >= pending_indent)
    {
      let const piece =
          raw.substring_of_length(indent, raw.length - indent).trim_blanks();
      if (!piece.is_empty()) {
        if (!pending_description.is_empty()) pending_description += ' ';
        pending_description.append(piece);
      }
      continue;
    }

    do_finalize_pending();
    if (raw[indent] != '-') continue;

    let const gap = double_space_gap(raw, indent);
    let const option_part = raw.substring_of_length(indent, gap - indent);
    pending_flags = extract_dash_flags(option_part);
    pending_indent = indent;
    pending_description = String{heap_allocator()};
    if (gap < raw.length)
      pending_description.append(
          raw.substring_of_length(gap, raw.length - gap).trim_blanks());
  }
  do_finalize_pending();

  let entries = ArrayList<help_entry>{heap_allocator()};
  let seen = HashSet{heap_allocator()};
  for (usize j = 0; j < view.length; j++) {
    let const at_word_start = j == 0 || view[j - 1] == ' ' ||
                              view[j - 1] == '\t' || view[j - 1] == '\n' ||
                              view[j - 1] == '(' || view[j - 1] == '[';
    if (view[j] != '-' || !at_word_start) continue;
    let end = j;
    while (end < view.length &&
           (view[end] == '-' || lexer::is_variable_name(view[end])))
      end++;
    let const flag = view.substring_of_length(j, end - j);
    let has_letter = false;
    for (usize k = 0; k < flag.length; k++)
      if (flag[k] != '-') {
        has_letter = !(flag[k] >= '0' && flag[k] <= '9');
        if (has_letter) break;
      }
    if (flag.length >= 2 && has_letter && seen.add(flag)) {
      let const description = descriptions.find(flag);
      entries.push(help_entry{String{flag}, description != nullptr
                                                ? String{description->view()}
                                                : String{heap_allocator()}});
    }
    j = end;
  }
  entries.shrink_to_fit();
  return entries;
}

/* Only a command whose help argument is not the plain --help qualifies, the
   ffmpeg family, whose manpage carries the options in a form the flag scanner
   does not read. */
static fn command_prefers_help_over_manpage(StringView command) throws -> bool

{
  let argument = HELP_ALLOWLIST.find(command);
  return argument.has_value() && StringView{*argument} != StringView{"--help"};
}

static fn command_path_is_trusted(StringView absolute_path) throws -> bool;

static fn manpage_options_for(StringView page_name, EvalContext &context) throws
    -> const ArrayList<help_entry> &
{
  if (let const cached = MANPAGE_OPTION_CACHE.find(page_name);
      cached != nullptr)
    return *cached;
  let const text = manpage_text_for(page_name, context);
  let parsed_options = parse_manpage_option_entries(text);
  return *MANPAGE_OPTION_CACHE.set(page_name, steal(parsed_options));
}

static fn manpage_subcommands_for(StringView page_name, StringView command,
                                  EvalContext &context) throws
    -> const ArrayList<help_entry> &
{
  let key = String{page_name};
  key += "\n";
  key += command;
  if (let const cached = MANPAGE_SUBCOMMAND_CACHE.find(key.view());
      cached != nullptr)
    return *cached;
  let const text = manpage_text_for(page_name, context);
  let parsed_subcommands = parse_help_subcommands(text, command);
  return *MANPAGE_SUBCOMMAND_CACHE.set(key.view(), steal(parsed_subcommands));
}

/* An empty entry records a page that is absent or untrusted, so the fork
   happens once per name for the session. */
static StringMap<String> MANPAGE_TEXT_CACHE{heap_allocator()};

fn manpage_text_for(StringView page_name, EvalContext &context) throws
    -> StringView
{
  if (let const cached = MANPAGE_TEXT_CACHE.find(page_name); cached != nullptr)
    return cached->view();

  let text = String{heap_allocator()};
  let const man_paths = context.get_program_resolver().search(
      "man", ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (man_paths.is_empty() ||
      !command_path_is_trusted(man_paths[0].text().view()))
  {
    LOG(Debug,
        "skipping the man fork for '%.*s' because man is absent or untrusted",
        static_cast<int>(page_name.length), page_name.data);

    return MANPAGE_TEXT_CACHE.set(page_name, steal(text))->view();
  }

  let locate_argv = ArrayList<String>{heap_allocator()};
  locate_argv.push(String{man_paths[0].text().view()});
  locate_argv.push(String{"-w"});
  locate_argv.push(String{page_name});
  let const location = capture_completion_program_output(locate_argv);
  if (!location.has_value() ||
      !location->view().find_character('/').has_value())
  {
    return MANPAGE_TEXT_CACHE.set(page_name, steal(text))->view();
  }

  let argv = ArrayList<String>{heap_allocator()};
  argv.push(String{man_paths[0].text().view()});
  argv.push(String{page_name});
  if (Maybe<String> page = capture_completion_program_output(argv);
      page.has_value() && !page->is_empty())
  {
    let const page_view = page->view();
    for (usize position = 0; position < page_view.length; position++) {
      let const byte = page_view[position];
      if (byte == '\b') {
        if (!text.is_empty()) text.pop_back();
        continue;
      }
      text.push(byte);
    }
  }

  return MANPAGE_TEXT_CACHE.set(page_name, steal(text))->view();
}

/* Runs only on an explicit tab and a dash token, so the ghost never forks man.
   None falls through to the spec and files. */
fn complete_from_manpage(StringView line, StringView token,
                         completion_mode mode, EvalContext &context,
                         StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>
{
  let const for_listing = mode == completion_mode::Listing;
  if (!for_listing) return None;
  if (token.is_empty() || token[0] != '-') return None;
  let const surface_command = command_word_of(line);
  if (surface_command.is_empty() ||
      os::has_directory_separator(surface_command))
    return None;

  let const resolved_name =
      resolve_completion_command(surface_command, context);
  let const command = resolved_name.view();

  if (command_prefers_help_over_manpage(command)) return None;

  /* git commit -<tab> reads the git-commit subcommand page when the index
     knows it. */
  let page_name = manpage_name_for(command);
  if (let const subcommand_word = second_word_of(line);
      subcommand_word.has_value())
  {
    if (!is_man_subcommand_index_built)
      build_man_subcommand_index(context.get_program_resolver());
    let combined = String{command};
    combined.push('-');
    combined.append(*subcommand_word);
    if (MAN_PAGE_FILE_PATHS.find(combined.view()) != nullptr) {
      page_name = steal(combined);
    } else {
      let is_known_man_subcommand = false;
      for (let const &entry :
           manpage_subcommands_for(page_name.view(), command, context))
        if (entry.name.view() == *subcommand_word) {
          is_known_man_subcommand = true;
          break;
        }
      if (is_known_man_subcommand ||
          is_known_help_subcommand(context.get_program_resolver(), command, {},
                                   *subcommand_word))
        return None;
    }
  }

  let const &options = manpage_options_for(page_name.view(), context);
  if (options.is_empty()) return None;

  let matches = matches_from_help_entries(options, token, descriptions);
  if (matches.is_empty()) return None;
  return matches;
}

/* One fork parses both the option and the subcommand caches so the raw text
   frees after. HELP_PARSED records a command that ran so it never forks twice.
 */
static StringMap<ArrayList<help_entry>> HELP_OPTION_CACHE{heap_allocator()};
static StringMap<ArrayList<help_entry>> HELP_SUBCOMMAND_CACHE{heap_allocator()};
static HashSet HELP_PARSED{heap_allocator()};

/* The check is permission-based, so a user tool directory like ~/.cargo/bin is
   trusted while a world-writable one like /tmp is not. */
static fn command_path_is_trusted(StringView absolute_path) throws -> bool
{
  if (!os::directory_is_trusted_for_exec(Path{absolute_path}.parent()))
    return false;
#if !defined _WIN32
  os::file_status status;
  if (!os::stat_path_following(absolute_path, status)) return false;
  let const is_owner_trusted =
      status.owner_id == 0 ||
      status.owner_id == static_cast<u32>(os::get_effective_user_id());
  if (!is_owner_trusted || (status.mode & 0022u) != 0) return false;
#endif
  return true;
}

static fn help_text_for(ProgramResolver &resolver, StringView command,
                        StringView subcommand = {},
                        bool should_use_default_argument = false) throws
    -> String
{
  let text = String{heap_allocator()};
  let help_argument = StringView{};
  if (let const configured_argument = HELP_ALLOWLIST.find(command);
      configured_argument.has_value())
    help_argument = StringView{*configured_argument};
  else if (should_use_default_argument)
    help_argument = StringView{"--help"};
  if (help_argument.is_empty()) return text;

  let const paths = resolver.search(command, ProgramResolver::SearchMode::First,
                                    ProgramResolver::Requirement::Runnable,
                                    ProgramResolver::CachePolicy::Bypass);
  if (!paths.is_empty() && command_path_is_trusted(paths[0].text().view())) {
    LOG(Debug, "the directory for '%.*s' is trusted, preparing the help fork",
        static_cast<int>(command.length), command.data);
    /* argv is the absolute path, then the subcommand chain split on spaces,
       then the help argument split on spaces, so git remote add runs as path,
       remote, add, --help. */
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{paths[0].text().view()});
    subcommand.for_each_ascii_whitespace_word(
        [&](StringView word) throws { argv.push(String{word}); });
    help_argument.for_each_ascii_whitespace_word(
        [&](StringView word) throws { argv.push(String{word}); });
    LOG(Debug, "forking '%.*s' for its --help text",
        static_cast<int>(command.length), command.data);
    if (Maybe<String> output = capture_completion_program_output(argv);
        output.has_value())
    {
      LOG(Debug, "the --help fork for '%.*s' produced %zu bytes",
          static_cast<int>(command.length), command.data, output->length());
      text = steal(*output);
    } else {
      LOG(Debug, "the --help fork for '%.*s' produced no output",
          static_cast<int>(command.length), command.data);
    }
  } else if (paths.is_empty()) {
    LOG(Debug, "the program '%.*s' is not on the path, skipping the help fork",
        static_cast<int>(command.length), command.data);
  } else {
    LOG(Debug,
        "the directory '%.*s' for '%.*s' is not trusted, skipping the help "
        "fork",
        static_cast<int>(paths[0].text().view().length),
        paths[0].text().view().data, static_cast<int>(command.length),
        command.data);
  }
  return text;
}

/* The parsed caches keep entries and free the raw text, so a reader that wants
   the text keeps its own copy. */
static StringMap<String> HELP_TEXT_CACHE{heap_allocator()};

fn help_text_of(StringView command, EvalContext &context) throws -> StringView
{
  if (let const cached = HELP_TEXT_CACHE.find(command); cached != nullptr)
    return cached->view();

  let text = help_text_for(context.get_program_resolver(), command);

  return HELP_TEXT_CACHE.set(command, steal(text))->view();
}

static fn parse_help_option_entries(StringView text) throws
    -> ArrayList<help_entry>
{
  let entries = ArrayList<help_entry>{heap_allocator()};
  let seen = HashSet{heap_allocator()};
  usize i = 0;
  while (i < text.length) {
    let const raw = text.next_line(i);

    let const start = skip_blanks(raw, 0);
    if (start >= raw.length || raw[start] != '-') continue;

    let const gap = double_space_gap(raw, start);
    let const option_part = raw.substring_of_length(start, gap - start);

    let description = StringView{};
    if (gap < raw.length)
      description =
          raw.substring_of_length(gap, raw.length - gap).trim_blanks();

    for (let const &flag : extract_dash_flags(option_part))
      if (seen.add(flag.view())) {
        entries.push(help_entry{String{flag.view()}, String{description}});
      }
  }
  return entries;
}

static fn help_cache_key(ProgramResolver &resolver, StringView command,
                         StringView subcommand) throws -> String
{
  let key = String{command};
  if (!subcommand.is_empty()) {
    key += " ";
    key += subcommand;
  }
  let const paths = resolver.search(command, ProgramResolver::SearchMode::First,
                                    ProgramResolver::Requirement::Runnable,
                                    ProgramResolver::CachePolicy::Bypass);
  if (!paths.is_empty()) {
    key += "\n";
    key += paths[0].text().view();
  }
  return key;
}

/* HELP_PARSED gates the fork so a second tab reads the parsed caches. */
static fn ensure_help_parsed(ProgramResolver &resolver, StringView command,
                             StringView subcommand = {}) throws -> String
{
  let const key = help_cache_key(resolver, command, subcommand);
  if (!HELP_PARSED.contains(key.view())) {
    let const text = help_text_for(resolver, command, subcommand, true);
    HELP_OPTION_CACHE.set(key.view(), parse_help_option_entries(text.view()));
    HELP_SUBCOMMAND_CACHE.set(key.view(),
                              parse_help_subcommands(text.view(), command));
    HELP_PARSED.add(key.view());
  }
  return key;
}

static fn help_entries_for(StringMap<ArrayList<help_entry>> &cache,
                           ProgramResolver &resolver, StringView command,
                           StringView subcommand = {}) throws
    -> const ArrayList<help_entry> &
{
  let const key = ensure_help_parsed(resolver, command, subcommand);
  return *cache.find(key.view());
}

static fn help_options_for(ProgramResolver &resolver, StringView command,
                           StringView subcommand = {}) throws
    -> const ArrayList<help_entry> &
{
  return help_entries_for(HELP_OPTION_CACHE, resolver, command, subcommand);
}

static fn is_plausible_subcommand_name(StringView name) wontthrow -> bool
{
  if (name.is_empty()) return false;
  let const first = name[0];
  let const starts_word = (first >= 'a' && first <= 'z') ||
                          (first >= 'A' && first <= 'Z') ||
                          (first >= '0' && first <= '9');
  if (!starts_word) return false;
  for (usize i = 0; i < name.length; i++) {
    let const c = name[i];
    let const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

/* A bare all-caps header with no colon, such as tailscale's "SUBCOMMANDS",
   opens a section only when the whole line is the single word. */
static fn line_opens_subcommand_section(StringView trimmed) wontthrow -> bool
{
  if (trimmed.is_empty()) return false;
  let const do_ends_with_ignoring_case = [&](StringView suffix) {
    if (trimmed.length < suffix.length) return false;
    let const offset = trimmed.length - suffix.length;
    for (usize i = 0; i < suffix.length; i++) {
      let a = trimmed[offset + i];
      let b = suffix[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) return false;
    }
    return true;
  };
  if (trimmed[trimmed.length - 1] == ':') {
    if (do_ends_with_ignoring_case(StringView{"commands:"}) ||
        do_ends_with_ignoring_case(StringView{"subcommands:"}) ||
        do_ends_with_ignoring_case(StringView{"commands are:"}) ||
        do_ends_with_ignoring_case(StringView{"example usage:"}))
      return true;
    /* git opens with "These are common Git commands used in various
       situations:", a colon-terminated line that names commands without
       matching a fixed suffix. */
    let const do_contains_word_ignoring_case = [&](StringView needle) {
      if (needle.length > trimmed.length) return false;
      let const do_is_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      };
      for (usize i = 0; i + needle.length <= trimmed.length; i++) {
        if (i > 0 && do_is_alpha(trimmed[i - 1])) continue;
        if (i + needle.length < trimmed.length &&
            do_is_alpha(trimmed[i + needle.length]))
          continue;
        let const candidate = trimmed.substring_of_length(i, needle.length);
        let const is_equal = [&]() {
          for (usize j = 0; j < needle.length; j++) {
            let a = candidate[j];
            let b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) return false;
          }
          return true;
        }();
        if (is_equal) return true;
      }
      return false;
    };
    return do_contains_word_ignoring_case(StringView{"commands"});
  }

  let const do_is_all_uppercase = [&]() {
    for (usize i = 0; i < trimmed.length; i++)
      if (trimmed[i] >= 'a' && trimmed[i] <= 'z') return false;
    return true;
  };
  let const do_equal_ignoring_case = [&](StringView word) {
    return trimmed.length == word.length && do_ends_with_ignoring_case(word);
  };
  return do_is_all_uppercase() &&
         (do_equal_ignoring_case(StringView{"commands"}) ||
          do_equal_ignoring_case(StringView{"subcommands"}));
}

/* git groups its subcommands under left-margin headers like "start a working
   area (see also: git help tutorial)". A header is not itself a subcommand and
   holds no double-space gap before a name, so it keeps an open section intact
   instead of closing it. */
static fn line_is_subcommand_group_header(StringView trimmed) wontthrow -> bool
{
  if (trimmed.is_empty()) return false;
  if (line_opens_subcommand_section(trimmed)) return true;
  if (trimmed[trimmed.length - 1] == ')') return true;
  let const do_contains = [&](StringView needle) {
    if (needle.length > trimmed.length) return false;
    for (usize i = 0; i + needle.length <= trimmed.length; i++)
      if (trimmed.substring_of_length(i, needle.length) == needle) return true;
    return false;
  };
  return do_contains(StringView{"(see also"}) ||
         do_contains(StringView{"see also:"});
}

/* cargo and other tools with subcommands but no manpage list them under a
   "Commands:" header as indented "name<spaces>description" lines. */
static fn parse_help_subcommands(StringView text, StringView command) throws
    -> ArrayList<help_entry>
{
  let subcommands = ArrayList<help_entry>{heap_allocator()};
  let seen = HashSet{heap_allocator()};
  let in_section = false;
  let saw_entry_in_section = false;
  usize i = 0;
  while (i < text.length) {
    let const raw = text.next_line(i);

    let const trim_start = skip_blanks(raw, 0);
    let const trimmed = raw.trim_blanks();

    if (line_opens_subcommand_section(trimmed)) {
      LOG(Debug, "help parse opens a subcommand section at '%.*s'",
          static_cast<int>(trimmed.length), trimmed.data);
      in_section = true;
      saw_entry_in_section = false;
      continue;
    }
    /* A grouped header such as git's "start a working area (see also: ...)"
       sits at the left margin and opens a section even when no section was
       open, since the indented entries beneath it are the subcommands. An
       indented line that happens to end with ')' is a wrapped option
       description, not a group header. */
    if (trim_start == 0 && line_is_subcommand_group_header(trimmed)) {
      in_section = true;
      saw_entry_in_section = false;
      continue;
    }
    if (!in_section) continue;
    if (trimmed.is_empty()) {
      if (saw_entry_in_section) in_section = false;
      continue;
    }
    /* A line that returns to the left margin ends the section. A blank line
       already closed it above, and a left-margin group header reopened it, so
       a left-margin line here is a non-header such as a trailing note. */
    if (trim_start == 0) {
      in_section = false;
      continue;
    }

    /* brew's help repeats the command word on every entry, "  brew install
       FORMULA|CASK...", so the surface command is stripped before the column
       split reads the real subcommand name. */
    let entry_text = trimmed;
    if (!command.is_empty() && entry_text.length > command.length + 1 &&
        entry_text.substring_of_length(0, command.length) == command &&
        entry_text[command.length] == ' ')
      entry_text = entry_text.substring_of_length(
          command.length + 1, entry_text.length - command.length - 1);

    let column_end = double_space_gap(entry_text, 0);

    /* A single-space separated help such as brew's "brew install
       FORMULA|CASK..." has no double-space gap, so the column boundary falls
       back to the first single space. The plausibility check then rejects
       argument tokens that are not valid subcommand names. */
    if (column_end >= entry_text.length) {
      usize single_space = 0;
      while (single_space < entry_text.length &&
             entry_text[single_space] != ' ')
        single_space++;
      column_end = single_space;
    }

    let description = StringView{};
    if (column_end < entry_text.length)
      description =
          entry_text
              .substring_of_length(column_end, entry_text.length - column_end)
              .trim_blanks();

    /* Each comma-separated alias such as `ft, fetch` becomes its own candidate
       under the shared description. */
    let const column = entry_text.substring_of_length(0, column_end);
    usize alias_start = 0;
    while (alias_start < column.length) {
      let alias_end = alias_start;
      while (alias_end < column.length && column[alias_end] != ',')
        alias_end++;
      let const alias =
          column.substring_of_length(alias_start, alias_end - alias_start)
              .trim_blanks();
      alias_start = alias_end + 1;

      if (!is_plausible_subcommand_name(alias)) continue;
      if (!seen.add(alias)) continue;
      subcommands.push(help_entry{String{alias}, String{description}});
      saw_entry_in_section = true;
    }
  }
  LOG(Debug, "help parse for command '%.*s' collected %zu subcommands",
      static_cast<int>(command.length), command.data, subcommands.count());
  return subcommands;
}

static fn help_subcommands_for(ProgramResolver &resolver, StringView command,
                               StringView subcommand = {}) throws
    -> const ArrayList<help_entry> &
{
  return help_entries_for(HELP_SUBCOMMAND_CACHE, resolver, command, subcommand);
}

static fn is_known_help_subcommand(ProgramResolver &resolver,
                                   StringView command,
                                   StringView subcommand_prefix,
                                   StringView word) throws -> bool
{
  for (let const &entry :
       help_subcommands_for(resolver, command, subcommand_prefix))
    if (entry.name.view() == word) return true;
  return false;
}

/* A line past this depth stops forking, so the fork count is bounded. */
static constexpr usize MAX_SUBCOMMAND_DEPTH = 4;

static fn settled_subcommand_chain(ProgramResolver &resolver,
                                   StringView resolved_command, StringView line,
                                   usize token_start) throws -> String
{
  let chain = String{heap_allocator()};

  let const surface_command = command_word_of(line);
  if (surface_command.is_empty()) return chain;

  usize depth_count = 0;
  usize position = static_cast<usize>(surface_command.data - line.data) +
                   surface_command.length;

  while (depth_count < MAX_SUBCOMMAND_DEPTH) {
    let const word = line.next_ascii_whitespace_word(position);
    if (word.is_empty()) break;
    let const start = static_cast<usize>(word.data - line.data);

    /* A word that reaches the token under the cursor is the token itself, so
       the chain ends before it. */
    if (start >= token_start || position > token_start) {
      break;
    }
    if (word[0] == '-') break;
    if (!is_known_help_subcommand(resolver, resolved_command, chain.view(),
                                  word))
      break;

    if (!chain.is_empty()) chain += " ";
    chain += word;
    depth_count++;
  }

  return chain;
}

fn complete_from_help(StringView line, StringView token, usize token_start,
                      completion_mode mode, EvalContext &context,
                      StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>
{
  let const for_listing = mode == completion_mode::Listing;
  if (!for_listing) return None;
  if (token.is_empty() || token[0] != '-') return None;
  let const surface_command = command_word_of(line);
  if (surface_command.is_empty() ||
      os::has_directory_separator(surface_command))
    return None;

  let const resolved_name = resolve_completion_alias(surface_command, context);

  let const chain = settled_subcommand_chain(
      context.get_program_resolver(), resolved_name.view(), line, token_start);

  let const &options = help_options_for(context.get_program_resolver(),
                                        resolved_name.view(), chain.view());
  if (options.is_empty()) return None;

  let matches = matches_from_help_entries(options, token, descriptions);
  if (matches.is_empty()) return None;
  return matches;
}

fn complete_from_help_subcommands(StringView line, StringView token,
                                  usize token_start, completion_mode mode,
                                  EvalContext &context,
                                  StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>
{
  let const for_listing = mode == completion_mode::Listing;
  LOG(Debug, "help subcommands entry token '%.*s' listing %d",
      static_cast<int>(token.length), token.data, for_listing ? 1 : 0);
  if (!for_listing) return None;
  if (!token.is_empty() && token[0] == '-') return None;
  if (os::has_directory_separator(token)) return None;
  let const surface_command = command_word_of(line);
  if (surface_command.is_empty() ||
      os::has_directory_separator(surface_command))
  {
    LOG(Debug, "help subcommands bail because the surface command is empty");
    return None;
  }

  let const resolved_name = resolve_completion_alias(surface_command, context);
  LOG(Debug, "help subcommands resolved the surface to '%.*s'",
      static_cast<int>(resolved_name.view().length), resolved_name.view().data);

  /* An empty chain at the first-argument position lists the base subcommands.
   */
  let const chain = settled_subcommand_chain(
      context.get_program_resolver(), resolved_name.view(), line, token_start);
  if (chain.is_empty()) {
    if (!is_first_argument_token(line, token_start)) {
      LOG(Debug, "help subcommands bail because the token is not the first "
                 "argument");
      return None;
    }

    /* A command the man index already lists never forks --help to relist them,
       the fork is reserved for a tool like cargo with no man pages. */
    if (is_man_subcommand_index_built) {
      let const man_subcommands =
          MAN_SUBCOMMAND_INDEX.find(resolved_name.view());
      if (man_subcommands != nullptr && !man_subcommands->is_empty()) {
        LOG(Debug, "help subcommands bail because the man index lists them");
        return None;
      }
    }
  }

  let const &subcommands = help_subcommands_for(
      context.get_program_resolver(), resolved_name.view(), chain.view());
  if (subcommands.is_empty()) {
    LOG(Debug, "help subcommands bail because the parsed subcommands are "
               "empty");
    return None;
  }

  let matches = matches_from_help_entries(subcommands, token, descriptions);
  if (matches.is_empty()) return None;
  return matches;
}

} /* namespace completion */

} /* namespace koshka */
