#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ceisu] [-n lines] [-p command] [-t tag] [file ...]");

HELP_DESCRIPTION_DECL("The more utility displays text one screen at a time.");

FLAG(MORE_CLEAR, Bool, 'c', "clear", "Clear each screen before displaying it.");
FLAG(MORE_EXIT, Bool, 'e', "exit", "Exit at end of input.");
FLAG(MORE_CASE, Bool, 'i', "ignore-case", "Ignore case in searches.");
FLAG(MORE_SQUEEZE, Bool, 's', "squeeze", "Collapse repeated blank lines.");
FLAG(MORE_PLAIN, Bool, 'u', "plain", "Suppress terminal underline handling.");
FLAG(MORE_LINES, String, 'n', "lines", "Use this many lines per screen.");
FLAG(MORE_COMMAND, String, 'p', "command", "Run this initial pager command.");
FLAG(MORE_TAG, String, 't', "tag", "Open the file containing this tag.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(More);

namespace koshka::koshkit {

More::More() = default;

pure fn More::kind() const wontthrow -> Utility::Kind { return Kind::More; }

fn More::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_MORE_COMMAND.is_set() && FLAG_MORE_TAG.is_set())
    return report_usage_error(ec, cxt, args[0].view());

  let tagged_source = String{cxt.scratch_allocator()};
  usize tagged_line_number = 1;
  if (FLAG_MORE_TAG.is_set()) {
    if (!operands.is_empty())
      return report_usage_error(ec, cxt, args[0].view());
    let const tags = Path{"tags"}.read_entire_file();
    if (!tags.has_value()) {
      report_soft_koshkit_error(ec, cxt, "more: cannot read tags");
      return 1;
    }
    usize position = 0;
    while (position < tags->length()) {
      let const remaining = tags->view().substring(position);
      let const line_length =
          remaining.find_character('\n').value_or(remaining.length);
      let const line = remaining.substring_of_length(0, line_length);
      let const first_tab = line.find_character('\t');
      if (first_tab.has_value() &&
          line.substring_of_length(0, *first_tab) == FLAG_MORE_TAG.value())
      {
        let const after_name = line.substring(*first_tab + 1);
        let const second_tab = after_name.find_character('\t');
        if (second_tab.has_value()) {
          tagged_source =
              String{cxt.scratch_allocator(),
                     after_name.substring_of_length(0, *second_tab)};
          let const parsed =
              utils::parse_decimal_u64(after_name.substring(*second_tab + 1));
          if (!parsed.is_error() && parsed.value() != 0 &&
              parsed.value() <= SIZE_MAX)
            tagged_line_number = static_cast<usize>(parsed.value());
          break;
        }
      }
      position += line_length + (line_length < remaining.length ? 1 : 0);
    }
    if (tagged_source.is_empty()) {
      report_soft_koshkit_error(ec, cxt,
                                "more: tag not found '" +
                                    String{FLAG_MORE_TAG.value()} + "'");
      return 1;
    }
  }

  let sources = source_list_from_operands(operands, cxt.scratch_allocator());
  if (FLAG_MORE_TAG.is_set()) {
    sources.clear();
    sources.push(tagged_source.view());
  }
  let const should_buffer = FLAG_MORE_COMMAND.is_set() ||
                            FLAG_MORE_TAG.is_set() ||
                            FLAG_MORE_SQUEEZE.is_enabled();
  i32 status = 0;
  for (let const source : sources) {
    if (should_buffer) {
      let const input = read_named_or_stdin(ec, source);
      if (!input.has_value()) {
        report_soft_koshkit_error(ec, cxt,
                                  "more: cannot read '" + String{source} +
                                      "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      usize output_start = 0;
      let line_number = usize{1};
      let const requested_line_number =
          FLAG_MORE_TAG.is_set() ? tagged_line_number : usize{1};
      while (line_number < requested_line_number &&
             output_start < input->length())
      {
        let const newline =
            input->view().substring(output_start).find_character('\n');
        if (!newline.has_value()) {
          output_start = input->length();
          break;
        }
        output_start += *newline + 1;
        line_number++;
      }
      if (FLAG_MORE_COMMAND.is_set()) {
        let command = FLAG_MORE_COMMAND.value();
        if (!command.is_empty() && command[0] == '/')
          command = command.substring(1);
        let pattern = String{cxt.scratch_allocator(), command};
        if (FLAG_MORE_CASE.is_enabled()) pattern.lowercase_ascii();
        usize search_position = output_start;
        while (search_position < input->length()) {
          let const remaining = input->view().substring(search_position);
          let const line_length =
              remaining.find_character('\n').value_or(remaining.length);
          let searchable =
              String{cxt.scratch_allocator(),
                     remaining.substring_of_length(0, line_length)};
          if (FLAG_MORE_CASE.is_enabled()) searchable.lowercase_ascii();
          if (std::strstr(searchable.c_str(), pattern.c_str()) != NULL) {
            output_start = search_position;
            break;
          }
          search_position +=
              line_length + (line_length < remaining.length ? 1 : 0);
        }
      }
      let const selected = input->view().substring(output_start);
      if (!FLAG_MORE_SQUEEZE.is_enabled()) {
        ec.print_to_stdout(selected);
        continue;
      }
      let squeezed = String{cxt.scratch_allocator()};
      bool was_blank = false;
      usize position = 0;
      while (position < selected.length) {
        let const remaining = selected.substring(position);
        let const line_length =
            remaining.find_character('\n').value_or(remaining.length);
        let const is_blank = line_length == 0;
        if (!is_blank || !was_blank) {
          squeezed += remaining.substring_of_length(0, line_length);
          if (line_length < remaining.length) squeezed += '\n';
        }
        was_blank = is_blank;
        position += line_length + (line_length < remaining.length ? 1 : 0);
      }
      ec.print_to_stdout(squeezed.view());
      continue;
    }

    let const input = open_named_or_stdin(ec, source);
    if (!input.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "more: cannot read '" + String{source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }
    defer
    {
      if (input->should_close) os::close_fd(input->descriptor);
    };
    char buffer[65536];
    loop
    {
      let const count = os::read_fd(input->descriptor, buffer, sizeof(buffer));
      if (!count.has_value()) {
        status = 1;
        break;
      }
      if (*count == 0) break;
      ec.print_to_stdout(StringView{buffer, *count});
    }
  }
  return status;
}

} // namespace koshka::koshkit
