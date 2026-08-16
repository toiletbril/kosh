#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[n]");

HELP_DESCRIPTION_DECL(
    "The return builtin stops the current function or sourced file and returns "
    "a status.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Return);

namespace koshka {

Return::Return() = default;

pure fn Return::kind() const wontthrow -> Builtin::Kind { return Kind::Return; }

fn Return::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  /* The default mood rejects a return outside a function or a sourced file,
     while the sh mood lets it end the script the way dash does. */
  if (!cxt.is_posix_mode() && !cxt.in_function_scope() && !cxt.is_sourcing()) {
    report_soft_builtin_error(
        ec, cxt, "can only `return' from a function or sourced script");
    return 2;
  }

  i64 status = cxt.last_exit_status();

  if (ec.args().count() > 1) {
    let const parsed_value = ec.args()[1].to<i64>();

    if (parsed_value.is_error()) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                ec.args()[1] + ": numeric argument required");
      cxt.request_return(2, ec.source_location());
      return 2;
    }

    if (ec.args().count() > 2) {
      report_soft_builtin_error(
          ec, cxt, ec.arg_location_at(2), "too many arguments",
          "return takes at most one status, e.g. `return 1`");

      if (cxt.in_subshell()) {
        cxt.request_exit(1, ec.source_location());
        return 1;
      }

      if (!cxt.shell_is_interactive()) {
        cxt.run_exit_trap();
        utils::quit(1, utils::farewell_policy::Goodbye);
      }

      return 1;
    }

    status = parsed_value.value();
  }

  status &= 0xFF;

  LOG(Debug, "return stopping the enclosing scope with status %lld",
      static_cast<long long>(status));
  cxt.request_return(status, ec.source_location());
  return static_cast<i32>(status);
}

} /* namespace koshka */
