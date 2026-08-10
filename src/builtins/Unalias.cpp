#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] name [name ...]");

HELP_DESCRIPTION_DECL("The unalias builtin removes each named alias.");

FLAG(ALL, Bool, 'a', "", "Remove every alias.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Unalias);

namespace koshka {

Unalias::Unalias() = default;

pure fn Unalias::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Unalias;
}

fn Unalias::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  if (FLAG_ALL.is_enabled()) {
    LOG(Debug, "unalias removing every alias");
    /* alias_definitions yields name='value', so the name ends at the equals. */
    for (let const &definition : cxt.alias_definitions()) {
      let const equals_position = definition.find_character('=');
      let const name_length =
          equals_position.has_value() ? *equals_position : definition.count();
      let const name = definition.substring_of_length(0, name_length);
      cxt.remove_alias(name);
    }
    return 0;
  }

  if (args.count() < 2) return report_usage_error(ec, cxt, ec.program());

  i32 status = 0;
  for (usize i = 1; i < args.count(); i++) {
    let const &name = args[i];
    LOG(All, "unalias removing alias '%s'", name.c_str());
    if (!cxt.remove_alias(name)) {
      report_soft_builtin_error(ec, cxt,
                                "The alias '" + name + "' was not found");
      status = 1;
    }
  }

  return status;
}

} // namespace koshka
