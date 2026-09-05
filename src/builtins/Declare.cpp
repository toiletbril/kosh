/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements variable and function declaration, attribute changes,
 * array creation, reusable declaration output, and function-name queries for
 * the declare builtin.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-aAfFgilnprtux] [+i] [name[=value] ...]");

HELP_DESCRIPTION_DECL(
    "The declare builtin declares variables and sets their attributes.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
/* The FLAG rows feed only the help text. The attribute letters are hand-parsed
   in execute. */
FLAG(DECLARE_INDEXED, Bool, 'a', "", "Declare an indexed array.");
FLAG(DECLARE_ASSOCIATIVE, Bool, 'A', "", "Declare an associative array.");
FLAG(DECLARE_FUNCTIONS, Bool, 'f', "",
     "Restrict to functions and print their recorded definitions.");
FLAG(DECLARE_FUNCTION_NAMES, Bool, 'F', "",
     "Print only the names of defined functions and report their existence "
     "through the exit status.");
FLAG(DECLARE_GLOBAL, Bool, 'g', "", "Accepted without effect.");
FLAG(DECLARE_INTEGER, Bool, 'i', "",
     "Mark an integer whose every assignment evaluates as arithmetic. The +i "
     "form removes the mark.");
FLAG(DECLARE_LOWERCASE, Bool, 'l', "",
     "Convert every assigned value to lowercase. The +l form removes the "
     "attribute.");
FLAG(DECLARE_NAMEREF, Bool, 'n', "", "Accepted without effect.");
FLAG(DECLARE_PRINT, Bool, 'p', "", "Print the matching declarations.");
FLAG(DECLARE_READONLY, Bool, 'r', "", "Accepted without effect.");
FLAG(DECLARE_TRACE, Bool, 't', "", "Accepted without effect.");
FLAG(DECLARE_UPPERCASE, Bool, 'u', "",
     "Convert every assigned value to uppercase. The +u form removes the "
     "attribute.");
FLAG(DECLARE_EXPORT, Bool, 'x', "", "Mark the variable for the environment.");

REGISTER_BUILTIN_FLAGS(Declare);

namespace koshka {

Declare::Declare() = default;

pure fn Declare::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Declare;
}

fn Declare::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  let should_make_associative = false;
  let should_make_indexed = false;
  let should_export = false;
  let should_print = false;
  let should_mark_integer_attribute = false;
  let should_unmark_integer_attribute = false;
  let should_mark_lowercase_attribute = false;
  let should_unmark_lowercase_attribute = false;
  let should_mark_uppercase_attribute = false;
  let should_unmark_uppercase_attribute = false;
  let should_mark_readonly = false;
  let should_restrict_to_functions = false;
  let should_print_function_names_only = false;
  let should_be_global = false;

  usize i = 1;
  i32 status = 0;
  for (; i < args.count(); i++) {
    let const arg = args[i].view();
    if (arg.length < 1 || (arg[0] != '-' && arg[0] != '+')) {
      break;
    }
    if (arg == "--") {
      i++;
      break;
    }
    let const is_remove_form = arg[0] == '+';
    for (usize c = 1; c < arg.length; c++) {
      switch (arg[c]) {
      case 'A': should_make_associative = true; break;
      case 'a': should_make_indexed = true; break;
      case 'x': should_export = true; break;
      case 'p': should_print = true; break;
      case 'i':
        if (is_remove_form)
          should_unmark_integer_attribute = true;
        else
          should_mark_integer_attribute = true;
        break;
      case 'l':
        if (is_remove_form)
          should_unmark_lowercase_attribute = true;
        else
          should_mark_lowercase_attribute = true;
        break;
      case 'u':
        if (is_remove_form)
          should_unmark_uppercase_attribute = true;
        else
          should_mark_uppercase_attribute = true;
        break;
      case 'f': should_restrict_to_functions = true; break;
      case 'F':
        should_restrict_to_functions = true;
        should_print_function_names_only = true;
        break;
      case 'r':
        /* bash refuses to drop the read-only attribute, so the +r form is left
           as a silent no-op rather than unmarking. */
        if (!is_remove_form) should_mark_readonly = true;
        break;
      case 'g': should_be_global = true; break;
      /* The remaining attribute letters carry no backing behavior yet and are
         accepted so a script that sets them keeps running. */
      case 'n':
      case 't': break;
      default: {
        let invalid = String{cxt.scratch_allocator()};
        invalid += arg[0];
        invalid += arg[c];
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                  "'" + invalid +
                                      "' is not a valid declare option");
        return 2;
      }
      }
    }
  }

  if (should_make_associative && should_make_indexed) {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "'-a' and '-A' cannot be used together");
    return 2;
  }

  if (should_mark_lowercase_attribute && should_mark_uppercase_attribute) {
    should_mark_lowercase_attribute = false;
    should_mark_uppercase_attribute = false;
    should_unmark_lowercase_attribute = true;
    should_unmark_uppercase_attribute = true;
  }

  /* A missing name turns the status to 1, silently for -F the way bash answers
     an existence probe, with a message for -f. */
  if (should_restrict_to_functions) {
    i32 status = 0;
    if (i >= args.count()) {
      for (let const &name : cxt.sorted_function_names()) {
        let line = String{cxt.scratch_allocator()};
        if (should_print_function_names_only) {
          line += "declare -f ";
          line.append(name.view());
        } else if (const String *source = cxt.find_function_source(name.view());
                   source != nullptr)
        {
          line.append(source->view());
        }
        line += '\n';
        ec.print_to_stdout(line.view());
      }
      return 0;
    }
    for (; i < args.count(); i++) {
      let const name = args[i].view();
      if (cxt.find_function(name) == nullptr) {
        if (!should_print_function_names_only)
          report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                    StringView{"'"} + name +
                                        "' is not a function");
        status = 1;
        continue;
      }
      if (should_print_function_names_only) {
        let line = String{cxt.scratch_allocator(), name};
        line += '\n';
        ec.print_to_stdout(line.view());
      } else if (const String *source = cxt.find_function_source(name);
                 source != nullptr)
      {
        if (!source->is_empty()) {
          let line = String{cxt.scratch_allocator(), source->view()};
          line += '\n';
          ec.print_to_stdout(line.view());
        }
      }
    }
    return status;
  }

  let const do_print_declaration = [&](StringView name) throws -> bool {
    if (const ArrayList<String> *elements = cxt.lookup_indexed_array(name);
        elements != nullptr)
    {
      let line = String{cxt.scratch_allocator(), "declare -a"};
      if (cxt.is_integer_variable(name)) line += 'i';
      if (cxt.is_lowercase_variable(name)) line += 'l';
      if (cxt.is_readonly(name)) line += 'r';
      if (cxt.is_uppercase_variable(name)) line += 'u';
      line += ' ';
      line.append(name);
      line += "=(";
      for (usize e = 0; e < elements->count(); e++) {
        if (e > 0) line += ' ';
        line += '[';
        char index_text[24];
        line.append(utils::int_to_text_into(static_cast<i64>(e), index_text,
                                            sizeof(index_text)));
        line += "]=\"";
        line += quote_for_declare((*elements)[e].view());
        line += '"';
      }
      line += ")\n";
      ec.print_to_stdout(line.view());
      return true;
    }

    if (cxt.is_associative_array(name)) {
      let const keys = cxt.associative_keys(name);
      let const values = cxt.associative_values(name);
      let line = String{cxt.scratch_allocator(), "declare -A"};
      if (cxt.is_integer_variable(name)) line += 'i';
      if (cxt.is_lowercase_variable(name)) line += 'l';
      if (cxt.is_readonly(name)) line += 'r';
      if (cxt.is_uppercase_variable(name)) line += 'u';
      line += ' ';
      line.append(name);
      line += "=(";
      for (usize e = 0; e < keys.count(); e++) {
        line += '[';
        line.append(keys[e].view());
        line += "]=\"";
        if (e < values.count()) line += quote_for_declare(values[e].view());
        line += "\" ";
      }
      line += ")\n";
      ec.print_to_stdout(line.view());
      return true;
    }

    if (const Maybe<String> value = cxt.get_variable_value(name)) {
      let attribute = String{cxt.scratch_allocator(), "-"};
      if (cxt.is_integer_variable(name)) attribute += 'i';
      if (cxt.is_lowercase_variable(name)) attribute += 'l';
      if (cxt.is_readonly(name)) attribute += 'r';
      if (cxt.is_uppercase_variable(name)) attribute += 'u';
      if (os::get_environment_variable(name).has_value()) attribute += 'x';
      if (attribute.count() == 1) attribute += '-';
      let line = String{cxt.scratch_allocator(), "declare "};
      line.append(attribute.view());
      line += ' ';
      line.append(name);
      line += "=\"";
      line += quote_for_declare(value->view());
      line += "\"\n";
      ec.print_to_stdout(line.view());
      return true;
    }

    if (cxt.is_integer_variable(name) || cxt.is_lowercase_variable(name) ||
        cxt.is_uppercase_variable(name))
    {
      let line = String{cxt.scratch_allocator(), "declare -"};
      if (cxt.is_integer_variable(name)) line += 'i';
      if (cxt.is_lowercase_variable(name)) line += 'l';
      if (cxt.is_readonly(name)) line += 'r';
      if (cxt.is_uppercase_variable(name)) line += 'u';
      if (cxt.is_exported(name)) line += 'x';
      line += ' ';
      line.append(name);
      line += '\n';
      ec.print_to_stdout(line.view());
      return true;
    }

    if (cxt.is_exported(name)) {
      let line = String{cxt.scratch_allocator(), "declare -x "};
      line.append(name);
      line += '\n';
      ec.print_to_stdout(line.view());
      return true;
    }

    return false;
  };

  if (should_print && i >= args.count()) {
    let const do_matches_attribute_filter = [&](StringView name) -> bool {
      if (should_export && !os::get_environment_variable(name).has_value()) {
        return false;
      }
      if (should_mark_integer_attribute && !cxt.is_integer_variable(name)) {
        return false;
      }
      if (should_mark_lowercase_attribute && !cxt.is_lowercase_variable(name)) {
        return false;
      }
      if (should_mark_uppercase_attribute && !cxt.is_uppercase_variable(name)) {
        return false;
      }
      if (should_make_indexed && cxt.lookup_indexed_array(name) == nullptr) {
        return false;
      }
      if (should_make_associative && !cxt.is_associative_array(name)) {
        return false;
      }
      if (should_mark_readonly && !cxt.is_readonly(name)) {
        return false;
      }
      return true;
    };

    let names = cxt.variable_names(cxt.scratch_allocator());
    for (let const &environment_name : os::environment_names())
      names.add(environment_name.view());

    let sorted_names = ArrayList<String>{cxt.scratch_allocator()};
    names.for_each([&](StringView name) { sorted_names.push_managed(name); });
    sorted_names.sort();

    for (let const &name : sorted_names) {
      if (do_matches_attribute_filter(name.view()))
        do_print_declaration(name.view());
    }

    return 0;
  }

  if (should_print) {
    i32 status = 0;
    for (; i < args.count(); i++) {
      let const name = args[i].view();
      if (!do_print_declaration(name)) {
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                  StringView{"'"} + name + "' is not defined");
        status = 1;
      }
    }
    return status;
  }

  for (; i < args.count(); i++) {
    let const operand = args[i].view();
    let const equals = operand.find_character('=');
    let name =
        equals.has_value() ? operand.substring_of_length(0, *equals) : operand;
    let const value =
        equals.has_value() ? operand.substring(*equals + 1) : StringView{};

    /* process_args passes a declare append through as name+=value, so a
       trailing plus on the name marks the append and is stripped before the
       attributes apply. */
    let const is_append =
        equals.has_value() && !name.is_empty() && name[name.count() - 1] == '+';
    if (is_append) name = name.substring_of_length(0, name.count() - 1);

    /* A subscripted operand such as a[0]=5 declares the base name's array and
       assigns the element, the way bash treats declare a[i]=v, so the
       attribute marks land on the base name rather than the bracketed text. */
    let const bracket = name.find_character('[');
    let const has_subscript =
        bracket.has_value() && name[name.count() - 1] == ']';
    let const subscript =
        has_subscript ? name.substring_of_length(*bracket + 1,
                                                 name.count() - *bracket - 2)
                      : StringView{};
    if (has_subscript) name = name.substring_of_length(0, *bracket);

    if (!name_is_valid_identifier(name)) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                StringView{"'"} + operand +
                                    "' is not a valid identifier");
      status = 1;
      continue;
    }

    if ((should_mark_integer_attribute || should_unmark_integer_attribute ||
         should_mark_lowercase_attribute || should_unmark_lowercase_attribute ||
         should_mark_uppercase_attribute ||
         should_unmark_uppercase_attribute) &&
        cxt.is_readonly(name))
    {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                StringView{"'"} + name + "' is read-only");
      status = 1;
      continue;
    }

    if (should_make_associative && cxt.lookup_indexed_array(name) != nullptr) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                StringView{"Unable to convert '"} + name +
                                    "' from an indexed array to an "
                                    "associative array");
      status = 1;
      continue;
    }
    if (should_make_indexed && cxt.is_associative_array(name)) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                StringView{"Unable to convert '"} + name +
                                    "' from an associative array to an "
                                    "indexed array");
      status = 1;
      continue;
    }

    if (!should_be_global)
      cxt.declare_local(
          name, !cxt.is_bash_compatible() ||
                    cxt.is_shopt_enabled(shopt_option_id::LocalvarInherit));

    /* The attribute applies before the assignment, so declare -i x+=3 already
       adds on this command the way bash applies the integer mark first. */
    if (should_mark_integer_attribute) cxt.mark_integer(name);
    if (should_unmark_integer_attribute) cxt.unmark_integer(name);
    if (should_unmark_lowercase_attribute) cxt.unmark_lowercase(name);
    if (should_unmark_uppercase_attribute) cxt.unmark_uppercase(name);
    if (should_mark_lowercase_attribute) cxt.mark_lowercase(name);
    if (should_mark_uppercase_attribute) cxt.mark_uppercase(name);

    LOG(All, "declare applying attributes to '%.*s'",
        static_cast<int>(name.length), name.data);

    if (has_subscript && equals.has_value()) {
      cxt.assign_array_element(name, subscript, value, is_append);
    } else if (should_make_associative) {
      LOG(All, "declare making '%.*s' an associative array",
          static_cast<int>(name.length), name.data);
      if (equals.has_value()) cxt.set_shell_variable(name, value);
      cxt.declare_associative_array(name);
    } else if (should_make_indexed) {
      if (cxt.lookup_indexed_array(name) == nullptr) {
        let values = ArrayList<String>{heap_allocator()};
        if (equals.has_value())
          values.push(String{heap_allocator(), value});
        else if (let const *scalar = cxt.lookup_shell_variable(name);
                 scalar != nullptr)
          values.push(String{heap_allocator(), scalar->view()});
        cxt.set_indexed_array(name, steal(values));
      }
    } else if (equals.has_value()) {
      if (is_append) {
        /* An integer name joins the appended expression as arithmetic. */
        let appended = String{cxt.scratch_allocator()};
        if (let const existing = cxt.get_variable_value(name))
          appended.append(existing->view());
        if (cxt.is_integer_variable(name))
          cxt.append_integer_expression(appended, value);
        else
          appended.append(value);
        cxt.set_shell_variable(name, appended.view());
      } else {
        cxt.set_shell_variable(name, value);
      }
    }

    if (should_export && !has_subscript && !should_make_associative &&
        !should_make_indexed)
    {
      LOG(All, "declare exporting '%.*s' to the environment",
          static_cast<int>(name.length), name.data);
      cxt.record_environment_change(name);
      cxt.mark_exported(name);
      if (let const stored = cxt.get_variable_value(name))
        os::set_environment_variable(name, stored->view());
    }

    /* The read-only mark applies after the assignment, so declare -r v=1 stores
       the value first and then locks it, the way bash rejects only a later
       write rather than the declaration's own assignment. */
    if (should_mark_readonly) cxt.mark_readonly(name);
  }

  return status;
}

} /* namespace koshka */
