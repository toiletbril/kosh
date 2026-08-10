#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Parser.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-e editor] [-lnr] [first [last]]",
                   "-s [old=new ...] [command]");

HELP_DESCRIPTION_DECL(
    "The fc builtin lists, edits, and executes commands from history.");

FLAG(FC_EDITOR, String, 'e', "", "Use editor to edit the selected commands.");
FLAG(FC_LIST, Bool, 'l', "", "List the selected commands.");
FLAG(FC_NO_NUMBERS, Bool, 'n', "", "Omit history numbers when listing.");
FLAG(FC_REVERSE, Bool, 'r', "", "Reverse the selected command order.");
FLAG(FC_EXECUTE, Bool, 's', "", "Execute a selected command.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Fc);

namespace shit {

struct fc_options
{
  bool should_list{false};
  bool should_number{true};
  bool should_reverse{false};
  bool should_execute{false};
  Maybe<StringView> editor{None};
};

enum class fc_selection_error : u8
{
  None,
  OutOfRange,
  NotFound,
};

struct fc_selection
{
  i64 index{-1};
  fc_selection_error error{fc_selection_error::None};
};

Fc::Fc() = default;

pure fn Fc::kind() const wontthrow -> Builtin::Kind { return Kind::Fc; }

static pure fn is_history_number(StringView text) wontthrow -> bool
{
  if (text.is_empty()) return false;
  if (text[0] == '-') text = text.substring(1);
  return !text.is_empty() && text.is_all_decimal_digits();
}

static fn
resolve_fc_selection(StringView specification,
                     const ArrayList<toiletline::history_event> &events,
                     i64 last_eligible_index, bool is_listing, bool is_first,
                     Maybe<usize> active_index) throws -> fc_selection
{
  if (last_eligible_index < 0)
    return fc_selection{-1, fc_selection_error::NotFound};

  if (is_history_number(specification)) {
    bool was_out_of_range = false;
    let const parsed =
        utils::parse_decimal_i64(specification, &was_out_of_range);
    if (parsed.is_error() || was_out_of_range) {
      return fc_selection{-1, fc_selection_error::OutOfRange};
    }

    let const number = parsed.value();
    let const has_negative_sign = specification[0] == '-';
    if (number < 0) {
      let index = last_eligible_index + number + 1;
      if (index < 0) index = 0;
      return fc_selection{index, fc_selection_error::None};
    }
    if (number == 0) {
      if (has_negative_sign) {
        if (!is_listing)
          return fc_selection{-1, fc_selection_error::OutOfRange};
        return fc_selection{active_index.has_value()
                                ? static_cast<i64>(*active_index)
                                : static_cast<i64>(events.count()) - 1,
                            fc_selection_error::None};
      }
      return fc_selection{last_eligible_index, fc_selection_error::None};
    }

    let const relative_number = number - static_cast<i64>(events[0].number);
    if (relative_number < 0 || relative_number >= last_eligible_index) {
      return fc_selection{is_first ? 0 : last_eligible_index,
                          fc_selection_error::None};
    }
    return fc_selection{relative_number, fc_selection_error::None};
  }

  for (i64 index = last_eligible_index; index >= 0; index--)
    if (events[static_cast<usize>(index)].command.starts_with(specification))
      return fc_selection{index, fc_selection_error::None};

  return fc_selection{-1, fc_selection_error::NotFound};
}

static fn report_fc_selection_error(const ExecContext &ec, EvalContext &cxt,
                                    SourceLocation location,
                                    fc_selection_error error) throws -> i32
{
  if (error == fc_selection_error::OutOfRange) {
    report_soft_builtin_error(ec, cxt, location,
                              "history specification out of range");
  } else {
    report_soft_builtin_error(ec, cxt, location, "no command found");
  }
  return 1;
}

static fn replace_all(StringView source, StringView pattern,
                      StringView replacement, Allocator allocator) throws
    -> String
{
  let result = String{allocator};
  if (pattern.is_empty()) {
    for (usize position = 0; position < source.length; position++)
      result.append(replacement);
    return result;
  }

  usize position = 0;
  while (position < source.length) {
    let const remaining = source.length - position;
    if (remaining >= pattern.length &&
        std::memcmp(source.data + position, pattern.data, pattern.length) == 0)
    {
      result.append(replacement);
      position += pattern.length;
    } else {
      result.push(source[position]);
      position++;
    }
  }

  return result;
}

static fn active_event_index(const ArrayList<toiletline::history_event> &events,
                             Maybe<usize> active_number) -> Maybe<usize>
{
  if (!active_number.has_value()) return None;
  for (usize index = events.count(); index-- > 0;)
    if (events[index].number == *active_number) return index;
  return None;
}

static fn remember_fc_command(
    EvalContext &cxt, const ArrayList<toiletline::history_event> &events,
    Maybe<usize> active_index, StringView command) throws -> bool
{
  if (command.is_empty()) return false;
  if (events.is_empty()) return false;

  let const replacement_index =
      active_index.has_value() ? *active_index : events.count() - 1;
  let const &replaced = events[replacement_index];
  if (!toiletline::history_rewrite_event(replaced.number,
                                         replaced.command.view(), command))
  {
    return false;
  }
  if (active_index.has_value()) cxt.set_current_history_event_number(None);

  return true;
}

static fn selected_source(const ArrayList<toiletline::history_event> &events,
                          i64 first_index, i64 last_index, bool should_reverse,
                          Allocator allocator) throws -> String
{
  let source = String{allocator};
  let index = should_reverse ? last_index : first_index;
  let const end_index = should_reverse ? first_index : last_index;
  let const step = should_reverse ? -1 : 1;

  for (; should_reverse ? index >= end_index : index <= end_index;
       index += step)
  {
    source.append(events[static_cast<usize>(index)].command.view());
    source.push('\n');
  }

  return source;
}

static fn execute_fc_command(const ExecContext &ec, EvalContext &cxt,
                             const ArrayList<String> &args,
                             const ArrayList<SourceLocation> &operand_locations,
                             const ArrayList<toiletline::history_event> &events,
                             Maybe<usize> active_index) throws -> i32
{
  usize operand_position = 1;
  struct substitution
  {
    StringView pattern;
    StringView replacement;
  };
  let substitutions = ArrayList<substitution>{cxt.scratch_allocator()};

  while (operand_position < args.count()) {
    let const operand = args[operand_position].view();
    let const separator = operand.find_character('=');
    if (!separator.has_value()) break;
    substitutions.push(substitution{operand.substring_of_length(0, *separator),
                                    operand.substring(*separator + 1)});
    operand_position++;
  }

  let const last_eligible_index = active_index.has_value()
                                      ? static_cast<i64>(*active_index) - 1
                                      : static_cast<i64>(events.count()) - 1;
  let selection =
      fc_selection{last_eligible_index, last_eligible_index < 0
                                            ? fc_selection_error::NotFound
                                            : fc_selection_error::None};
  if (operand_position < args.count()) {
    selection =
        resolve_fc_selection(args[operand_position].view(), events,
                             last_eligible_index, false, false, active_index);
    if (args[operand_position] == "-0" &&
        selection.error == fc_selection_error::OutOfRange)
    {
      selection.error = fc_selection_error::NotFound;
    }
  }
  if (selection.error != fc_selection_error::None) {
    let const location = operand_position < operand_locations.count()
                             ? operand_locations[operand_position]
                             : ec.source_location();
    return report_fc_selection_error(ec, cxt, location, selection.error);
  }

  if (cxt.is_posix_option_on() && operand_position + 1 < args.count()) {
    report_soft_builtin_error(ec, cxt, operand_locations[operand_position + 1],
                              "too many arguments");
    return 1;
  }

  let command = String{cxt.scratch_allocator(),
                       events[static_cast<usize>(selection.index)].command};
  for (let const &replacement : substitutions)
    command = replace_all(command.view(), replacement.pattern,
                          replacement.replacement, cxt.scratch_allocator());

  ec.print_to_stderr(command.view());
  ec.print_to_stderr("\n");
  if (!cxt.has_history_transaction() &&
      !remember_fc_command(cxt, events, active_index, command.view()))
  {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "cannot replace the history event");
    return 1;
  }
  return cxt.run_source(command.view(), "fc", return_handling::Reject,
                        ec.source_location(), StringView{"fc"});
}

static fn list_fc_commands(const ExecContext &ec, EvalContext &cxt,
                           const fc_options &options,
                           const ArrayList<toiletline::history_event> &events,
                           i64 first_index, i64 last_index) throws -> i32
{
  let output = String{cxt.scratch_allocator()};
  let index = options.should_reverse ? last_index : first_index;
  let const end_index = options.should_reverse ? first_index : last_index;
  let const step = options.should_reverse ? -1 : 1;

  for (; options.should_reverse ? index >= end_index : index <= end_index;
       index += step)
  {
    let const &event = events[static_cast<usize>(index)];
    if (options.should_number)
      output += String::from(event.number, cxt.scratch_allocator());
    output += cxt.is_posix_option_on() ? "\t" : "\t ";
    output.append(event.command.view());
    output.push('\n');
  }

  ec.print_to_stdout(output.view());
  return 0;
}

static fn editor_name(EvalContext &cxt, const fc_options &options) throws
    -> String
{
  if (options.editor.has_value())
    return String{cxt.scratch_allocator(), *options.editor};
  if (let value = cxt.get_variable_value("FCEDIT");
      value.has_value() && !value->is_empty())
  {
    return String{cxt.scratch_allocator(), value->view()};
  }
  if (let value = cxt.get_variable_value("EDITOR");
      value.has_value() && !value->is_empty())
  {
    return String{cxt.scratch_allocator(), value->view()};
  }
  return cxt.is_posix_option_on() ? String{"ed"} : String{"vi"};
}

static fn edit_fc_commands(const ExecContext &ec, EvalContext &cxt,
                           const fc_options &options,
                           const ArrayList<toiletline::history_event> &events,
                           Maybe<usize> active_index, i64 first_index,
                           i64 last_index) throws -> i32
{
  let const source =
      selected_source(events, first_index, last_index, options.should_reverse,
                      cxt.scratch_allocator());
  let temp_path = os::write_to_named_temp_file(Path::temp_directory(), "sfc",
                                               source.view());
  if (!temp_path.has_value()) {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "cannot create the editor file");
    return 1;
  }
  defer { unused(os::remove_file(temp_path->text().view())); };

  let editor_command = editor_name(cxt, options);
  editor_command.push(' ');
  append_shell_quoted_arg(editor_command, temp_path->text().view(), true);

  i32 editor_status = 1;
  {
    let const saved_terminal_exec = cxt.terminal_exec_allowed();
    cxt.set_terminal_exec_allowed(false);
    defer { cxt.set_terminal_exec_allowed(saved_terminal_exec); };
    editor_status = cxt.run_source(editor_command.view(), "fc editor",
                                   return_handling::Propagate,
                                   ec.source_location(), StringView{"fc"});
  }

  if (editor_status != 0) {
    return 1;
  }

  let edited = temp_path->read_entire_file();
  if (!edited.has_value()) {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "cannot read the editor file");
    return 1;
  }
  if (!os::remove_file(temp_path->text().view())) {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "cannot remove the editor file");
    return 1;
  }
  if (edited->is_empty()) return 0;

  edited->normalize_crlf_line_endings();
  let const ast_mark = AST_ARENA->mark();
  let const function_mark = FUNCTION_ARENA->mark();
  {
    defer
    {
      FUNCTION_ARENA->release(function_mark);
      AST_ARENA->release(ast_mark);
    };
    let parser = Parser{
        Lexer{String{edited->view()}, *AST_ARENA, false, None, cxt.mood()}
    };
    unused(parser.construct_ast());
  }

  ec.print_to_stderr(edited->view());
  if (edited->back() != '\n') ec.print_to_stderr("\n");

  let recorded_commands = ArrayList<String>{heap_allocator()};
  let const should_replace_active =
      active_index.has_value() && !cxt.has_history_transaction();
  let should_end_transaction = should_replace_active;
  if (should_end_transaction) cxt.begin_history_transaction(recorded_commands);
  defer
  {
    if (should_end_transaction) cxt.end_history_transaction();
  };

  let const status =
      cxt.run_source(edited->view(), "fc", return_handling::Consume,
                     ec.source_location(), StringView{"fc"}, true);
  if (should_end_transaction) {
    cxt.end_history_transaction();
    should_end_transaction = false;
  }

  if (should_replace_active) {
    let const &active = events[*active_index];
    if (!toiletline::history_rewrite_event(active.number, active.command.view(),
                                           recorded_commands))
    {
      report_soft_builtin_error(ec, cxt, ec.source_location(),
                                "Unable to replace the active history event");
      return 1;
    }
    cxt.set_current_history_event_number(None);
  }

  return status;
}

fn Fc::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args =
      parse_flags_vec(FLAG_LIST, ec.args(), ec.source_location().position,
                      nullptr, &ec.arg_locations(), &operand_locations,
                      builtin_error_context(ec.program()), true);
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  fc_options options{
      FLAG_FC_LIST.is_enabled(), !FLAG_FC_NO_NUMBERS.is_enabled(),
      FLAG_FC_REVERSE.is_enabled(), FLAG_FC_EXECUTE.is_enabled(),
      FLAG_FC_EDITOR.is_set() ? Maybe<StringView>{FLAG_FC_EDITOR.value()}
                              : None};
  if (options.editor.has_value() && *options.editor == "-")
    options.should_execute = true;

  let const events = toiletline::history_events(cxt.scratch_allocator());
  let const active_index =
      active_event_index(events, cxt.current_history_event_number());

  if (options.should_execute)
    return execute_fc_command(ec, cxt, args, operand_locations, events,
                              active_index);
  if (events.is_empty()) return 0;

  let const last_eligible_index = active_index.has_value()
                                      ? static_cast<i64>(*active_index) - 1
                                      : static_cast<i64>(events.count()) - 1;
  if (last_eligible_index < 0) return 0;

  i64 first_index = last_eligible_index;
  i64 last_index = last_eligible_index;
  usize operand_position = 1;

  if (operand_position < args.count()) {
    let const first = resolve_fc_selection(
        args[operand_position].view(), events, last_eligible_index,
        options.should_list, true, active_index);
    if (first.error != fc_selection_error::None)
      return report_fc_selection_error(
          ec, cxt, operand_locations[operand_position], first.error);
    first_index = first.index;

    if (operand_position + 1 < args.count()) {
      let const last = resolve_fc_selection(
          args[operand_position + 1].view(), events, last_eligible_index,
          options.should_list, false, active_index);
      if (last.error != fc_selection_error::None)
        return report_fc_selection_error(
            ec, cxt, operand_locations[operand_position + 1], last.error);
      last_index = last.index;
    } else if (first_index == static_cast<i64>(events.count()) - 1) {
      last_index = first_index;
    } else {
      last_index = options.should_list ? last_eligible_index : first_index;
    }
  } else if (options.should_list) {
    first_index = last_eligible_index - 15;
    if (first_index < 0) first_index = 0;
  }

  if (last_index < first_index) {
    let const saved_first = first_index;
    first_index = last_index;
    last_index = saved_first;
    options.should_reverse = true;
  }

  if (options.should_list)
    return list_fc_commands(ec, cxt, options, events, first_index, last_index);
  return edit_fc_commands(ec, cxt, options, events, active_index, first_index,
                          last_index);
}

} /* namespace shit */
