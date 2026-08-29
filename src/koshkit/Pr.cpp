#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-dFfrt] [-h header] [-l length] [-n] [-o offset] "
                   "[-w width] [file ...]");

HELP_DESCRIPTION_DECL("The pr utility paginates text files.");

FLAG(PR_DOUBLE_SPACE, Bool, 'd', "double-space", "Double-space output lines.");
FLAG(PR_FORM_FEED, Bool, 'F', "form-feed", "Use form feeds between pages.");
FLAG(PR_FORM_FEED_ALIAS, Bool, 'f', "form-feed-alias",
     "Use form feeds between pages.");
FLAG(PR_HEADER, String, 'h', "header", "Use this page header.");
FLAG(PR_LENGTH, String, 'l', "length", "Use this page length.");
FLAG(PR_NUMBER, Bool, 'n', "number-lines", "Number output lines.");
FLAG(PR_OFFSET, String, 'o', "indent", "Indent each output line.");
FLAG(PR_NO_ERRORS, Bool, 'r', "no-file-warnings", "Suppress file warnings.");
FLAG(PR_NO_HEADER, Bool, 't', "omit-header", "Omit page headers and trailers.");
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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
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
      while (line_index < lines.count() && emitted_page_lines < data_line_limit)
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
