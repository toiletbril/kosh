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
  let setting_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const settings = parse_util_operands(
      FLAG_LIST, args, &arg_locations, &setting_locations, false, false, true);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const should_report_all = FLAG_STTY_ALL.is_enabled();
  let const should_encode = FLAG_STTY_ENCODE.is_enabled();
  if (should_report_all && should_encode) {
    report_soft_koshkit_util_error(ec, cxt, FLAG_STTY_ENCODE.value_location(),
                                   args[0].view(),
                                   "-a and -g cannot be used together");
    return 1;
  }
  let const should_report =
      settings.is_empty() || should_report_all || should_encode;
  let const terminal = ec.in_fd.value_or(KOSH_STDIN);
  if (should_report) {
    let const output = os::terminal_settings(
        terminal, should_encode, should_report_all, cxt.scratch_allocator());
    if (!output.has_value()) {
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     "standard input is not a terminal");
      return 1;
    }
    ec.print_to_stdout(output->view());
  }
  if (!settings.is_empty()) {
    if (!os::is_fd_a_tty(terminal)) {
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     "standard input is not a terminal");
      return 1;
    }

    let const result = os::apply_terminal_settings(terminal, settings);
    if (result.kind == os::terminal_settings_apply_kind::SystemError) {
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     os::last_system_error_message());
      return 1;
    }
    if (result.kind == os::terminal_settings_apply_kind::InvalidSetting) {
      report_soft_koshkit_util_error(
          ec, cxt, setting_locations[result.setting_position], args[0].view(),
          "invalid terminal setting");
      return 1;
    }
  }
  return 0;
}

} // namespace koshka::koshkit
