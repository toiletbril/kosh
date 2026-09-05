/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the readonly builtin. The
 * readonly builtin marks each named variable read-only.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[name[=value] ...]");

HELP_DESCRIPTION_DECL(
    "The readonly builtin marks each named variable read-only.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(READONLY_PRINT, Bool, 'p', "",
     "List the read-only variables in a reusable form.");

REGISTER_BUILTIN_FLAGS(Readonly);

namespace koshka {

Readonly::Readonly() = default;

pure fn Readonly::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Readonly;
}

fn Readonly::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  /* A bare readonly lists every read-only variable, in the declare -r form
     under the bash mood and the POSIX readonly form otherwise. */
  if (args.count() == 1) {
    let const is_declare_form = cxt.is_bash_compatible();
    let out = String{cxt.scratch_allocator()};
    for (let const &name : cxt.readonly_names()) {
      out += is_declare_form ? "declare -r " : "readonly ";
      out += name;
      if (let const value = cxt.get_variable_value(name)) {
        if (is_declare_form) {
          out += "=\"";
          out += quote_for_declare(value->view());
          out += "\"";
        } else {
          out += "='";
          for (usize k = 0; k < value->count(); k++) {
            if ((*value)[k] == '\'')
              out += "'\\''";
            else
              out.push((*value)[k]);
          }
          out += "'";
        }
      }
      out += "\n";
    }
    ec.print_to_stdout(out);
    return 0;
  }

  let has_error = false;
  for (usize i = 1; i < args.count(); i++) {
    let const &arg = args[i];
    let const parts = NameValueArg::from(arg);

    if (!name_is_valid_identifier(parts.get_name())) {
      let const loc = i < operand_locations.count() ? operand_locations[i]
                                                    : ec.source_location();
      report_soft_builtin_error(
          ec, cxt, loc, StringView{"'"} + arg + "' is not a valid identifier");
      has_error = true;
      continue;
    }

    LOG(All, "readonly marking '%.*s' against later assignment",
        static_cast<int>(parts.get_name().length), parts.get_name().data);

    if (parts.get_value().has_value()) {
      cxt.set_shell_variable(parts.get_name(), *parts.get_value());
      cxt.mark_readonly(parts.get_name());
    } else {
      cxt.mark_readonly(arg);
    }
  }

  return has_error ? 1 : 0;
}

} /* namespace koshka */
