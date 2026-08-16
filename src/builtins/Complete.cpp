#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-abcdefgjksuv] [-o option] [-A action] [-G globpat] "
                   "[-W wordlist] [-F function] [-C command] [-X filterpat] "
                   "[-P prefix] [-S suffix] [-pr] [name ...]");
HELP_DESCRIPTION_DECL(
    "The complete builtin registers a completion spec for a command.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(COMPLETE_WORDLIST, String, 'W', "",
     "Register the word list as the command's candidates.");
FLAG(COMPLETE_FUNCTION, String, 'F', "",
     "Register the function to run on an explicit tab, COMPREPLY style.");
FLAG(COMPLETE_OPTION, ManyStrings, 'o', "",
     "default, bashdefault, and dirnames fall back to filename completion, "
     "any other option is accepted without effect.");
FLAG(COMPLETE_PRINT, Bool, 'p', "",
     "Print the named specs, or every spec, in a replayable form.");
FLAG(COMPLETE_DEFAULT, Bool, 'D', "",
     "Register the default spec used for a command with no spec of its own.");
FLAG(COMPLETE_REMOVE, Bool, 'r', "", "Accepted without effect.");
FLAG(COMPLETE_ACTION, String, 'A', "", "Accept the completion action.");
FLAG(COMPLETE_GLOB, String, 'G', "", "Accept the completion glob.");
FLAG(COMPLETE_COMMAND, String, 'C', "", "Accept the completion command.");
FLAG(COMPLETE_FILTER, String, 'X', "", "Accept the completion filter.");
FLAG(COMPLETE_PREFIX, String, 'P', "", "Accept the completion prefix.");
FLAG(COMPLETE_SUFFIX, String, 'S', "", "Accept the completion suffix.");
FLAG(COMPLETE_EMPTY, Bool, 'E', "", "Accept the empty-command spec.");
FLAG(COMPLETE_INITIAL, Bool, 'I', "", "Accept the initial-word spec.");
FLAG(COMPLETE_ALIAS, Bool, 'a', "", "Accept the alias action.");
FLAG(COMPLETE_BUILTIN, Bool, 'b', "", "Accept the builtin action.");
FLAG(COMPLETE_COMMANDS, Bool, 'c', "", "Accept the command action.");
FLAG(COMPLETE_DIRECTORY, Bool, 'd', "", "Accept the directory action.");
FLAG(COMPLETE_DISABLED, Bool, 'e', "", "Accept the enabled action.");
FLAG(COMPLETE_FILE, Bool, 'f', "", "Accept the file action.");
FLAG(COMPLETE_GROUP, Bool, 'g', "", "Accept the group action.");
FLAG(COMPLETE_JOB, Bool, 'j', "", "Accept the job action.");
FLAG(COMPLETE_KEYWORD, Bool, 'k', "", "Accept the keyword action.");
FLAG(COMPLETE_SERVICE, Bool, 's', "", "Accept the service action.");
FLAG(COMPLETE_USER, Bool, 'u', "", "Accept the user action.");
FLAG(COMPLETE_VARIABLE, Bool, 'v', "", "Accept the variable action.");

REGISTER_BUILTIN_FLAGS(Complete);

namespace koshka {

Complete::Complete() = default;

pure fn Complete::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Complete;
}

fn Complete::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = parse_flags_vec(
      FLAG_LIST, ec.args(), ec.source_location().position, nullptr,
      &ec.arg_locations(), nullptr, builtin_error_context(ec.program()));
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let function_name =
      String{cxt.scratch_allocator(), FLAG_COMPLETE_FUNCTION.is_set()
                                          ? FLAG_COMPLETE_FUNCTION.value()
                                          : StringView{}};
  let word_list =
      String{cxt.scratch_allocator(), FLAG_COMPLETE_WORDLIST.is_set()
                                          ? FLAG_COMPLETE_WORDLIST.value()
                                          : StringView{}};
  let should_use_default = false;
  for (usize i = 0; i < FLAG_COMPLETE_OPTION.count(); i++) {
    let const option = FLAG_COMPLETE_OPTION.get(i);
    if (option == "default" || option == "bashdefault" || option == "dirnames")
    {
      should_use_default = true;
    }
  }
  let const is_default_completion = FLAG_COMPLETE_DEFAULT.is_enabled();
  let const should_print_specs = FLAG_COMPLETE_PRINT.is_enabled();
  let commands = ArrayList<String>{cxt.scratch_allocator()};
  for (usize i = 1; i < args.count(); i++)
    commands.push_managed(args[i].view());

  /* -p reports failure when a named command has no spec, since the
     bash-completion loader reads a successful print as a registered spec. */
  if (should_print_specs) {
    let const do_print_one_spec = [&](StringView command,
                                      const completion_spec &spec) throws {
      let line = String{cxt.scratch_allocator(), "complete "};
      if (spec.should_use_default) line += "-o default ";
      if (!spec.function_name.is_empty()) {
        line += "-F ";
        line += spec.function_name.view();
        line += ' ';
      }
      if (!spec.word_list.is_empty()) {
        /* Each embedded quote is written as the '\'' escape so a list carrying
           an apostrophe replays as valid shell. */
        line += "-W '";
        let const list = spec.word_list.view();
        for (usize i = 0; i < list.length; i++) {
          if (list[i] == '\'')
            line += "'\\''";
          else
            line.push(list[i]);
        }
        line += "' ";
      }
      line += command;
      line += '\n';
      ec.print_to_stdout(line.view());
    };
    if (commands.is_empty()) {
      cxt.completion_specs().for_each(
          [&](StringView command, const completion_spec &spec) {
            do_print_one_spec(command, spec);
          });
      return 0;
    }

    i32 print_status = 0;
    for (let const &command : commands) {
      const completion_spec *spec = cxt.lookup_completion_spec(command.view());
      if (spec == nullptr) {
        print_status = 1;
        continue;
      }
      do_print_one_spec(command.view(), *spec);
    }
    return print_status;
  }

  let const do_make_spec = [&]() throws -> completion_spec {
    let spec = completion_spec{};
    spec.function_name = String{heap_allocator(), function_name};
    spec.word_list = String{heap_allocator(), word_list};
    spec.should_use_default = should_use_default;
    spec.defining_runtime = RuntimeState::capture(cxt);
    return spec;
  };

  if (is_default_completion) {
    LOG(Debug, "complete registering the default spec with function '%s'",
        function_name.c_str());
    cxt.register_default_completion_spec(do_make_spec());
    return 0;
  }

  for (let const &command : commands) {
    LOG(Debug, "complete registering spec for '%s'", command.c_str());
    cxt.register_completion_spec(command.view(), do_make_spec());
  }
  return 0;
}

} /* namespace koshka */
