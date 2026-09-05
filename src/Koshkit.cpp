/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements runtime registration and dispatch for bundled koshkit
 * utilities. It also provides shared argument parsing, input opening, help,
 * error reporting, signal formatting, size formatting, and duration parsing.
 * The central unit owns the utility registry and common dispatch. Each source
 * under koshkit implements one command.
 */

#include "Koshkit.hpp"

#include "Builtin.hpp"
#include "Cli.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace koshkit {

Utility::Utility() = default;

flatten fn find_util(StringView name) throws -> Maybe<Utility::Kind>
{
  return KOSHKIT_UTILS.find(name);
}

/* Zero-initialized so it is immune to static-init order, filled by each
   utility's registrar. */
static const FlagList *KOSHKIT_UTIL_FLAG_LISTS[KOSHKIT_UTIL_COUNT] = {};

fn register_koshkit_util_flags(Utility::Kind chosen,
                               const FlagList *flags) wontthrow -> void
{
  KOSHKIT_UTIL_FLAG_LISTS[static_cast<usize>(chosen)] = flags;
}

fn koshkit_util_flag_list(Utility::Kind chosen) wontthrow -> const FlagList *
{
  return KOSHKIT_UTIL_FLAG_LISTS[static_cast<usize>(chosen)];
}

fn util_names() throws -> const ArrayList<String> &
{
  static ArrayList<String> names = [] throws {
    let collected = ArrayList<String>{heap_allocator()};
    for (const static_string_entry<Utility::Kind> &entry : KOSHKIT_ENTRIES)
      collected.push(entry.key.to_string());
    return collected;
  }();
  return names;
}

fn print_environment(const ExecContext &ec, EvalContext &cxt) throws -> void
{
  unused(cxt.materialize_kosh_identity());
  let output = String{cxt.scratch_allocator()};
  for (let const &name : os::environment_names()) {
    let const value = os::get_environment_variable(name.view());
    output += name.view();
    output += '=';
    if (value.has_value()) output += value->view();
    output += '\n';
  }
  ec.print_to_stdout(output);
}

fn run_util(Utility::Kind chosen, const ExecContext &ec, EvalContext &cxt,
            const ArrayList<String> &args,
            const ArrayList<SourceLocation> &arg_locations) throws -> i32
{
  LOG(Debug, "dispatching koshkit utility %d with %zu arguments", ENUM(chosen),
      args.count());
  switch (chosen) {
    UTILITY_SWITCH_CASES();
  }
  unreachable("unhandled koshkit utility of kind %d", ENUM(chosen));
}

[[noreturn]] fn rethrow_with_prefix(const ErrorWithLocation &error,
                                    StringView prefix) throws -> void
{
  let const message = prefix + ": " + error.message();
  if (!error.detail_message().is_empty()) {
    let rewrapped = ErrorWithLocationAndDetails{
        error.location(), message.view(), error.detail_message()};
    if (error.is_script_fatal()) rewrapped.set_script_fatal();
    rewrapped.set_command_status(error.command_status());
    throw rewrapped;
  }

  let rewrapped = ErrorWithLocation{error.location(), message.view()};
  if (error.is_script_fatal()) rewrapped.set_script_fatal();
  rewrapped.set_command_status(error.command_status());
  throw rewrapped;
}

fn render_with_prefix(const ErrorWithLocation &error, StringView prefix,
                      StringView source, EvalContext &context) throws -> String
{
  let const message = prefix + ": " + error.message();
  if (!error.detail_message().is_empty())
    return ErrorWithLocationAndDetails{error.location(), message.view(),
                                       error.detail_message()}
        .to_string(source, &context);

  return ErrorWithLocation{error.location(), message.view()}.to_string(
      source, &context);
}

fn dispatch(const ExecContext &ec, EvalContext &cxt, usize name_index,
            Maybe<Utility::Kind> chosen) throws -> i32
{
  ASSERT(name_index < ec.args().count());
  let const name = ec.args()[name_index].view();
  if (!chosen.has_value()) chosen = find_util(name);
  if (!chosen.has_value())
    throw ErrorWithLocation{ec.arg_location_at(name_index),
                            "koshkit has no utility named '" + String{name} +
                                "'"};

  ArrayList<String> shifted{cxt.scratch_allocator()};
  shifted.reserve(ec.args().count() - name_index);
  let shifted_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  shifted_locations.reserve(ec.args().count() - name_index);
  for (usize i = name_index; i < ec.args().count(); i++) {
    shifted.push(String{cxt.scratch_allocator(), ec.args()[i].view()});
    shifted_locations.push(ec.arg_location_at(i));
  }

  try {
    return run_util(*chosen, ec, cxt, shifted, shifted_locations);
  } catch (const BrokenPipeExit &) {
    throw;
  } catch (const ErrorWithLocation &e) {
    let const invocation_name = ec.is_multicall ? String{heap_allocator(), name}
                                                : String{"koshkit "} + name;
    rethrow_with_prefix(e, invocation_name);
  } catch (const Error &error) {
    relocate_error(error, ec.source_location());
  }
}

fn run_as_multicall(StringView util_name, ArrayList<String> operands,
                    EvalContext &cxt) throws -> i32
{
  let const chosen = find_util(util_name);
  ASSERT(chosen.has_value());

  /* The scan stops at --, where a later --version is an operand. */
  for (const String &operand : operands) {
    if (operand == "--") break;
    if (operand == "--version") {
      show_version();
      return 0;
    }
  }

  ArrayList<String> args{heap_allocator()};
  args.reserve(operands.count() + 1);
  args.push(String{util_name});
  for (String &operand : operands)
    args.push(steal(operand));

  let arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  let ec = ExecContext::from_resolved(
      SourceLocation{}, ResolvedCommand::from_builtin(Builtin::Kind::Koshkit),
      steal(args), steal(arg_locations));
  ec.is_multicall = true;

  try {
    return run_util(*chosen, ec, cxt, ec.args(), ec.arg_locations());
  } catch (const BrokenPipeExit &) {
    return 141;
  } catch (const ErrorWithLocation &e) {
    show_message(render_with_prefix(
        e, util_name, utils::merge_args_to_string(ec.args()), cxt));
    return 1;
  } catch (const Error &e) {
    show_message(e.to_string());
    return 1;
  } catch (const std::exception &e) {
    show_message(String{util_name} + ": " + e.what());
    return 1;
  } catch (...) {
    show_message(String{util_name} + ": unexpected error");
    return 1;
  }
}

fn parse_util_operands(const FlagList &flags, const ArrayList<String> &args,
                       const ArrayList<SourceLocation> *arg_locations,
                       ArrayList<SourceLocation> *operand_locations,
                       bool should_accept_negative_number_operand,
                       bool should_allow_options_after_operands,
                       bool should_accept_unknown_flag_operand) throws
    -> ArrayList<String>
{
  ArrayList<String> operands = parse_flags_vec(
      flags, args, 0, NULL, arg_locations, operand_locations, {},
      should_accept_negative_number_operand,
      should_allow_options_after_operands, should_accept_unknown_flag_operand);
  /* The first operand is the utility name, dropped to leave the real arguments.
   */
  if (!operands.is_empty()) operands.remove(0);
  if (operand_locations != nullptr && !operand_locations->is_empty())
    operand_locations->remove(0);
  return operands;
}

fn print_util_help(const ExecContext &ec, StringView name, StringView synopsis,
                   StringView description, const FlagList &flags) throws -> void
{
  let help_text = String{heap_allocator()};

  if (ec.is_multicall) {
    help_text += "\n";
    help_text += wrap_text("This utility is bundled with the Koshka shell and "
                           "runs from the kosh binary reached through a "
                           "symlink, not a system program of the same name.",
                           HELP_INDENT, HELP_WRAP_WIDTH);
    help_text += "\n\n";
  }

  if (!description.is_empty()) {
    help_text += "DESCRIPTION\n";
    help_text += wrap_text(description, HELP_INDENT, HELP_WRAP_WIDTH);
    help_text += "\n\n";
  }
  SynopsisList synopsis_lines{synopsis};
  help_text += make_synopsis(name, synopsis_lines);
  help_text += '\n';
  help_text += make_flag_help(flags);
  help_text += '\n';

  ec.print_to_stdout(help_text);
}

fn read_fd_to_string(os::descriptor fd) throws -> Maybe<String>
{
  return os::read_fd_to_string(fd, heap_allocator());
}

fn confirm_koshkit_action(const ExecContext &ec, StringView prompt) throws
    -> bool
{
  ec.print_to_stderr(prompt);

  char first_byte = '\0';
  bool is_first_byte = true;
  loop
  {
    char byte = '\0';
    let const read_count = os::read_fd(ec.in_fd.value_or(KOSH_STDIN), &byte, 1);
    if (!read_count.has_value() || *read_count == 0) break;
    if (is_first_byte) {
      first_byte = byte;
      is_first_byte = false;
    }
    if (byte == '\n') break;
  }

  return first_byte == 'y' || first_byte == 'Y';
}

fn read_named_or_stdin(const ExecContext &ec, StringView path) throws
    -> Maybe<String>
{
  if (path == "-") return read_fd_to_string(ec.in_fd.value_or(KOSH_STDIN));

  let const fd = os::open_file_descriptor(path, os::file_open_mode::Read);
  if (!fd.has_value()) return None;
  defer { os::close_fd(*fd); };
  return read_fd_to_string(*fd);
}

fn open_named_or_stdin(const ExecContext &ec, StringView path) wontthrow
    -> Maybe<input_descriptor>
{
  if (path == "-")
    return input_descriptor{ec.in_fd.value_or(KOSH_STDIN), false};

  let const descriptor =
      os::open_file_descriptor(path, os::file_open_mode::Read);
  if (!descriptor.has_value()) return None;
  return input_descriptor{*descriptor, true};
}

fn source_list_from_operands(const ArrayList<String> &operands,
                             Allocator allocator,
                             usize first_operand_index) throws
    -> ArrayList<StringView>
{
  let sources = ArrayList<StringView>{allocator};
  if (first_operand_index >= operands.count()) {
    sources.push(StringView{"-"});
  } else {
    sources.reserve(operands.count() - first_operand_index);
    for (usize i = first_operand_index; i < operands.count(); i++)
      sources.push(operands[i].view());
  }
  return sources;
}

fn format_signal_list() throws -> String
{
  let out = String{heap_allocator()};
  for (let const name : os::signal_names()) {
    if (let const number = os::signal_number_from_name(name);
        number.has_value())
    {
      out += String::from(*number, heap_allocator());
      out += ") SIG";
      out += name;
      out += '\n';
    }
  }
  return out;
}

fn format_human_size(u64 bytes, Allocator allocator) throws -> String
{
  if (bytes < 1024) return String::from(bytes, allocator);

  static const char units[] = {'K', 'M', 'G', 'T', 'P'};
  double value = static_cast<double>(bytes);
  usize unit = 0;
  /* The condition reads unit, so the last unit P stays reachable. */
  while (value >= 1024.0 && unit < sizeof(units)) {
    value /= 1024.0;
    unit++;
  }

  /* A value that rounds up to 1024 crosses over to the next unit. */
  if (value >= 1023.5 && unit < sizeof(units)) {
    value /= 1024.0;
    unit++;
  }

  String out{allocator};
  /* A scaled value below ten keeps one decimal, otherwise it rounds whole. */
  let const tenths = static_cast<u64>(value * 10.0 + 0.5);
  if (value < 10.0 && tenths < 100) {
    out += String::from(tenths / 10, allocator);
    out += '.';
    out += String::from(tenths % 10, allocator);
  } else {
    out += String::from(static_cast<u64>(value + 0.5), allocator);
  }
  out.push(units[unit - 1]);
  return out;
}

fn parse_koshkit_duration_seconds(StringView text, StringView utility_name,
                                  Allocator allocator) throws -> f64
{
  let const do_throw_invalid = [&]() throws -> void {
    throw ErrorWithDetails{
        String{allocator, utility_name}
        + ": invalid duration '" + text + "'",
        "Use a non-negative number with an optional `s`, `m`, `h`, or `d` "
        "suffix, e.g. `" +
            String{allocator, utility_name}
        + " 5`"
    };
  };

  f64 multiplier = 1.0;
  usize number_length = text.length;
  if (number_length != 0) {
    switch (text[number_length - 1]) {
    case 's': multiplier = 1.0; break;
    case 'm': multiplier = 60.0; break;
    case 'h': multiplier = 60.0 * 60.0; break;
    case 'd': multiplier = 60.0 * 60.0 * 24.0; break;
    default: break;
    }
    if (multiplier != 1.0 || text[number_length - 1] == 's') {
      number_length--;
    }
  }

  let const number =
      String{allocator, text.substring_of_length(0, number_length)};
  let const parsed_value = number.to<f64>();
  if (parsed_value.is_error()) do_throw_invalid();

  let const value = parsed_value.value();
  if (__builtin_isnan(value) || value < 0.0) do_throw_invalid();

  return value * multiplier;
}

cold noinline fn report_soft_koshkit_error(const ExecContext &ec,
                                           EvalContext &cxt,
                                           StringView message) throws -> void
{
  /* The fallback line covers the rare case with no source to caret against. */
  const ErrorWithLocation located{ec.source_location(), message};
  if (const String *source = cxt.current_source(); source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(String{message} + "\n");
}

cold noinline fn report_soft_koshkit_error(EvalContext &cxt,
                                           SourceLocation location,
                                           StringView message) throws -> void
{
  const ErrorWithLocation located{steal(location), message};
  if (const String *source = cxt.current_source(); source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(String{message} + "\n");
}

cold noinline fn report_soft_koshkit_error(const ExecContext &ec,
                                           EvalContext &cxt, StringView message,
                                           StringView note) throws -> void
{
  report_soft_koshkit_error(ec, cxt, message);
  show_message(Note{String{note}}.to_string());
}

cold noinline fn report_soft_koshkit_util_error(const ExecContext &ec,
                                                EvalContext &cxt,
                                                StringView utility_name,
                                                StringView message) throws
    -> void
{
  report_soft_koshkit_error(ec, cxt, String{utility_name} + ": " + message);
}

cold noinline fn report_soft_koshkit_util_error(
    const ExecContext &, EvalContext &cxt, SourceLocation location,
    StringView utility_name, StringView message) throws -> void
{
  let const prefixed = String{utility_name} + ": " + message;
  report_soft_koshkit_error(cxt, steal(location), prefixed.view());
}

} /* namespace koshkit */

} /* namespace koshka */
