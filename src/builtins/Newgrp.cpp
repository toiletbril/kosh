#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[group]");
HELP_DESCRIPTION_DECL("The newgrp builtin changes the group of the shell.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Newgrp);

namespace koshka {

Newgrp::Newgrp() = default;

pure fn Newgrp::kind() const wontthrow -> Builtin::Kind { return Kind::Newgrp; }

fn Newgrp::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  /* The group change must outlive this command, so it re-execs system newgrp.
   */
  let const found = cxt.get_program_resolver().search(
      "newgrp", ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (found.count() == 0) {
    report_soft_builtin_error(
        ec, cxt, "Unable to run newgrp because its program was not found");
    return 127;
  }

  let command_args = ArrayList<String>{heap_allocator()};
  command_args.push_managed("newgrp");
  let command_arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  command_arg_locations.push(ec.source_location());
  for (usize i = 1; i < args.count(); i++) {
    command_args.push_managed(args[i]);
    command_arg_locations.push(ec.arg_location_at(i));
  }

  let command = ExecContext::from_resolved(
      ec.source_location(), ResolvedCommand::from_program(found[0]),
      steal(command_args), steal(command_arg_locations));

  LOG(Info, "newgrp handing the shell off to '%s'", found[0].text().c_str());

  try {
    unused(cxt.materialize_kosh_identity());
    os::replace_process(steal(command));
  } catch (const ErrorBase &error) {
    report_soft_builtin_error(ec, cxt, error.message());
    return 126;
  }

  report_soft_builtin_error(
      ec, cxt, "the file is not an executable and has no interpreter");
  return 126;
}

} // namespace koshka
