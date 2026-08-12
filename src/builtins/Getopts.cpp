#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("optstring name [argument ...]");

HELP_DESCRIPTION_DECL(
    "The getopts builtin parses the option arguments against optstring.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Getopts);

namespace koshka {

Getopts::Getopts() = default;

pure fn Getopts::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Getopts;
}

fn Getopts::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  if (args.count() < 3) return report_usage_error(ec, cxt, ec.program());

  let const &optstring = args[1];
  let const &name = args[2];
  let const is_silent = !optstring.is_empty() && optstring[0] == ':';

  bool should_print_diagnostic = !is_silent;
  if (let value = cxt.get_variable_value("OPTERR"); value.has_value()) {
    let const parsed_value = (*value).to<i64>();
    if (!parsed_value.is_error() && parsed_value.value() == 0) {
      should_print_diagnostic = false;
    }
  }

  let explicit_operands = ArrayList<String>{cxt.scratch_allocator()};
  if (args.count() > 3) {
    for (usize i = 3; i < args.count(); i++)
      explicit_operands.push(args[i]);
  }
  /* The common while-getopts form names no operands, so the positional
     parameters are read in place with no per-call copy. */
  const ArrayList<String> &operands =
      args.count() > 3 ? explicit_operands : cxt.positional_params();

  i64 optind = 1;
  if (let value = cxt.get_variable_value("OPTIND"); value.has_value()) {
    let const parsed_value = (*value).to<i64>();
    optind = parsed_value.is_error() ? 1 : parsed_value.value();
  }

  LOG(Debug, "getopts parsing into '%s' at OPTIND %lld of %zu operands",
      name.c_str(), static_cast<long long>(optind), operands.count());

  /* A script that resets OPTIND starts a fresh scan at the first letter. */
  if (optind != cxt.getopts_last_optind()) cxt.set_getopts_char_index(1);
  let char_index = cxt.getopts_char_index();

  let const do_finish = [&](i32 code) -> i32 {
    cxt.set_shell_variable("OPTIND",
                           String::from(optind, cxt.scratch_allocator()));
    cxt.set_getopts_char_index(char_index);
    cxt.set_getopts_last_optind(optind);
    return code;
  };

  if (optind < 1 || static_cast<usize>(optind) > operands.count()) {
    cxt.set_shell_variable(name, "?");
    return do_finish(1);
  }

  ASSERT(static_cast<usize>(optind) - 1 < operands.count());
  let const &current = operands[static_cast<usize>(optind) - 1];
  if (current.length() < 2 || current[0] != '-') {
    cxt.set_shell_variable(name, "?");
    return do_finish(1);
  }
  if (current == "--") {
    optind++;
    cxt.set_shell_variable(name, "?");
    return do_finish(1);
  }

  /* An index at or past the operand length reads a fresh operand at the first
     letter, guarding against a stale index reading past the end. */
  if (char_index >= current.length()) char_index = 1;
  ASSERT(char_index < current.length());
  let const option = current[char_index];
  let option_as_string = String{cxt.scratch_allocator()};
  option_as_string.push(option);
  let const spec = optstring.find_character(option);

  let const do_advance_letter = [&]() {
    char_index++;
    if (char_index >= current.length()) {
      optind++;
      char_index = 1;
    }
  };

  if (option == ':' || !spec.has_value()) {
    do_advance_letter();
    cxt.set_shell_variable(name, "?");
    if (is_silent) {
      cxt.set_shell_variable("OPTARG", option_as_string);
    } else {
      cxt.unset_shell_variable("OPTARG");
      if (should_print_diagnostic) {
        let const operand_arg_index =
            args.count() > 3 ? Maybe<usize>{static_cast<usize>(optind) + 2}
                             : Maybe<usize>{None};
        if (operand_arg_index.has_value()) {
          report_soft_builtin_error(
              ec, cxt, ec.arg_location_at(*operand_arg_index),
              StringView{"Illegal option -- "} + option_as_string);
        } else {
          report_soft_builtin_error(
              ec, cxt, StringView{"Illegal option -- "} + option_as_string);
        }
      }
    }
    return do_finish(0);
  }

  let const should_take_argument =
      *spec + 1 < optstring.length() && optstring[*spec + 1] == ':';
  if (should_take_argument) {
    if (char_index + 1 < current.length()) {
      let const optarg = current.substring(char_index + 1);
      cxt.set_shell_variable("OPTARG", optarg);
      optind++;
      char_index = 1;
    } else if (static_cast<usize>(optind) < operands.count()) {
      let const &optarg = operands[static_cast<usize>(optind)];
      cxt.set_shell_variable("OPTARG", optarg);
      optind += 2;
      char_index = 1;
    } else {
      optind++;
      char_index = 1;
      if (is_silent) {
        cxt.set_shell_variable(name, ":");
        cxt.set_shell_variable("OPTARG", option_as_string);
      } else {
        cxt.set_shell_variable(name, "?");
        cxt.unset_shell_variable("OPTARG");
        if (should_print_diagnostic) {
          let const operand_arg_index =
              args.count() > 3 ? Maybe<usize>{static_cast<usize>(optind) + 2}
                               : Maybe<usize>{None};
          if (operand_arg_index.has_value()) {
            report_soft_builtin_error(
                ec, cxt, ec.arg_location_at(*operand_arg_index),
                StringView{"Option requires an argument -- "} +
                    option_as_string);
          } else {
            report_soft_builtin_error(
                ec, cxt,
                StringView{"Option requires an argument -- "} +
                    option_as_string);
          }
        }
      }
      return do_finish(0);
    }
    cxt.set_shell_variable(name, option_as_string);
    return do_finish(0);
  }

  LOG(All, "getopts advancing past option '%s'", option_as_string.c_str());
  do_advance_letter();
  cxt.unset_shell_variable("OPTARG");
  cxt.set_shell_variable(name, option_as_string);
  return do_finish(0);
}

} // namespace koshka
