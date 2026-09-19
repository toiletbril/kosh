/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the nl utility. It recognizes logical page sections and
 * formats selected line numbers with configurable styles, widths, increments,
 * and separators.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-p] [-b type] [-d delim] [-f type] [-h type] "
                   "[-i incr] [-l num] [-n format] [-s sep] [-v start] "
                   "[-w width] [file]");

HELP_DESCRIPTION_DECL("The nl utility numbers input lines.");

FLAG(NL_BODY, String, 'b', "body-numbering", "Select body line numbering.");
FLAG(NL_DELIMITER, String, 'd', "section-delimiter",
     "Use these section delimiter bytes.");
FLAG(NL_FOOTER, String, 'f', "footer-numbering",
     "Select footer line numbering.");
FLAG(NL_HEADER, String, 'h', "header-numbering",
     "Select header line numbering.");
FLAG(NL_INCREMENT, String, 'i', "line-increment", "Increment by this value.");
FLAG(NL_BLANK_GROUP, String, 'l', "join-blank-lines",
     "Number this many adjacent blank lines as one.");
FLAG(NL_FORMAT, String, 'n', "number-format", "Use ln, rn, or rz format.");
FLAG(NL_NO_RESET, Bool, 'p', "no-renumber", "Do not reset at logical pages.");
FLAG(NL_SEPARATOR, String, 's', "number-separator",
     "Use this separator after numbers.");
FLAG(NL_START, String, 'v', "starting-line-number", "Start at this number.");
FLAG(NL_WIDTH, String, 'w', "number-width", "Use this number field width.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Nl);

namespace koshka::koshkit {

enum class nl_style : uchar
{
  All,
  NonEmpty,
  None,
};

static constexpr static_string_entry<nl_style> NL_STYLE_ENTRIES[] = {
    {SSK("a"), nl_style::All     },
    {SSK("t"), nl_style::NonEmpty},
    {SSK("n"), nl_style::None    },
};
static constexpr StaticStringMap NL_STYLES{NL_STYLE_ENTRIES};

static fn nl_style_numbers(nl_style style, bool is_empty, u64 &blank_count,
                           u64 blank_group) wontthrow -> bool
{
  switch (style) {
  case nl_style::None: return false;

  case nl_style::All:
    if (!is_empty) {
      blank_count = 0;
      return true;
    }

    blank_count++;
    if (blank_count < blank_group) return false;

    blank_count = 0;
    return true;

  case nl_style::NonEmpty: break;
  }

  blank_count = 0;
  return !is_empty;
}

enum class nl_number_format : uchar
{
  Left,
  Right,
  Zero,
};

static fn append_nl_number(String &output, i64 number, usize width,
                           nl_number_format format, StringView separator) throws
    -> void
{
  let const digits = String::from(number, output.allocator());
  let const padding = width > digits.length() ? width - digits.length() : 0;
  if (format != nl_number_format::Left)
    output.append_repeated(format == nl_number_format::Zero ? '0' : ' ',
                           padding);
  output += digits.view();
  if (format == nl_number_format::Left) output.append_repeated(' ', padding);
  output += separator;
}

Nl::Nl() = default;

pure fn Nl::kind() const wontthrow -> Utility::Kind { return Kind::Nl; }

fn Nl::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 1) return report_usage_error(ec, cxt, args[0].view());

  let const body_style = NL_STYLES.find(
      FLAG_NL_BODY.is_set() ? FLAG_NL_BODY.value() : StringView{"t"});
  let const header_style = NL_STYLES.find(
      FLAG_NL_HEADER.is_set() ? FLAG_NL_HEADER.value() : StringView{"n"});
  let const footer_style = NL_STYLES.find(
      FLAG_NL_FOOTER.is_set() ? FLAG_NL_FOOTER.value() : StringView{"n"});
  if (!body_style.has_value() || !header_style.has_value() ||
      !footer_style.has_value())
  {
    let const *invalid_flag = !body_style.has_value()     ? &FLAG_NL_BODY
                              : !header_style.has_value() ? &FLAG_NL_HEADER
                                                          : &FLAG_NL_FOOTER;
    KOSHKIT_REPORT_ERROR_AT(
        invalid_flag->value_location(),
        "unsupported numbering style '" + invalid_flag->value() + "'",
        "use a for all lines, t for nonempty lines, or n for no lines");
    return 1;
  }

  let const number_format_name =
      FLAG_NL_FORMAT.is_set() ? FLAG_NL_FORMAT.value() : StringView{"rn"};
  static constexpr static_string_entry<nl_number_format>
      NUMBER_FORMAT_ENTRIES[] = {
          {SSK("ln"), nl_number_format::Left },
          {SSK("rn"), nl_number_format::Right},
          {SSK("rz"), nl_number_format::Zero },
  };
  static constexpr StaticStringMap NUMBER_FORMATS{NUMBER_FORMAT_ENTRIES};
  let const number_format = NUMBER_FORMATS.find(number_format_name);
  if (!number_format.has_value()) {
    KOSHKIT_REPORT_ERROR_AT(FLAG_NL_FORMAT.value_location(),
                            "invalid number format '" + FLAG_NL_FORMAT.value() +
                                "'",
                            "use ln, rn, or rz");
    return 1;
  }

  u64 increment = 1;
  if (FLAG_NL_INCREMENT.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_NL_INCREMENT.value());
    if (parsed.is_error() || parsed.value() > INT64_MAX) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_NL_INCREMENT.value_location(),
          "invalid increment '" + FLAG_NL_INCREMENT.value() + "'",
          "use a decimal integer from 0 through 9223372036854775807");
      return 1;
    }

    increment = parsed.value();
  }

  u64 blank_group = 1;
  if (FLAG_NL_BLANK_GROUP.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_NL_BLANK_GROUP.value());
    if (parsed.is_error() || parsed.value() == 0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_NL_BLANK_GROUP.value_location(),
                              "invalid blank group '" +
                                  FLAG_NL_BLANK_GROUP.value() + "'",
                              "use a positive decimal integer");
      return 1;
    }

    blank_group = parsed.value();
  }

  u64 width_value = 6;
  if (FLAG_NL_WIDTH.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_NL_WIDTH.value());
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_NL_WIDTH.value_location(),
          "invalid width '" + FLAG_NL_WIDTH.value() + "'",
          "use a positive decimal integer within the platform size limit");
      return 1;
    }

    width_value = parsed.value();
  }

  u64 start_value = 1;
  if (FLAG_NL_START.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_NL_START.value());
    if (parsed.is_error() || parsed.value() > INT64_MAX) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_NL_START.value_location(),
          "invalid starting line number '" + FLAG_NL_START.value() + "'",
          "use a decimal integer from 0 through 9223372036854775807");
      return 1;
    }

    start_value = parsed.value();
  }

  let const separator =
      FLAG_NL_SEPARATOR.is_set() ? FLAG_NL_SEPARATOR.value() : StringView{"\t"};
  let const delimiter = FLAG_NL_DELIMITER.is_set() ? FLAG_NL_DELIMITER.value()
                                                   : StringView{"\\:"};
  if (delimiter.length != 2) {
    KOSHKIT_REPORT_ERROR_AT(FLAG_NL_DELIMITER.value_location(),
                            "the section delimiter must be two bytes");
    return 1;
  }

  let const source = operands.is_empty() ? StringView{"-"} : operands[0].view();
  let const input = open_named_or_stdin(ec, source);
  if (!input.has_value()) {
    report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                   "cannot read '" +
                                       String{cxt.scratch_allocator(), source} +
                                       "': " + os::last_system_error_message());
    return 1;
  }
  defer
  {
    if (input->should_close) os::close_fd(input->descriptor);
  };

  let reader = utils::BufferedLineReader{input->descriptor};
  let output = String{cxt.scratch_allocator()};
  nl_style section_style = *body_style;
  i64 number = static_cast<i64>(start_value);
  u64 blank_count = 0;

  loop
  {
    let const result = reader.next();
    if (result == utils::BufferedLineReader::Result::End) break;
    if (result == utils::BufferedLineReader::Result::Error) {
      if (os::INTERRUPT_REQUESTED) return 130;
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     os::last_system_error_message());
      return 1;
    }

    let const line = reader.get_line();
    let const delimiter_count = line.length / delimiter.length;
    bool is_section_delimiter =
        delimiter_count >= 1 && delimiter_count <= 3 &&
        line.length == delimiter_count * delimiter.length;
    for (usize position = 0; position < delimiter_count && is_section_delimiter;
         position++)
      is_section_delimiter =
          line.substring_of_length(position * delimiter.length,
                                   delimiter.length) == delimiter;
    if (is_section_delimiter) {
      section_style = delimiter_count == 3   ? *header_style
                      : delimiter_count == 2 ? *body_style
                                             : *footer_style;
      if (!FLAG_NL_NO_RESET.is_enabled())
        number = static_cast<i64>(start_value);
      blank_count = 0;
      output += '\n';
      continue;
    }

    if (nl_style_numbers(section_style, line.is_empty(), blank_count,
                         blank_group))
    {
      append_nl_number(output, number, static_cast<usize>(width_value),
                       *number_format, separator);
      if (increment > static_cast<u64>(INT64_MAX - number))
        number = INT64_MAX;
      else
        number += static_cast<i64>(increment);
    } else {
      output.append_repeated(' ', static_cast<usize>(width_value));
      output += separator;
    }
    output += line;
    output += '\n';
  }

  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
