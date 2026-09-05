/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements command lookup and execution that bypasses shell
 * functions. It supports terse and verbose classification, default-PATH
 * lookup, restricted-shell checks, and direct dispatch of aliases, builtins,
 * bundled utilities, and programs.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-v] [-V] name [argument ...]");

HELP_DESCRIPTION_DECL(
    "The command builtin runs a command past a same-named function.");

FLAG(SHOW, Bool, 'v', "", "Print the resolution of the name in a terse form.");
FLAG(SHOW_VERBOSE, Bool, 'V', "",
     "Print the resolution of the name verbosely.");
FLAG(COMMAND_DEFAULT_PATH, Bool, 'p', "",
     "Resolve against a default PATH that finds the standard utilities.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(CommandBuiltin);

namespace koshka {

CommandBuiltin::CommandBuiltin() = default;

pure fn CommandBuiltin::kind() const wontthrow -> Builtin::Kind
{
  return Kind::CommandBuiltin;
}

fn CommandBuiltin::execute(ExecContext &ec, EvalContext &cxt) const throws
    -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  if (FLAG_COMMAND_DEFAULT_PATH.is_enabled() &&
      cxt.restricted_enforcement_active())
  {
    throw ErrorWithLocation{ec.source_location(),
                            "command -p is forbidden in a restricted shell"};
  }

  if (args.count() < 2) return 0;
  if (!FLAG_SHOW.is_enabled() && !FLAG_SHOW_VERBOSE.is_enabled()) {
    cxt.guard_restricted_path(args[1].view(), ec.arg_location_at(1),
                              restricted_path_use::Command);
  }

  let default_resolver = ProgramResolver{String{"/usr/bin:/bin"}};
  let &resolver = FLAG_COMMAND_DEFAULT_PATH.is_enabled()
                      ? default_resolver
                      : cxt.get_program_resolver();

  if (FLAG_SHOW.is_enabled() || FLAG_SHOW_VERBOSE.is_enabled()) {
    let const is_verbose = FLAG_SHOW_VERBOSE.is_enabled();
    bool did_find_any = false;

    for (usize argument_index = 1; argument_index < args.count();
         argument_index++)
    {
      let const &name = args[argument_index];
      LOG(Debug, "command resolving '%s' past shell functions", name.c_str());

      if (os::has_directory_separator(name.view())) {
        let const paths =
            resolver.search(name, ProgramResolver::SearchMode::First,
                            ProgramResolver::Requirement::Runnable,
                            ProgramResolver::CachePolicy::Bypass);
        if (!paths.is_empty()) {
          ec.print_to_stdout(is_verbose ? name + " is " + name + "\n"
                                        : name + "\n");
          did_find_any = true;
        } else if (is_verbose) {
          ec.print_to_stdout(name + ": not found\n");
        }
        continue;
      }

      if (utils::is_posix_reserved_word(name.view())) {
        ec.print_to_stdout(is_verbose ? name + " is a shell keyword\n"
                                      : name + "\n");
        did_find_any = true;
        continue;
      }
      if (let const alias = cxt.get_alias(name.view()); alias.has_value()) {
        if (is_verbose)
          ec.print_to_stdout(name + " is aliased to `" + *alias + "'\n");
        else
          ec.print_to_stdout("alias " + name + "='" + *alias + "'\n");
        did_find_any = true;
        continue;
      }
      if (cxt.has_functions() && cxt.find_function(name.view()) != nullptr) {
        ec.print_to_stdout(is_verbose ? name + " is a function\n"
                                      : name + "\n");
        did_find_any = true;
        continue;
      }
      if (search_builtin(name.view()).has_value()) {
        ec.print_to_stdout(is_verbose ? name + " is a shell builtin\n"
                                      : name + "\n");
        did_find_any = true;
        continue;
      }
      if (let const paths =
              resolver.search(name, ProgramResolver::SearchMode::First,
                              ProgramResolver::Requirement::Regular,
                              ProgramResolver::CachePolicy::ReadOnly);
          !paths.is_empty())
      {
        let resolved_text = String{cxt.scratch_allocator()};
        if (is_verbose) {
          resolved_text += name;
          resolved_text += " is ";
        }
        resolved_text += paths[0].text();
        resolved_text += "\n";
        ec.print_to_stdout(resolved_text);
        did_find_any = true;
        continue;
      }
      if ((cxt.koshkit() || cxt.mood() == mimic_mood::Default) &&
          koshkit::find_util(name.view()).has_value())
      {
        ec.print_to_stdout(is_verbose ? name + " is a built-in utility\n"
                                      : name + "\n");
        did_find_any = true;
        continue;
      }
      if (is_verbose) {
        report_soft_builtin_error(ec, cxt,
                                  "The command '" + name + "' was not found");
      }
    }

    return did_find_any ? 0 : 1;
  }

  let operand_args = ArrayList<String>{heap_allocator()};
  let operand_arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  for (usize i = 1; i < args.count(); i++) {
    operand_args.push_managed(args[i]);
    operand_arg_locations.push(ec.arg_location_at(i));
  }

  Maybe<ExecContext> sub;
  try {
    let const *source = cxt.current_source();
    sub = ExecContext::make_from(
        ec.source_location(), source != nullptr ? source->view() : StringView{},
        steal(operand_args), cxt.mood(), cxt.koshkit(),
        cxt.is_shopt_enabled("checkhash"), resolver,
        steal(operand_arg_locations));
  } catch (const CommandResolutionErrorWithLocation &resolution_error) {
    LOG(Debug, "command handled a resolution error: %s",
        resolution_error.message().c_str());
    const String *source = cxt.current_source();
    show_message(resolution_error.to_string(
        source != nullptr ? source->view() : StringView{}, &cxt));
    return static_cast<i32>(resolution_error.command_status());
  }
  return utils::execute_context(steal(*sub), cxt, execution_mode::Foreground);
}

} /* namespace koshka */
