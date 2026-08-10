#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-lwc] [file ...]");

HELP_DESCRIPTION_DECL(
    "The wc utility counts the lines, words, and bytes of each file.");

FLAG(WC_LINES, Bool, 'l', "", "Print the newline count.");
FLAG(WC_WORDS, Bool, 'w', "", "Print the word count.");
FLAG(WC_BYTES, Bool, 'c', "", "Print the byte count.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Wc);

namespace koshka {

namespace koshkit {

static fn is_blank(char c) wontthrow -> bool
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

struct wc_row
{
  StringView name;
  u64 line_count;
  u64 word_count;
  u64 byte_count;
};

static fn decimal_digit_count(u64 value) wontthrow -> usize
{
  usize digit_count = 1;

  while (value >= 10) {
    value /= 10;
    digit_count++;
  }

  return digit_count;
}

static fn format_counts(u64 lines, u64 words, u64 bytes, bool should_show_lines,
                        bool should_show_words, bool should_show_bytes,
                        StringView name, usize field_width,
                        Allocator allocator) throws -> String
{
  String line{allocator};
  bool has_field = false;

  let const do_emit_field = [&line, &has_field, allocator,
                             field_width](u64 value) throws -> void {
    if (has_field) line += ' ';

    let const digits = String::from(value, allocator);
    for (usize i = digits.count(); i < field_width; i++)
      line += ' ';

    line += digits.view();
    has_field = true;
  };

  if (should_show_lines) do_emit_field(lines);
  if (should_show_words) do_emit_field(words);
  if (should_show_bytes) do_emit_field(bytes);

  if (!name.is_empty()) {
    line += ' ';
    line += name;
  }

  line += '\n';
  return line;
}

Wc::Wc() = default;

pure fn Wc::kind() const wontthrow -> Utility::Kind { return Kind::Wc; }

fn Wc::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  bool should_show_lines = FLAG_WC_LINES.is_enabled();
  bool should_show_words = FLAG_WC_WORDS.is_enabled();
  bool should_show_bytes = FLAG_WC_BYTES.is_enabled();
  if (!should_show_lines && !should_show_words && !should_show_bytes) {
    should_show_lines = true;
    should_show_words = true;
    should_show_bytes = true;
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  ArrayList<wc_row> rows{cxt.scratch_allocator()};
  u64 total_lines = 0;
  u64 total_words = 0;
  u64 total_bytes = 0;
  i32 status = 0;
  for (const StringView &source : sources) {
    let const input = open_named_or_stdin(ec, source);
    if (!input.has_value()) {
      report_soft_koshkit_error(
          ec, cxt,
          "wc: " + String{cxt.scratch_allocator(), source} + ": " +
              os::last_system_error_message());
      status = 1;
      continue;
    }
    defer
    {
      if (input->should_close) os::close_fd(input->descriptor);
    };
    u64 lines = 0;
    u64 words = 0;
    u64 bytes = 0;
    bool is_in_word = false;
    bool did_read_fail = false;
    char buffer[65536];

    loop
    {
      let const read_size =
          os::read_fd(input->descriptor, buffer, sizeof(buffer));
      if (!read_size.has_value()) {
        if (os::INTERRUPT_REQUESTED) return 130;
        report_soft_koshkit_error(
            ec, cxt,
            "wc: " + String{cxt.scratch_allocator(), source} + ": " +
                os::last_system_error_message());
        status = 1;
        did_read_fail = true;
        break;
      }
      if (*read_size == 0) break;
      bytes += *read_size;

      for (usize i = 0; i < *read_size; i++) {
        let const c = buffer[i];
        if (c == '\n') lines++;
        if (is_blank(c)) {
          is_in_word = false;
        } else if (!is_in_word) {
          is_in_word = true;
          words++;
        }
      }
    }
    if (did_read_fail) continue;

    total_lines += lines;
    total_words += words;
    total_bytes += bytes;

    let const name = source == "-" ? StringView{} : source;
    rows.push(wc_row{name, lines, words, bytes});
  }

  u64 max_count = 0;
  if (should_show_lines && total_lines > max_count) {
    max_count = total_lines;
  }
  if (should_show_words && total_words > max_count) {
    max_count = total_words;
  }
  if (should_show_bytes && total_bytes > max_count) {
    max_count = total_bytes;
  }

  let const field_width = decimal_digit_count(max_count);

  let output = String{cxt.scratch_allocator()};
  for (const wc_row &row : rows)
    output +=
        format_counts(row.line_count, row.word_count, row.byte_count,
                      should_show_lines, should_show_words, should_show_bytes,
                      row.name, field_width, cxt.scratch_allocator());

  if (sources.count() > 1)
    output +=
        format_counts(total_lines, total_words, total_bytes, should_show_lines,
                      should_show_words, should_show_bytes, StringView{"total"},
                      field_width, cxt.scratch_allocator());

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshkit

} // namespace koshka
