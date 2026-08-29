#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n | tabstop-list]");

HELP_DESCRIPTION_DECL("The tabs utility sets terminal tab stops.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tabs);

namespace koshka::koshkit {

Tabs::Tabs() = default;

pure fn Tabs::kind() const wontthrow -> Utility::Kind { return Kind::Tabs; }

fn Tabs::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  unused(arg_locations);
  if (args.count() > 2) return report_usage_error(ec, cxt, args[0].view());
  if (args.count() == 2 && args[1].view() == "--help") {
    print_util_help(ec, args[0].view(), HELP_SYNOPSIS[0], HELP_DESCRIPTION,
                    FLAG_LIST);
    return 0;
  }
  let interval = u64{8};
  if (args.count() == 2) {
    let value = args[1].view();
    if (value.length > 1 && value[0] == '-') value = value.substring(1);
    let const parsed = utils::parse_decimal_u64(value);
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > 160)
      throw Error{"tabs: invalid tab interval"};
    interval = parsed.value();
  }
  let output = String{cxt.scratch_allocator(), "\r\033[3g"};
  u64 column = 1;
  for (u64 stop = interval + 1; stop <= 160; stop += interval) {
    output.append_repeated(' ', static_cast<usize>(stop - column));
    output += "\033H";
    column = stop;
  }
  output += '\r';
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
