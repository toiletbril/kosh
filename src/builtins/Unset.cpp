#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-f] [-v] name ...");

HELP_DESCRIPTION_DECL(
    "The unset builtin removes each named shell variable or function.");

FLAG(UNSET_FUNCTION, Bool, 'f', "", "Remove each named shell function.");
FLAG(UNSET_VARIABLE, Bool, 'v', "", "Remove variables, the default.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Unset);

namespace koshka {

Unset::Unset() = default;

pure fn Unset::kind() const wontthrow -> Builtin::Kind { return Kind::Unset; }

fn Unset::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const names = PARSE_BUILTIN_ARGS(ec);

  ASSERT(!names.is_empty());

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let const should_unset_function = FLAG_UNSET_FUNCTION.is_enabled();
  let has_error = false;
  for (usize i = 1; i < names.count(); i++) {
    let const &name = names[i];
    if (should_unset_function) {
      LOG(All, "unset removing function '%s'", name.c_str());
      cxt.unset_function(name);
    } else if (let const bracket = name.view().find_character('[');
               bracket.has_value() && name.view()[name.count() - 1] == ']')
    {
      const StringView array_name =
          name.view().substring_of_length(0, *bracket);
      const StringView subscript = name.view().substring_of_length(
          *bracket + 1, name.count() - *bracket - 2);
      try {
        cxt.unset_array_element(array_name, subscript);
      } catch (const ErrorWithLocation &) {
        throw;
      } catch (const Error &error) {
        LOG(All, "unset swallowed an array element error: %s",
            error.message().c_str());
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                  error.message().view());
        has_error = true;
      }
    } else if (!FLAG_UNSET_VARIABLE.is_enabled() &&
               cxt.lookup_shell_variable(name.view()) == nullptr &&
               cxt.lookup_indexed_array(name.view()) == nullptr &&
               !cxt.is_associative_array(name.view()) &&
               cxt.find_function(name.view()) != nullptr)
    {
      LOG(All, "unset removing function '%s' since no variable is set",
          name.c_str());
      cxt.unset_function(name);
    } else {
      /* A read-only name throws, the rest are still unset, matching dash. */
      LOG(All, "unset removing variable '%s'", name.c_str());
      try {
        cxt.unset_shell_variable(name);
      } catch (const Error &error) {
        LOG(All, "unset swallowed a read-only variable error: %s",
            error.message().c_str());
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                  error.message().view());
        has_error = true;
      }
    }
  }

  return has_error ? 1 : 0;
}

} // namespace koshka
