/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the tabs utility in koshkit.
 * The tabs utility sets terminal tab stops.
 */

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
  static constexpr static_string_entry<StringView> TEMPLATE_ENTRIES[] = {
      {SSK("-a"),  "1,10,16,36,72"                                   },
      {SSK("-a2"), "1,10,16,40,72"                                   },
      {SSK("-c"),  "1,8,12,16,20,55"                                 },
      {SSK("-c2"), "1,6,10,14,49"                                    },
      {SSK("-c3"), "1,6,10,14,18,22,26,30,34,38,42,46,50,54,58,62,67"},
      {SSK("-f"),  "1,7,11,15,19,23"                                 },
      {SSK("-p"),  "1,5,9,13,17,21,25,29,33,37,41,45,49,53,57,61"    },
      {SSK("-s"),  "1,10,55"                                         },
      {SSK("-u"),  "1,12,20,44"                                      },
  };
  static constexpr StaticStringMap TEMPLATES{TEMPLATE_ENTRIES};
  let stops = ArrayList<u64>{cxt.scratch_allocator()};
  let specification = args.count() == 2 ? args[1].view() : StringView{"-8"};
  if (let const canned = TEMPLATES.find(specification); canned.has_value())
    specification = *canned;
  if (specification.length > 1 && specification[0] == '-' &&
      specification[1] >= '0' && specification[1] <= '9')
  {
    let const parsed = utils::parse_decimal_u64(specification.substring(1));
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > 160)
      throw Error{"tabs: invalid tab interval"};
    for (u64 stop = parsed.value() + 1; stop <= 160; stop += parsed.value())
      stops.push(stop);
  } else {
    usize position = 0;
    u64 previous_stop = 0;
    while (position <= specification.length) {
      let const remaining = specification.substring(position);
      let const item_length =
          remaining.find_character(',').value_or(remaining.length);
      let item = remaining.substring_of_length(0, item_length);
      let const is_relative = !item.is_empty() && item[0] == '+';
      if (is_relative) item = item.substring(1);
      let const parsed = utils::parse_decimal_u64(item);
      if (parsed.is_error() || parsed.value() == 0)
        throw Error{"tabs: invalid tab stop"};
      let const stop =
          is_relative ? previous_stop + parsed.value() : parsed.value();
      if (stop <= previous_stop || stop > 160)
        throw Error{"tabs: tab stops must increase through column 160"};
      stops.push(stop);
      previous_stop = stop;
      if (item_length == remaining.length) break;
      position += item_length + 1;
    }
  }
  let output = String{cxt.scratch_allocator(), "\r\033[3g"};
  u64 column = 1;
  for (let const stop : stops) {
    output.append_repeated(' ', static_cast<usize>(stop - column));
    output += "\033H";
    column = stop;
  }
  output += '\r';
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
