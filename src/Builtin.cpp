/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the builtin registry and dispatch interface. It
 * defines builtin identities, metadata, construction, lookup, and evaluator
 * execution contracts. The central registry keeps the packed name table,
 * metadata arrays, factory switch, and common dispatch consistent. Each source
 * under builtins implements one command.
 */

#include "Builtin.hpp"

#include "Cli.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

fn builtin_error_context(StringView program) throws -> String
{
  return StringView{"Builtin `"} + program + "`";
}

fn builtin_error_message(StringView program, StringView message) throws
    -> String
{
  return builtin_error_context(program) + ": " + message;
}

cold fn show_builtin_help_impl(const ExecContext &ec, StringView description,
                               const SynopsisList &synopsis_lines,
                               const FlagList &flags,
                               StringView extra_sections) throws -> void
{
  ASSERT(!ec.args().is_empty());

  let help_text = String{heap_allocator()};
  if (!description.is_empty()) {
    help_text += "DESCRIPTION\n";
    help_text += wrap_text(description, HELP_INDENT, HELP_WRAP_WIDTH);
    help_text += "\n\n";
  }
  help_text += make_synopsis(ec.args()[0].view(), synopsis_lines);
  help_text += '\n';
  help_text += make_flag_help(flags);
  help_text += '\n';
  /* The per-builtin generated text, the OPTION SWITCHES table of set and the
     OPTION NAMES list of shopt, lands after the flag sections. */
  if (!extra_sections.is_empty()) {
    help_text += extra_sections;
    help_text += '\n';
  }
  ec.print_to_stdout(help_text);
}

flatten fn search_builtin(StringView builtin_name) throws
    -> Maybe<Builtin::Kind>
{
  return BUILTINS.find(builtin_name);
}

/* The per-kind flag lists, a zero-initialized table immune to static-init
   order, filled by each builtin file's registrar after its FLAG_LIST is
   built, since both sit in the same translation unit in order. */
static const FlagList *BUILTIN_FLAG_LISTS[BUILTIN_KIND_COUNT] = {};
static const StringView *BUILTIN_HELP_DESCRIPTIONS[BUILTIN_KIND_COUNT] = {};
static const SynopsisList *BUILTIN_HELP_SYNOPSES[BUILTIN_KIND_COUNT] = {};

fn register_builtin_help(Builtin::Kind kind, const FlagList *flags,
                         const StringView *description,
                         const SynopsisList *synopsis) wontthrow -> void
{
  let const index = static_cast<usize>(kind);
  BUILTIN_FLAG_LISTS[index] = flags;
  BUILTIN_HELP_DESCRIPTIONS[index] = description;
  BUILTIN_HELP_SYNOPSES[index] = synopsis;
}

fn builtin_flag_list(Builtin::Kind kind) wontthrow -> const FlagList *
{
  return BUILTIN_FLAG_LISTS[static_cast<usize>(kind)];
}

fn builtin_help_description(Builtin::Kind kind) wontthrow -> StringView
{
  let const description = BUILTIN_HELP_DESCRIPTIONS[static_cast<usize>(kind)];
  return description != nullptr ? *description : StringView{};
}

fn builtin_help_synopsis(Builtin::Kind kind) wontthrow -> const SynopsisList *
{
  return BUILTIN_HELP_SYNOPSES[static_cast<usize>(kind)];
}

fn is_special_builtin_name(StringView name) wontthrow -> bool
{
  /* The POSIX special builtin set, matched by name. The colon and the dot are
     special while their plain-word siblings true and source-as-a-program are
     not, so the kind cannot decide this. */
  static constexpr PackedStringKey SPECIAL_BUILTIN_KEYS[] = {
      SSK(":"),    SSK("."),     SSK("break"),  SSK("continue"), SSK("eval"),
      SSK("exec"), SSK("exit"),  SSK("export"), SSK("readonly"), SSK("return"),
      SSK("set"),  SSK("shift"), SSK("times"),  SSK("trap"),     SSK("unset"),
  };
  static constexpr StaticStringSet SPECIAL_BUILTINS{SPECIAL_BUILTIN_KEYS};
  return SPECIAL_BUILTINS.contains(name);
}

fn builtin_names() throws -> const ArrayList<String> &
{
  static ArrayList<String> names = [] throws {
    let collected = ArrayList<String>{heap_allocator()};
    for (const static_string_entry<Builtin::Kind> &entry : BUILTIN_ENTRIES)
      collected.push(entry.key.to_string());
    return collected;
  }();
  return names;
}

fn execute_builtin(ExecContext &&ec, EvalContext &cxt) throws -> i32
{
  ASSERT(!ec.args().is_empty());

  /* A builtin runs inside the shell process, so it keeps the shell's own signal
     handlers. Resetting them to the default here would let a Ctrl-C during a
     builtin terminate the whole shell, and would cost two extra syscalls on
     every builtin command. */
  defer { ec.close_fds(); };

  /* A builtin stage of a pipeline carries the pipe ends in its context. A
     builtin that runs a sub-command, such as eval, command, or the dot source,
     evaluates a fresh command that builds its own context from the shell's real
     descriptors and never sees these pipe ends. The pipe descriptors are placed
     on the real shell fd 0, 1, and 2 for the duration of the builtin so any
     sub-command it spawns inherits them, and the originals are restored after.
     A single builtin that is not a pipeline stage carries no pipe fds, so it
     pays for none of this. */
  const bool has_pipe_descriptors =
      ec.in_fd.has_value() || ec.out_fd.has_value() || ec.err_fd.has_value();
  /* A bare 2>&1 or 1>&2 on a builtin carries no file descriptor, only a routing
     flag, so the placement runs whenever either a descriptor or a cross-route
     is present. Otherwise `cd /bad 2>&1` would leave the builtin's stderr on
     the terminal instead of following the standard output. */
  let const has_dup_routing = ec.should_duplicate_error_to_output ||
                              ec.should_duplicate_output_to_error;

  let saved_descriptors = ArrayList<os::saved_descriptor>{heap_allocator()};
  if (has_pipe_descriptors || has_dup_routing) {
    if (ec.in_fd)
      saved_descriptors.push(os::save_and_replace_descriptor(0, *ec.in_fd));
    if (ec.out_fd)
      saved_descriptors.push(os::save_and_replace_descriptor(1, *ec.out_fd));
    if (ec.err_fd)
      saved_descriptors.push(os::save_and_replace_descriptor(2, *ec.err_fd));
    /* The cross-route runs after the files are placed on 0, 1, and 2, so 2>&1
       copies the descriptor the standard output now points at. The saves push
       onto the same stack after the file saves, so the restore unwinds the
       route before the files, the reverse of the apply order. The same
       came-last ordering the spawn paths use decides a mixed 2>&1 1>&2. */
    ec.apply_dup_routing(
        [&]() {
          saved_descriptors.push(os::save_and_replace_descriptor(
              2, os::descriptor_for_shell_fd(1)));
        },
        [&]() {
          saved_descriptors.push(os::save_and_replace_descriptor(
              1, os::descriptor_for_shell_fd(2)));
        });
  }
  defer
  {
    for (usize i = saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(saved_descriptors[i - 1]);
  };

  /* Each builtin is a stateless dispatch object, so its case constructs it on
     the stack and runs it, which avoids a heap allocation on every builtin
     command. */
  LOG(Debug, "dispatching builtin '%s' with %zu arguments",
      ec.program().c_str(), ec.args().count());
  try {
    switch (ec.builtin_kind()) {
      BUILTIN_SWITCH_CASES();
    default:
      unreachable("Unhandled builtin of kind %d", ENUM(ec.builtin_kind()));
    }
  } catch (const BrokenPipeExit &) {
    return KOSH_BROKEN_PIPE_EXIT_STATUS;
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (const Error &e) {
    if (cxt.is_bash_compatible()) {
      if (!e.detail_message().is_empty())
        report_soft_builtin_error(ec, cxt, e.message(), e.detail_message());
      else
        report_soft_builtin_error(ec, cxt, e.message());
      return static_cast<i32>(e.command_status());
    }

    let const prefixed = builtin_error_message(ec.program(), e.message());
    if (!e.detail_message().is_empty()) {
      let relocated = ErrorWithLocationAndDetails{
          ec.source_location(), prefixed.view(), e.detail_message()};
      relocated.set_command_status(e.command_status());
      throw relocated;
    }
    let relocated = ErrorWithLocation{ec.source_location(), prefixed.view()};
    relocated.set_command_status(e.command_status());
    throw relocated;
  }
  unreachable("execute_builtin reached the end without dispatching");
}

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message) throws -> void
{
  const ErrorWithLocation located{ec.source_location(),
                                  builtin_error_message(ec.program(), message)};
  if (const String *source = cxt.current_source(); source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(builtin_error_message(ec.program(), message) + "\n");
}

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message, StringView note) throws -> void
{
  report_soft_builtin_error(ec, cxt, message);
  show_message(Note{String{note}}.to_string());
}

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             SourceLocation location, StringView message) throws
    -> void
{
  const ErrorWithLocation located{steal(location),
                                  builtin_error_message(ec.program(), message)};
  if (const String *source = cxt.current_source(); source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(builtin_error_message(ec.program(), message) + "\n");
}

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             SourceLocation location, StringView message,
                             StringView note) throws -> void
{
  report_soft_builtin_error(ec, cxt, steal(location), message);
  show_message(Note{String{note}}.to_string());
}

fn report_usage_error(const ExecContext &ec, EvalContext &cxt,
                      StringView program_name) throws -> i32
{
  /* A missing required argument reads the same located caret in every mood, the
     kosh feature form, rather than the soft unlocated line the bash mood gives
     a thrown builtin error. The trailing note points the reader at the
     per-command help the way a compiler points past an error at a hint. The
     fallback line is for the rare case with no source to caret against, such as
     the multicall entry. */
  const ErrorWithLocation located{
      ec.source_location(), String{program_name} + ": Not enough arguments"};
  if (const String *source = cxt.current_source(); source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(String{program_name} + ": Not enough arguments.\n");
  show_message(Note{String{"Try `"} + program_name + " --help` for more info"}
                   .to_string());
  return 2;
}

fn report_usage_error(EvalContext &cxt, SourceLocation location,
                      StringView program_name) throws -> i32
{
  const ErrorWithLocation located{
      steal(location), String{program_name} + ": Not enough arguments"};
  if (const String *source = cxt.current_source(); source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(String{program_name} + ": Not enough arguments.\n");
  show_message(Note{String{"Try `"} + program_name + " --help` for more info"}
                   .to_string());
  return 2;
}

fn make_error_for_arg(const ExecContext &ec, usize index,
                      StringView message) throws -> ErrorWithLocation
{
  let const prefixed = builtin_error_message(ec.program(), message);
  return ErrorWithLocation{ec.arg_location_at(index), prefixed.view()};
}

fn make_error_for_arg(const ExecContext &ec, usize index, StringView message,
                      StringView note) throws -> ErrorWithLocationAndDetails
{
  let const prefixed = builtin_error_message(ec.program(), message);
  return ErrorWithLocationAndDetails{ec.arg_location_at(index), prefixed.view(),
                                     note};
}

fn quote_for_declare(StringView value) throws -> String
{
  let quoted = String{heap_allocator()};
  for (usize i = 0; i < value.length; i++) {
    const char c = value[i];
    if (c == '"' || c == '\\' || c == '$' || c == '`') quoted += '\\';
    quoted += c;
  }
  return quoted;
}

fn parse_optional_integer_arg(const ExecContext &ec, i64 default_value) throws
    -> i64
{
  if (ec.args().count() <= 1) return default_value;
  let const parsed_value = ec.args()[1].to<i64>();
  if (parsed_value.is_error()) throw parsed_value.error();
  return parsed_value.value();
}

Builtin::Builtin() = default;

pure fn name_is_valid_identifier(StringView name) wontthrow -> bool
{
  return lexer::word_is_variable_name(name);
}

fn run_cd_to_directory(EvalContext &cxt, const ExecContext &ec,
                       StringView target) throws -> i32
{
  ArrayList<String> cd_args{heap_allocator()};
  cd_args.push(String{"cd"});
  cd_args.push(String{target});
  let cd_arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  let routed = ExecContext::from_resolved(
      ec.source_location(), ResolvedCommand::from_builtin(Builtin::Kind::Cd),
      steal(cd_args), steal(cd_arg_locations));
  return execute_builtin(steal(routed), cxt);
}

static fn abbreviate_home_directory(StringView path, Allocator allocator) throws
    -> String
{
  let const home = os::get_home_directory();
  if (home.has_value()) {
    let const home_view = home->text().view();
    if (path == home_view) return String{allocator, "~"};
    if (path.length > home_view.length && path.starts_with(home_view) &&
        os::is_directory_separator(path[home_view.length]))
    {
      let result = String{allocator, "~"};
      result.append(path.substring(home_view.length));
      return result;
    }

    let const path_value = Path{path};
    if (path_value.is_same_file_as(*home)) return String{allocator, "~"};

    for (usize position = os::path_root_length(path); position < path.length;
         position++)
    {
      if (!os::is_directory_separator(path[position])) continue;

      let const prefix = Path{path.substring_of_length(0, position)};
      if (!prefix.is_same_file_as(*home)) continue;

      let result = String{allocator, "~"};
      result.append(path.substring(position));
      return result;
    }
  }
  return String{allocator, path};
}

fn parse_directory_stack_rotation(StringView arg, usize ring_count,
                                  SourceLocation location,
                                  usize &index_out) throws -> bool
{
  if (arg.length < 2 || (arg[0] != '+' && arg[0] != '-')) return false;
  let const digits = arg.substring(1);
  if (!digits.is_all_decimal_digits()) return false;

  let const parsed = digits.to<i64>();
  if (parsed.is_error()) return false;
  let const number = static_cast<usize>(parsed.value());
  if (number >= ring_count) {
    throw ErrorWithLocationAndDetails{
        location,
        StringView{"the directory stack rotation '"} + arg +
            "' is past the end of the stack",
        "Run `dirs -v` to see the numbered stack"};
  }

  index_out = arg[0] == '+' ? number : ring_count - 1 - number;
  return true;
}

fn logical_working_directory(const EvalContext &cxt) throws -> Path
{
  let physical_directory = Path::current_directory();
  let const logical_pwd = cxt.get_variable_value("PWD");
  if (logical_pwd.has_value() && !logical_pwd->is_empty() &&
      os::path_is_absolute(logical_pwd->view()))
  {
    let logical_directory = Path{logical_pwd->view()};
    if (logical_directory.is_same_file_as(physical_directory))
      return logical_directory;
  }

  return physical_directory;
}

fn print_directory_stack(EvalContext &cxt, const ExecContext &ec,
                         bool should_print_one_per_line,
                         bool should_print_numbers,
                         bool should_print_full_paths,
                         Maybe<usize> selected_index) throws -> void
{
  let const &stack = cxt.directory_stack();
  let const pwd = logical_working_directory(cxt).text().clone();
  let const entry_count = stack.count() + 1;
  let const first_index = selected_index.value_or(0);
  let const end_index =
      selected_index.has_value() ? first_index + 1 : entry_count;

  let out = String{cxt.scratch_allocator()};
  for (usize i = first_index; i < end_index; i++) {
    let const entry = i == 0 ? pwd.view() : stack[stack.count() - i].view();
    if (should_print_numbers) {
      out += String::from(i, cxt.scratch_allocator());
      out += "  ";
    }
    if (should_print_full_paths)
      out.append(entry);
    else
      out.append(
          abbreviate_home_directory(entry, cxt.scratch_allocator()).view());
    if (should_print_one_per_line || should_print_numbers ||
        selected_index.has_value() || i + 1 == entry_count)
      out += '\n';
    else
      out += ' ';
  }
  ec.print_to_stdout(out);
}

} /* namespace koshka */
