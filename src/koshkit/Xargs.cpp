#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ptx] [-E eof] [-I replace] [-L lines] [-n count] [-s "
                   "bytes] [utility [argument ...]]");

HELP_DESCRIPTION_DECL(
    "The xargs utility builds and invokes argument lists from standard input.");

FLAG(XARGS_EOF, String, 'E', "eof", "Stop at this logical argument.");
FLAG(XARGS_REPLACE, String, 'I', "replace",
     "Replace this string with each input line.");
FLAG(XARGS_MAX_LINES, String, 'L', "max-lines",
     "Use at most this many input lines per invocation.");
FLAG(XARGS_MAX_ARGUMENTS, String, 'n', "max-args",
     "Use at most this many input arguments per invocation.");
FLAG(XARGS_PROMPT, Bool, 'p', "prompt", "Ask before each invocation.");
FLAG(XARGS_MAX_SIZE, String, 's', "max-size",
     "Limit each command to this many bytes.");
FLAG(XARGS_TRACE, Bool, 't', "trace", "Write each command before invoking it.");
FLAG(XARGS_EXIT, Bool, 'x', "exit", "Stop when the size limit cannot be met.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Xargs);

namespace koshka::koshkit {

struct xargs_item
{
  String value;
  usize line_number;
};

static fn parse_xargs_limit(StringView value, StringView name) throws -> usize
{
  let const parsed = utils::parse_decimal_u64(value);
  if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX)
    throw Error{"xargs: invalid " + String{name} + " '" + String{value} + "'"};
  return static_cast<usize>(parsed.value());
}

static fn parse_xargs_items(StringView input, Allocator allocator) throws
    -> ArrayList<xargs_item>
{
  let items = ArrayList<xargs_item>{allocator};
  usize position = 0;
  usize line_number = 0;
  while (position < input.length) {
    while (position < input.length &&
           (input[position] == ' ' || input[position] == '\t' ||
            input[position] == '\n'))
    {
      if (input[position] == '\n') line_number++;
      position++;
    }
    if (position == input.length) break;

    let value = String{allocator};
    let const item_line = line_number;
    char quote = '\0';
    while (position < input.length) {
      let const byte = input[position++];
      if (quote != '\0') {
        if (byte == quote) {
          quote = '\0';
        } else {
          if (byte == '\n')
            throw Error{"xargs: unmatched quote before newline"};
          value += byte;
        }
        continue;
      }
      if (byte == '\'' || byte == '"') {
        quote = byte;
        continue;
      }
      if (byte == '\\') {
        if (position == input.length) throw Error{"xargs: trailing backslash"};
        let const escaped = input[position++];
        if (escaped == '\n') {
          line_number++;
        } else {
          value += escaped;
        }
        continue;
      }
      if (byte == ' ' || byte == '\t' || byte == '\n') {
        if (byte == '\n') line_number++;
        break;
      }
      value += byte;
    }
    if (quote != '\0') throw Error{"xargs: unmatched quote"};
    items.push(xargs_item{steal(value), item_line});
  }
  return items;
}

static fn parse_xargs_lines(StringView input, Allocator allocator) throws
    -> ArrayList<xargs_item>
{
  let items = ArrayList<xargs_item>{allocator};
  usize position = 0;
  usize line_number = 0;
  while (position < input.length) {
    let const remaining = input.substring(position);
    let const line_length =
        remaining.find_character('\n').value_or(remaining.length);
    let const line =
        remaining.substring_of_length(0, line_length).trim_blanks();
    if (!line.is_empty())
      items.push(xargs_item{
          String{allocator, line},
          line_number
      });
    position += line_length + (line_length < remaining.length ? 1 : 0);
    line_number++;
  }
  return items;
}

static fn append_replaced(String &output, StringView source, StringView needle,
                          StringView replacement) throws -> void
{
  if (needle.is_empty()) {
    output += source;
    return;
  }
  usize position = 0;
  while (position + needle.length <= source.length) {
    if (source.substring_of_length(position, needle.length) == needle) {
      output += replacement;
      position += needle.length;
    } else {
      output += source[position++];
    }
  }
  output += source.substring(position);
}

static fn xargs_command_size(const ArrayList<String> &command) wontthrow
    -> usize
{
  usize size = 0;
  for (let const &argument : command) {
    if (argument.length() > SIZE_MAX - size - 1) return SIZE_MAX;
    size += argument.length() + 1;
  }
  return size;
}

static fn trace_xargs_command(const ExecContext &ec,
                              const ArrayList<String> &command,
                              Allocator allocator) throws -> void
{
  let output = String{allocator};
  for (usize index = 0; index < command.count(); index++) {
    if (index != 0) output += ' ';
    output += command[index].view();
  }
  output += '\n';
  ec.print_to_stderr(output);
}

Xargs::Xargs() = default;

pure fn Xargs::kind() const wontthrow -> Utility::Kind { return Kind::Xargs; }

fn Xargs::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_XARGS_PROMPT.is_enabled()) FLAG_XARGS_TRACE.enable();
  let const input = read_fd_to_string(ec.in_fd.value_or(KOSH_STDIN));
  if (!input.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "xargs: " + os::last_system_error_message());
    return 1;
  }

  let items = FLAG_XARGS_REPLACE.is_set()
                  ? parse_xargs_lines(input->view(), cxt.scratch_allocator())
                  : parse_xargs_items(input->view(), cxt.scratch_allocator());
  if (FLAG_XARGS_EOF.is_set()) {
    usize kept_count = 0;
    while (kept_count < items.count() &&
           items[kept_count].value.view() != FLAG_XARGS_EOF.value())
      kept_count++;
    while (items.count() > kept_count)
      items.pop_back();
  }

  let base = ArrayList<String>{cxt.scratch_allocator()};
  let base_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  if (operands.is_empty()) {
    base.push(String{"echo"});
    base_locations.push(ec.source_location());
  } else {
    for (usize index = 0; index < operands.count(); index++) {
      let const &operand = operands[index];
      base.push(operand.clone());
      base_locations.push(operand_locations[index]);
    }
  }
  let const base_size = xargs_command_size(base);
  let const maximum_arguments =
      FLAG_XARGS_MAX_ARGUMENTS.is_set()
          ? parse_xargs_limit(FLAG_XARGS_MAX_ARGUMENTS.value(),
                              "argument count")
          : SIZE_MAX;
  let const maximum_lines =
      FLAG_XARGS_MAX_LINES.is_set()
          ? parse_xargs_limit(FLAG_XARGS_MAX_LINES.value(), "line count")
          : SIZE_MAX;
  let const maximum_size =
      FLAG_XARGS_MAX_SIZE.is_set()
          ? parse_xargs_limit(FLAG_XARGS_MAX_SIZE.value(), "byte count")
          : usize{131072};

  usize item_position = 0;
  bool should_run_empty = items.is_empty();
  i32 status = 0;
  while (item_position < items.count() || should_run_empty) {
    should_run_empty = false;
    let command = base.clone();
    let command_locations = base_locations.clone();
    if (FLAG_XARGS_REPLACE.is_set()) {
      let const replacement = items[item_position++].value.view();
      for (usize index = 1; index < command.count(); index++) {
        let replaced = String{cxt.scratch_allocator()};
        append_replaced(replaced, command[index].view(),
                        FLAG_XARGS_REPLACE.value(), replacement);
        command[index] = steal(replaced);
      }
    } else {
      let const first_line = item_position < items.count()
                                 ? items[item_position].line_number
                                 : usize{0};
      let command_size = base_size;
      usize added_count = 0;
      while (item_position < items.count() && added_count < maximum_arguments &&
             items[item_position].line_number - first_line < maximum_lines)
      {
        let const argument_size = items[item_position].value.length();
        let const candidate_size =
            command_size == SIZE_MAX ||
                    argument_size > SIZE_MAX - command_size - 1
                ? SIZE_MAX
                : command_size + argument_size + 1;
        if (candidate_size > maximum_size) {
          if (added_count == 0) {
            if (FLAG_XARGS_EXIT.is_enabled())
              throw Error{"xargs: one argument exceeds the size limit"};
            command.push(items[item_position].value.clone());
            command_locations.push(ec.source_location());
            item_position++;
          }
          break;
        }
        command.push(items[item_position].value.clone());
        command_locations.push(ec.source_location());
        command_size = candidate_size;
        item_position++;
        added_count++;
      }
    }

    if (FLAG_XARGS_TRACE.is_enabled())
      trace_xargs_command(ec, command, cxt.scratch_allocator());
    if (FLAG_XARGS_PROMPT.is_enabled())
      throw Error{
          "xargs: interactive prompting requires a controlling terminal"};
    Maybe<ExecContext> sub;
    try {
      let const *source = cxt.current_source();
      sub = ExecContext::make_from(
          ec.source_location(),
          source != nullptr ? source->view() : StringView{}, steal(command),
          cxt.mood(), cxt.koshkit(), cxt.get_program_resolver(),
          steal(command_locations));
    } catch (const CommandResolutionErrorWithLocation &resolution_error) {
      let const *source = cxt.current_source();
      show_message(resolution_error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      return static_cast<i32>(resolution_error.command_status());
    }

    let snapshot = cxt.snapshot_state();
    cxt.enter_subshell();
    i32 command_status = 0;
    try {
      command_status =
          utils::execute_context(steal(*sub), cxt, execution_mode::Foreground);
    } catch (...) {
      cxt.leave_subshell();
      cxt.restore_state(steal(snapshot));
      throw;
    }
    if (cxt.has_pending_control_flow()) {
      command_status = static_cast<i32>(cxt.pending_control_flow().value);
      cxt.clear_control_flow();
    }
    cxt.leave_subshell();
    cxt.restore_state(steal(snapshot));

    if (command_status == 255) return 124;
    if (command_status != 0) status = 1;
  }
  return status;
}

} // namespace koshka::koshkit
