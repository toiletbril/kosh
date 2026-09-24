/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the compgen builtin. The
 * compgen builtin writes the completion candidates for a word. Each requested
 * action contributes its own candidates in the bash action order, and the glob
 * and the word list are appended after them. The action names are resolved
 * through one static table.
 */

#include "../Builtin.hpp"
#include "../CLI.hpp"
#include "../Completion.hpp"
#include "../Eval.hpp"
#include "../Lexer.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Tokens.hpp"
#include "../Utils.hpp"
#include "../base/Path.hpp"
#include "../base/Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-abcdefgjksuv] [-V varname] [-W wordlist] [-G glob] "
                   "[-A action] [-P prefix] [-S suffix] [-X filterpat] "
                   "[-F function] [-C command] [word]");
HELP_DESCRIPTION_DECL(
    "The compgen builtin writes the completion candidates for a word.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(COMPGEN_WORDLIST, String, 'W', "",
     "Expand the word list the way the shell does and filter to the entries "
     "that start with the word.");
FLAG(COMPGEN_GLOB, String, 'G', "", "Probe the filesystem with the glob.");
FLAG(COMPGEN_FILE, Bool, 'f', "", "List matching filenames.");
FLAG(COMPGEN_ACTION, String, 'A', "", "List the candidates of the action.");
FLAG(COMPGEN_VARIABLE_NAME, String, 'V', "",
     "Store the candidates in the named indexed array and write nothing to "
     "standard output.");
FLAG(COMPGEN_PREFIX, String, 'P', "", "Prepend the prefix to each candidate.");
FLAG(COMPGEN_SUFFIX, String, 'S', "", "Append the suffix to each candidate.");
FLAG(COMPGEN_FILTER, String, 'X', "",
     "Remove matching candidates, with leading ! reversing the filter and "
     "unescaped & expanding to the completion word.");
FLAG(COMPGEN_FUNCTION, String, 'F', "", "Accepted without effect.");
FLAG(COMPGEN_COMMAND, String, 'C', "", "Accepted without effect.");
FLAG(COMPGEN_OPTION, String, 'o', "", "Accept the completion option.");
FLAG(COMPGEN_COMMANDS, Bool, 'c', "", "List matching commands.");
FLAG(COMPGEN_ALIAS, Bool, 'a', "", "List matching alias names.");
FLAG(COMPGEN_BUILTIN, Bool, 'b', "", "List matching builtin names.");
FLAG(COMPGEN_DIRECTORY, Bool, 'd', "", "List matching directory names.");
FLAG(COMPGEN_EXPORT, Bool, 'e', "", "List matching exported variable names.");
FLAG(COMPGEN_GROUP, Bool, 'g', "", "List matching group names.");
FLAG(COMPGEN_JOB, Bool, 'j', "", "List matching job names.");
FLAG(COMPGEN_KEYWORD, Bool, 'k', "", "List matching shell keywords.");
FLAG(COMPGEN_SERVICE, Bool, 's', "", "List matching service names.");
FLAG(COMPGEN_USER, Bool, 'u', "", "List matching user names.");
FLAG(COMPGEN_VARIABLE, Bool, 'v', "", "List matching shell variable names.");

REGISTER_BUILTIN_FLAGS(Compgen);

namespace koshka {

Compgen::Compgen() = default;

enum class compgen_action : u8
{
  Alias,
  ArrayVar,
  Binding,
  Builtin,
  Command,
  Directory,
  Disabled,
  Enabled,
  Export,
  File,
  Function,
  Group,
  HelpTopic,
  Hostname,
  Job,
  Keyword,
  Running,
  Service,
  SetOpt,
  ShOpt,
  Signal,
  Stopped,
  User,
  Variable,
};

static constexpr static_string_entry<compgen_action> COMPGEN_ACTION_ENTRIES[] =
    {
        {SSK("alias"),     compgen_action::Alias    },
        {SSK("arrayvar"),  compgen_action::ArrayVar },
        {SSK("binding"),   compgen_action::Binding  },
        {SSK("builtin"),   compgen_action::Builtin  },
        {SSK("command"),   compgen_action::Command  },
        {SSK("directory"), compgen_action::Directory},
        {SSK("disabled"),  compgen_action::Disabled },
        {SSK("enabled"),   compgen_action::Enabled  },
        {SSK("export"),    compgen_action::Export   },
        {SSK("file"),      compgen_action::File     },
        {SSK("function"),  compgen_action::Function },
        {SSK("group"),     compgen_action::Group    },
        {SSK("helptopic"), compgen_action::HelpTopic},
        {SSK("hostname"),  compgen_action::Hostname },
        {SSK("job"),       compgen_action::Job      },
        {SSK("keyword"),   compgen_action::Keyword  },
        {SSK("running"),   compgen_action::Running  },
        {SSK("service"),   compgen_action::Service  },
        {SSK("setopt"),    compgen_action::SetOpt   },
        {SSK("shopt"),     compgen_action::ShOpt    },
        {SSK("signal"),    compgen_action::Signal   },
        {SSK("stopped"),   compgen_action::Stopped  },
        {SSK("user"),      compgen_action::User     },
        {SSK("variable"),  compgen_action::Variable },
};

static constexpr StaticStringMap COMPGEN_ACTIONS{COMPGEN_ACTION_ENTRIES};

static pure fn action_bit(compgen_action action) wontthrow -> u32
{
  return 1U << static_cast<u32>(action);
}

struct compgen_filter
{
  explicit compgen_filter(Allocator allocator)
      : pattern(allocator), active(allocator)
  {}

  String pattern;
  Bitset active;
  bool is_negated{false};
  extglob_mode extglob{extglob_mode::Disabled};
};

static fn compile_filter(StringView raw_filter, StringView word,
                         Allocator allocator, extglob_mode extglob) throws
    -> compgen_filter
{
  let compiled = compgen_filter{allocator};
  compiled.extglob = extglob;

  let const upper_bound = raw_filter.length + word.length;
  compiled.pattern.reserve(upper_bound);
  compiled.active.reserve(upper_bound);

  usize position = 0;
  if (!raw_filter.is_empty() && raw_filter[0] == '!') {
    compiled.is_negated = true;
    position++;
  }

  while (position < raw_filter.length) {
    let const character = raw_filter[position++];
    if (character == '\\' && position < raw_filter.length) {
      compiled.pattern.push(raw_filter[position++]);
      compiled.active.push(false);
      continue;
    }
    if (character == '&') {
      compiled.pattern.append(word);
      for (usize i = 0; i < word.length; i++)
        compiled.active.push(false);
      continue;
    }
    compiled.pattern.push(character);
    compiled.active.push(character != '\\');
  }

  return compiled;
}

static fn candidate_is_excluded(StringView candidate,
                                const compgen_filter &filter) throws -> bool
{
  let const matches = utils::glob_matches(filter.pattern.view(), candidate,
                                          filter.active, 0, filter.extglob);
  return filter.is_negated ? !matches : matches;
}

struct compgen_sink
{
  compgen_sink(Allocator text_allocator, Maybe<StringView> variable_name,
               StringView prefix, StringView suffix)
      : text(text_allocator), values(heap_allocator()),
        variable_name(variable_name), prefix(prefix), suffix(suffix)
  {}

  fn push_candidate(StringView candidate) throws -> void
  {
    has_any_matched = true;

    if (variable_name.has_value()) {
      let value = String{heap_allocator()};
      value.reserve(prefix.length + candidate.length + suffix.length);
      value.append(prefix);
      value.append(candidate);
      value.append(suffix);
      values.push(steal(value));

      return;
    }

    text.reserve(text.count() + prefix.length + candidate.length +
                 suffix.length + 1);
    text.append(prefix);
    text.append(candidate);
    text.append(suffix);
    text.push('\n');
  }

  fn publish(ExecContext &ec, EvalContext &cxt) throws -> i32
  {
    if (variable_name.has_value())
      cxt.set_indexed_array(*variable_name, steal(values));
    else if (has_any_matched)
      ec.print_to_stdout(text.view());

    return has_any_matched ? 0 : 1;
  }

  String text;
  ArrayList<String> values;
  Maybe<StringView> variable_name;
  StringView prefix;
  StringView suffix;
  bool has_any_matched{false};
};

struct compgen_emitter
{
  compgen_sink &sink;
  StringView word;
  const Maybe<compgen_filter> &filter;

  fn push_filtered(StringView candidate) throws -> void
  {
    if (filter.has_value() && candidate_is_excluded(candidate, *filter)) {
      return;
    }

    sink.push_candidate(candidate);
  }

  fn push_prefixed(StringView candidate) throws -> void
  {
    if (!candidate.starts_with(word)) return;

    push_filtered(candidate);
  }
};

enum class table_field_mode : u8
{
  FirstField,
  FieldsAfterFirst,
};

static fn push_table_file_names(compgen_emitter &emitter, StringView path,
                                table_field_mode mode,
                                Allocator allocator) throws -> void
{
  let const contents = Path{path}.read_entire_file();
  if (!contents.has_value()) return;

  for (let const &line : utils::split_lines(contents->view(), allocator)) {
    let const comment_start = line.find_character('#');
    let const body = comment_start.has_value()
                         ? line.substring_of_length(0, *comment_start)
                         : line;

    usize position = 0;
    usize field_index = 0;
    while (position < body.length) {
      while (position < body.length && lexer::is_whitespace(body[position]))
        position++;

      let const field_start = position;
      while (position < body.length && !lexer::is_whitespace(body[position]))
        position++;

      if (position == field_start) break;

      let const field =
          body.substring_of_length(field_start, position - field_start);
      let const is_wanted = mode == table_field_mode::FirstField
                                ? field_index == 0
                                : field_index > 0;
      if (is_wanted) emitter.push_prefixed(field);

      field_index++;
      if (mode == table_field_mode::FirstField) break;
    }
  }
}

static fn run_compgen_actions(EvalContext &cxt, u32 action_mask,
                              compgen_emitter &emitter) throws -> void
{
  let const do_wants = [&](compgen_action action) wontthrow -> bool {
    return (action_mask & action_bit(action)) != 0;
  };
  let const do_push_name = [&](StringView name)
                               throws -> void { emitter.push_prefixed(name); };

  if (do_wants(compgen_action::Alias)) cxt.for_each_alias_name(do_push_name);

  let const should_scan_shell_variables =
      do_wants(compgen_action::ArrayVar) || do_wants(compgen_action::Variable);
  let const shell_variable_names =
      should_scan_shell_variables ? cxt.variable_names(cxt.scratch_allocator())
                                  : HashSet{cxt.scratch_allocator()};
  let const should_scan_environment =
      do_wants(compgen_action::Export) || do_wants(compgen_action::Variable);
  let const environment_names = should_scan_environment
                                    ? os::environment_names()
                                    : ArrayList<String>{heap_allocator()};

  if (do_wants(compgen_action::ArrayVar)) {
    shell_variable_names.for_each([&](StringView name) throws {
      if (cxt.lookup_indexed_array(name) == nullptr &&
          !cxt.is_associative_array(name))
      {
        return;
      }

      emitter.push_prefixed(name);
    });
  }

  if (do_wants(compgen_action::Builtin) || do_wants(compgen_action::Enabled) ||
      do_wants(compgen_action::HelpTopic))
  {
    for (let const &name : builtin_names())
      emitter.push_prefixed(name.view());
  }

  if (do_wants(compgen_action::Command)) {
    cxt.get_program_resolver().begin_explicit_completion(
        ProgramResolver::CompletionRefresh::Fresh);
    defer { cxt.get_program_resolver().end_explicit_completion(); };
    let const scratch = completion::ScopedCompletionScratch{};

    for (let const &candidate :
         completion::complete_command_names_by_prefix(emitter.word, cxt))
    {
      emitter.push_filtered(candidate.view());
    }
  }

  if (do_wants(compgen_action::Directory) || do_wants(compgen_action::File)) {
    cxt.get_program_resolver().begin_explicit_completion(
        ProgramResolver::CompletionRefresh::Fresh);
    defer { cxt.get_program_resolver().end_explicit_completion(); };

    if (do_wants(compgen_action::Directory)) {
      let const scratch = completion::ScopedCompletionScratch{};

      for (let const &candidate :
           completion::complete_filesystem_names_by_prefix(
               emitter.word, cxt, Path::current_directory(),
               completion::completion_filesystem_mode::Directories))
      {
        emitter.push_filtered(candidate.view());
      }
    }

    if (do_wants(compgen_action::File)) {
      let const scratch = completion::ScopedCompletionScratch{};

      for (let const &candidate :
           completion::complete_filesystem_names_by_prefix(
               emitter.word, cxt, Path::current_directory(),
               completion::completion_filesystem_mode::Files))
      {
        emitter.push_filtered(candidate.view());
      }
    }
  }

  if (do_wants(compgen_action::Export)) {
    for (let const &name : environment_names)
      emitter.push_prefixed(name.view());
  }

  if (do_wants(compgen_action::Function)) {
    cxt.for_each_function_name(do_push_name);
  }

  if (do_wants(compgen_action::Group)) {
    for (let const &name : os::enumerate_groups())
      emitter.push_prefixed(name.view());
  }

  if (do_wants(compgen_action::Hostname)) {
    push_table_file_names(emitter, "/etc/hosts",
                          table_field_mode::FieldsAfterFirst,
                          cxt.scratch_allocator());
  }

  if (do_wants(compgen_action::Job) || do_wants(compgen_action::Running) ||
      do_wants(compgen_action::Stopped))
  {
    for (let const &entry : cxt.jobs()) {
      let const is_wanted = do_wants(compgen_action::Job) ||
                            (do_wants(compgen_action::Running) &&
                             entry.state == job::State::Running) ||
                            (do_wants(compgen_action::Stopped) &&
                             entry.state == job::State::Stopped);
      if (!is_wanted) continue;

      emitter.push_prefixed(entry.command.view());
    }
  }

  if (do_wants(compgen_action::Keyword)) {
    for (let const &name : keyword_names())
      emitter.push_prefixed(name.view());
  }

  if (do_wants(compgen_action::Service)) {
    push_table_file_names(emitter, "/etc/services",
                          table_field_mode::FirstField,
                          cxt.scratch_allocator());
  }

  if (do_wants(compgen_action::SetOpt)) {
    for (let const name : shell_option_names(false))
      emitter.push_prefixed(name);
  }

  if (do_wants(compgen_action::ShOpt)) {
    for (let const name : shopt_option_name_list())
      emitter.push_prefixed(name);
  }

  if (do_wants(compgen_action::Signal)) {
    emitter.push_prefixed("EXIT");

    let prefixed_name = String{cxt.scratch_allocator()};
    for (let const name : os::signal_names()) {
      prefixed_name.clear();
      prefixed_name.append("SIG");
      prefixed_name.append(name);
      emitter.push_prefixed(prefixed_name.view());
    }
  }

  if (do_wants(compgen_action::User)) {
    for (let const &name : os::enumerate_users())
      emitter.push_prefixed(name.view());
  }

  if (do_wants(compgen_action::Variable)) {
    shell_variable_names.for_each(do_push_name);

    for (let const &name : environment_names) {
      if (shell_variable_names.contains(name.view())) continue;

      emitter.push_prefixed(name.view());
    }
  }
}

pure fn Compgen::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Compgen;
}

fn Compgen::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = parse_flags_vec(
      FLAG_LIST, ec.args(), ec.source_location().position, nullptr,
      &ec.arg_locations(), nullptr, builtin_error_context(ec.program()));
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let const variable_name =
      FLAG_COMPGEN_VARIABLE_NAME.is_set()
          ? Maybe<StringView>{FLAG_COMPGEN_VARIABLE_NAME.value()}
          : None;
  if (variable_name.has_value()) {
    let const target_location = FLAG_COMPGEN_VARIABLE_NAME.value_location();

    if (!name_is_valid_identifier(*variable_name)) {
      report_soft_builtin_error(ec, cxt, target_location,
                                StringView{"'"} + *variable_name +
                                    "' is not a valid identifier");

      return 2;
    }

    if (cxt.is_associative_array(*variable_name)) {
      report_soft_builtin_error(ec, cxt, target_location,
                                StringView{"'"} + *variable_name +
                                    "' is not an indexed array");

      return 1;
    }

    if (cxt.is_readonly(*variable_name)) {
      report_soft_builtin_error(ec, cxt, target_location,
                                StringView{"'"} + *variable_name +
                                    "' is read-only");

      return 1;
    }
  }

  u32 action_mask = 0;
  if (FLAG_COMPGEN_ALIAS.is_enabled()) {
    action_mask |= action_bit(compgen_action::Alias);
  }
  if (FLAG_COMPGEN_BUILTIN.is_enabled()) {
    action_mask |= action_bit(compgen_action::Builtin);
  }
  if (FLAG_COMPGEN_COMMANDS.is_enabled()) {
    action_mask |= action_bit(compgen_action::Command);
  }
  if (FLAG_COMPGEN_DIRECTORY.is_enabled()) {
    action_mask |= action_bit(compgen_action::Directory);
  }
  if (FLAG_COMPGEN_EXPORT.is_enabled()) {
    action_mask |= action_bit(compgen_action::Export);
  }
  if (FLAG_COMPGEN_FILE.is_enabled()) {
    action_mask |= action_bit(compgen_action::File);
  }
  if (FLAG_COMPGEN_GROUP.is_enabled()) {
    action_mask |= action_bit(compgen_action::Group);
  }
  if (FLAG_COMPGEN_JOB.is_enabled()) {
    action_mask |= action_bit(compgen_action::Job);
  }
  if (FLAG_COMPGEN_KEYWORD.is_enabled()) {
    action_mask |= action_bit(compgen_action::Keyword);
  }
  if (FLAG_COMPGEN_SERVICE.is_enabled()) {
    action_mask |= action_bit(compgen_action::Service);
  }
  if (FLAG_COMPGEN_USER.is_enabled()) {
    action_mask |= action_bit(compgen_action::User);
  }
  if (FLAG_COMPGEN_VARIABLE.is_enabled()) {
    action_mask |= action_bit(compgen_action::Variable);
  }

  if (FLAG_COMPGEN_ACTION.is_set()) {
    let const name = FLAG_COMPGEN_ACTION.value();
    let const resolved = COMPGEN_ACTIONS.find(name);
    if (!resolved.has_value()) {
      report_soft_builtin_error(ec, cxt, FLAG_COMPGEN_ACTION.value_location(),
                                StringView{"'"} + name +
                                    "' is not a valid action name");

      return 2;
    }

    action_mask |= action_bit(*resolved);
  }

  let const wordlist = FLAG_COMPGEN_WORDLIST.is_set()
                           ? Maybe<StringView>{FLAG_COMPGEN_WORDLIST.value()}
                           : None;
  let const glob_pattern = FLAG_COMPGEN_GLOB.is_set()
                               ? Maybe<StringView>{FLAG_COMPGEN_GLOB.value()}
                               : None;
  let const filter_pattern =
      FLAG_COMPGEN_FILTER.is_set()
          ? Maybe<StringView>{FLAG_COMPGEN_FILTER.value()}
          : None;
  let const word = args.count() > 1 ? args[1].view() : StringView{};

  let const scratch = cxt.scratch_mark();
  defer { cxt.scratch_release(scratch); };

  Maybe<compgen_filter> filter = None;
  if (filter_pattern.has_value()) {
    filter = compile_filter(*filter_pattern, word, cxt.scratch_allocator(),
                            cxt.get_extglob_mode());
  }

  let const prefix =
      FLAG_COMPGEN_PREFIX.is_set() ? FLAG_COMPGEN_PREFIX.value() : StringView{};
  let const suffix =
      FLAG_COMPGEN_SUFFIX.is_set() ? FLAG_COMPGEN_SUFFIX.value() : StringView{};
  let sink =
      compgen_sink{cxt.scratch_allocator(), variable_name, prefix, suffix};
  let emitter = compgen_emitter{sink, word, filter};

  if (action_mask != 0) {
    LOG(Debug, "compgen collecting actions 0x%x for prefix '%.*s'", action_mask,
        static_cast<int>(word.length), word.data);
    run_compgen_actions(cxt, action_mask, emitter);
  }

  if (glob_pattern.has_value()) {
    LOG(All, "compgen expanding glob '%.*s' for prefix '%.*s'",
        static_cast<int>(glob_pattern->length), glob_pattern->data,
        static_cast<int>(word.length), word.data);

    for (let const &match : cxt.expand_glob_lenient(*glob_pattern))
      emitter.push_prefixed(match.view());
  }

  if (wordlist.has_value()) {
    LOG(Debug, "compgen filtering word list for prefix '%.*s'",
        static_cast<int>(word.length), word.data);

    for (let const &candidate : cxt.expand_wordlist_to_fields(*wordlist))
      emitter.push_prefixed(candidate.view());
  }

  return sink.publish(ec, cxt);
}

} /* namespace koshka */
