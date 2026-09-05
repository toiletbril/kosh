/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the sed utility in koshkit.
 * The sed utility edits text streams.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-En] [-e script]... [-f script-file]... [script] "
                   "[file ...]");

HELP_DESCRIPTION_DECL("The sed utility edits text streams.");

FLAG(SED_EXTENDED, Bool, 'E', "extended-regexp",
     "Use extended regular expressions.");
FLAG(SED_EXPRESSION, ManyStrings, 'e', "expression", "Add an editing script.");
FLAG(SED_FILE, ManyStrings, 'f', "file", "Read an editing script from a file.");
FLAG(SED_QUIET, Bool, 'n', "quiet", "Suppress automatic printing.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Sed);

namespace koshka::koshkit {

enum class sed_address_kind : u8
{
  Every,
  Line,
  Last,
  Regex,
};

enum class sed_command_kind : u8
{
  Substitute,
  Delete,
  Print,
  Quit,
  LineNumber,
  Append,
  Insert,
  Change,
  Translate,
};

struct sed_address
{
  sed_address_kind kind{sed_address_kind::Every};
  u64 line_number{0};
  os::compiled_regex expression{};
  bool has_expression{false};
};

struct sed_command
{
  sed_address address;
  sed_address second_address;
  sed_command_kind kind;
  os::compiled_regex expression{};
  bool has_expression{false};
  String replacement;
  bool is_global{false};
  bool should_print{false};
  bool is_negated{false};
  bool has_second_address{false};
  bool is_range_active{false};
};

static fn parse_sed_delimited(StringView script, usize &position,
                              char delimiter, Allocator allocator) throws
    -> String
{
  String text{allocator};

  while (position < script.length) {
    let const byte = script[position++];
    if (byte == delimiter) return text;
    if (byte == '\\' && position < script.length) {
      let const escaped = script[position++];
      if (escaped == delimiter)
        text += escaped;
      else {
        text += '\\';
        text += escaped;
      }
    } else {
      text += byte;
    }
  }

  throw Error{"sed: unterminated delimited expression"};
}

static fn compile_sed_expression(StringView expression, bool is_extended,
                                 os::compiled_regex &compiled) throws -> void
{
  let const result =
      is_extended ? os::compile_regex(expression,
                                      os::case_sensitivity::Sensitive, compiled)
                  : os::compile_basic_regex(
                        expression, os::case_sensitivity::Sensitive, compiled);
  if (result != os::regex_compile_result::Ok)
    throw Error{"sed: invalid regular expression '" + String{expression} + "'"};
}

static fn parse_sed_address(StringView script, usize &position,
                            bool is_extended, Allocator allocator,
                            sed_address &address) throws -> bool
{
  if (position == script.length) return false;
  if (script[position] >= '0' && script[position] <= '9') {
    u64 line_number = 0;

    while (position < script.length && script[position] >= '0' &&
           script[position] <= '9')
    {
      let const digit = static_cast<u64>(script[position++] - '0');
      if (line_number > (UINT64_MAX - digit) / 10)
        throw Error{"sed: line address is too large"};
      line_number = line_number * 10 + digit;
    }
    if (line_number == 0) throw Error{"sed: line addresses begin at one"};
    address.kind = sed_address_kind::Line;
    address.line_number = line_number;
    return true;
  }
  if (script[position] == '$') {
    position++;
    address.kind = sed_address_kind::Last;
    return true;
  }
  if (script[position] == '/') {
    position++;
    let const expression =
        parse_sed_delimited(script, position, '/', allocator);
    address.kind = sed_address_kind::Regex;
    compile_sed_expression(expression.view(), is_extended, address.expression);
    address.has_expression = true;
    return true;
  }
  return false;
}

static fn parse_sed_script(StringView script, bool is_extended,
                           Allocator allocator) throws -> ArrayList<sed_command>
{
  let commands = ArrayList<sed_command>{allocator};
  usize position = 0;

  while (position < script.length) {
    while (position < script.length &&
           (script[position] == ';' || script[position] == '\n' ||
            script[position] == ' ' || script[position] == '\t'))
      position++;
    if (position == script.length) break;
    if (script[position] == '#') {
      while (position < script.length && script[position] != '\n')
        position++;
      continue;
    }

    sed_address address{};
    unused(
        parse_sed_address(script, position, is_extended, allocator, address));
    sed_address second_address{};
    bool has_second_address = false;
    if (position < script.length && script[position] == ',') {
      position++;
      has_second_address = parse_sed_address(script, position, is_extended,
                                             allocator, second_address);
      if (!has_second_address) throw Error{"sed: missing second address"};
    }

    while (position < script.length &&
           (script[position] == ' ' || script[position] == '\t'))
      position++;
    bool is_negated = false;
    if (position < script.length && script[position] == '!') {
      position++;
      is_negated = true;
      while (position < script.length &&
             (script[position] == ' ' || script[position] == '\t'))
        position++;
    }
    if (position == script.length) throw Error{"sed: missing command"};

    let const command_byte = script[position++];
    sed_command command{steal(address),
                        steal(second_address),
                        sed_command_kind::Print,
                        {},
                        false,
                        String{allocator},
                        false,
                        false,
                        is_negated,
                        has_second_address,
                        false};
    switch (command_byte) {
    case 'd': command.kind = sed_command_kind::Delete; break;
    case 'p': command.kind = sed_command_kind::Print; break;
    case 'q': command.kind = sed_command_kind::Quit; break;
    case '=': command.kind = sed_command_kind::LineNumber; break;
    case 'a':
    case 'i':
    case 'c': {
      command.kind = command_byte == 'a'   ? sed_command_kind::Append
                     : command_byte == 'i' ? sed_command_kind::Insert
                                           : sed_command_kind::Change;
      while (position < script.length &&
             (script[position] == ' ' || script[position] == '\t'))
        position++;
      if (position < script.length && script[position] == '\\') position++;
      let const text_start = position;
      while (position < script.length && script[position] != '\n')
        position++;
      command.replacement =
          String{allocator,
                 script.substring_of_length(text_start, position - text_start)};
      break;
    }
    case 'y': {
      command.kind = sed_command_kind::Translate;
      if (position == script.length)
        throw Error{"sed: translation lacks a delimiter"};
      let const delimiter = script[position++];
      command.replacement =
          parse_sed_delimited(script, position, delimiter, allocator);
      let const destination =
          parse_sed_delimited(script, position, delimiter, allocator);
      if (command.replacement.length() != destination.length())
        throw Error{"sed: translation strings have different lengths"};
      command.replacement += destination.view();
      break;
    }
    case 's': {
      command.kind = sed_command_kind::Substitute;
      if (position == script.length)
        throw Error{"sed: substitution lacks a delimiter"};
      let const delimiter = script[position++];
      let const expression =
          parse_sed_delimited(script, position, delimiter, allocator);
      command.replacement =
          parse_sed_delimited(script, position, delimiter, allocator);
      compile_sed_expression(expression.view(), is_extended,
                             command.expression);
      command.has_expression = true;

      while (position < script.length && script[position] != ';' &&
             script[position] != '\n')
      {
        if (script[position] == 'g')
          command.is_global = true;
        else if (script[position] == 'p')
          command.should_print = true;
        else if (script[position] != ' ' && script[position] != '\t')
          throw Error{"sed: unsupported substitution flag"};
        position++;
      }
      break;
    }
    default:
      throw Error{
          "sed: unsupported command '" +
          String{allocator, StringView{&command_byte, 1}}
          + "'"
      };
    }

    commands.push(steal(command));
  }

  return commands;
}

static fn sed_address_matches(sed_address &address, StringView line,
                              u64 line_number, bool is_last) throws -> bool
{
  switch (address.kind) {
  case sed_address_kind::Every: return true;
  case sed_address_kind::Line: return line_number == address.line_number;
  case sed_address_kind::Last: return is_last;
  case sed_address_kind::Regex:
    return os::regex_matches(address.expression, line);
  }
  return false;
}

static fn sed_command_matches(sed_command &command, StringView line,
                              u64 line_number, bool is_last) throws -> bool
{
  if (!command.has_second_address)
    return sed_address_matches(command.address, line, line_number, is_last);
  if (!command.is_range_active) {
    if (!sed_address_matches(command.address, line, line_number, is_last))
      return false;
    let const did_end_immediately =
        command.second_address.kind == sed_address_kind::Line &&
        command.second_address.line_number <= line_number;
    command.is_range_active = !did_end_immediately;
    return true;
  }

  if (sed_address_matches(command.second_address, line, line_number, is_last))
    command.is_range_active = false;
  return true;
}

static fn append_sed_replacement(String &output, StringView replacement,
                                 StringView subject,
                                 const ArrayList<os::regex_span> &spans) throws
    -> void
{
  for (usize position = 0; position < replacement.length; position++) {
    let const byte = replacement[position];
    if (byte == '&' && !spans.is_empty()) {
      output += subject.substring_of_length(
          static_cast<usize>(spans[0].start),
          static_cast<usize>(spans[0].end - spans[0].start));
    } else if (byte == '\\' && position + 1 < replacement.length) {
      let const escaped = replacement[++position];
      if (escaped >= '1' && escaped <= '9') {
        let const group = static_cast<usize>(escaped - '0');
        if (group < spans.count() && spans[group].start >= 0)
          output += subject.substring_of_length(
              static_cast<usize>(spans[group].start),
              static_cast<usize>(spans[group].end - spans[group].start));
      } else {
        output += escaped;
      }
    } else {
      output += byte;
    }
  }
}

static fn append_sed_pattern_space(String &output, StringView line,
                                   bool has_terminating_newline) throws -> void
{
  output += line;
  if (has_terminating_newline) output += '\n';
}

static fn apply_sed_substitution(sed_command &command, String &line,
                                 Allocator allocator) throws -> bool
{
  String result{allocator};
  usize consumed = 0;
  bool did_replace = false;
  bool did_previous_match_consume = false;

  while (consumed <= line.length()) {
    let const subject = line.view().substring(consumed);
    let spans = ArrayList<os::regex_span>{allocator};
    String error_message{allocator};
    let const match_result =
        os::execute_regex(command.expression, subject, spans, error_message,
                          allocator, consumed != 0);
    if (match_result == os::regex_match_result::Error)
      throw Error{"sed: " + error_message};
    if (match_result == os::regex_match_result::NoMatch) {
      result += subject;
      break;
    }

    ASSERT(!spans.is_empty());
    let const match_start = static_cast<usize>(spans[0].start);
    let const match_end = static_cast<usize>(spans[0].end);
    result += subject.substring_of_length(0, match_start);

    if (match_start == 0 && match_end == 0 && did_previous_match_consume) {
      if (consumed == line.length()) break;
      result += line[consumed++];
      did_previous_match_consume = false;
      continue;
    }

    append_sed_replacement(result, command.replacement.view(), subject, spans);
    did_replace = true;
    consumed += match_end;

    if (!command.is_global) {
      result += line.view().substring(consumed);
      break;
    }
    if (match_end == match_start) {
      if (consumed == line.length()) break;
      result += line[consumed++];
      did_previous_match_consume = false;
    } else {
      did_previous_match_consume = true;
    }
  }

  if (did_replace) line = steal(result);
  return did_replace;
}

Sed::Sed() = default;

pure fn Sed::kind() const wontthrow -> Utility::Kind { return Kind::Sed; }

fn Sed::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  String script{cxt.scratch_allocator()};
  for (usize index = 0; index < FLAG_SED_EXPRESSION.count(); index++) {
    if (!script.is_empty()) script += '\n';
    script += FLAG_SED_EXPRESSION.get(index);
  }
  for (usize index = 0; index < FLAG_SED_FILE.count(); index++) {
    let const file_script = read_named_or_stdin(ec, FLAG_SED_FILE.get(index));
    if (!file_script.has_value())
      throw Error{"sed: cannot read script file '" +
                  String{FLAG_SED_FILE.get(index)} + "'"};
    if (!script.is_empty()) script += '\n';
    script += file_script->view();
  }

  usize source_start = 0;
  if (script.is_empty()) {
    if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
    script += operands[0].view();
    source_start = 1;
  }

  let commands = parse_sed_script(script.view(), FLAG_SED_EXTENDED.is_enabled(),
                                  cxt.scratch_allocator());
  defer
  {
    for (let &command : commands) {
      if (command.address.has_expression)
        os::free_regex(command.address.expression);
      if (command.second_address.has_expression)
        os::free_regex(command.second_address.expression);
      if (command.has_expression) os::free_regex(command.expression);
    }
  };

  let const sources = source_list_from_operands(
      operands, cxt.scratch_allocator(), source_start);
  let contents = ArrayList<String>{cxt.scratch_allocator()};
  contents.reserve(sources.count());
  let lines = ArrayList<StringView>{cxt.scratch_allocator()};
  i32 status = 0;

  for (let const source : sources) {
    let content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "sed: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 2;
      continue;
    }
    contents.push(steal(*content));
    for (let const line : utils::split_lines(contents.back().view(),
                                             cxt.scratch_allocator(), true))
      lines.push(line);
  }

  let output = String{cxt.scratch_allocator()};
  bool should_quit = false;
  for (usize line_index = 0; line_index < lines.count() && !should_quit;
       line_index++)
  {
    let const source_line = lines[line_index];
    let const has_terminating_newline =
        !source_line.is_empty() && source_line[source_line.length - 1] == '\n';
    String line{cxt.scratch_allocator(),
                source_line.without_trailing_newline()};
    String appended_text{cxt.scratch_allocator()};
    bool should_delete = false;
    let const line_number = static_cast<u64>(line_index + 1);

    for (let &command : commands) {
      let is_match = sed_command_matches(command, line.view(), line_number,
                                         line_index + 1 == lines.count());
      if (command.is_negated) is_match = !is_match;
      if (!is_match) continue;

      switch (command.kind) {
      case sed_command_kind::Substitute:
        if (apply_sed_substitution(command, line, cxt.scratch_allocator()) &&
            command.should_print)
        {
          append_sed_pattern_space(output, line.view(),
                                   has_terminating_newline);
        }
        break;
      case sed_command_kind::Delete: should_delete = true; break;
      case sed_command_kind::Print:
        append_sed_pattern_space(output, line.view(), has_terminating_newline);
        break;
      case sed_command_kind::Quit: should_quit = true; break;
      case sed_command_kind::LineNumber:
        output += String::from(line_number, cxt.scratch_allocator());
        output += '\n';
        break;
      case sed_command_kind::Append:
        appended_text += command.replacement.view();
        appended_text += '\n';
        break;
      case sed_command_kind::Insert:
        output += command.replacement.view();
        output += '\n';
        break;
      case sed_command_kind::Change:
        if (!command.has_second_address || !command.is_range_active ||
            line_index + 1 == lines.count())
        {
          output += command.replacement.view();
          output += '\n';
        }
        should_delete = true;
        break;
      case sed_command_kind::Translate: {
        let const source_length = command.replacement.length() / 2;
        char translation[256];

        for (u16 byte = 0; byte < 256; byte++)
          translation[byte] = static_cast<char>(byte);
        for (usize source_position = 0; source_position < source_length;
             source_position++)
          translation[static_cast<u8>(command.replacement[source_position])] =
              command.replacement[source_length + source_position];

        String translated{cxt.scratch_allocator()};
        translated.reserve(line.length());
        for (usize position = 0; position < line.length(); position++)
          translated += translation[static_cast<u8>(line[position])];
        line = steal(translated);
        break;
      }
      }
      if (should_delete || should_quit) break;
    }

    if (!should_delete && !FLAG_SED_QUIET.is_enabled()) {
      append_sed_pattern_space(output, line.view(), has_terminating_newline);
    }
    output += appended_text.view();
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
