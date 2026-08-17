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
static fn read_up_to_lines(os::descriptor fd, u64 max_lines,
                           Allocator allocator) throws -> Maybe<String>
{
  String out{allocator};
  u64 line_count = 0;
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

static fn read_up_to_bytes(os::descriptor fd, u64 max_bytes,
                           Allocator allocator) throws -> Maybe<String>
{
  String out{allocator};
  u64 byte_count = 0;
  char buffer[4096];
  while (byte_count < max_bytes) {
    if (os::INTERRUPT_REQUESTED) break;
    let const read_count = os::read_fd(fd, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) break;
    let const remaining_count = max_bytes - byte_count;
    let const take_count = remaining_count < *read_count
                               ? static_cast<usize>(remaining_count)
                               : *read_count;
    out.append(StringView{buffer, take_count});
    byte_count += take_count;
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
                                           u64 drop_count) wontthrow -> usize
{
  u64 total_line_count = 0;
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\n') total_line_count++;
  }

  if (text.length > 0 && text[text.length - 1] != '\n') {
    total_line_count++;
  }

  if (drop_count >= total_line_count) return 0;
  let const keep_count = total_line_count - drop_count;

  u64 lines_seen = 0;
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\n') {
      lines_seen++;
      if (lines_seen == keep_count) return i + 1;
    }
  }

  return text.length;
}

static fn byte_prefix_length_dropping_last(StringView text,
                                           u64 drop_count) wontthrow -> usize
{
  if (drop_count >= text.length) return 0;

  return text.length - static_cast<usize>(drop_count);
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

  u64 count = 10;
  bool is_all_but_last = false;
  if (is_byte_mode) {
    let const raw = FLAG_HEAD_BYTES.value();
    is_all_but_last = raw.length > 0 && raw[0] == '-';
    let const magnitude = is_all_but_last ? raw.substring(1) : raw;
    let const parsed_value = utils::parse_decimal_u64(magnitude);
    if (parsed_value.is_error()) {
      throw ErrorWithDetails{
          "head: invalid byte count '" + String{cxt.scratch_allocator(), raw}
            +
              "'",
          "The count must be an integer"
      };
    }
    count = parsed_value.value();
  } else if (has_lines_flag) {
    let const raw = FLAG_HEAD_LINES.value();
    is_all_but_last = raw.length > 0 && raw[0] == '-';
    let const magnitude = is_all_but_last ? raw.substring(1) : raw;
    let const parsed_value = utils::parse_decimal_u64(magnitude);
    if (parsed_value.is_error()) {
      throw ErrorWithDetails{
          "head: invalid line count '" + String{cxt.scratch_allocator(), raw}
            +
              "'",
          "The count must be an integer"
      };
    }
    count = parsed_value.value();
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
    let text_view = StringView{};
    if (is_all_but_last) {
      text = read_all(fd, cxt.scratch_allocator());
      if (text.has_value()) {
        let const keep_length =
            is_byte_mode
                ? byte_prefix_length_dropping_last(text->view(), count)
                : line_prefix_length_dropping_last(text->view(), count);
        text_view = text->view().substring_of_length(0, keep_length);
      }
    } else {
      text = is_byte_mode
                 ? read_up_to_bytes(fd, count, cxt.scratch_allocator())
                 : read_up_to_lines(fd, count, cxt.scratch_allocator());
      if (text.has_value()) text_view = text->view();
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
    output += text_view;
    ec.print_to_stdout(output);
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
