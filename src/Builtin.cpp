/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the builtin registry and dispatch interface. It
 * defines builtin identities, metadata, construction, lookup, and evaluator
 * execution contracts. Each source under builtins implements one command.
 */

#include "Builtin.hpp"

#include "CLI.hpp"
#include "CLIColors.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"
#include "base/Debug.hpp"
#include "base/Path.hpp"
#include "base/Trace.hpp"

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
  let const should_color = colors::stdout_wants_color();
  help_text += make_flag_help(flags, should_color);
  help_text += '\n';
  if (!extra_sections.is_empty()) {
    help_text += extra_sections;
    help_text += '\n';
  }
  ec.print_to_stdout(format_cli_help(help_text.view(), should_color));
}

flatten fn search_builtin(StringView builtin_name) throws
    -> Maybe<Builtin::Kind>
{
  return BUILTINS.find(builtin_name);
}

pure fn builtin_is_hidden_by_mood(Builtin::Kind kind, mimic_mood mood) wontthrow
    -> bool
{
  if (mood != mimic_mood::Posix) return false;

  return kind == Builtin::Kind::Let || kind == Builtin::Kind::Time;
}

pure fn name_is_keyword_in_mood(StringView name, mimic_mood mood) wontthrow
    -> bool
{
  if (utils::is_posix_reserved_word(name)) return true;

  let const is_posix = mood == mimic_mood::Posix;
  if (let const keyword = KEYWORDS.find(name); keyword.has_value()) {
    return !is_posix ||
           (*keyword != Token::Kind::Time && *keyword != Token::Kind::Function);
  }

  return !is_posix && BASH_KEYWORDS.contains(name);
}

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
    for (let const &entry : BUILTIN_ENTRIES)
      collected.push(entry.key.to_string());
    return collected;
  }();
  return names;
}

static constexpr i32 STAGE_ASSIGNMENT_FAILURE_STATUS = 4;

fn declaration_assignment_failure_status(const EvalContext &cxt) wontthrow
    -> i32
{
  return cxt.job_table_store().is_in_pipeline_stage()
             ? STAGE_ASSIGNMENT_FAILURE_STATUS
             : 1;
}

static pure fn is_declaration_builtin_kind(Builtin::Kind kind) wontthrow -> bool
{
  switch (kind) {
  case Builtin::Kind::Declare:
  case Builtin::Kind::Export:
  case Builtin::Kind::Readonly: return true;
  default: return false;
  }
}

fn execute_builtin(ExecContext &&ec, EvalContext &cxt) throws -> i32
{
  ASSERT(!ec.args().is_empty());

  cxt.evaluation_metrics_store().add_builtin_run(
      cxt.runtime_state().stats_enabled());

  defer { ec.close_fds(); };

  const bool has_pipe_descriptors =
      ec.in_fd.has_value() || ec.out_fd.has_value() || ec.err_fd.has_value() ||
      !ec.nonstandard_fds.is_empty();
  let const has_dup_routing = ec.should_duplicate_error_to_output ||
                              ec.should_duplicate_output_to_error;

  let saved_descriptors = ArrayList<os::saved_descriptor>{heap_allocator()};
  if (has_pipe_descriptors || has_dup_routing) {
    if (ec.in_fd)
      saved_descriptors.push(os::save_and_replace_descriptor(0, *ec.in_fd));
    ec.apply_output_routing(
        [&]() {
          if (ec.out_fd) {
            saved_descriptors.push(
                os::save_and_replace_descriptor(1, *ec.out_fd));
          }
        },
        [&]() {
          if (ec.err_fd) {
            saved_descriptors.push(
                os::save_and_replace_descriptor(2, *ec.err_fd));
          }
        },
        [&]() {
          saved_descriptors.push(os::save_and_replace_descriptor(
              2, os::descriptor_for_shell_fd(1)));
        },
        [&]() {
          saved_descriptors.push(os::save_and_replace_descriptor(
              1, os::descriptor_for_shell_fd(2)));
        });
    ec.apply_nonstandard_routing(
        [&](os::descriptor file_fd, i32 target_fd) {
          saved_descriptors.push(
              os::save_and_replace_descriptor(target_fd, file_fd));
        },
        [&](i32 dup_from_fd, i32 target_fd) {
          saved_descriptors.push(os::save_and_replace_descriptor(
              target_fd, os::descriptor_for_shell_fd(dup_from_fd)));
        },
        [&](i32 target_fd) {
          saved_descriptors.push(os::save_descriptor(target_fd));
          os::close_fd(os::descriptor_for_shell_fd(target_fd));
        });
  }
  defer
  {
    for (usize i = saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(saved_descriptors[i - 1]);
  };

  LOG(Debug, "dispatching builtin '%s' with %zu arguments",
      ec.program().c_str(), ec.args().count());
  try {
    if (ec.args().count() > 1 && ec.args()[1] == "--help" &&
        SHOULD_DISPATCH_BUILTIN_HELP[static_cast<usize>(ec.builtin_kind())])
    {
      show_builtin_help_impl(ec, builtin_help_description(ec.builtin_kind()),
                             *builtin_help_synopsis(ec.builtin_kind()),
                             *builtin_flag_list(ec.builtin_kind()));
      return 0;
    }

    switch (ec.builtin_kind()) {
      BUILTIN_SWITCH_CASES();
    default:
      unreachable("Unhandled builtin of kind %d", ENUM(ec.builtin_kind()));
    }
  } catch (const BrokenPipeExit &) {
    let const pipe_trap = cxt.trap_store().find(StringView{"PIPE"});
    let const is_pipe_signal_ignored =
        (pipe_trap.has_value() && pipe_trap.value()->action_text.is_empty()) ||
        (!pipe_trap.has_value() &&
         cxt.is_signal_ignored_at_startup(StringView{"PIPE"}));
    if (!is_pipe_signal_ignored) return KOSH_BROKEN_PIPE_EXIT_STATUS;

    report_soft_builtin_error(ec, cxt, "write error: Broken pipe");
    return 1;
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (const Error &e) {
    if (e.is_script_fatal()) throw;

    if (cxt.runtime_state().is_bash_compatible() && !e.is_line_discarding()) {
      if (!e.detail_message().is_empty())
        report_soft_builtin_error(ec, cxt, e.message(), e.detail_message());
      else
        report_soft_builtin_error(ec, cxt, e.message());
      if (is_declaration_builtin_kind(ec.builtin_kind()))
        return declaration_assignment_failure_status(cxt);
      return static_cast<i32>(e.command_status());
    }

    let const prefixed = builtin_error_message(ec.program(), e.message());
    if (!e.detail_message().is_empty()) {
      let relocated = ErrorWithLocationAndDetails{
          ec.source_location(), prefixed.view(), e.detail_message()};
      relocated.take_line_discard_marks(e);
      relocated.set_command_status(e.command_status());
      throw relocated;
    }
    let relocated = ErrorWithLocation{ec.source_location(), prefixed.view()};
    relocated.take_line_discard_marks(e);
    relocated.set_command_status(e.command_status());
    throw relocated;
  }
  unreachable("execute_builtin reached the end without dispatching");
}

static fn builtin_error_ends_posix_script(const ExecContext &ec,
                                          const EvalContext &cxt) wontthrow
    -> bool
{
  if (!cxt.runtime_state().is_posix_mode()) return false;
  if (ec.builtin_kind() == Builtin::Kind::Trap) return false;

  return ec.builtin_kind() == Builtin::Kind::Local ||
         is_special_builtin_name(ec.program());
}

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message) throws -> void
{
  report_soft_builtin_error(ec, cxt, ec.source_location(), message);
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
  ErrorWithLocation located{location,
                            builtin_error_message(ec.program(), message)};
  if (builtin_error_ends_posix_script(ec, cxt)) {
    located.set_command_status(2);
    throw located;
  }

  if (const String *source = cxt.source_store().current_source();
      source != nullptr)
  {
    show_message(located.to_string(source->view(), &cxt));
    cxt.print_source_backtrace(location, false);
  } else
    print_error(builtin_error_message(ec.program(), message) + "\n");
}

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             SourceLocation location, StringView message,
                             StringView note) throws -> void
{
  report_soft_builtin_error(ec, cxt, steal(location), message);
  show_message(Note{String{note}}.to_string());
}

fn report_loop_control_without_loop(const ExecContext &ec,
                                    EvalContext &cxt) throws -> void
{
  if (!cxt.runtime_state().is_bash_compatible() ||
      cxt.runtime_state().is_posix_option_on())
    return;

  report_soft_builtin_error(
      ec, cxt, "only meaningful in a `for', `while', or `until' loop");
}

fn report_usage_error(const ExecContext &ec, EvalContext &cxt,
                      StringView program_name) throws -> i32
{
  return report_usage_error(cxt, ec.source_location(), program_name);
}

fn report_usage_error(EvalContext &cxt, SourceLocation location,
                      StringView program_name) throws -> i32
{
  const ErrorWithLocation located{
      steal(location), String{program_name} + ": Not enough arguments"};
  if (const String *source = cxt.source_store().current_source();
      source != nullptr)
    show_message(located.to_string(source->view(), &cxt));
  else
    print_error(String{program_name} + ": Not enough arguments.\n");
  show_message(Note{String{"Try `"} + program_name + " --help` for more info"}
                   .to_string());
  return 2;
}

fn bind_declared_self_nameref(const ExecContext &ec, EvalContext &cxt,
                              usize arg_index, StringView name,
                              bool is_local) throws -> bool
{
  try {
    cxt.bind_self_nameref(name, is_local);
  } catch (const Error &error) {
    report_soft_builtin_error(ec, cxt, ec.arg_location_at(arg_index),
                              error.message().view());
    return false;
  }

  cxt.warn_circular_nameref(name);
  return true;
}

fn declare_nameref(const ExecContext &ec, EvalContext &cxt, usize arg_index,
                   StringView name, Maybe<StringView> target,
                   bool should_mark_readonly) throws -> bool
{
  try {
    if (target.has_value()) {
      cxt.bind_nameref(name, *target);
    } else {
      cxt.guard_nameref_name(name);
      cxt.variable_store().attributes().set(name, variable_attribute::Nameref,
                                            true);
      cxt.variable_store().attributes().mark_declared(name);
    }
  } catch (const Error &error) {
    report_soft_builtin_error(ec, cxt, ec.arg_location_at(arg_index),
                              error.message().view());
    return false;
  }

  if (should_mark_readonly)
    cxt.variable_store().attributes().mark_readonly(name);
  return true;
}

fn continue_job(job &job) throws -> void
{
  let const cont = os::signal_number_from_name("CONT");
  if (!cont.has_value())
    throw Error{"This platform does not support continuing stopped jobs"};
  bool did_resume = true;
  if (job.is_primary_process_active)
    did_resume = os::signal_process(job.pid, *cont);
  for (let const process : job.earlier_pipeline_processes)
    if (!os::signal_process(process, *cont)) did_resume = false;
  if (!did_resume) throw Error{"Unable to continue the stopped job"};
  job.state = job::State::Running;
}

fn finish_exit_builtin(const ExecContext &ec, EvalContext &cxt, i64 status,
                       StringView invalid_status_note,
                       StringView too_many_message,
                       StringView too_many_note) throws -> i32
{
  if (ec.args().count() > 1) {
    let const parsed_status = ec.args()[1].to<i64>();

    if (parsed_status.is_error()) {
      let const message =
          StringView{"'"} + ec.args()[1] + "' is not a numeric exit status";
      if (invalid_status_note.is_empty()) {
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(1), message);
      } else {
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(1), message,
                                  invalid_status_note);
      }
      return 2;
    }

    if (ec.args().count() > 2 && !too_many_message.is_empty()) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(2),
                                too_many_message, too_many_note);

      if (cxt.execution_store().shell_is_interactive()) return 2;

      status = 1;
    } else {
      status = parsed_status.value();
    }
  }

  LOG(Debug, "%s ending the shell with status %lld", ec.program().c_str(),
      static_cast<long long>(status));

  if (cxt.in_subshell()) {
    let const masked_status = status & 0xFF;
    cxt.request_exit(masked_status, ec.source_location());
    return static_cast<i32>(masked_status);
  }

  cxt.run_exit_trap(static_cast<i32>(status & 0xFF));
  utils::quit(static_cast<i32>(status), utils::farewell_policy::Goodbye);
}

fn report_invalid_identifier(const ExecContext &ec, EvalContext &cxt,
                             SourceLocation location, StringView name) throws
    -> void
{
  report_soft_builtin_error(ec, cxt, location,
                            StringView{"'"} + name +
                                "' is not a valid identifier");
}

pure fn get_operand_location(const ExecContext &ec,
                             const ArrayList<SourceLocation> &operand_locations,
                             usize index) wontthrow -> SourceLocation
{
  return index < operand_locations.count() ? operand_locations[index]
                                           : ec.source_location();
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

static pure fn is_declare_key_meta(char c) wontthrow -> bool
{
  switch (c) {
  case ' ':
  case '"':
  case '\'':
  case '\\':
  case '$':
  case '`':
  case '|':
  case '&':
  case ';':
  case '(':
  case ')':
  case '<':
  case '>':
  case '!':
  case '{':
  case '}':
  case '*':
  case '[':
  case ']':
  case '?':
  case '^':
  case '~':
  case '#': return true;
  default:
    return static_cast<unsigned char>(c) < 0x20 ||
           static_cast<unsigned char>(c) == 0x7f;
  }
}

fn append_declare_value(String &out, StringView value,
                        bool is_utf8_locale) throws -> void
{
  if (utils::should_ansi_c_quote(value, is_utf8_locale)) {
    utils::append_ansi_c_quoted(out, value, is_utf8_locale);
    return;
  }

  out += '"';
  out += quote_for_declare(value);
  out += '"';
}

fn append_declare_key(String &out, StringView key, bool is_utf8_locale) throws
    -> void
{
  if (utils::should_ansi_c_quote(key, is_utf8_locale)) {
    utils::append_ansi_c_quoted(out, key, is_utf8_locale);
    return;
  }

  let is_plain = !key.is_empty() && key != "@";
  for (usize i = 0; i < key.length && is_plain; i++)
    is_plain = !is_declare_key_meta(key[i]);

  if (is_plain) {
    out.append(key);
    return;
  }

  out += '"';
  out += quote_for_declare(key);
  out += '"';
}

static fn append_value_attribute_letters(EvalContext &cxt, StringView name,
                                         String &out) throws -> void
{
  if (cxt.is_integer_variable(name)) out += 'i';
  if (cxt.variable_store().attributes().is_lowercase(name)) out += 'l';
  if (cxt.is_readonly(name)) out += 'r';
  if (cxt.variable_store().attributes().is_uppercase(name)) out += 'u';
}

static fn append_array_attribute_letters(EvalContext &cxt, StringView name,
                                         String &out) throws -> void
{
  append_value_attribute_letters(cxt, name, out);
  if (cxt.is_exported(name)) out += 'x';
}

static fn append_valueless_array_declaration(EvalContext &cxt, StringView name,
                                             char kind, String &out) throws
    -> void
{
  out += "declare -";
  out += kind;
  append_array_attribute_letters(cxt, name, out);
  out += ' ';
  out.append(name);
  out += '\n';
}

fn append_variable_declaration(EvalContext &cxt, StringView name,
                               String &out) throws -> bool
{
  if (cxt.scope_store().is_self_reference(name)) rarely
    {
      out += "declare -n ";
      out.append(name);
      out += "=\"";
      out.append(name);
      out += "\"\n";
      return true;
    }

  if (cxt.variable_store().attributes().is_nameref(name)) rarely
    {
      out += "declare -n";
      if (cxt.is_readonly(name)) out += 'r';
      out += ' ';
      out.append(name);
      if (let const target = cxt.variable_store().shell_variables().find(name);
          target.has_value())
      {
        out += "=\"";
        out += quote_for_declare(target->view());
        out += '"';
      }
      out += '\n';

      return true;
    }

  let const is_utf8_locale = cxt.get_glob_charset() == glob_charset::Utf8;
  let const is_directory_stack = cxt.is_bash_directory_stack_special(name);
  let const is_argument_array = cxt.is_bash_argument_array(name);
  let const elements = cxt.variable_store().indexed_arrays().find(name);

  if (elements.has_value() && !is_directory_stack && !is_argument_array &&
      cxt.is_valueless_array(name))
  {
    append_valueless_array_declaration(cxt, name, 'a', out);
    return true;
  }

  if (elements.has_value() || is_directory_stack || is_argument_array) {
    let line = String{cxt.scratch_allocator(), "declare -a"};
    append_array_attribute_letters(cxt, name, line);
    line += ' ';
    line.append(name);
    line += "=(";

    let subscripts = ArrayList<String>{cxt.scratch_allocator()};
    let values = ArrayList<String>{cxt.scratch_allocator()};
    let element_count = usize{0};
    if (elements.has_value()) {
      subscripts = cxt.collect_array_subscripts(name);
      values = cxt.collect_array_elements(name);
      ASSERT(subscripts.count() == values.count());
      element_count = subscripts.count();
    }
    if (is_directory_stack) {
      element_count = cxt.variable_store().directory_stack().count() + 1;
    } else if (is_argument_array) {
      element_count = cxt.dynamic_array_element_count(
          name == BASH_ARGUMENT_COUNT_VARIABLE
              ? EvalContext::DynamicArray::ArgumentCount
              : EvalContext::DynamicArray::ArgumentValue);
    }

    for (usize e = 0; e < element_count; e++) {
      if (e > 0) line += ' ';
      line += '[';
      char index_text[24];
      if (is_directory_stack || is_argument_array)
        line.append(utils::int_to_text_into(static_cast<i64>(e), index_text,
                                            sizeof(index_text)));
      else
        line.append(subscripts[e].view());
      line += "]=";

      let directory_stack_element = Maybe<String>{};
      let argument_array_element = String{cxt.scratch_allocator()};
      let element = StringView{};
      if (is_directory_stack) {
        directory_stack_element =
            cxt.get_bash_directory_stack_element(e, cxt.scratch_allocator());
        element = directory_stack_element->view();
      } else if (is_argument_array) {
        argument_array_element = cxt.dynamic_array_element_text(
            name == BASH_ARGUMENT_COUNT_VARIABLE
                ? EvalContext::DynamicArray::ArgumentCount
                : EvalContext::DynamicArray::ArgumentValue,
            e, cxt.scratch_allocator());
        element = argument_array_element.view();
      } else {
        element = values[e].view();
      }

      append_declare_value(line, element, is_utf8_locale);
    }

    line += ")\n";
    out.append(line.view());

    return true;
  }

  if (cxt.is_associative_array(name) && cxt.is_valueless_array(name)) {
    append_valueless_array_declaration(cxt, name, 'A', out);
    return true;
  }

  if (cxt.is_associative_array(name)) {
    let const keys = cxt.associative_keys(name);
    let const values = cxt.associative_values(name);
    let line = String{cxt.scratch_allocator(), "declare -A"};
    append_array_attribute_letters(cxt, name, line);
    line += ' ';
    line.append(name);
    line += "=(";

    for (usize e = 0; e < keys.count(); e++) {
      line += '[';
      append_declare_key(line, keys[e].view(), is_utf8_locale);
      line += "]=";
      append_declare_value(line,
                           e < values.count() ? values[e].view() : StringView{},
                           is_utf8_locale);
      line += ' ';
    }

    line += ")\n";
    out.append(line.view());

    return true;
  }

  if (const Maybe<String> value = cxt.get_variable_value(name)) {
    let attribute = String{cxt.scratch_allocator(), "-"};
    append_value_attribute_letters(cxt, name, attribute);
    if (os::get_environment_variable(name).has_value()) attribute += 'x';
    if (attribute.count() == 1) attribute += '-';

    let line = String{cxt.scratch_allocator(), "declare "};
    line.append(attribute.view());
    line += ' ';
    line.append(name);
    line += '=';
    append_declare_value(line, value->view(), is_utf8_locale);
    line += '\n';
    out.append(line.view());

    return true;
  }

  if (cxt.is_integer_variable(name) ||
      cxt.variable_store().attributes().is_lowercase(name) ||
      cxt.variable_store().attributes().is_uppercase(name) ||
      cxt.is_readonly(name) ||
      cxt.variable_store().attributes().is_declared(name) ||
      cxt.scope_store().has_current_local(name))
  {
    let attribute = String{cxt.scratch_allocator(), "-"};
    append_value_attribute_letters(cxt, name, attribute);
    if (cxt.is_exported(name)) attribute += 'x';
    if (attribute.count() == 1) attribute += '-';

    let line = String{cxt.scratch_allocator(), "declare "};
    line.append(attribute.view());
    line += ' ';
    line.append(name);
    line += '\n';
    out.append(line.view());

    return true;
  }

  if (cxt.is_exported(name)) {
    let line = String{cxt.scratch_allocator(), "declare -x "};
    line.append(name);
    line += '\n';
    out.append(line.view());

    return true;
  }

  return false;
}

fn parse_optional_integer_arg(const ExecContext &ec, i64 default_value) throws
    -> i64
{
  if (ec.args().count() <= 1) return default_value;
  let const parsed_value = ec.args()[1].to<i64>();
  if (parsed_value.is_error()) throw parsed_value.error();
  return parsed_value.value();
}

pure fn name_is_valid_identifier(StringView name) wontthrow -> bool
{
  return lexer::word_is_variable_name(name);
}

pure fn name_is_valid_assignment_target(StringView name) wontthrow -> bool
{
  let const bracket = name.find_character('[');
  if (!bracket.has_value()) return lexer::word_is_variable_name(name);

  return *bracket > 0 && name[name.length - 1] == ']' &&
         lexer::word_is_variable_name(name.substring_of_length(0, *bracket));
}

fn run_cd_to_directory(EvalContext &cxt, const ExecContext &ec,
                       StringView target) throws -> i32
{
  ArrayList<String> cd_args{heap_allocator()};
  cd_args.push(String{"cd"});
  cd_args.push(String{target});
  let cd_arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  let routed = ExecContext::make_from_resolved(
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
  let const &stack = cxt.variable_store().directory_stack();
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
