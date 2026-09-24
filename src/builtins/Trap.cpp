/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements trap installation, removal, reusable listing,
 * signal-name normalization, and EXIT, DEBUG, ERR, and RETURN condition
 * handling.
 */

#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"
#include "../base/Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-lp] [--] [action condition ...]");

HELP_DESCRIPTION_DECL(
    "The trap builtin sets the action to run for each named condition.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(TRAP_PRINT, Bool, 'p', "", "Print the set traps in a reusable form.");
FLAG(TRAP_LIST, Bool, 'l', "list", "Print the table of signal names.");

REGISTER_BUILTIN_FLAGS(Trap);

namespace koshka {

Trap::Trap() = default;

pure fn Trap::kind() const wontthrow -> Builtin::Kind { return Kind::Trap; }

namespace {

fn normalize_condition(StringView raw, Allocator allocator) throws -> String
{
  let name = String{allocator};
  for (usize i = 0; i < raw.count(); i++)
    name.push(static_cast<char>(toupper(static_cast<unsigned char>(raw[i]))));
  if (name.starts_with("SIG") && name.count() > 3) {
    name = String{allocator, name.substring(3)};
  }
  if (name == "0") return String{allocator, "EXIT"};

  if (name.view().is_all_decimal_digits()) {
    let const parsed = name.view().to<i64>();
    if (!parsed.is_error()) {
      if (let const signal_name =
              os::signal_name_from_number(static_cast<i32>(parsed.value())))
        return *signal_name;
    }
  }
  return name;
}

/* The sh mood behaves like dash. Dash knows EXIT and the real signals and
   reports DEBUG, ERR, and RETURN as a bad trap. Each of those three is also
   inert there, since every dispatch site holds them behind the mood. */
fn is_valid_trap_condition(StringView condition, mimic_mood mood) throws -> bool
{
  static constexpr PackedStringKey SPECIAL_CONDITION_KEYS[] = {
      SSK("EXIT"),
      SSK("DEBUG"),
      SSK("ERR"),
      SSK("RETURN"),
  };
  static constexpr StaticStringSet SPECIAL_CONDITIONS{SPECIAL_CONDITION_KEYS};

  if (SPECIAL_CONDITIONS.contains(condition))
    return mood != mimic_mood::Posix || condition == "EXIT";

  if (condition.is_all_decimal_digits()) return false;

  return os::signal_number_from_name(condition).has_value();
}

struct listed_trap
{
  i64 order;
  StringView condition;
  StringView action;
};

/* Bash walks EXIT, then every real signal in ascending number order, then
   DEBUG, ERR, and RETURN. No signal number reaches the special base. */
fn trap_listing_order(StringView condition) throws -> i64
{
  static constexpr i64 SPECIAL_CONDITION_BASE = 1000;

  if (condition == "EXIT") return 0;
  if (condition == "DEBUG") return SPECIAL_CONDITION_BASE;
  if (condition == "ERR") return SPECIAL_CONDITION_BASE + 1;
  if (condition == "RETURN") return SPECIAL_CONDITION_BASE + 2;

  if (let const number = os::signal_number_from_name(condition))
    return static_cast<i64>(*number);

  return SPECIAL_CONDITION_BASE + 3;
}

fn format_listed_condition(StringView condition,
                           bool should_include_signal_prefix,
                           Allocator allocator) throws -> String
{
  if (should_include_signal_prefix &&
      os::signal_number_from_name(condition).has_value())
  {
    let prefixed = String{allocator, "SIG"};
    prefixed += condition;
    return prefixed;
  }
  return String{allocator, condition};
}

} /* namespace */

fn Trap::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  /* Dash accepts no trap option at all. A leading operand that opens with a
     dash and carries more than that dash is an illegal option there, and the
     special builtin ends a non-interactive shell with status 2. A lone dash and
     the separator stay operands. */
  if (cxt.is_posix_mode() && args.count() > 1 && args[1].count() > 1 &&
      args[1].starts_with("-") && args[1] != "--")
  {
    let option = String{cxt.scratch_allocator()};
    option += args[1][1];

    let bad_option = make_error_for_arg(
        ec, 1, "'-" + option + "' is not a valid trap option",
        "Use `--` before an operand that begins with a dash");
    bad_option.set_command_status(2);
    throw bad_option;
  }

  /* Bash reads the trap options with getopt. Every option word ahead of the
     first operand is read, a joined cluster is accepted, the list form wins
     over the print form, and `--` ends the options. The sh mood has already
     rejected every option above, and the separator remains an operand marker
     there. */
  usize operand_index = 1;
  let should_print_listing = false;
  let should_list_signals = false;

  while (operand_index < args.count()) {
    let const &word = args[operand_index];

    if (word == "--") {
      operand_index++;
      break;
    }

    if (cxt.is_posix_mode()) break;

    if (word.count() < 2 || !word.starts_with("-")) break;

    if (word == "--list") {
      should_list_signals = true;
      operand_index++;
      continue;
    }

    for (usize i = 1; i < word.count(); i++) {
      if (word[i] == 'l') {
        should_list_signals = true;
        continue;
      }

      if (word[i] == 'p') {
        should_print_listing = true;
        continue;
      }

      let option = String{cxt.scratch_allocator()};
      option += word[i];

      report_soft_builtin_error(
          ec, cxt, ec.arg_location_at(operand_index),
          "'-" + option + "' is not a valid trap option",
          "Use `--` before an operand that begins with a dash");
      return 2;
    }

    operand_index++;
  }

  if (should_list_signals) {
    ec.print_to_stdout(koshkit::format_signal_list());
    return 0;
  }

  let const has_filter = should_print_listing && operand_index < args.count();
  if (operand_index >= args.count() || has_filter) {
    let const should_include_signal_prefix = cxt.is_bash_compatible();

    let out = String{cxt.scratch_allocator()};
    let const do_append_listing = [&](StringView condition, StringView action)
                                      throws -> void {
      out += "trap -- ";
      /* Bash wraps every listed action in single quotes. An action holding no
         character that needs them is wrapped as well. */
      append_shell_quoted_arg(out, action, true);
      out += ' ';
      out += format_listed_condition(condition, should_include_signal_prefix,
                                     cxt.scratch_allocator());
      out += '\n';
    };

    /* A filtered listing follows the operand order, prints a repeated operand
       twice, and reports an operand that names no condition. */
    if (has_filter) {
      let has_invalid_operand = false;

      for (usize i = operand_index; i < args.count(); i++) {
        let const condition =
            normalize_condition(args[i], cxt.scratch_allocator());
        if (!is_valid_trap_condition(condition.view(), cxt.mood())) {
          report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                    args[i] + ": invalid signal specification",
                                    "List the signal names with `trap -l`");
          has_invalid_operand = true;
          continue;
        }

        if (cxt.is_signal_ignored_at_startup(condition.view())) {
          do_append_listing(condition.view(), "");
          continue;
        }

        let const *action = cxt.traps().find(condition.view());
        if (action != nullptr)
          do_append_listing(condition.view(), action->view());
      }

      ec.print_to_stdout(out);
      return has_invalid_operand ? 1 : 0;
    }

    let listed = ArrayList<listed_trap>{cxt.scratch_allocator()};
    cxt.traps().for_each([&](StringView condition, const String &action) {
      listed.push(
          listed_trap{trap_listing_order(condition), condition, action.view()});
    });

    let const ignored_signals = cxt.get_startup_ignored_signals();
    let ignored_names = ArrayList<String>{cxt.scratch_allocator()};
    if (ignored_signals != 0) {
      ignored_names.reserve(os::ENTRY_IGNORED_SIGNAL_LIMIT);

      for (i32 number = 1; number <= os::ENTRY_IGNORED_SIGNAL_LIMIT; number++) {
        if ((ignored_signals & (u64{1} << (number - 1))) == 0) continue;

        let name = os::signal_name_from_number(number);
        if (!name.has_value()) continue;
        if (cxt.traps().find(name->view()) != nullptr) continue;

        ignored_names.push(String{cxt.scratch_allocator(), name->view()});
        listed.push(listed_trap{static_cast<i64>(number),
                                ignored_names.back().view(), ""});
      }
    }

    listed.sort([](const listed_trap &left, const listed_trap &right) {
      return left.order < right.order;
    });

    for (usize i = 0; i < listed.count(); i++)
      do_append_listing(listed[i].condition, listed[i].action);

    ec.print_to_stdout(out);
    return 0;
  }

  ASSERT(operand_index < args.count());

  let const action_index = operand_index;

  if (action_index + 1 == args.count()) {
    let const condition =
        normalize_condition(args[action_index], cxt.scratch_allocator());
    if (!is_valid_trap_condition(condition.view(), cxt.mood())) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(action_index),
                                args[action_index] +
                                    ": invalid signal specification",
                                "List the signal names with `trap -l`");

      /* Dash reports 1 for the reset form, and bash reports 2. */
      return cxt.is_posix_mode() ? 1 : 2;
    }

    LOG(Info, "trap resetting condition '%s' to its default",
        condition.c_str());
    cxt.remove_trap(condition);
    return 0;
  }

  let const &action = args[action_index];
  let const is_reset = action == "-";

  i32 status = 0;
  for (usize i = action_index + 1; i < args.count(); i++) {
    let const condition = normalize_condition(args[i], cxt.scratch_allocator());
    if (!is_valid_trap_condition(condition.view(), cxt.mood())) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                args[i] + ": invalid signal specification",
                                "List the signal names with `trap -l`");
      status = 1;
      continue;
    }

    LOG(Info, "trap %s action for signal '%s'",
        is_reset ? "resetting the" : "setting", condition.c_str());
    if (is_reset)
      cxt.remove_trap(condition);
    else
      cxt.set_trap(condition, action);
  }

  return status;
}

} /* namespace koshka */
