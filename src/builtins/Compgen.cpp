#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Completion.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-c] [-V varname] [-W wordlist] [-G glob] [-A action] "
                   "[-P prefix] [-S suffix] [-X filterpat] [-F function] "
                   "[-C command] [word]");
HELP_DESCRIPTION_DECL(
    "The compgen builtin writes the completion candidates for a word.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(COMPGEN_WORDLIST, String, 'W', "",
     "Expand the word list the way the shell does and filter to the entries "
     "that start with the word.");
FLAG(COMPGEN_GLOB, String, 'G', "", "Probe the filesystem with the glob.");
FLAG(COMPGEN_FILE, Bool, 'f', "", "List matching filenames.");
FLAG(COMPGEN_ACTION, String, 'A', "", "List commands for the command action.");
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
FLAG(COMPGEN_ALIAS, Bool, 'a', "", "Accept the alias action.");
FLAG(COMPGEN_BUILTIN, Bool, 'b', "", "Accept the builtin action.");
FLAG(COMPGEN_DIRECTORY, Bool, 'd', "", "Accept the directory action.");
FLAG(COMPGEN_DISABLED, Bool, 'e', "", "Accept the enabled action.");
FLAG(COMPGEN_GROUP, Bool, 'g', "", "Accept the group action.");
FLAG(COMPGEN_JOB, Bool, 'j', "", "Accept the job action.");
FLAG(COMPGEN_KEYWORD, Bool, 'k', "", "Accept the keyword action.");
FLAG(COMPGEN_SERVICE, Bool, 's', "", "Accept the service action.");
FLAG(COMPGEN_USER, Bool, 'u', "", "Accept the user action.");
FLAG(COMPGEN_VARIABLE, Bool, 'v', "", "Accept the variable action.");

REGISTER_BUILTIN_FLAGS(Compgen);

namespace koshka {

Compgen::Compgen() = default;

struct compgen_filter
{
  explicit compgen_filter(Allocator allocator)
      : pattern(allocator), active(allocator)
  {}

  String pattern;
  Bitset active;
  bool is_negated{false};
};

static fn compile_filter(StringView raw_filter, StringView word,
                         Allocator allocator) throws -> compgen_filter
{
  let compiled = compgen_filter{allocator};
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
                                const compgen_filter &filter,
                                const EvalContext &cxt) throws -> bool
{
  let const matches =
      utils::glob_matches(filter.pattern.view(), candidate, filter.active, 0,
                          cxt.extglob_enabled());
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
      let value = String{heap_allocator(), prefix};
      value.append(candidate);
      value.append(suffix);
      values.push(steal(value));

      return;
    }

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

  let const wordlist = FLAG_COMPGEN_WORDLIST.is_set()
                           ? Maybe<StringView>{FLAG_COMPGEN_WORDLIST.value()}
                           : None;
  let const glob_pattern = FLAG_COMPGEN_GLOB.is_set()
                               ? Maybe<StringView>{FLAG_COMPGEN_GLOB.value()}
                               : None;
  let const action = FLAG_COMPGEN_ACTION.is_set()
                         ? Maybe<StringView>{FLAG_COMPGEN_ACTION.value()}
                         : None;
  let const filter_pattern =
      FLAG_COMPGEN_FILTER.is_set()
          ? Maybe<StringView>{FLAG_COMPGEN_FILTER.value()}
          : None;
  let const should_list_commands = FLAG_COMPGEN_COMMANDS.is_enabled();
  let const should_list_files = FLAG_COMPGEN_FILE.is_enabled();
  let const word = args.count() > 1 ? args[1].view() : StringView{};

  Maybe<compgen_filter> filter = None;
  if (filter_pattern.has_value())
    filter = compile_filter(*filter_pattern, word, cxt.scratch_allocator());

  let const prefix =
      FLAG_COMPGEN_PREFIX.is_set() ? FLAG_COMPGEN_PREFIX.value() : StringView{};
  let const suffix =
      FLAG_COMPGEN_SUFFIX.is_set() ? FLAG_COMPGEN_SUFFIX.value() : StringView{};
  let sink =
      compgen_sink{cxt.scratch_allocator(), variable_name, prefix, suffix};

  if (should_list_commands || (action.has_value() && *action == "command")) {
    cxt.get_program_resolver().begin_explicit_completion(
        ProgramResolver::CompletionRefresh::Fresh);
    defer { cxt.get_program_resolver().end_explicit_completion(); };

    for (let const &candidate : completion::complete_command_names(
             word, completion::command_match_mode::Prefix, cxt))
    {
      if (filter.has_value() &&
          candidate_is_excluded(candidate.view(), *filter, cxt))
      {
        continue;
      }
      sink.push_candidate(candidate.view());
    }

    return sink.publish(ec, cxt);
  }

  if (glob_pattern.has_value()) {
    LOG(All, "compgen expanding glob '%.*s' for prefix '%.*s'",
        static_cast<int>(glob_pattern->length), glob_pattern->data,
        static_cast<int>(word.length), word.data);

    for (let const &match : cxt.expand_glob_lenient(*glob_pattern)) {
      if (!match.view().starts_with(word)) continue;
      if (filter.has_value() &&
          candidate_is_excluded(match.view(), *filter, cxt))
      {
        continue;
      }
      sink.push_candidate(match.view());
    }

    return sink.publish(ec, cxt);
  }

  if (should_list_files) {
    cxt.get_program_resolver().begin_explicit_completion(
        ProgramResolver::CompletionRefresh::Fresh);
    defer { cxt.get_program_resolver().end_explicit_completion(); };

    for (let const &candidate : completion::complete_filesystem_names(
             word, cxt, Path::current_directory()))
    {
      if (filter.has_value() &&
          candidate_is_excluded(candidate.view(), *filter, cxt))
      {
        continue;
      }
      sink.push_candidate(candidate.view());
    }

    return sink.publish(ec, cxt);
  }

  if (!wordlist.has_value()) return sink.publish(ec, cxt);

  LOG(Debug, "compgen filtering word list for prefix '%.*s'",
      static_cast<int>(word.length), word.data);

  for (let const &candidate : cxt.expand_wordlist_to_fields(*wordlist)) {
    if (!candidate.view().starts_with(word)) continue;
    if (filter.has_value() &&
        candidate_is_excluded(candidate.view(), *filter, cxt))
    {
      continue;
    }
    sink.push_candidate(candidate.view());
  }

  return sink.publish(ec, cxt);
}

} /* namespace koshka */
