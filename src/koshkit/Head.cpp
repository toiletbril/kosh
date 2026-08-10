#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n count] [-c count] [file ...]");

HELP_DESCRIPTION_DECL("The head utility writes the first lines of each file.");

FLAG(HEAD_LINES, String, 'n', "", "Write the first count lines.");
FLAG(HEAD_BYTES, String, 'c', "", "Write the first count bytes.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Head);

namespace koshka {

namespace koshkit {

/* Stopping at max_lines keeps an endless producer such as yes from running
   forever, since reading to the end would never return. */
static fn read_up_to_lines(os::descriptor fd, i64 max_lines,
                           Allocator allocator) throws -> Maybe<String>
{
  String out{allocator};
  i64 line_count = 0;
  char buffer[4096];
  while (line_count < max_lines) {
    if (os::INTERRUPT_REQUESTED) break;
    let const read_count = os::read_fd(fd, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) break;
    usize span_length = 0;
    while (span_length < *read_count && line_count < max_lines) {
      if (buffer[span_length] == '\n') line_count++;
      span_length++;
    }
    out.append(StringView{buffer, span_length});
  }

  return out;
}

static fn read_up_to_bytes(os::descriptor fd, i64 max_bytes,
                           Allocator allocator) throws -> Maybe<String>
{
  String out{allocator};
  i64 byte_count = 0;
  char buffer[4096];
  while (byte_count < max_bytes) {
    if (os::INTERRUPT_REQUESTED) break;
    let const read_count = os::read_fd(fd, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) break;
    let const remaining_count = static_cast<usize>(max_bytes - byte_count);
    let const take_count =
        *read_count < remaining_count ? *read_count : remaining_count;
    out.append(StringView{buffer, take_count});
    byte_count += static_cast<i64>(take_count);
  }

  return out;
}

static fn read_all(os::descriptor fd, Allocator allocator) throws
    -> Maybe<String>
{
  if (os::INTERRUPT_REQUESTED) return String{allocator};
  return os::read_fd_to_string(fd, allocator);
}

static fn line_prefix_length_dropping_last(StringView text,
                                           i64 drop_count) wontthrow -> usize
{
  i64 total_line_count = 0;
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\n') total_line_count++;
  }

  if (text.length > 0 && text[text.length - 1] != '\n') {
    total_line_count++;
  }

  let const keep_count = total_line_count - drop_count;
  if (keep_count <= 0) return 0;

  i64 lines_seen = 0;
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\n') {
      lines_seen++;
      if (lines_seen == keep_count) return i + 1;
    }
  }

  return text.length;
}

static fn byte_prefix_length_dropping_last(StringView text,
                                           i64 drop_count) wontthrow -> usize
{
  let const total_length = static_cast<i64>(text.length);
  let const keep_length = total_length - drop_count;
  if (keep_length <= 0) return 0;

  return static_cast<usize>(keep_length);
}

Head::Head() = default;

pure fn Head::kind() const wontthrow -> Utility::Kind { return Kind::Head; }

fn Head::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const has_bytes_flag = FLAG_HEAD_BYTES.is_set();
  let const has_lines_flag = FLAG_HEAD_LINES.is_set();
  let const is_byte_mode =
      has_bytes_flag && (!has_lines_flag || FLAG_HEAD_BYTES.position() >
                                                FLAG_HEAD_LINES.position());

  i64 count = 10;
  bool is_all_but_last = false;
  if (is_byte_mode) {
    let const raw = FLAG_HEAD_BYTES.value();
    let const parsed_value = raw.to<i64>();
    if (parsed_value.is_error()) {
      throw ErrorWithDetails{
          "head: invalid byte count '" + String{cxt.scratch_allocator(), raw}
            +
              "'",
          "The count must be an integer"
      };
    }
    is_all_but_last = raw.length > 0 && raw[0] == '-';
    count = is_all_but_last ? -parsed_value.value() : parsed_value.value();
  } else if (has_lines_flag) {
    let const raw = FLAG_HEAD_LINES.value();
    let const parsed_value = raw.to<i64>();
    if (parsed_value.is_error()) {
      throw ErrorWithDetails{
          "head: invalid line count '" + String{cxt.scratch_allocator(), raw}
            +
              "'",
          "The count must be an integer"
      };
    }
    is_all_but_last = raw.length > 0 && raw[0] == '-';
    count = is_all_but_last ? -parsed_value.value() : parsed_value.value();
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let const should_print_headers = sources.count() > 1;
  i32 status = 0;
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    os::descriptor fd;
    bool was_opened = false;
    if (sources[source_index] == "-") {
      fd = ec.in_fd.value_or(KOSH_STDIN);
    } else {
      let const opened_fd = os::open_file_descriptor(sources[source_index],
                                                     os::file_open_mode::Read);
      if (!opened_fd.has_value()) {
        report_soft_koshkit_error(
            ec, cxt,
            "head: cannot open '" +
                String{cxt.scratch_allocator(), sources[source_index]} +
                "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      fd = *opened_fd;
      was_opened = true;
    }

    let text = Maybe<String>{};
    if (is_all_but_last) {
      let const whole = read_all(fd, cxt.scratch_allocator());
      if (whole.has_value()) {
        let const keep_length =
            is_byte_mode
                ? byte_prefix_length_dropping_last(whole->view(), count)
                : line_prefix_length_dropping_last(whole->view(), count);
        text = String{cxt.scratch_allocator(),
                      whole->view().substring_of_length(0, keep_length)};
      }
    } else {
      text = is_byte_mode
                 ? read_up_to_bytes(fd, count, cxt.scratch_allocator())
                 : read_up_to_lines(fd, count, cxt.scratch_allocator());
    }

    if (was_opened) os::close_fd(fd);
    /* A Ctrl-C during the read returns 130 rather than freezing the utility. */
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!text.has_value()) {
      report_soft_koshkit_error(
          ec, cxt,
          "head: cannot read '" +
              String{cxt.scratch_allocator(), sources[source_index]} +
              "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let output = String{cxt.scratch_allocator()};
    if (should_print_headers) {
      if (source_index > 0) output += '\n';
      output += "==> ";
      output += sources[source_index];
      output += " <==\n";
    }
    output += text->view();
    ec.print_to_stdout(output);
  }

  return status;
}

} // namespace koshkit

} // namespace koshka
