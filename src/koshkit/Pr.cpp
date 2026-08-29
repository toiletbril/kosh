#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[+page] [-column] [-adFfmrt] [-h header] [-l length] "
                   "[-n] [-o offset] [-s separator] [-w width] [file ...]");

HELP_DESCRIPTION_DECL("The pr utility paginates text files.");

FLAG(PR_ACROSS, Bool, 'a', "across", "Write columns across each row.");
FLAG(PR_DOUBLE_SPACE, Bool, 'd', "double-space", "Double-space output lines.");
FLAG(PR_FORM_FEED, Bool, 'F', "form-feed", "Use form feeds between pages.");
FLAG(PR_FORM_FEED_ALIAS, Bool, 'f', "form-feed-alias",
     "Use form feeds between pages.");
FLAG(PR_HEADER, String, 'h', "header", "Use this page header.");
FLAG(PR_LENGTH, String, 'l', "length", "Use this page length.");
FLAG(PR_MERGE, Bool, 'm', "merge", "Write files in parallel columns.");
FLAG(PR_NUMBER, Bool, 'n', "number-lines", "Number output lines.");
FLAG(PR_OFFSET, String, 'o', "indent", "Indent each output line.");
FLAG(PR_NO_ERRORS, Bool, 'r', "no-file-warnings", "Suppress file warnings.");
FLAG(PR_NO_HEADER, Bool, 't', "omit-header", "Omit page headers and trailers.");
FLAG(PR_SEPARATOR, String, 's', "separator",
     "Separate columns with this text.");
FLAG(PR_WIDTH, String, 'w', "width", "Use this page width.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Pr);

namespace koshka::koshkit {

static fn parse_pr_number(StringView text, StringView name) throws -> usize
{
  let const parsed = utils::parse_decimal_u64(text);
  if (parsed.is_error() || parsed.value() > SIZE_MAX)
    throw Error{"pr: invalid " + String{name} + " '" + String{text} + "'"};
  return static_cast<usize>(parsed.value());
}

static fn append_pr_indent(String &output, usize offset) throws -> void
{
  for (usize position = 0; position < offset; position++)
    output += ' ';
}

Pr::Pr() = default;

pure fn Pr::kind() const wontthrow -> Utility::Kind { return Kind::Pr; }

fn Pr::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let filtered_args = ArrayList<String>{cxt.scratch_allocator()};
  let filtered_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  filtered_args.push(args[0].clone());
  filtered_locations.push(arg_locations[0]);
  usize column_count = 1;
  usize first_page = 1;
  for (usize index = 1; index < args.count(); index++) {
    let const argument = args[index].view();
    let numeric = argument;
    bool is_page = false;
    bool is_column = false;
    if (numeric.length > 1 && numeric[0] == '+') {
      is_page = true;
      numeric = numeric.substring(1);
    } else if (numeric.length > 1 && numeric[0] == '-' && numeric[1] >= '0' &&
               numeric[1] <= '9')
    {
      is_column = true;
      numeric = numeric.substring(1);
    }
    if (is_page || is_column) {
      let const parsed = utils::parse_decimal_u64(numeric);
      if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX)
        throw Error{"pr: invalid numeric layout operand '" + args[index] + "'"};
      if (is_page)
        first_page = static_cast<usize>(parsed.value());
      else
        column_count = static_cast<usize>(parsed.value());
      continue;
    }
    filtered_args.push(args[index].clone());
    filtered_locations.push(arg_locations[index]);
  }
  let const operands =
      parse_util_operands(FLAG_LIST, filtered_args, &filtered_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const page_length =
      FLAG_PR_LENGTH.is_set()
          ? parse_pr_number(FLAG_PR_LENGTH.value(), "length")
          : 66;
  let const page_width = FLAG_PR_WIDTH.is_set()
                             ? parse_pr_number(FLAG_PR_WIDTH.value(), "width")
                             : 72;
  let const offset = FLAG_PR_OFFSET.is_set()
                         ? parse_pr_number(FLAG_PR_OFFSET.value(), "offset")
                         : 0;
  if (!FLAG_PR_NO_HEADER.is_enabled() && page_length < 10)
    throw Error{"pr: page length is too small for headers"};

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  if (FLAG_PR_MERGE.is_enabled()) {
    let contents = ArrayList<String>{cxt.scratch_allocator()};
    contents.reserve(sources.count());
    for (let const source : sources) {
      let content = read_named_or_stdin(ec, source);
      if (!content.has_value()) {
        if (!FLAG_PR_NO_ERRORS.is_enabled())
          report_soft_koshkit_error(
              ec, cxt,
              "pr: cannot read '" + String{source} +
                  "': " + os::last_system_error_message());
        status = 1;
        contents.push(String{cxt.scratch_allocator()});
      } else {
        contents.push(steal(*content));
      }
    }
    let columns = ArrayList<ArrayList<StringView>>{cxt.scratch_allocator()};
    columns.reserve(contents.count());
    usize row_count = 0;
    for (let &content : contents) {
      let lines =
          utils::split_lines(content.view(), cxt.scratch_allocator(), true);
      for (let &line : lines)
        line = line.without_trailing_newline();
      if (lines.count() > row_count) row_count = lines.count();
      columns.push(steal(lines));
    }
    let const separator = FLAG_PR_SEPARATOR.is_set() ? FLAG_PR_SEPARATOR.value()
                                                     : StringView{"\t"};
    for (usize row = 0; row < row_count; row++) {
      append_pr_indent(output, offset);
      bool has_output_column = false;
      for (let const &column : columns) {
        if (row >= column.count()) continue;
        if (has_output_column) output += separator;
        output += column[row];
        has_output_column = true;
      }
      output += '\n';
    }
    ec.print_to_stdout(output);
    return status;
  }

  for (let const source : sources) {
    let const content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      if (!FLAG_PR_NO_ERRORS.is_enabled())
        report_soft_koshkit_error(ec, cxt,
                                  "pr: cannot read '" +
                                      String{cxt.scratch_allocator(), source} +
                                      "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let lines =
        utils::split_lines(content->view(), cxt.scratch_allocator(), true);
    for (let &line : lines)
      line = line.without_trailing_newline();
    let const data_line_limit =
        FLAG_PR_NO_HEADER.is_enabled() ? page_length : page_length - 10;
    usize line_index = 0;
    usize page_number = 1;
    u64 source_line_number = 1;

    while (line_index < lines.count()) {
      if (data_line_limit == 0 || column_count > SIZE_MAX / data_line_limit)
        throw Error{"pr: page capacity is invalid"};
      let const page_capacity = data_line_limit * column_count;
      let const remaining_count = lines.count() - line_index;
      let const page_item_count =
          remaining_count < page_capacity ? remaining_count : page_capacity;
      if (page_number < first_page) {
        line_index += page_item_count;
        source_line_number += page_item_count;
        page_number++;
        continue;
      }
      if (!FLAG_PR_NO_HEADER.is_enabled()) {
        output += "\n\n";
        append_pr_indent(output, offset);
        let const title = FLAG_PR_HEADER.is_set() ? FLAG_PR_HEADER.value()
                          : source == "-"         ? StringView{}
                                                  : source;
        output += title;
        output += "  Page ";
        output += String::from(page_number, cxt.scratch_allocator());
        output += "\n\n\n";
      }

      usize emitted_page_lines = 0;
      if (column_count > 1) {
        let const row_count =
            (page_item_count + column_count - 1) / column_count;
        let const separator = FLAG_PR_SEPARATOR.is_set()
                                  ? FLAG_PR_SEPARATOR.value()
                                  : StringView{"\t"};
        for (usize row = 0; row < row_count; row++) {
          append_pr_indent(output, offset);
          bool has_output_column = false;
          for (usize column = 0; column < column_count; column++) {
            let const relative_index = FLAG_PR_ACROSS.is_enabled()
                                           ? row * column_count + column
                                           : column * row_count + row;
            if (relative_index >= page_item_count) continue;
            if (has_output_column) output += separator;
            let const line = lines[line_index + relative_index];
            let const available_width =
                page_width > offset ? page_width - offset : 0;
            output += line.substring_of_length(0, line.length < available_width
                                                      ? line.length
                                                      : available_width);
            has_output_column = true;
          }
          output += '\n';
          emitted_page_lines++;
          if (FLAG_PR_DOUBLE_SPACE.is_enabled() &&
              emitted_page_lines < data_line_limit)
          {
            output += '\n';
            emitted_page_lines++;
          }
        }
        line_index += page_item_count;
        source_line_number += page_item_count;
      } else {
        while (line_index < lines.count() &&
               emitted_page_lines < data_line_limit)
        {
          append_pr_indent(output, offset);
          if (FLAG_PR_NUMBER.is_enabled()) {
            let const digits =
                String::from(source_line_number++, cxt.scratch_allocator());
            for (usize position = digits.length(); position < 5; position++)
              output += ' ';
            output += digits.view();
            output += '\t';
          }
          let const line = lines[line_index++];
          let const available_width =
              page_width > offset ? page_width - offset : 0;
          output += line.substring_of_length(
              0, line.length < available_width ? line.length : available_width);
          output += '\n';
          emitted_page_lines++;
          if (FLAG_PR_DOUBLE_SPACE.is_enabled() &&
              emitted_page_lines < data_line_limit)
          {
            output += '\n';
            emitted_page_lines++;
          }
        }
      }

      if (!FLAG_PR_NO_HEADER.is_enabled()) {
        while (emitted_page_lines++ < data_line_limit)
          output += '\n';
        output += "\n\n\n\n\n";
      }
      if (line_index < lines.count())
        output += FLAG_PR_FORM_FEED.is_enabled() ||
                          FLAG_PR_FORM_FEED_ALIAS.is_enabled()
                      ? '\f'
                      : '\n';
      page_number++;
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
