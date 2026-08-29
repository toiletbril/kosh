#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ceisu] [-n lines] [file ...]");

HELP_DESCRIPTION_DECL("The more utility displays text one screen at a time.");

FLAG(MORE_CLEAR, Bool, 'c', "clear", "Clear each screen before displaying it.");
FLAG(MORE_EXIT, Bool, 'e', "exit", "Exit at end of input.");
FLAG(MORE_CASE, Bool, 'i', "ignore-case", "Ignore case in searches.");
FLAG(MORE_SQUEEZE, Bool, 's', "squeeze", "Collapse repeated blank lines.");
FLAG(MORE_PLAIN, Bool, 'u', "plain", "Suppress terminal underline handling.");
FLAG(MORE_LINES, String, 'n', "lines", "Use this many lines per screen.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(More);

namespace koshka::koshkit {

More::More() = default;

pure fn More::kind() const wontthrow -> Utility::Kind { return Kind::More; }

fn More::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  i32 status = 0;
  for (let const source : sources) {
    let const input = open_named_or_stdin(ec, source);
    if (!input.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "more: cannot read '" + String{source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }
    defer
    {
      if (input->should_close) os::close_fd(input->descriptor);
    };
    char buffer[65536];
    loop
    {
      let const count = os::read_fd(input->descriptor, buffer, sizeof(buffer));
      if (!count.has_value()) {
        status = 1;
        break;
      }
      if (*count == 0) break;
      ec.print_to_stdout(StringView{buffer, *count});
    }
  }
  return status;
}

} // namespace koshka::koshkit
