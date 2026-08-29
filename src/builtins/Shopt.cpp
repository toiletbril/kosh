#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

/* An option whose pattern engine is not yet wired still records its state so a
   later query reads it back. */

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-supqo] [optname ...]");

HELP_DESCRIPTION_DECL(
    "The shopt builtin sets, unsets, and queries the bash shell options.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(SHOPT_SET, Bool, 's', "", "Enable each named option.");
FLAG(SHOPT_UNSET, Bool, 'u', "", "Disable each named option.");
FLAG(SHOPT_QUIET, Bool, 'q', "",
     "Suppress the status output, the scripted probe form.");
FLAG(SHOPT_PRINT, Bool, 'p', "",
     "Print in the replayable form, shopt -s or -u per line, and set -o or "
     "+o behind -o.");
FLAG(SHOPT_SET_OPTIONS, Bool, 'o', "", "Operate on the set -o option names.");

REGISTER_BUILTIN_FLAGS(Shopt);

namespace koshka {

namespace {

const StringView SHOPT_OPTION_NAMES[] = {
    "autocd",
    "assoc_expand_once",
    "cdable_vars",
    "cdspell",
    "checkhash",
    "checkjobs",
    "checkwinsize",
    "cmdhist",
    "complete_fullquote",
    "direxpand",
    "dirspell",
    "dotglob",
    "execfail",
    "expand_aliases",
    "extdebug",
    "extglob",
    "extquote",
    "failglob",
    "force_fignore",
    "globasciiranges",
    "globskipdots",
    "globstar",
    "gnu_errfmt",
    "histappend",
    "histreedit",
    "histverify",
    "hostcomplete",
    "huponexit",
    "inherit_errexit",
    "interactive_comments",
    "lastpipe",
    "lithist",
    "localvar_inherit",
    "localvar_unset",
    "login_shell",
    "mailwarn",
    "no_empty_cmd_completion",
    "nocaseglob",
    "nocasematch",
    "nullglob",
    "progcomp",
    "progcomp_alias",
    "promptvars",
    "restricted_shell",
    "shift_verbose",
    "sourcepath",
    "varredir_close",
    "xpg_echo",
};

constexpr PackedStringKey SHOPT_OPTION_KEYS[] = {
    SSK("autocd"),
    SSK("assoc_expand_once"),
    SSK("cdable_vars"),
    SSK("cdspell"),
    SSK("checkhash"),
    SSK("checkjobs"),
    SSK("checkwinsize"),
    SSK("cmdhist"),
    SSK("complete_fullquote"),
    SSK("direxpand"),
    SSK("dirspell"),
    SSK("dotglob"),
    SSK("execfail"),
    SSK("expand_aliases"),
    SSK("extdebug"),
    SSK("extglob"),
    SSK("extquote"),
    SSK("failglob"),
    SSK("force_fignore"),
    SSK("globasciiranges"),
    SSK("globskipdots"),
    SSK("globstar"),
    SSK("gnu_errfmt"),
    SSK("histappend"),
    SSK("histreedit"),
    SSK("histverify"),
    SSK("hostcomplete"),
    SSK("huponexit"),
    SSK("inherit_errexit"),
    SSK("interactive_comments"),
    SSK("lastpipe"),
    SSK("lithist"),
    SSK("localvar_inherit"),
    SSK("localvar_unset"),
    SSK("login_shell"),
    SSK("mailwarn"),
    SSK("no_empty_cmd_completion"),
    SSK("nocaseglob"),
    SSK("nocasematch"),
    SSK("nullglob"),
    SSK("progcomp"),
    SSK("progcomp_alias"),
    SSK("promptvars"),
    SSK("restricted_shell"),
    SSK("shift_verbose"),
    SSK("sourcepath"),
    SSK("varredir_close"),
    SSK("xpg_echo"),
};
constexpr StaticStringSet SHOPT_OPTIONS{SHOPT_OPTION_KEYS};

pure fn is_known_shopt_option(StringView name) wontthrow -> bool
{
  return SHOPT_OPTIONS.contains(name);
}

fn shopt_status_line(StringView name, bool on, Allocator allocator) throws
    -> String
{
  constexpr usize NAME_FIELD_WIDTH = 20;
  let line = String{allocator, name};
  while (line.count() < NAME_FIELD_WIDTH)
    line += ' ';
  line += on ? "\ton\n" : "\toff\n";
  return line;
}

fn format_option_names_help(Allocator allocator) throws -> String
{
  let section = String{allocator, "OPTION NAMES\n"};
  let const total = countof(SHOPT_OPTION_NAMES);
  utils::append_name_columns(
      section, total, [](usize index) { return SHOPT_OPTION_NAMES[index]; });
  return section;
}

/* The bash -p line is a command the shell replays to restore the state, so it
   must execute when a completion script captures it through $(shopt -p name).
 */
fn shopt_reusable_line(StringView name, bool on, bool as_set_option,
                       Allocator allocator) throws -> String
{
  let line = String{allocator};
  if (as_set_option)
    line += on ? "set -o " : "set +o ";
  else
    line += on ? "shopt -s " : "shopt -u ";
  line += name;
  line += '\n';
  return line;
}

} /* namespace */

fn shopt_option_name_list() throws -> const ArrayList<StringView> &
{
  static ArrayList<StringView> names = [] throws {
    let collected = ArrayList<StringView>{heap_allocator()};
    collected.reserve(countof(SHOPT_OPTION_NAMES));
    for (let const &name : SHOPT_OPTION_NAMES)
      collected.push(name);
    return collected;
  }();
  return names;
}

Shopt::Shopt() = default;

pure fn Shopt::kind() const wontthrow -> Builtin::Kind { return Kind::Shopt; }

fn Shopt::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled())
    SHOW_BUILTIN_HELP_EXTRA_AND_RETURN(
        ec, format_option_names_help(cxt.scratch_allocator()).view());

  let const should_enable = FLAG_SHOPT_SET.is_enabled();
  let const should_disable = FLAG_SHOPT_UNSET.is_enabled();
  let const is_quiet = FLAG_SHOPT_QUIET.is_enabled();
  let const should_operate_on_set_options = FLAG_SHOPT_SET_OPTIONS.is_enabled();
  let const should_print_reusable = FLAG_SHOPT_PRINT.is_enabled();
  let names = ArrayList<StringView>{cxt.scratch_allocator()};
  let name_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  for (usize i = 1; i < args.count(); i++) {
    names.push(args[i].view());
    name_locations.push(operand_locations[i]);
  }

  let const do_format_status_line = [&](StringView name, bool on) throws {
    return should_print_reusable
               ? shopt_reusable_line(name, on, should_operate_on_set_options,
                                     cxt.scratch_allocator())
               : shopt_status_line(name, on, cxt.scratch_allocator());
  };

  i32 status = 0;
  let const do_reject_unknown =
      [&](StringView name, const SourceLocation &location) throws -> bool {
    if (is_known_shopt_option(name)) return false;
    status = 1;
    if (!is_quiet)
      report_soft_builtin_error(ec, cxt, location,
                                StringView{"'"} + name +
                                    "' is not a valid shell option name");
    return true;
  };

  /* shopt -o operates on the set -o options, the bridge bash provides so the
     same options answer either builtin. A config probes shopt -qo posix. */
  if (should_operate_on_set_options) {
    if (names.is_empty()) {
      if (!is_quiet) {
        for (let const &name : shell_option_names(false)) {
          let on = query_shell_option(cxt, name);
          if (!on.has_value()) continue;

          if (should_enable && !*on) {
            continue;
          }
          if (should_disable && *on) {
            continue;
          }

          ec.print_to_stdout(do_format_status_line(name, *on).view());
        }
      }
      return 0;
    }
    for (usize n = 0; n < names.count(); n++) {
      let const &name = names[n];
      let const &location = name_locations[n];
      if (should_enable || should_disable) {
        if (!apply_shell_option(cxt, name, should_enable)) {
          if (is_quiet)
            status = 1;
          else
            throw ErrorWithLocation{
                location, StringView{"Unknown shopt option '"} + name + "'"};
        }
      } else if (Maybe<bool> on = query_shell_option(cxt, name); on.has_value())
      {
        if (!*on) status = 1;
        if (!is_quiet)
          ec.print_to_stdout(do_format_status_line(name, *on).view());
      } else {
        if (is_quiet)
          status = 1;
        else
          throw ErrorWithLocation{
              location, StringView{"Unknown shopt option '"} + name + "'"};
      }
    }
    return status;
  }

  if (should_enable || should_disable) {
    if (names.is_empty()) {
      if (!is_quiet) {
        for (let const &name : SHOPT_OPTION_NAMES) {
          let const is_on = cxt.is_shopt_enabled(name);
          if (should_enable && !is_on) {
            continue;
          }
          if (should_disable && is_on) {
            continue;
          }

          ec.print_to_stdout(do_format_status_line(name, is_on).view());
        }
      }
      return 0;
    }

    for (usize n = 0; n < names.count(); n++) {
      let const &name = names[n];
      let const &location = name_locations[n];
      if (do_reject_unknown(name, location)) continue;
      if (name == "restricted_shell") continue;
      LOG(Info, "shopt setting '%.*s' to %s", static_cast<int>(name.length),
          name.data, should_enable ? "on" : "off");
      cxt.set_shopt_option(name, should_enable);
    }
    return status;
  }

  /* A named query reports a non-zero status when any option is off, which the
     -q form relies on. */
  if (names.is_empty()) {
    if (!is_quiet) {
      for (let const &name : SHOPT_OPTION_NAMES)
        ec.print_to_stdout(
            do_format_status_line(name, cxt.is_shopt_enabled(name)).view());
    }
    return 0;
  }

  for (usize n = 0; n < names.count(); n++) {
    let const &name = names[n];
    let const &location = name_locations[n];
    if (do_reject_unknown(name, location)) continue;
    let const is_on = cxt.is_shopt_enabled(name);
    if (!is_on) status = 1;
    if (!is_quiet)
      ec.print_to_stdout(do_format_status_line(name, is_on).view());
  }
  return status;
}

} /* namespace koshka */
