/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements scan for command completion. It scans tolerant shell
 * input and produces semantic candidates, highlighting, syntax, paths, or
 * command metadata.
 */

#include "Arena.hpp"
#include "Builtin.hpp"
#include "CliColors.hpp"
#include "Completion.hpp"
#include "CompletionInternal.hpp"
#include "CompletionPolicy.hpp"
#include "Debug.hpp"
#include "HashSet.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace completion {

using namespace internal;

#if !defined NDEBUG
static usize DEBUG_SHELL_LEXICAL_SCAN_BYTE_COUNT = 0;
#endif

static constexpr StringView DATE_FORMAT_CANDIDATES[] = {
    "+%%", "+%a", "+%A", "+%b", "+%B", "+%c", "+%C", "+%d", "+%D", "+%e",
    "+%F", "+%g", "+%G", "+%h", "+%H", "+%I", "+%j", "+%m", "+%M", "+%n",
    "+%p", "+%r", "+%R", "+%S", "+%t", "+%T", "+%u", "+%U", "+%V", "+%w",
    "+%W", "+%x", "+%X", "+%y", "+%Y", "+%z", "+%Z",
};

static fn previous_settled_word(StringView line, usize token_start) wontthrow
    -> StringView
{
  let end = token_start;
  while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t'))
    end--;
  let start = end;
  while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t')
    start--;

  let const word = line.substring_of_length(start, end - start);
  if (word.length > 1 && word[0] == '-' && word[word.length - 1] == '=') {
    return word.substring_of_length(0, word.length - 1);
  }

  return word;
}

/* Keyed by the source file's absolute path and refreshed when the mtime moves.
 */
struct cached_target_list
{
  i64 mtime;
  ArrayList<String> targets;
};
static StringMap<cached_target_list> BUILD_TARGET_CACHE{heap_allocator()};

static fn settled_option_value(StringView line, StringView option) throws
    -> Maybe<String>
{
  usize cword = 0;
  let const words = split_completion_words(line, line.length, cword);
  for (usize i = 1; i < words.count(); i++) {
    let const word = words[i].view();
    if (word == option && i + 1 < words.count() && i + 1 != cword) {
      return String{words[i + 1].view()};
    }
    if (word.length > option.length && word.starts_with(option)) {
      return String{word.substring(option.length)};
    }
  }
  return None;
}

static fn make_target_is_artifact(StringView name, const Path &directory) throws
    -> bool
{
  if (os::has_directory_separator(name)) return true;
  if (name == StringView{"GNUmakefile"} || name == StringView{"Makefile"} ||
      name == StringView{"makefile"})
  {
    return true;
  }

  let candidate = directory.clone();
  candidate.push_component(name);
  return candidate.exists();
}

static fn parse_make_database_targets(StringView database,
                                      const Path &directory) throws
    -> ArrayList<String>
{
  let targets = ArrayList<String>{heap_allocator()};
  let in_files_section = false;
  let skip_next_rule = false;
  usize i = 0;
  while (i < database.length) {
    let const text = database.next_line(i);

    if (text.starts_with(StringView{"# Files"})) {
      in_files_section = true;
      continue;
    }
    if (text.starts_with(StringView{"# Finished Make data base"})) break;
    if (!in_files_section) continue;
    if (text.starts_with(StringView{"# Not a target"})) {
      skip_next_rule = true;
      continue;
    }
    if (text.is_empty() || text[0] == '#') continue;
    /* The disowned rule follows its "# Not a target" comment immediately. */
    if (skip_next_rule) {
      skip_next_rule = false;
      continue;
    }
    if (text[0] == '.' || text[0] == '\t') continue;
    let const colon = text.find_character(':');
    if (!colon.has_value() || *colon == 0) continue;
    let const name = text.substring_of_length(0, *colon);
    if (make_target_is_artifact(name, directory)) continue;
    targets.push(String{name});
  }
  return targets;
}

static fn parse_colon_led_names(StringView listing) throws -> ArrayList<String>
{
  let names = ArrayList<String>{heap_allocator()};
  usize i = 0;
  while (i < listing.length) {
    let const text = listing.next_line(i);
    let const colon = text.find_character(':');
    if (!colon.has_value() || *colon == 0) continue;
    let const name = text.substring_of_length(0, *colon);
    if (name.find_character(' ').has_value()) continue;
    names.push(String{name});
  }
  return names;
}

static fn parse_tsh_node_names(StringView listing) throws -> ArrayList<String>
{
  let names = ArrayList<String>{heap_allocator()};
  usize i = 0;
  let has_passed_rule = false;
  while (i < listing.length) {
    let const row = listing.next_line(i);

    if (!has_passed_rule) {
      if (!row.is_empty() && row[0] == '-') has_passed_rule = true;
      continue;
    }

    usize field_end = 0;
    while (field_end < row.length && row[field_end] != ' ' &&
           row[field_end] != '\t')
      field_end++;

    if (field_end > 0)
      names.push(String{row.substring_of_length(0, field_end)});
  }
  return names;
}

/* A tolerant scan that tracks only strings, escapes, and brace nesting, no
   JSON machinery. */
static fn parse_package_json_scripts(StringView text) throws
    -> ArrayList<String>
{
  let scripts = ArrayList<String>{heap_allocator()};
  let const section = StringView{"\"scripts\""};
  usize at = 0;
  let is_found = false;
  for (; at + section.length <= text.length; at++)
    if (text.substring_of_length(at, section.length) == section) {
      is_found = true;
      break;
    }
  if (!is_found) return scripts;
  let i = at + section.length;
  while (i < text.length && text[i] != '{')
    i++;
  if (i >= text.length) return scripts;
  i++;
  usize depth = 1;
  let expecting_key = true;
  while (i < text.length && depth > 0) {
    let const byte = text[i];
    if (byte == '"') {
      let const start = ++i;
      while (i < text.length && text[i] != '"') {
        if (text[i] == '\\') i++;
        i++;
      }
      if (expecting_key && depth == 1) {
        scripts.push(String{text.substring_of_length(start, i - start)});
      }
      expecting_key = false;
      i++;
      continue;
    }
    if (byte == ':') expecting_key = false;
    if (byte == ',') expecting_key = true;
    if (byte == '{') depth++;
    if (byte == '}') depth--;
    i++;
  }
  return scripts;
}

/* The Host lines of the ssh config without the glob patterns, and the first
   fields of known_hosts without the hashed rows. */
static fn collect_ssh_hosts() throws -> ArrayList<String>
{
  let hosts = ArrayList<String>{heap_allocator()};
  let const home = os::get_home_directory();
  if (!home.has_value()) return hosts;

  let seen = HashSet{heap_allocator()};
  let const do_push_unique = [&](StringView host) throws {
    if (host.is_empty() || !seen.add(host)) return;
    hosts.push(String{host});
  };

  let config_path = home->clone();
  config_path.push_component(".ssh/config");
  if (Maybe<String> config = config_path.read_entire_file(); config.has_value())
  {
    let const text = config->view();
    usize i = 0;
    while (i < text.length) {
      let row = text.next_line(i);
      while (!row.is_empty() && (row[0] == ' ' || row[0] == '\t'))
        row = row.substring(1);
      if (!(row.starts_with(StringView{"Host "}) ||
            row.starts_with(StringView{"Host\t"})))
        continue;
      row = row.substring(5);
      /* A name carrying a pattern byte is a rule, not a reachable host. */
      usize k = 0;
      while (k < row.length) {
        k = skip_blanks(row, k);
        let const start = k;
        while (k < row.length && row[k] != ' ' && row[k] != '\t')
          k++;
        let const name = row.substring_of_length(start, k - start);
        if (!name.find_character('*').has_value() &&
            !name.find_character('?').has_value() &&
            !name.find_character('!').has_value())
        {
          do_push_unique(name);
        }
      }
    }
  }

  let known_hosts_path = home->clone();
  known_hosts_path.push_component(".ssh/known_hosts");
  if (Maybe<String> known = known_hosts_path.read_entire_file();
      known.has_value())
  {
    let const text = known->view();
    usize i = 0;
    while (i < text.length) {
      let const row = text.next_line(i);
      /* A hashed row opens with |1| and hides its host on purpose. */
      if (row.is_empty() || row[0] == '#' || row[0] == '|') continue;
      usize field_end = 0;
      while (field_end < row.length && row[field_end] != ' ' &&
             row[field_end] != '\t')
        field_end++;
      let field = row.substring_of_length(0, field_end);
      while (!field.is_empty()) {
        let const comma = field.find_character(',');
        let host =
            comma.has_value() ? field.substring_of_length(0, *comma) : field;
        field = comma.has_value() ? field.substring(*comma + 1) : StringView{};
        if (host.length > 2 && host[0] == '[') {
          let const close = host.find_character(']');
          if (close.has_value()) host = host.substring_of_length(1, *close - 1);
        }
        do_push_unique(host);
      }
    }
  }
  return hosts;
}

/* Null means the source file is missing. The result points into the cache. */
template <typename Collector>
static fn cached_targets_for(const Path &source_file, Collector collect) throws
    -> const ArrayList<String> *
{
  let const absolute_source_file = source_file.to_absolute();
  let const mtime = absolute_source_file.modification_time();
  if (!mtime.has_value()) return nullptr;
  let const key = absolute_source_file.text().view();
  if (const cached_target_list *cached = BUILD_TARGET_CACHE.find(key);
      cached != nullptr && cached->mtime == *mtime)
    return &cached->targets;
  return &BUILD_TARGET_CACHE.set(key, cached_target_list{*mtime, collect()})
              ->targets;
}

fn internal::complete_from_process_arguments(StringView line, StringView token,
                                             usize token_start,
                                             completion_mode mode) throws
    -> Maybe<ArrayList<String>>
{
  let const for_listing = mode == completion_mode::Listing;
  if (!for_listing) return None;

  let const command = command_word_of(line);
  let const is_by_pid = command == "kill" || command == "wait";
  let const is_by_name = command == "pkill" || command == "killall";
  if (!is_by_pid && !is_by_name) {
    return None;
  }

  if (!token.is_empty() && token[0] == '-') {
    return None;
  }

  /* A signal operand is not a process, so the flag scan answers instead. */
  let const previous_word = previous_settled_word(line, token_start);
  if (previous_word == "-s" || previous_word == "-n" ||
      previous_word == "--signal")
  {
    return None;
  }

  let const processes = os::enumerate_processes();
  let candidates = ArrayList<String>{completion_allocator()};
  let seen = HashSet{completion_allocator()};
  for (const os::process_entry &process : processes) {
    if (is_by_name) {
      let const name = process.name.view();
      if (name.is_empty() || !name.starts_with(token) || !seen.add(name)) {
        continue;
      }
      candidates.push(String{completion_allocator(), name});
    } else {
      let pid_text = String::from(process.pid, completion_allocator());
      if (!pid_text.view().starts_with(token)) continue;
      candidates.push(steal(pid_text));
    }
  }

  if (candidates.is_empty()) return None;
  return candidates;
}

fn internal::complete_from_tools_with_targets(StringView line, StringView token,
                                              usize token_start,
                                              completion_mode mode,
                                              EvalContext &context) throws
    -> Maybe<ArrayList<String>>
{
  let const for_listing = mode == completion_mode::Listing;
  if (!for_listing) return None;
  if (!token.is_empty() && token[0] == '-') return None;
  let const command = command_word_of(line);
  if (command.is_empty()) return None;

  /* A `koshkit make` routes make through the multicall dispatcher, so the build
     tool is the second word when koshkit is the command word. */
  let const tool =
      (command == "koshkit") ? second_word_of(line).value_or(command) : command;

  /* The name resolves to a path first, since the helper runs the path directly
     with no PATH search, and a probe that overruns the deadline is killed. */
  let const probe_timeout_nanos = 2'000'000'000ULL;
  let const do_capture = [&](const ArrayList<String> &probe_argv)
                             throws -> String {
    if (probe_argv.is_empty()) return String{heap_allocator()};
    let const resolved = context.get_program_resolver().search(
        probe_argv[0].view(), ProgramResolver::SearchMode::First,
        ProgramResolver::Requirement::Runnable,
        ProgramResolver::CachePolicy::Bypass);
    if (resolved.is_empty()) return String{heap_allocator()};
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{resolved[0].text().view()});
    for (usize i = 1; i < probe_argv.count(); i++)
      argv.push(String{probe_argv[i].view()});
    return os::capture_program_output(argv, probe_timeout_nanos)
        .value_or(String{heap_allocator()});
  };

  let owned_targets = ArrayList<String>{heap_allocator()};
  const ArrayList<String> *targets = &owned_targets;

  Maybe<tool_with_targets_kind> tool_kind = TOOLS_WITH_TARGETS.find(tool);
  if (!tool_kind.has_value()) return None;

  switch (tool_kind.value()) {
  case tool_with_targets_kind::make: {
    let const directory =
        settled_option_value(line, "-C").value_or(String{"."});
    let makefile_name = settled_option_value(line, "-f");
    if (!makefile_name.has_value()) {
      for (let const candidate :
           {StringView{"GNUmakefile"}, StringView{"makefile"},
            StringView{"Makefile"}})
      {
        let probe = Path{directory.view()};
        probe.push_component(candidate);
        if (probe.exists()) {
          makefile_name = String{candidate};
          break;
        }
      }
      if (!makefile_name.has_value()) return None;
    }
    let makefile_path = Path{directory.view()};
    makefile_path.push_component(makefile_name->view());
    if (!makefile_path.exists()) return None;
    let const make_directory = Path{directory.view()};
    targets = cached_targets_for(makefile_path, [&]() throws {
      let probe = ArrayList<String>{heap_allocator()};
      probe.push(String{"make"});
      probe.push(String{"-C"});
      probe.push(String{directory.view()});
      probe.push(String{"-f"});
      probe.push(String{makefile_name->view()});
      probe.push(String{"-pRrq"});
      probe.push(String{":"});
      let database_targets =
          parse_make_database_targets(do_capture(probe).view(), make_directory);
      if (!database_targets.is_empty()) return database_targets;
      let const intrinsic_targets =
          koshkit::collect_makefile_targets(context, makefile_path);
      let filtered = ArrayList<String>{heap_allocator()};
      let seen = HashSet{heap_allocator()};
      for (const String &name : intrinsic_targets) {
        if (make_target_is_artifact(name.view(), make_directory) ||
            !seen.add(name.view()))
          continue;
        filtered.push(name.clone());
      }
      return filtered;
    });
    break;
  }
  case tool_with_targets_kind::ninja: {
    let const directory =
        settled_option_value(line, "-C").value_or(String{"."});
    let build_file = Path{directory.view()};
    build_file.push_component(settled_option_value(line, "-f")
                                  .value_or(String{"build.ninja"})
                                  .view());
    targets = cached_targets_for(build_file, [&]() throws {
      let probe = ArrayList<String>{heap_allocator()};
      probe.push(String{"ninja"});
      probe.push(String{"-C"});
      probe.push(String{directory.view()});
      probe.push(String{"-t"});
      probe.push(String{"targets"});
      return parse_colon_led_names(do_capture(probe).view());
    });
    break;
  }
  case tool_with_targets_kind::cmake: {
    if (previous_settled_word(line, token_start) != "--target") return None;
    let const build_directory = settled_option_value(line, "--build");
    if (!build_directory.has_value()) return None;
    let cache_file = Path{build_directory->view()};
    cache_file.push_component("CMakeCache.txt");
    targets = cached_targets_for(cache_file, [&]() throws {
      let probe = ArrayList<String>{heap_allocator()};
      probe.push(String{"cmake"});
      probe.push(String{"--build"});
      probe.push(String{build_directory->view()});
      probe.push(String{"--target"});
      probe.push(String{"help"});
      let names = ArrayList<String>{heap_allocator()};
      let const help = do_capture(probe);
      let const text = help.view();
      usize i = 0;
      while (i < text.length) {
        let const row = text.next_line(i);
        if (!row.starts_with(StringView{"... "})) continue;
        let name = row.substring(4);
        if (let const space = name.find_character(' '); space.has_value())
          name = name.substring_of_length(0, *space);
        if (!name.is_empty()) names.push(String{name});
      }
      return names;
    });
    break;
  }
  case tool_with_targets_kind::node_runner: {
    if (second_word_of(line) != "run") return None;
    let const package_path = Path{StringView{"package.json"}};
    targets = cached_targets_for(package_path, [&]() throws {
      let const contents = package_path.read_entire_file();
      return contents.has_value() ? parse_package_json_scripts(contents->view())
                                  : ArrayList<String>{heap_allocator()};
    });
    break;
  }
  case tool_with_targets_kind::ssh: {
    if (os::has_directory_separator(token) ||
        token.find_character(':').has_value())
    {
      return None;
    }
    owned_targets = collect_ssh_hosts();
    break;
  }
  case tool_with_targets_kind::teleport: {
    if (second_word_of(line) != "ssh") return None;
    let probe = ArrayList<String>{heap_allocator()};
    probe.push(String{"tsh"});
    probe.push(String{"ls"});
    owned_targets = parse_tsh_node_names(do_capture(probe).view());
    break;
  }
  }

  if (targets == nullptr) return None;
  let candidates = ArrayList<String>{heap_allocator()};
  for (let const &target : *targets)
    if (target.view().starts_with(token))
      candidates.push(String{target.view()});
  if (candidates.is_empty()) return None;
  return candidates;
}

static fn append_flag_forms(const FlagList &flags, StringView token_filter,
                            ArrayList<String> &out) throws -> void
{
  for (const Flag *flag : flags) {
    if (flag->short_name() != '\0') {
      let form = String{"-"};
      form.push(flag->short_name());
      if (token_filter.is_empty() || form.view().starts_with(token_filter)) {
        out.push(steal(form));
      }
    }
    if (!flag->long_name().is_empty()) {
      let form = String{"--"};
      form += flag->long_name();
      if (token_filter.is_empty() || form.view().starts_with(token_filter)) {
        out.push(steal(form));
      }
    }
  }
}

/* Null means the kind registered no flags. */
static fn dash_candidates_for(Maybe<Builtin::Kind> builtin_kind) throws
    -> const ArrayList<String> *
{
  static Maybe<ArrayList<String>> per_kind_candidates[BUILTIN_KIND_COUNT]{};
  static bool was_per_kind_built[BUILTIN_KIND_COUNT]{};
  static ArrayList<String> binary_candidates{heap_allocator()};
  static bool was_binary_built = false;

  if (!builtin_kind.has_value()) {
    if (!was_binary_built) {
      append_flag_forms(kosh_binary_flag_list(), StringView{},
                        binary_candidates);
      was_binary_built = true;
    }
    return &binary_candidates;
  }

  let const index = static_cast<usize>(*builtin_kind);
  if (!was_per_kind_built[index]) {
    per_kind_candidates[index] = ArrayList<String>{heap_allocator()};
    if (*builtin_kind == Builtin::Kind::Kill) {
      for (let const name : os::signal_names()) {
        let with_dash = String{"-"};
        with_dash += name;
        per_kind_candidates[index]->push(steal(with_dash));
      }
    } else {
      let const flags = builtin_flag_list(*builtin_kind);
      if (flags == nullptr) return nullptr;
      append_flag_forms(*flags, StringView{}, *per_kind_candidates[index]);
      if (*builtin_kind == Builtin::Kind::Set) {
        const String &letters = shell_option_letters();
        for (usize i = 0; i < letters.count(); i++) {
          let switch_form = String{"-"};
          switch_form.push(letters[i]);
          per_kind_candidates[index]->push(steal(switch_form));
        }
        per_kind_candidates[index]->push(String{"-o"});
      }
    }
    was_per_kind_built[index] = true;
  }
  return &*per_kind_candidates[index];
}

static fn push_variable_name_candidates(StringView token, EvalContext &context,
                                        ArrayList<String> &candidates) throws
    -> void
{
  let seen = HashSet{heap_allocator()};
  let const do_add_name = [&](StringView name) throws {
    if (!name.starts_with(token)) return;
    if (!seen.add(name)) return;
    candidates.push(String{name});
  };

  context.variable_names().for_each(
      [&](StringView name) { do_add_name(name); });

  for (let const &name : os::environment_names())
    do_add_name(name.view());

  let dynamic_names = ArrayList<StringView>{heap_allocator()};
  context.append_dynamic_variable_names(dynamic_names);
  for (let const name : dynamic_names)
    do_add_name(name);
}

fn internal::complete_from_builtin_flags(StringView line, StringView token,
                                         usize token_start,
                                         EvalContext &context) throws
    -> Maybe<ArrayList<String>>
{
  let const command = command_word_of(line);
  if (command.is_empty()) return None;

  let const builtin_kind = search_builtin(command);
  /* Matched by basename so both kosh and a path to it answer. */
  let shell_binary_name = command;
  for (usize i = command.length; i > 0; i--)
    if (os::is_directory_separator(command[i - 1])) {
      shell_binary_name = command.substring(i);
      break;
    }
  let const completes_shell_binary =
      !builtin_kind.has_value() && shell_binary_name == "kosh";

  let const previous_word = previous_settled_word(line, token_start);
  let const wants_operand = token.is_empty() || token[0] != '-';

  let candidates = ArrayList<String>{heap_allocator()};
  let const do_push_matching = [&](StringView candidate) throws {
    if (candidate.starts_with(token)) candidates.push(String{candidate});
  };
  let const do_push_signal_names = [&]() throws {
    for (let const name : os::signal_names())
      do_push_matching(name);
  };

  {
    let const is_koshkit_builtin =
        builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Koshkit;
    Maybe<koshkit::Utility::Kind> util_for_flags;
    bool should_offer_util_names = false;
    if (is_koshkit_builtin) {
      if (previous_word == command) {
        if (wants_operand) should_offer_util_names = true;
      } else if (let const second = second_word_of(line); second.has_value()) {
        util_for_flags = koshkit::find_util(*second);
      }
    } else if (!completes_shell_binary && context.koshkit() &&
               context.get_program_resolver().get_status(command) ==
                   ProgramResolver::Status::Missing)
    {
      util_for_flags = koshkit::find_util(command);
    }

    if (should_offer_util_names) {
      let names = ArrayList<String>{heap_allocator()};
      for (const String &name : koshkit::util_names())
        if (name.view().starts_with(token)) names.push(String{name.view()});
      if (!names.is_empty()) return names;
      return None;
    }

    if (util_for_flags.has_value()) {
      let const takes_signal_name =
          *util_for_flags == koshkit::Utility::Kind::Timeout ||
          *util_for_flags == koshkit::Utility::Kind::Pkill ||
          *util_for_flags == koshkit::Utility::Kind::Killall;
      if (takes_signal_name &&
          (previous_word == "-s" || previous_word == "--signal"))
      {
        do_push_signal_names();
        if (!candidates.is_empty()) return candidates;
        return None;
      }

      if (*util_for_flags == koshkit::Utility::Kind::Find &&
          previous_word == "-type")
      {
        for (let const entry_type : {"d", "f", "l"})
          do_push_matching(entry_type);
        if (!candidates.is_empty()) return candidates;
        return None;
      }

      if (*util_for_flags == koshkit::Utility::Kind::Date &&
          token.starts_with("+"))
      {
        for (let const candidate : DATE_FORMAT_CANDIDATES)
          do_push_matching(candidate);
        if (!candidates.is_empty()) return candidates;
        return None;
      }

      if (wants_operand) return None;
      let const flags = koshkit::koshkit_util_flag_list(*util_for_flags);
      if (flags == nullptr) return None;
      let forms = ArrayList<String>{heap_allocator()};
      append_flag_forms(*flags, token, forms);
      if (forms.is_empty()) return None;
      return forms;
    }
  }

  if (!builtin_kind.has_value() && !completes_shell_binary) return None;

  let const completes_set_builtin =
      builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Set;

  if (completes_set_builtin || completes_shell_binary) {
    /* set -o and set +o name an option by long name, no dash on the operand. */
    if (completes_set_builtin &&
        (previous_word == "-o" || previous_word == "+o"))
    {
      for (let const name : shell_option_names(true))
        do_push_matching(name);
      if (!candidates.is_empty()) return candidates;
      return None;
    }

    if (previous_word == "--mood" || previous_word == "-M" ||
        previous_word == "--init-moods" || previous_word == "-L")
    {
      for (mimic_mood mood : {mimic_mood::Default, mimic_mood::Bash,
                              mimic_mood::Posix, mimic_mood::BashPosix})
        do_push_matching(mood_name(mood));
      if (!candidates.is_empty()) return candidates;
      return None;
    }

#if !defined NDEBUG
    if (completes_shell_binary &&
        (previous_word == "--debug-logging" || previous_word == "-X"))
    {
      for (let const level : {"info", "debug", "all"})
        do_push_matching(level);
      if (!candidates.is_empty()) return candidates;
      return None;
    }
#endif
  }

  /* A shopt operand is an option name, no dash required. */
  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Shopt &&
      wants_operand)
  {
    /* shopt -o crosses over to the set option names. */
    if (previous_word == "-o") {
      for (let const name : shell_option_names(true))
        do_push_matching(name);
    } else {
      for (let const name : shopt_option_name_list())
        do_push_matching(name);
    }

    if (!candidates.is_empty()) return candidates;
    return None;
  }

  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Kill) {
    /* kill -s and kill -n both resolve a signal name or a number. */
    if (previous_word == "-s" || previous_word == "-n") {
      do_push_signal_names();
      if (!candidates.is_empty()) return candidates;
      return None;
    }

    /* A bare kill operand completes the %job ids, the one live table here. */
    if (wants_operand) {
      for (let const &background_job : context.jobs()) {
        let job_id = String{"%"};
        job_id += String::from(background_job.id, heap_allocator());
        do_push_matching(job_id.view());
      }
      if (!candidates.is_empty()) return candidates;
      return None;
    }
  }

  /* A trap operand past the action names a signal or a special condition. */
  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Trap &&
      wants_operand && previous_word != command)
  {
    for (let const condition : {"DEBUG", "ERR", "EXIT", "RETURN"})
      do_push_matching(condition);
    do_push_signal_names();
    if (!candidates.is_empty()) return candidates;
    return None;
  }

  /* An enable operand is a builtin name. */
  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Enable &&
      wants_operand)
  {
    for (let const &name : builtin_names())
      do_push_matching(name.view());
    if (!candidates.is_empty()) return candidates;
    return None;
  }

  /* complete -o and compgen -o name a completion option. */
  if (builtin_kind.has_value() &&
      (*builtin_kind == Builtin::Kind::Complete ||
       *builtin_kind == Builtin::Kind::Compgen) &&
      previous_word == "-o")
  {
    for (let const option : {"bashdefault", "default", "dirnames"})
      do_push_matching(option);
    if (!candidates.is_empty()) return candidates;
    return None;
  }

  /* compgen -V names the indexed array that receives the candidates. */
  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Compgen &&
      previous_word == "-V")
  {
    push_variable_name_candidates(token, context, candidates);
    if (!candidates.is_empty()) return candidates;
    return None;
  }

  /* assimilate --link-mood names the symlink spellings it installs. */
  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Assimilate &&
      previous_word == "--link-mood")
  {
    for (let const link_mood : {"bash", "dash", "kosh", "sh"})
      do_push_matching(link_mood);
    if (!candidates.is_empty()) return candidates;
    return None;
  }

  /* A bare unset operand is a variable name with no leading $. */
  if (builtin_kind.has_value() && *builtin_kind == Builtin::Kind::Unset &&
      (token.is_empty() || token[0] != '-'))
  {
    /* unset -f removes a function, the plain and -v forms a variable. */
    let unsets_function = false;
    let const prefix = line.substring_of_length(0, token_start);
    usize scan_position = 0;
    while (scan_position < prefix.length) {
      scan_position = skip_blanks(prefix, scan_position);
      let const word_begin = scan_position;
      while (scan_position < prefix.length && prefix[scan_position] != ' ' &&
             prefix[scan_position] != '\t')
        scan_position++;

      let const arg =
          prefix.substring_of_length(word_begin, scan_position - word_begin);
      if (arg.length >= 2 && arg[0] == '-' && arg[1] != '-' &&
          arg.find_character('f').has_value())
        unsets_function = true;
    }

    if (unsets_function) {
      let seen = HashSet{heap_allocator()};
      context.for_each_function_name([&](StringView name) {
        if (!name.starts_with(token)) return;
        if (!seen.add(name)) return;
        candidates.push(String{name});
      });
    } else {
      push_variable_name_candidates(token, context, candidates);
    }

    if (!candidates.is_empty()) return candidates;
    return None;
  }

  if (wants_operand) return None;

  const ArrayList<String> *dash_candidates = dash_candidates_for(
      completes_shell_binary ? Maybe<Builtin::Kind>{None} : builtin_kind);
  if (dash_candidates == nullptr) return None;
  for (let const &candidate : *dash_candidates)
    do_push_matching(candidate.view());
  if (candidates.is_empty()) return None;
  return candidates;
}

static pure fn entry_is_unrequested_dash_word(
    StringView entry, bool should_offer_dash_entries) wontthrow -> bool
{
  return !should_offer_dash_entries && !entry.is_empty() && entry[0] == '-';
}

/* The description opens after a space, so a value holding a parenthesis such as
   a filename is left whole. */
static fn push_spec_candidate(StringView entry, ArrayList<String> &candidates,
                              StringMap<String> &descriptions) throws -> void
{
  let const paren = entry.find_character('(');
  if (paren.has_value() && *paren > 0 && entry[*paren - 1] == ' ' &&
      entry[entry.length - 1] == ')')
  {
    let name = entry.substring_of_length(0, *paren);
    while (!name.is_empty() && name[name.length - 1] == ' ')
      name = name.substring_of_length(0, name.length - 1);
    let const description =
        entry.substring_of_length(*paren + 1, entry.length - *paren - 2);
    if (!name.is_empty()) {
      candidates.push(String{name});
      if (!description.is_empty()) descriptions.set(name, String{description});
      return;
    }
  }
  candidates.push(String{entry});
}

fn internal::complete_from_spec(StringView line, StringView token, usize cursor,
                                completion_mode mode, EvalContext &context,
                                StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>
{
  if (!context.is_shopt_enabled(shopt_option_id::Progcomp)) return None;

  let const for_listing = mode == completion_mode::Listing;
  let const command = command_word_of(line.substring_of_length(0, cursor));
  if (command.is_empty()) return None;

  /* A cobra-style function truncates its description to COLUMNS, so the width
     is set wide for the run and restored after. The ghost path keeps COLUMNS
     untouched. */
  Maybe<String> saved_columns;
  if (for_listing) {
    saved_columns = context.get_variable_value("COLUMNS");
    context.set_shell_variable("COLUMNS", "100000");
  }
  defer
  {
    if (for_listing) {
      if (saved_columns.has_value())
        context.set_shell_variable("COLUMNS", saved_columns->view());
      else
        context.unset_shell_variable("COLUMNS");
    }
  };
  /* The surface name wins when it has a spec of its own, otherwise it resolves
     through an alias and a symlink. */
  const completion_spec *spec = context.lookup_completion_spec(command);
  String resolved_command{heap_allocator()};
  if (spec == nullptr &&
      context.is_shopt_enabled(shopt_option_id::ProgcompAlias))
  {
    resolved_command = resolve_completion_command(command, context);
    if (resolved_command.view() != command)
      spec = context.lookup_completion_spec(resolved_command.view());
  }
  LOG(All,
      "spec lookup for '%.*s' %s, listing %d, function '%s', %zu word-list "
      "bytes",
      static_cast<int>(command.length), command.data,
      spec != nullptr ? "hit" : "missed", for_listing ? 1 : 0,
      spec != nullptr ? spec->function_name.c_str() : "",
      spec != nullptr ? spec->word_list.length() : 0);

  /* No command-specific spec. The default -D loader sources the per-command
     file and returns 124 to ask for a retry, otherwise it produced the
     candidates itself. */
  if (spec == nullptr) {
    if (!for_listing) return None;
    const completion_spec *def = context.default_completion_spec();
    if (def == nullptr || def->function_name.is_empty()) return None;
    let const default_spec = def->clone(heap_allocator());
    usize default_cword = 0;
    let const default_words =
        split_completion_words(line, cursor, default_cword);
    i32 status = 0;
    let const reply = context.run_completion_function(
        default_spec.function_name.view(), default_words, default_cword, line,
        cursor, &status);
    if (status != 124) {
      let const wants_dash_entries = !token.is_empty() && token[0] == '-';
      let loaded = ArrayList<String>{heap_allocator()};
      for (let const &entry : reply) {
        if (entry_is_unrequested_dash_word(entry.view(), wants_dash_entries))
          continue;
        push_spec_candidate(entry.view(), loaded, descriptions);
      }
      /* An empty reply never claims the completion, so the cascade falls to the
         filesystem the way bash-completion's -o default behaves. */
      if (loaded.is_empty()) return None;
      return loaded;
    }
    spec = context.lookup_completion_spec(command);
    if (spec == nullptr) return None;
  }

  let const active_spec = spec->clone(heap_allocator());
  let candidates = ArrayList<String>{heap_allocator()};

  let const should_offer_dash_words = !token.is_empty() && token[0] == '-';

  if (!active_spec.word_list.is_empty()) {
    /* The -W list expands through the same shared path compgen -W reads. */
    let const saved_runtime_state =
        context.enter_definition_state(active_spec.defining_runtime);
    defer
    {
      context.leave_definition_state(saved_runtime_state,
                                     definition_state_exit::RestoreCaller);
    };
    for (let const &word : context.expand_wordlist_to_fields(
             active_spec.word_list.view(), for_listing))
    {
      if (entry_is_unrequested_dash_word(word.view(), should_offer_dash_words))
        continue;
      if (word.view().starts_with(token)) candidates.push(String{word.view()});
    }
  }

  /* COMPREPLY is already filtered to the current word, so its entries are taken
     as they are under the same dash gate. */
  if (for_listing && !active_spec.function_name.is_empty()) {
    usize cword = 0;
    let const words = split_completion_words(line, cursor, cword);
    let const reply = context.run_completion_function(
        active_spec.function_name.view(), words, cword, line, cursor);
    for (let const &entry : reply) {
      if (entry_is_unrequested_dash_word(entry.view(), should_offer_dash_words))
        continue;
      push_spec_candidate(entry.view(), candidates, descriptions);
    }
  }

  if (candidates.is_empty()) return None;
  return candidates;
}

static fn consider_shell_lexical_frame(
    const shell_lexical_frame &frame, usize body_end, usize depth,
    shell_lexical_scan_target *target) wontthrow -> void
{
  if (target == nullptr) return;
  if (frame.kind != shell_lexical_frame_kind::command &&
      frame.kind != shell_lexical_frame_kind::backtick)
  {
    return;
  }
  if (target->cursor < frame.body_start || target->cursor > body_end) return;
  if (depth < target->frame_depth) return;
  target->range = completion_command_range{frame.body_start, body_end};
  target->frame_depth = depth;
}

static fn collect_shell_heredoc(StringView source, usize &position, usize end,
                                shell_lexical_state &state) throws -> void
{
  position += 2;
  let should_strip_tabs = false;
  if (position < end && source[position] == '-') {
    should_strip_tabs = true;
    position++;
  }
  while (position < end &&
         (source[position] == ' ' || source[position] == '\t'))
    position++;

  let delimiter = String{state.pending_heredocs.allocator()};
  char delimiter_quote = 0;
  while (position < end) {
    let const delimiter_byte = source[position];
    if (delimiter_quote != 0) {
      position++;
      if (delimiter_byte == delimiter_quote)
        delimiter_quote = 0;
      else
        delimiter.push(delimiter_byte);
      continue;
    }
    if (delimiter_byte == '\\') {
      position++;
      if (position < end) {
        delimiter.push(source[position]);
        position++;
      }
      continue;
    }
    if (delimiter_byte == '\'' || delimiter_byte == '"') {
      delimiter_quote = delimiter_byte;
      position++;
      continue;
    }
    if (lexer::is_whitespace(delimiter_byte) ||
        lexer::is_shell_sentinel(delimiter_byte))
    {
      break;
    }
    delimiter.push(delimiter_byte);
    position++;
  }

  if (!delimiter.is_empty())
    state.pending_heredocs.push(
        shell_pending_heredoc{steal(delimiter), should_strip_tabs});
}

fn internal::advance_shell_lexical_state(
    StringView source, usize end, shell_lexical_state &state,
    shell_lexical_scan_target *target) throws -> void
{
  if (end > source.length) end = source.length;
  let i = state.source_position;
#if !defined NDEBUG
  let const scan_start = i;
#endif
  let const do_pop_frame = [&]() wontthrow -> shell_lexical_frame {
    let const frame_depth = state.frames.count();
    let const frame = state.frames.back();
    while (!state.constructs.is_empty() &&
           state.constructs.back().frame_depth >= frame_depth)
    {
      state.constructs.pop_back();
    }
    state.frames.pop_back();
    state.quote = frame.parent_quote;
    return frame;
  };
  let const do_close_group = [](shell_lexical_frame &frame) wontthrow -> void {
    if (frame.group_depth == 0) return;

    frame.group_depth--;
    if (frame.is_in_array_value &&
        frame.group_depth <= frame.array_value_group_depth)
    {
      frame.is_in_array_value = false;
    }
  };

  while (i < end) {
    if (target != nullptr && target->should_stop_at_token_boundary &&
        i >= target->cursor && !state.is_in_heredoc && !state.is_in_comment &&
        state.quote == 0 && state.frames.count() == target->frame_depth &&
        is_active_token_boundary(source, i))
    {
      target->range.end = i;
      break;
    }

    if (state.is_in_heredoc) {
      let const &heredoc = state.pending_heredocs[state.active_heredoc_index];
      let line_end = i;
      while (line_end < end && source[line_end] != '\n')
        line_end++;
      if (line_end == end && end < source.length) {
        i = end;
        break;
      }

      let content_start = i;
      if (heredoc.should_strip_tabs)
        while (content_start < line_end && source[content_start] == '\t')
          content_start++;
      let const content = lexer::heredoc_line_content(
          source.substring_of_length(content_start, line_end - content_start));
      if (content == heredoc.delimiter.view()) {
        state.active_heredoc_index++;
        if (state.active_heredoc_index == state.pending_heredocs.count()) {
          state.pending_heredocs.clear();
          state.active_heredoc_index = 0;
          state.is_in_heredoc = false;
        }
      }
      i = line_end < end ? line_end + 1 : line_end;
      continue;
    }

    let const c = source[i];

    if (state.is_in_comment) {
      if (c == '\n') {
        state.is_in_comment = false;
        /* The separator block never reaches this newline, so the frame is
           returned to command position here. */
        let &comment_frame =
            state.frames.is_empty() ? state.root_frame : state.frames.back();
        comment_frame.is_command_position = true;
        i++;
        if (!state.pending_heredocs.is_empty()) state.is_in_heredoc = true;
      } else {
        i++;
      }
      continue;
    }

    if (state.quote == '\'') {
      if (state.is_in_ansi_c_quote && c == '\\' && i + 1 < end) {
        i += 2;
        continue;
      }
      if (c == '\'') {
        state.quote = 0;
        state.is_in_ansi_c_quote = false;
      }
      i++;
      continue;
    }

    if (c == '\\') {
      i += i + 1 < end ? 2 : 1;
      continue;
    }

    if (state.quote != 0) {
      if (c == state.quote) {
        state.quote = 0;
        i++;
        continue;
      }

      if (state.quote != '"') {
        i++;
        continue;
      }

      if (c != '$' && c != '`') {
        i++;
        continue;
      }
    }

    /* A backslash escapes the next byte inside `$'...'`, so the quote is
       tracked apart from a plain single quote. Inside double quotes the `$'`
       is literal. */
    if (c == '$' && state.quote == 0 && i + 1 < end && source[i + 1] == '\'') {
      state.quote = '\'';
      state.is_in_ansi_c_quote = true;
      i += 2;
      continue;
    }

    if (c == '\'' || c == '"') {
      state.quote = c;
      i++;
      continue;
    }

    if (c == '`') {
      if (!state.frames.is_empty() &&
          state.frames.back().kind == shell_lexical_frame_kind::backtick)
      {
        let const frame = do_pop_frame();
        consider_shell_lexical_frame(frame, i, state.frames.count(), target);
      } else {
        state.frames.push(shell_lexical_frame{
            i + 1, 0, 0, 0, shell_lexical_frame_kind::backtick, state.quote});
        state.quote = 0;
      }
      i++;
      continue;
    }

    if (c == '$' && i + 1 < end && source[i + 1] == '(') {
      if (i + 2 < end && source[i + 2] == '(') {
        state.frames.push(shell_lexical_frame{
            i + 3, 0, 0, 0, shell_lexical_frame_kind::arithmetic, state.quote});
        state.quote = 0;
        i += 3;
        continue;
      }

      state.frames.push(shell_lexical_frame{
          i + 2, 0, 0, 0, shell_lexical_frame_kind::command, state.quote});
      state.quote = 0;
      i += 2;
      continue;
    }

    if (c == '$' && i + 1 < end && source[i + 1] == '{') {
      state.frames.push(shell_lexical_frame{
          i + 2, 0, 0, 0, shell_lexical_frame_kind::parameter, state.quote});
      state.quote = 0;
      i += 2;
      continue;
    }

    let const is_command_code =
        state.frames.is_empty() ||
        state.frames.back().kind == shell_lexical_frame_kind::command ||
        state.frames.back().kind == shell_lexical_frame_kind::backtick;
    /* `<<<` is a here-string. The scan reaches its second byte as well, so the
       neighbours on both sides are checked. */
    let const is_heredoc_operator = c == '<' && i + 1 < end &&
                                    source[i + 1] == '<' &&
                                    !(i + 2 < end && source[i + 2] == '<') &&
                                    !(i > 0 && source[i - 1] == '<');
    if (is_command_code && is_heredoc_operator) {
      collect_shell_heredoc(source, i, end, state);
      continue;
    }

    if (!state.frames.is_empty() &&
        state.frames.back().kind == shell_lexical_frame_kind::parameter)
    {
      if (c == '}') {
        do_pop_frame();
      }
      i++;
      continue;
    }

    let &frame =
        state.frames.is_empty() ? state.root_frame : state.frames.back();
    let const is_word_boundary =
        i == frame.body_start || source[i - 1] == ' ' ||
        source[i - 1] == '\t' || source[i - 1] == '\n' ||
        source[i - 1] == ';' || source[i - 1] == '&' || source[i - 1] == '|' ||
        source[i - 1] == '(' || source[i - 1] == ')';
    if (is_command_code && c == '#' && is_word_boundary) {
      state.is_in_comment = true;
      i++;
      continue;
    }

    let const do_word_matches = [&](StringView word) {
      if (i + word.length > end) return false;
      if (source.substring_of_length(i, word.length) != word) return false;
      let const word_end = i + word.length;
      return word_end == source.length ||
             lexer::is_whitespace(source[word_end]) ||
             lexer::is_shell_sentinel(source[word_end]) ||
             (source[word_end] == '\r' && word_end + 1 < source.length &&
              source[word_end + 1] == '\n');
    };

    let const is_word_start =
        i == frame.body_start || lexer::is_whitespace(source[i - 1]) ||
        source[i - 1] == '\n' || source[i - 1] == ';' || source[i - 1] == '&' ||
        source[i - 1] == '|' || source[i - 1] == '(' || source[i - 1] == ')';
    if (is_word_start && !frame.is_in_array_value &&
        lexer::is_part_of_identifier(c))
    {
      let word_end = i;
      while (word_end < end && !lexer::is_whitespace(source[word_end]) &&
             !lexer::is_shell_sentinel(source[word_end]) &&
             !(source[word_end] == '\r' && word_end + 1 < end &&
               source[word_end + 1] == '\n'))
      {
        word_end++;
      }
      let const word = source.substring_of_length(i, word_end - i);
      LOG(All, "scanning word '%.*s' at command position %d",
          static_cast<int>(word.length), word.data, frame.is_command_position);
      let active_construct =
          state.constructs.is_empty() ? nullptr : &state.constructs.back();
      let const is_active_construct_in_frame =
          active_construct != nullptr &&
          active_construct->frame_depth == state.frames.count();
      if (is_active_construct_in_frame &&
          active_construct->kind == highlight_construct::conditional &&
          word == "]]")
      {
        state.constructs.pop_back();
        frame.is_command_position = false;
      } else if (is_active_construct_in_frame &&
                 active_construct->kind == highlight_construct::function &&
                 active_construct->phase ==
                     highlight_construct_phase::function_name &&
                 word != "function")
      {
        if (word_is_function_name(word)) state.known_function_names.add(word);
        active_construct->phase = highlight_construct_phase::body;
        frame.is_command_position = false;
      } else if (is_active_construct_in_frame &&
                 active_construct->kind == highlight_construct::function &&
                 active_construct->phase == highlight_construct_phase::body &&
                 word == "}")
      {
        state.constructs.pop_back();
        frame.is_command_position = false;
      } else if (is_active_construct_in_frame &&
                 active_construct->kind == highlight_construct::for_ &&
                 active_construct->phase ==
                     highlight_construct_phase::for_variable &&
                 word != "for")
      {
        active_construct->phase = highlight_construct_phase::for_in;
        frame.is_command_position = false;
      } else if (is_active_construct_in_frame &&
                 active_construct->kind == highlight_construct::for_ &&
                 active_construct->phase == highlight_construct_phase::for_in &&
                 word == "in")
      {
        active_construct->phase = highlight_construct_phase::for_do;
        frame.is_command_position = false;
      } else if (frame.is_command_position && do_word_matches("case")) {
        frame.has_seen_case_keyword = true;
        frame.is_command_position = false;
      } else if (frame.has_seen_case_keyword && do_word_matches("in")) {
        frame.has_seen_case_keyword = false;
        frame.case_depth++;
        frame.is_case_pattern_expected = true;
        frame.is_command_position = false;
      } else if (frame.is_command_position && frame.case_depth > 0 &&
                 do_word_matches("esac"))
      {
        frame.case_depth--;
        frame.is_case_pattern_expected = false;
        frame.is_command_position = false;
      } else if (frame.is_command_position &&
                 lexer::word_looks_like_assignment(word))
      {
        frame.is_command_position = true;
      } else if (frame.is_command_position && word_is_function_name(word) &&
                 word_defines_function(source, word_end, end))
      {
        state.known_function_names.add(word);
        frame.is_command_position = false;
      } else if (frame.is_command_position) {
        if (let const is_next_command =
                advance_shell_keyword_state(word, state.frames.count(), state);
            is_next_command.has_value())
          frame.is_command_position = *is_next_command;
        else if (!lexer::is_shell_sentinel(c))
          frame.is_command_position = false;
      }

      /* The words inside `name=(...)` are element values, so the scan stays out
         of them until the list closes. A declare or local operand carries the
         same form outside command position. */
      if (!word.is_empty() && word[word.length - 1] == '=' && word_end < end &&
          source[word_end] == '(' && lexer::word_looks_like_assignment(word))
      {
        frame.is_in_array_value = true;
        frame.array_value_group_depth = frame.group_depth;
      }
    }

    if (c == '\n' || c == '|' || c == '&' || c == ';') {
      frame.is_command_position = true;
      if (c == ';' && i + 1 < end &&
          (source[i + 1] == ';' || source[i + 1] == '&') &&
          frame.case_depth > 0)
      {
        frame.is_case_pattern_expected = true;
      }
      if (c == '\n' && !state.pending_heredocs.is_empty()) {
        state.is_in_heredoc = true;
      }
    }

    /* A bare `((` in command position opens arithmetic, where `<<` is a shift
       and never a here-document. */
    if (is_command_code && c == '(' && i + 1 < end && source[i + 1] == '(' &&
        frame.is_command_position)
    {
      state.frames.push(shell_lexical_frame{
          i + 2, 0, 0, 0, shell_lexical_frame_kind::arithmetic, state.quote});
      state.quote = 0;
      i += 2;
      continue;
    }

    if (state.frames.is_empty()) {
      if (c == '(')
        frame.group_depth++;
      else if (c == ')' && frame.group_depth > 0)
        do_close_group(frame);
      else if (c == ')' && frame.case_depth > 0 &&
               frame.is_case_pattern_expected)
      {
        frame.is_case_pattern_expected = false;
        frame.is_command_position = true;
      }
      i++;
      continue;
    }

    if (c == '(') {
      frame.group_depth++;
      i++;
      continue;
    }

    if (c == ')' && frame.kind == shell_lexical_frame_kind::arithmetic) {
      if (frame.group_depth > 0) {
        frame.group_depth--;
        i++;
        continue;
      }
      if (i + 1 < end && source[i + 1] == ')') {
        do_pop_frame();
        i += 2;
        continue;
      }
    }

    if (c == ')' && frame.kind == shell_lexical_frame_kind::command) {
      if (frame.group_depth > 0) {
        do_close_group(frame);
        i++;
        continue;
      }
      if (frame.case_depth > 0 && frame.is_case_pattern_expected) {
        frame.is_case_pattern_expected = false;
        frame.is_command_position = true;
        i++;
        continue;
      }

      let const closed_frame = do_pop_frame();
      consider_shell_lexical_frame(closed_frame, i, state.frames.count(),
                                   target);
      i++;
      continue;
    }

    i++;
  }

  state.source_position = i;
#if !defined NDEBUG
  DEBUG_SHELL_LEXICAL_SCAN_BYTE_COUNT += i - scan_start;
#endif
}

#if !defined NDEBUG
pure fn debug_shell_lexical_scan_byte_count() wontthrow -> usize
{
  return DEBUG_SHELL_LEXICAL_SCAN_BYTE_COUNT;
}
#endif

fn internal::command_substitution_range(StringView line, usize cursor) throws
    -> completion_command_range
{
  let state = shell_lexical_state{completion_allocator()};
  let target = shell_lexical_scan_target{
      cursor, completion_command_range{0, line.length}
  };

  advance_shell_lexical_state(line, cursor, state, &target);
  for (usize frame_index = 0; frame_index < state.frames.count(); frame_index++)
    consider_shell_lexical_frame(state.frames[frame_index], line.length,
                                 frame_index + 1, &target);

  target.should_stop_at_token_boundary = true;
  advance_shell_lexical_state(line, line.length, state, &target);

  return target.range;
}

} /* namespace completion */

} /* namespace koshka */
