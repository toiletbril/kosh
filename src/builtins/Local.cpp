#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-aAilnrux] name[=value] ...");
HELP_DESCRIPTION_DECL(
    "The local builtin declares each named variable local to the current "
    "function.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
/* The attribute letters are hand-parsed in execute, so these FLAG rows only
   feed the help text and never the parser. */
FLAG(LOCAL_INDEXED, Bool, 'a', "", "Declare an indexed array.");
FLAG(LOCAL_ASSOCIATIVE, Bool, 'A', "", "Declare an associative array.");
FLAG(LOCAL_INTEGER, Bool, 'i', "",
     "Mark an integer whose every assignment evaluates as arithmetic.");
FLAG(LOCAL_LOWERCASE, Bool, 'l', "", "Accepted without effect.");
FLAG(LOCAL_NAMEREF, Bool, 'n', "", "Accepted without effect.");
FLAG(LOCAL_READONLY, Bool, 'r', "", "Accepted without effect.");
FLAG(LOCAL_UPPERCASE, Bool, 'u', "", "Accepted without effect.");
FLAG(LOCAL_EXPORT, Bool, 'x', "",
     "Export the local into the environment, visible to a child process.");

REGISTER_BUILTIN_FLAGS(Local);

namespace koshka {

Local::Local() = default;

pure fn Local::kind() const wontthrow -> Builtin::Kind { return Kind::Local; }

fn Local::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  if (!cxt.in_function_scope())
    throw ErrorWithDetails{
        "Unable to declare a local variable outside a function",
        "`local` only works inside a function body"};

  bool should_make_indexed = false;
  bool should_make_associative = false;
  bool should_mark_integer = false;
  bool should_mark_export = false;
  usize first_name = 1;
  for (; first_name < args.count(); first_name++) {
    let const arg = args[first_name].view();
    if (arg.length < 1 || arg[0] != '-') {
      break;
    }
    if (arg == "--") {
      first_name++;
      break;
    }
    for (usize c = 1; c < arg.length; c++) {
      switch (arg[c]) {
      case 'a': should_make_indexed = true; break;
      case 'A': should_make_associative = true; break;
      case 'i': should_mark_integer = true; break;
      case 'x': should_mark_export = true; break;
      case 'r':
      case 'l':
      case 'u':
      case 'n': break;
      default: {
        let invalid = String{heap_allocator()};
        invalid += arg[c];
        let bad_option = make_error_for_arg(
            ec, first_name, "'-" + invalid + "' is not a valid local option");
        bad_option.set_command_status(2);
        throw bad_option;
      }
      }
    }
  }

  for (usize i = first_name; i < args.count(); i++) {
    let const &arg = args[i];
    let const equals_position = arg.find_character('=');

    /* A bare name declares the local without touching the value, so the
       caller's binding stays readable until the body assigns it, matching
       dash. */
    let name = equals_position.has_value()
                   ? arg.substring_of_length(0, *equals_position)
                   : arg.view();
    /* process_args passes a local append through as name+=value, so a trailing
       plus on the name marks the append and is stripped before the binding. */
    let const is_append = !name.is_empty() && name[name.count() - 1] == '+';
    if (is_append) name = name.substring_of_length(0, name.count() - 1);

    /* The append reads the name's own value only when it is already local in
       this scope, so a first local += starts from empty the way bash localizes
       it fresh. */
    let const was_already_local =
        is_append && cxt.is_local_in_current_scope(name);
    LOG(All, "local declaring '%.*s' in the function scope",
        static_cast<int>(name.length), name.data);
    cxt.declare_local(name);
    if (should_mark_integer) cxt.mark_integer(name);

    if (should_make_associative) {
      cxt.declare_associative_array(name);
    } else if (should_make_indexed) {
      if (cxt.lookup_indexed_array(name) == nullptr)
        cxt.set_indexed_array(name, ArrayList<String>{heap_allocator()});
    } else if (equals_position.has_value()) {
      let const value = arg.substring(*equals_position + 1);
      if (is_append) {
        let appended = String{cxt.scratch_allocator()};
        if (was_already_local)
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

    if (should_mark_export && !should_make_indexed && !should_make_associative)
    {
      cxt.mark_exported(name);
      cxt.set_shell_variable(name, cxt.get_variable_value(name)
                                       .value_or(String{heap_allocator()})
                                       .view());
    }
  }

  return 0;
}

} // namespace koshka
