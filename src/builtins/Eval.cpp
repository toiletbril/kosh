#include "../Eval.hpp"

#include "../Builtin.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[arg ...]");

HELP_DESCRIPTION_DECL(
    "The eval builtin runs its arguments as a command in the current shell.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Eval);

namespace koshka {

Eval::Eval() = default;

pure fn Eval::kind() const wontthrow -> Builtin::Kind { return Kind::Eval; }

fn Eval::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help")
    SHOW_BUILTIN_HELP_AND_RETURN(ec);

  /* A leading -- ends eval's option scan, matching bash eval -- "$code". */
  usize first = 1;
  if (ec.args().count() > 1) {
    let const &lead = ec.args()[1];
    if (lead == "--") {
      first = 2;
    } else if (lead.length() >= 2 && lead[0] == '-') {
      let invalid_option = String{cxt.scratch_allocator()};
      invalid_option += lead[0];
      invalid_option += lead[1];

      let note = String{cxt.scratch_allocator()};
      note += "Try `";
      note.append(ec.program().view());
      note += " --help` for more info";

      report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                invalid_option + ": invalid option",
                                note.view());
      return 2;
    }
  }

  let joined = String{cxt.scratch_allocator()};
  for (usize i = first; i < ec.args().count(); i++) {
    if (i > first) joined += ' ';
    joined.append(ec.args()[i].view());
  }

  if (joined.is_empty()) return 0;

  LOG(Debug, "eval running %zu joined bytes in the current shell",
      joined.length());

  return cxt.run_source(joined, "eval", return_handling::Propagate,
                        ec.source_location(), StringView{"eval"});
}

} // namespace koshka
