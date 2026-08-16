#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[NAME[=VALUE] ...]");
HELP_DESCRIPTION_DECL(
    "The export builtin moves a variable into the environment.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(EXPORT_PRINT, Bool, 'p', "",
     "List the exported variables in a reusable form.");
FLAG(EXPORT_UNMARK, Bool, 'n', "",
     "Remove the export attribute, keeping the variable in the shell.");
FLAG(EXPORT_FUNCTION, Bool, 'f', "",
     "Export a function into the environment for a child shell.");

REGISTER_BUILTIN_FLAGS(Export);

namespace koshka {

Export::Export() = default;

pure fn Export::kind() const wontthrow -> Builtin::Kind { return Kind::Export; }

fn Export::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  if (FLAG_EXPORT_FUNCTION.is_enabled()) {
    let has_error = false;
    for (usize i = 1; i < args.count(); i++) {
      let const name = String{cxt.scratch_allocator(), args[i].view()};
      let const *source = cxt.find_function_source(name.view());
      if (source == nullptr) {
        let const loc = i < operand_locations.count() ? operand_locations[i]
                                                      : ec.source_location();
        report_soft_builtin_error(
            ec, cxt, loc, StringView{"'"} + name + "' is not a function");
        has_error = true;
        continue;
      }

      let const src = source->view();
      usize body_start = 0;
      if (let const close_paren = src.find_character(')');
          close_paren.has_value())
        body_start = close_paren.value() + 1;
      while (body_start < src.length &&
             (src[body_start] == ' ' || src[body_start] == '\t' ||
              src[body_start] == '\n' || src[body_start] == '\r'))
      {
        body_start++;
      }
      let const body = src.substring(body_start);

      let env_key = String{cxt.scratch_allocator(), "BASH_FUNC_"};
      env_key += name.view();
      env_key += "%%";
      if (FLAG_EXPORT_UNMARK.is_enabled()) {
        cxt.record_environment_change(env_key.view());
        os::unset_environment_variable(env_key);
        continue;
      }

      let value = String{cxt.scratch_allocator(), "() "};
      if (!body.is_empty() && body[0] == '{') {
        value += body;
      } else {
        value += "{ ";
        value += body;
        value += " }";
      }

      cxt.record_environment_change(env_key.view());
      os::set_environment_variable(env_key, value);
    }
    return has_error ? 1 : 0;
  }

  if (FLAG_EXPORT_PRINT.is_enabled() || args.count() == 1) {
    let const is_declare_form = cxt.is_bash_compatible();
    let names = os::environment_names();
    names.sort();

    let out = String{cxt.scratch_allocator()};
    for (let const &name : names) {
      let const value = os::get_environment_variable(name).value_or(
          String{cxt.scratch_allocator()});
      out += is_declare_form ? "declare -x " : "export ";
      out += name;
      if (is_declare_form) {
        out += "=\"";
        out += quote_for_declare(value.view());
        out += "\"";
      } else {
        out += "='";
        for (usize k = 0; k < value.count(); k++) {
          if (value[k] == '\'')
            out += "'\\''";
          else
            out.push(value[k]);
        }
        out += "'";
      }
      out += "\n";
    }
    ec.print_to_stdout(out);
    return 0;
  }

  if (FLAG_EXPORT_UNMARK.is_enabled()) {
    let has_error = false;
    for (usize i = 1; i < args.count(); i++) {
      let const parts = NameValueArg::from(args[i]);
      let const name = String{cxt.scratch_allocator(), parts.get_name()};
      if (!name_is_valid_identifier(name.view())) {
        let const loc = i < operand_locations.count() ? operand_locations[i]
                                                      : ec.source_location();
        report_soft_builtin_error(ec, cxt, loc,
                                  StringView{"'"} + args[i] +
                                      "' is not a valid identifier");
        has_error = true;
        continue;
      }

      if (parts.get_value().has_value())
        cxt.set_shell_variable(name, *parts.get_value());

      LOG(All, "export removing '%s' from the environment", name.c_str());
      cxt.unexport_shell_variable(name);
    }

    return has_error ? 1 : 0;
  }

  let has_error = false;
  for (usize i = 1; i < args.count(); i++) {
    let const &arg = args[i];
    let const parts = NameValueArg::from(arg);

    let name = String{cxt.scratch_allocator()};
    let value = String{cxt.scratch_allocator()};
    let const has_new_value = parts.get_value().has_value();
    if (!has_new_value) {
      name = arg;
      value =
          cxt.get_variable_value(arg).value_or(String{cxt.scratch_allocator()});
    } else {
      name = String{cxt.scratch_allocator(), parts.get_name()};
      value = String{cxt.scratch_allocator(), *parts.get_value()};
    }

    if (!name_is_valid_identifier(name.view())) {
      let const loc = i < operand_locations.count() ? operand_locations[i]
                                                    : ec.source_location();
      report_soft_builtin_error(
          ec, cxt, loc, StringView{"'"} + arg + "' is not a valid identifier");
      has_error = true;
      continue;
    }

    if (cxt.is_readonly(name)) {
      if (has_new_value) {
        let const loc = i < operand_locations.count() ? operand_locations[i]
                                                      : ec.source_location();
        report_soft_builtin_error(ec, cxt, loc,
                                  StringView{"'"} + name + "' is read-only");
        has_error = true;
        continue;
      }
      cxt.record_environment_change(name);
      os::set_environment_variable(name, value);
      cxt.mark_exported(name);
      if (name == "PATH")
        cxt.get_program_resolver().assign_path(String{value.view()});
      continue;
    }

    /* An integer name evaluates its new value as arithmetic, so the environment
       receives the decimal result. */
    let const is_integer_name = cxt.is_integer_variable(name.view());
    if (has_new_value && is_integer_name) {
      char result_text[24];
      value = String{
          cxt.scratch_allocator(),
          utils::int_to_text_into(
              value.is_empty() ? 0 : cxt.evaluate_arithmetic(value.view()),
              result_text, sizeof(result_text))};
    }

    /* The unset here is this move, not a user unset, so the integer mark it
       clears is put back. */
    LOG(All, "export moving '%s' into the environment", name.c_str());
    cxt.unset_shell_variable(name);
    if (is_integer_name) cxt.mark_integer(name.view());
    cxt.record_environment_change(name);
    os::set_environment_variable(name, value);
    cxt.mark_exported(name);
    /* The unset above pointed the resolver at the now-removed environment PATH,
       so an export PATH=... refreshes it to the value just placed in the
       environment. */
    if (name == "PATH")
      cxt.get_program_resolver().assign_path(String{value.view()});
  }

  return has_error ? 1 : 0;
}

} /* namespace koshka */
