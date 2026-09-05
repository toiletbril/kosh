/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements command classification across keywords, aliases,
 * functions, builtins, bundled utilities, and PATH entries. Its flags select
 * type words, executable paths, forced PATH lookup, or every resolution.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("name [name ...]");

HELP_DESCRIPTION_DECL(
    "The type builtin reports how each name resolves as a command.");

FLAG(TYPE_WORD, Bool, 't', "",
     "Print only the word naming the type, such as builtin or file.");
FLAG(TYPE_PATH, Bool, 'p', "",
     "Print the disk path, or nothing when the name is not a file.");
FLAG(TYPE_FORCE_PATH, Bool, 'P', "",
     "Search the PATH and print the disk path even for a name that is also a "
     "builtin.");
FLAG(TYPE_ALL, Bool, 'a', "",
     "Print every location of each name, the keyword, alias, function, or "
     "builtin and every matching file on the PATH.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Type);

namespace koshka {

Type::Type() = default;

pure fn Type::kind() const wontthrow -> Builtin::Kind { return Kind::Type; }

fn Type::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  let const should_print_word = FLAG_TYPE_WORD.is_enabled();
  let const should_print_path = FLAG_TYPE_PATH.is_enabled();
  let const should_force_path = FLAG_TYPE_FORCE_PATH.is_enabled();

  let out = String{cxt.scratch_allocator()};
  let missing_names = ArrayList<String>{cxt.scratch_allocator()};
  bool did_find_all = true;

  for (usize i = 1; i < args.count(); i++) {
    let const &name = args[i];

    LOG(Debug, "type classifying '%s' in resolution order", name.c_str());

    if (should_force_path) {
      let const paths = cxt.get_program_resolver().search(
          name,
          FLAG_TYPE_ALL.is_enabled() ? ProgramResolver::SearchMode::All
                                     : ProgramResolver::SearchMode::First,
          FLAG_TYPE_ALL.is_enabled() ? ProgramResolver::Requirement::Runnable
                                     : ProgramResolver::Requirement::Execution,
          FLAG_TYPE_ALL.is_enabled() ? ProgramResolver::CachePolicy::Bypass
                                     : ProgramResolver::CachePolicy::ReadOnly);
      if (paths.count() != 0) {
        for (let const &path : paths) {
          if (should_print_word)
            out += "file";
          else
            out += path.text();
          out += "\n";
        }
      } else {
        did_find_all = false;
      }
      continue;
    }

    StringView word{};
    Maybe<String> alias_value;
    if (utils::is_posix_reserved_word(name.view()) || name.view() == "[[" ||
        name.view() == "]]" || name.view() == "function" ||
        name.view() == "time")
    {
      word = "keyword";
    } else if (let const alias = cxt.get_alias(name.view()); alias.has_value())
    {
      word = "alias";
      alias_value = alias;
    } else if (cxt.has_functions() && cxt.find_function(name) != nullptr) {
      word = "function";
    } else if (search_builtin(name.view()).has_value()) {
      word = "builtin";
    } else if ((cxt.koshkit() || cxt.mood() == mimic_mood::Default) &&
               koshkit::find_util(name.view()).has_value() &&
               cxt.get_program_resolver().get_status(
                   name, ProgramResolver::StatusLookup::Authoritative) ==
                   ProgramResolver::Status::Missing)
    {
      word = "builtin";
    }

    let const do_describe_resolution = [&](StringView type_word) throws {
      out += name;
      if (type_word == "alias") {
        out += " is an alias for ";
        out += *alias_value;
      } else if (type_word == "keyword") {
        out += " is a shell keyword";
      } else if (type_word == "function") {
        out += " is a shell function";
      } else {
        out += " is a shell builtin";
      }
      out += "\n";
    };

    if (FLAG_TYPE_ALL.is_enabled()) {
      bool has_any = false;
      if (!word.is_empty()) {
        has_any = true;
        if (should_print_word) {
          out += word;
          out += "\n";
        } else if (!should_print_path) {
          do_describe_resolution(word);
        }
      }
      for (let const &path : cxt.get_program_resolver().search(
               name, ProgramResolver::SearchMode::All,
               ProgramResolver::Requirement::Runnable,
               ProgramResolver::CachePolicy::Bypass))
      {
        has_any = true;
        if (should_print_word) {
          out += "file\n";
        } else if (should_print_path) {
          out += path.text();
          out += "\n";
        } else {
          out += name;
          out += " is ";
          out += path.text();
          out += "\n";
        }
      }
      if (!has_any) {
        if (!should_print_word && !should_print_path) {
          missing_names.push_managed(name);
        }
        did_find_all = false;
      }
      continue;
    }

    if (!word.is_empty()) {
      if (should_print_word) {
        out += word;
        out += "\n";
      } else if (!should_print_path) {
        do_describe_resolution(word);
      }
      continue;
    }

    if (let const paths = cxt.get_program_resolver().search(
            name, ProgramResolver::SearchMode::First,
            ProgramResolver::Requirement::Execution,
            ProgramResolver::CachePolicy::ReadOnly);
        paths.count() != 0)
    {
      if (should_print_word) {
        out += "file\n";
      } else if (should_print_path) {
        out += paths[0].text();
        out += "\n";
      } else {
        out += name;
        out += " is ";
        out += paths[0].text();
        out += "\n";
      }
    } else {
      if (!should_print_word && !should_print_path) {
        missing_names.push_managed(name);
      }
      did_find_all = false;
    }
  }

  ec.print_to_stdout(out);

  for (let const &name : missing_names)
    report_soft_builtin_error(ec, cxt,
                              "The command '" + name + "' was not found");

  return did_find_all ? 0 : 1;
}

} /* namespace koshka */
