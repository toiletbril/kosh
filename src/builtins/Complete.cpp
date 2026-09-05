/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the complete builtin. The
 * complete builtin registers a completion spec for a command.
 */

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

static fn
append_completion_specification_line(String &output, StringView command,
                                     const completion_spec &spec) throws -> void
{
  output += "complete ";
  if (spec.should_use_default) output += "-o default ";
  if (!spec.word_list.is_empty()) {
    output += "-W ";
    append_shell_quoted_arg(output, spec.word_list.view(), true);
    output += ' ';
  }
  if (!spec.function_name.is_empty()) {
    output += "-F ";
    output += spec.function_name.view();
    output += ' ';
  }
  append_shell_quoted_arg(output, command);
  output += '\n';
}

static fn completion_specification_reusable_lines(const EvalContext &cxt) throws
    -> String
{
  let lines = String{heap_allocator()};
  let names = ArrayList<String>{heap_allocator()};
  cxt.completion_specs().for_each(
      [&](StringView command, const completion_spec &) -> void {
        names.push_managed(command);
      });
  names.sort();

  for (let const &name : names) {
    let const *spec = cxt.lookup_completion_spec(name.view());
    ASSERT(spec != nullptr);
    append_completion_specification_line(lines, name.view(), *spec);
  }

  if (let const *spec = cxt.default_completion_spec(); spec != nullptr)
    append_completion_specification_line(lines, "-D", *spec);

  return lines;
}

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

  if (should_print_specs) {
    if (is_default_completion) {
      let output = String{cxt.scratch_allocator()};
      let const *spec = cxt.default_completion_spec();
      if (spec == nullptr) {
        report_soft_builtin_error(
            ec, cxt, "The default completion specification was not found");
        return 1;
      }
      append_completion_specification_line(output, "-D", *spec);
      ec.print_to_stdout(output.view());
      return 0;
    }

    if (commands.is_empty()) {
      ec.print_to_stdout(completion_specification_reusable_lines(cxt).view());
      return 0;
    }

    let output = String{cxt.scratch_allocator()};
    i32 print_status = 0;
    for (let const &command : commands) {
      let const *spec = cxt.lookup_completion_spec(command.view());
      if (spec == nullptr) {
        report_soft_builtin_error(ec, cxt,
                                  "The command '" + command +
                                      "' has no completion specification");
        print_status = 1;
        continue;
      }
      append_completion_specification_line(output, command.view(), *spec);
    }
    ec.print_to_stdout(output.view());
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
