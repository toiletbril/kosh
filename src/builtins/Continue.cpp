#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[n]");

HELP_DESCRIPTION_DECL(
    "The continue builtin skips to the next iteration of an enclosing loop.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Continue);

namespace koshka {

Continue::Continue() = default;

pure fn Continue::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Continue;
}

fn Continue::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  if (cxt.loop_depth() == 0) {
    LOG(All, "continue outside a loop does nothing");
    return 0;
  }

  i64 level = 1;
  if (ec.args().count() > 1) {
    let const parsed_level = ec.args()[1].to<i64>();

    if (parsed_level.is_error()) {
      if (!cxt.is_bash_compatible()) throw parsed_level.error();

      LOG(All, "continue rejecting a non-numeric count under bash mood");
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                "'" + ec.args()[1] +
                                    "' is not a valid loop count");

      if (!cxt.shell_is_interactive()) {
        if (cxt.in_subshell()) {
          cxt.request_exit(2, ec.source_location());
          return 2;
        }

        cxt.run_exit_trap();
        utils::quit(2, utils::farewell_policy::Goodbye);
      }

      cxt.request_break(static_cast<i64>(cxt.loop_depth()),
                        ec.source_location());
      return 2;
    }

    level = parsed_level.value();
  }

  if (level < 1) {
    if (!cxt.is_bash_compatible()) {
      throw make_error_for_arg(ec, 1,
                               "Unable to continue because '" + ec.args()[1] +
                                   "' is not a valid loop count");
    }

    LOG(All, "continue abandoning every enclosing loop for a count below one");
    report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                              "'" + ec.args()[1] +
                                  "' is not a valid loop count");
    cxt.request_break(static_cast<i64>(cxt.loop_depth()), ec.source_location());
    return 1;
  }

  LOG(All, "continue skipping to the next iteration of %lld loops",
      static_cast<long long>(level));
  cxt.request_continue(level, ec.source_location());
  return 0;
}

} // namespace koshka
