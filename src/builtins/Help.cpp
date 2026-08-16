#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../ResolvedCommand.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-dms] [pattern ...]");

HELP_DESCRIPTION_DECL(
    "The help builtin lists the builtins or displays the help for one.");

FLAG(SHORT, Bool, 'd', "\0",
     "Display a short description instead of the full help.");
FLAG(MANPAGE, Bool, 'm', "\0", "Accepted without effect.");
FLAG(SUMMARY, Bool, 's', "\0", "Accepted without effect.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Help);

namespace koshka {

Help::Help() = default;

pure fn Help::kind() const wontthrow -> Builtin::Kind { return Kind::Help; }

fn Help::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (args.count() <= 1) {
    let out = String{cxt.scratch_allocator()};
    for (let const &name : builtin_names()) {
      out.append(name.view());
      out += '\n';
    }
    ec.print_to_stdout(out);
    return 0;
  }

  i32 status = 0;
  for (usize i = 1; i < args.count(); i++) {
    let const &name = args[i];
    let const resolved = search_builtin(name.view());
    if (!resolved.has_value()) {
      report_soft_builtin_error(
          ec, cxt,
          i < operand_locations.count() ? operand_locations[i]
                                        : ec.source_location(),
          StringView{"'"} + name + "' is not a shell builtin",
          "Run `help` with no operand to list every builtin");
      status = 1;
      continue;
    }

    let forwarded = ArrayList<String>{heap_allocator()};
    forwarded.push(String{heap_allocator(), name.view()});
    forwarded.push(String{"--help"});
    let forwarded_locations = ArrayList<SourceLocation>{heap_allocator()};
    let sub = ExecContext::from_resolved(
        ec.source_location(), ResolvedCommand::from_builtin(*resolved),
        steal(forwarded), steal(forwarded_locations));
    execute_builtin(steal(sub), cxt);
  }

  return status;
}

} /* namespace koshka */
