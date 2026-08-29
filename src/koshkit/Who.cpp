#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[am i]");

HELP_DESCRIPTION_DECL("The who utility writes logged-in users.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Who);

namespace koshka::koshkit {

Who::Who() = default;

pure fn Who::kind() const wontthrow -> Utility::Kind { return Kind::Who; }

fn Who::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty() &&
      (operands.count() != 2 || operands[0].view() != "am" ||
       operands[1].view() != "i"))
    return report_usage_error(ec, cxt, args[0].view());
  if (operands.is_empty()) {
    let const sessions = os::logged_in_users();
    let output = String{cxt.scratch_allocator()};

    for (let const &session : sessions) {
      output += session.user.view();
      output += ' ';
      output += session.terminal.view();
      if (session.login_time != 0) {
        output += ' ';
        output +=
            utils::format_unix_timestamp(session.login_time, "%b %e %H:%M");
      }
      output += '\n';
    }

    ec.print_to_stdout(output);
    return 0;
  }

  let const user = os::get_current_user();
  let const terminal = os::terminal_name(ec.in_fd.value_or(KOSH_STDIN));
  if (!user.has_value() || !terminal.has_value()) return 1;
  ec.print_to_stdout(user->view());
  ec.print_to_stdout(" ");
  ec.print_to_stdout(terminal->view());
  ec.print_to_stdout("\n");
  return 0;
}

} // namespace koshka::koshkit
