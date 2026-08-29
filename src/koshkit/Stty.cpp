#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a | -g] [operand ...]");

HELP_DESCRIPTION_DECL(
    "The stty utility reports or changes terminal attributes.");

FLAG(STTY_ALL, Bool, 'a', "all", "Write all current settings.");
FLAG(STTY_ENCODE, Bool, 'g', "save", "Write settings in a reusable form.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Stty);

namespace koshka::koshkit {

Stty::Stty() = default;

pure fn Stty::kind() const wontthrow -> Utility::Kind { return Kind::Stty; }

fn Stty::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  unused(arg_locations);
  defer { reset_flags(FLAG_LIST); };
  let settings = ArrayList<String>{cxt.scratch_allocator()};
  bool should_report = args.count() == 1;
  bool should_encode = false;
  for (usize index = 1; index < args.count(); index++) {
    let const argument = args[index].view();
    if (argument == "--help") {
      print_util_help(ec, args[0].view(), HELP_SYNOPSIS[0], HELP_DESCRIPTION,
                      FLAG_LIST);
      return 0;
    }
    if (argument == "-a") {
      should_report = true;
      continue;
    }
    if (argument == "-g") {
      should_report = true;
      should_encode = true;
      continue;
    }
    settings.push(args[index].clone());
  }
  let const terminal = ec.in_fd.value_or(KOSH_STDIN);
  if (should_report) {
    let const output =
        os::terminal_settings(terminal, should_encode, cxt.scratch_allocator());
    if (!output.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "stty: standard input is not a terminal");
      return 1;
    }
    ec.print_to_stdout(output->view());
  }
  if (!settings.is_empty() && !os::apply_terminal_settings(terminal, settings))
  {
    report_soft_koshkit_error(ec, cxt, "stty: invalid terminal setting");
    return 1;
  }
  return 0;
}

} // namespace koshka::koshkit
