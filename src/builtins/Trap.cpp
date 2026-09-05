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
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[action condition ...]");

HELP_DESCRIPTION_DECL(
    "The trap builtin sets the action to run for each named condition.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(TRAP_PRINT, Bool, 'p', "", "Print the set traps in a reusable form.");

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

fn is_valid_trap_condition(StringView condition) throws -> bool
{
  static constexpr PackedStringKey SPECIAL_CONDITION_KEYS[] = {
      SSK("EXIT"),
      SSK("DEBUG"),
      SSK("ERR"),
      SSK("RETURN"),
  };
  static constexpr StaticStringSet SPECIAL_CONDITIONS{SPECIAL_CONDITION_KEYS};

  if (SPECIAL_CONDITIONS.contains(condition)) return true;

  if (condition.is_all_decimal_digits()) return false;

  return os::signal_number_from_name(condition).has_value();
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

  if (!cxt.is_posix_mode() && args.count() == 2 &&
      (args[1] == "-l" || args[1] == "--list"))
  {
    ec.print_to_stdout(koshkit::format_signal_list());
    return 0;
  }

  let const is_print_form =
      !cxt.is_posix_mode() && args.count() >= 2 && args[1] == "-p";
  if (args.count() == 1 || is_print_form) {
    let const should_include_signal_prefix = cxt.is_bash_compatible();
    let const has_filter = is_print_form && args.count() > 2;

    let out = String{cxt.scratch_allocator()};
    cxt.traps().for_each([&](StringView condition, const String &action) {
      if (has_filter) {
        let was_requested = false;
        for (usize i = 2; i < args.count(); i++)
          if (normalize_condition(args[i], cxt.scratch_allocator()).view() ==
              condition)
          {
            was_requested = true;
            break;
          }
        if (!was_requested) return;
      }

      out += "trap -- ";
      append_shell_quoted_arg(out, action.view());
      out += ' ';
      out += format_listed_condition(condition, should_include_signal_prefix,
                                     cxt.scratch_allocator());
      out += '\n';
    });
    ec.print_to_stdout(out);
    return 0;
  }

  ASSERT(args.count() > 1);

  usize action_index = 1;
  if (args[action_index] == "--") action_index++;
  if (action_index >= args.count()) return 0;

  if (action_index + 1 == args.count()) {
    let const condition =
        normalize_condition(args[action_index], cxt.scratch_allocator());
    if (!is_valid_trap_condition(condition.view())) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(action_index),
                                args[action_index] +
                                    ": invalid signal specification",
                                "List the signal names with `trap -l`");
      return 2;
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
    if (!is_valid_trap_condition(condition.view())) {
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
