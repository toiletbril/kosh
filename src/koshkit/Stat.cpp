/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the stat utility. It renders the status of a file or of
 * the filesystem that holds it through a directive format, and it supplies the
 * default, terse, and filesystem layouts that a caller selects with a flag.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-Lft] [-c format] [--printf=format] file ...");

HELP_DESCRIPTION_DECL(
    "The stat utility displays the status of a file or a filesystem.");

FLAG(STAT_DEREFERENCE, Bool, 'L', "dereference", "Follow symbolic links.");
FLAG(STAT_FILESYSTEM, Bool, 'f', "file-system",
     "Report the filesystem that holds the operand.");
FLAG(STAT_TERSE, Bool, 't', "terse", "Print the terse single-line form.");
FLAG(STAT_FORMAT, String, 'c', "format",
     "Render this directive format and a newline for every operand.");
FLAG(STAT_PRINTF, String, '\0', "printf",
     "Render this directive format with backslash escapes and no newline.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Stat);

namespace koshka::koshkit {

namespace {

constexpr StringView DEFAULT_FILE_FORMAT =
    "  File: %N\n"
    "  Size: %-10s\tBlocks: %-10b IO Block: %-6o %F\n"
    "Device: %Hd,%Ld\tInode: %-10i  Links: %h\n"
    "Access: (%04a/%10.10A)  Uid: (%5u/%8U)   Gid: (%5g/%8G)\n"
    "Access: %x\n"
    "Modify: %y\n"
    "Change: %z\n"
    " Birth: %w\n";

constexpr StringView DEVICE_FILE_FORMAT =
    "  File: %N\n"
    "  Size: %-10s\tBlocks: %-10b IO Block: %-6o %F\n"
    "Device: %Hd,%Ld\tInode: %-10i  Links: %-5h Device type: %Hr,%Lr\n"
    "Access: (%04a/%10.10A)  Uid: (%5u/%8U)   Gid: (%5g/%8G)\n"
    "Access: %x\n"
    "Modify: %y\n"
    "Change: %z\n"
    " Birth: %w\n";

constexpr StringView TERSE_FILE_FORMAT =
    "%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o\n";

constexpr StringView DEFAULT_FILESYSTEM_FORMAT =
    "  File: \"%n\"\n"
    "    ID: %-8i Namelen: %-7l Type: %T\n"
    "Block size: %-10s Fundamental block size: %S\n"
    "Blocks: Total: %-10b Free: %-10f Available: %a\n"
    "Inodes: Total: %-10c Free: %d\n";

constexpr StringView TERSE_FILESYSTEM_FORMAT =
    "%n %i %l %t %s %S %b %f %a %c %d\n";

struct directive_spec
{
  usize width{0};
  usize precision{0};
  char sub_field{'\0'};
  char letter{'\0'};
  bool is_left_aligned{false};
  bool is_zero_padded{false};
  bool is_alternate{false};
  bool has_precision{false};
};

struct file_subject
{
  os::file_status status{};
  StringView display_name;
  String quoted_name;
  String mount_point;
  u64 optimal_block_size{0};
  bool has_optimal_block_size{false};
  bool has_mount_point{false};
};

fn append_padded(String &output, StringView text, const directive_spec &spec,
                 bool is_numeric) throws -> void
{
  StringView body = text;
  String padded_body{output.allocator()};
  if (spec.has_precision && !is_numeric && body.length > spec.precision) {
    body = body.substring_of_length(0, spec.precision);
  }

  if (spec.has_precision && is_numeric && body.length < spec.precision) {
    for (usize index = body.length; index < spec.precision; index++)
      padded_body += "0";

    padded_body += body;
    body = padded_body.view();
  }

  if (body.length >= spec.width) {
    output += body;
    return;
  }

  let const pad_length = spec.width - body.length;
  if (spec.is_left_aligned) {
    output += body;
    for (usize index = 0; index < pad_length; index++)
      output += " ";

    return;
  }

  let const pad_text = spec.is_zero_padded && is_numeric ? "0" : " ";
  for (usize index = 0; index < pad_length; index++)
    output += pad_text;

  output += body;
}

fn decimal_of(u64 value, Allocator allocator) throws -> String
{
  return String::from(value, allocator);
}

fn hex_of(u64 value, Allocator allocator) throws -> String
{
  return String::from_in_base(value, false, int_base::hex, allocator);
}

fn octal_of(u64 value, bool is_alternate, Allocator allocator) throws -> String
{
  let text = String::from_in_base(value, false, int_base::octal, allocator);
  if (!is_alternate) return text;

  let prefixed = String{allocator, "0"};
  prefixed += text.view();
  return prefixed;
}

fn quote_name(StringView name, const os::file_status &status,
              Allocator allocator) throws -> String
{
  let text = String{allocator, "'"};
  text += name;
  text += "'";
  if (os::file_type_letter(status.mode) != 'l') return text;

  let const target = os::read_symlink(name, allocator);
  if (!target.has_value()) return text;

  text += " -> '";
  text += target->view();
  text += "'";
  return text;
}

fn find_mount_point(StringView path, Allocator allocator) throws -> String
{
  os::file_status start{};
  if (!os::stat_path_following(path, start)) return String{allocator, "?"};

  let absolute = String{allocator};
  if (path.length == 0 || path[0] != '/') {
    absolute += os::read_current_directory().view();
    absolute += "/";
  }

  absolute += path;

  let best = String{allocator, absolute.view()};
  loop
  {
    usize slash_position = best.length();
    while (slash_position > 0 && best.view()[slash_position - 1] != '/')
      slash_position--;

    if (slash_position == 0) break;

    let const parent_length = slash_position > 1 ? slash_position - 1 : 1;
    let parent =
        String{allocator, best.view().substring_of_length(0, parent_length)};
    os::file_status parent_status{};
    if (!os::stat_path_following(parent.view(), parent_status)) break;

    if (parent_status.device_id != start.device_id) break;

    if (parent.length() == best.length()) break;

    best = steal(parent);
  }

  return best;
}

fn render_file_directive(String &output, const directive_spec &spec,
                         file_subject &subject, EvalContext &cxt) throws -> void
{
  let const allocator = cxt.scratch_allocator();
  const os::file_status &status = subject.status;

  switch (spec.letter) {
  case 'a':
    append_padded(
        output,
        octal_of(status.mode & 07777u, spec.is_alternate, allocator).view(),
        spec, true);
    return;

  case 'A':
    append_padded(output, os::format_mode_string(status.mode).view(), spec,
                  false);
    return;

  case 'b':
    append_padded(output, decimal_of(status.blocks, allocator).view(), spec,
                  true);
    return;

  case 'B': append_padded(output, "512", spec, true); return;

  case 'C': append_padded(output, "?", spec, false); return;

  case 'd':
    if (spec.sub_field == 'H') {
      append_padded(
          output,
          decimal_of(os::device_major(status.device_id), allocator).view(),
          spec, true);
      return;
    }

    if (spec.sub_field == 'L') {
      append_padded(
          output,
          decimal_of(os::device_minor(status.device_id), allocator).view(),
          spec, true);
      return;
    }

    append_padded(output, decimal_of(status.device_id, allocator).view(), spec,
                  true);
    return;

  case 'D':
    append_padded(output, hex_of(status.device_id, allocator).view(), spec,
                  true);
    return;

  case 'f':
    append_padded(output, hex_of(status.mode, allocator).view(), spec, true);
    return;

  case 'F': append_padded(output, file_type_name(status), spec, false); return;

  case 'g':
    append_padded(output, decimal_of(status.group_id, allocator).view(), spec,
                  true);
    return;

  case 'G': {
    let const name = os::gid_to_groupname(status.group_id);
    append_padded(output,
                  name.has_value()
                      ? name->view()
                      : decimal_of(status.group_id, allocator).view(),
                  spec, false);
    return;
  }

  case 'h':
    append_padded(output, decimal_of(status.link_count, allocator).view(), spec,
                  true);
    return;

  case 'i':
    append_padded(output, decimal_of(status.file_id, allocator).view(), spec,
                  true);
    return;

  case 'm':
    if (!subject.has_mount_point) {
      subject.mount_point = find_mount_point(subject.display_name, allocator);
      subject.has_mount_point = true;
    }

    append_padded(output, subject.mount_point.view(), spec, false);
    return;

  case 'n': append_padded(output, subject.display_name, spec, false); return;

  case 'N':
    if (subject.quoted_name.is_empty()) {
      subject.quoted_name = quote_name(subject.display_name, status, allocator);
    }

    append_padded(output, subject.quoted_name.view(), spec, false);
    return;

  case 'o':
    if (!subject.has_optimal_block_size) {
      os::filesystem_status filesystem{};
      if (os::stat_filesystem(subject.display_name, filesystem)) {
        subject.optimal_block_size = filesystem.block_size;
      }

      subject.has_optimal_block_size = true;
    }

    append_padded(output,
                  decimal_of(subject.optimal_block_size, allocator).view(),
                  spec, true);
    return;

  case 'r':
    if (spec.sub_field == 'H') {
      append_padded(
          output,
          decimal_of(os::device_major(status.special_device_id), allocator)
              .view(),
          spec, true);
      return;
    }

    if (spec.sub_field == 'L') {
      append_padded(
          output,
          decimal_of(os::device_minor(status.special_device_id), allocator)
              .view(),
          spec, true);
      return;
    }

    append_padded(output,
                  decimal_of(status.special_device_id, allocator).view(), spec,
                  true);
    return;

  case 'R':
    append_padded(output, hex_of(status.special_device_id, allocator).view(),
                  spec, true);
    return;

  case 's':
    append_padded(output, decimal_of(status.size, allocator).view(), spec,
                  true);
    return;

  case 't':
    append_padded(
        output,
        hex_of(os::device_major(status.special_device_id), allocator).view(),
        spec, true);
    return;

  case 'T':
    append_padded(
        output,
        hex_of(os::device_minor(status.special_device_id), allocator).view(),
        spec, true);
    return;

  case 'u':
    append_padded(output, decimal_of(status.owner_id, allocator).view(), spec,
                  true);
    return;

  case 'U': {
    let const name = os::uid_to_username(status.owner_id);
    append_padded(output,
                  name.has_value()
                      ? name->view()
                      : decimal_of(status.owner_id, allocator).view(),
                  spec, false);
    return;
  }

  case 'w': append_padded(output, "-", spec, false); return;

  case 'W': append_padded(output, "0", spec, true); return;

  case 'x':
    append_padded(output,
                  format_file_timestamp(status.access_time,
                                        status.access_nanoseconds, allocator)
                      .view(),
                  spec, false);
    return;

  case 'X':
    append_padded(
        output,
        decimal_of(static_cast<u64>(status.access_time), allocator).view(),
        spec, true);
    return;

  case 'y':
    append_padded(output,
                  format_file_timestamp(status.modification_time,
                                        status.modification_nanoseconds,
                                        allocator)
                      .view(),
                  spec, false);
    return;

  case 'Y':
    append_padded(
        output,
        decimal_of(static_cast<u64>(status.modification_time), allocator)
            .view(),
        spec, true);
    return;

  case 'z':
    append_padded(output,
                  format_file_timestamp(status.change_time,
                                        status.change_nanoseconds, allocator)
                      .view(),
                  spec, false);
    return;

  case 'Z':
    append_padded(
        output,
        decimal_of(static_cast<u64>(status.change_time), allocator).view(),
        spec, true);
    return;

  default: break;
  }

  output += "?";
}

fn render_filesystem_directive(String &output, const directive_spec &spec,
                               const os::filesystem_status &filesystem,
                               StringView display_name, EvalContext &cxt) throws
    -> void
{
  let const allocator = cxt.scratch_allocator();

  switch (spec.letter) {
  case 'a':
    append_padded(output,
                  decimal_of(filesystem.available_blocks, allocator).view(),
                  spec, true);
    return;

  case 'b':
    append_padded(output, decimal_of(filesystem.total_blocks, allocator).view(),
                  spec, true);
    return;

  case 'c':
    append_padded(output, decimal_of(filesystem.total_files, allocator).view(),
                  spec, true);
    return;

  case 'd':
    append_padded(output, decimal_of(filesystem.free_files, allocator).view(),
                  spec, true);
    return;

  case 'f':
    append_padded(output, decimal_of(filesystem.free_blocks, allocator).view(),
                  spec, true);
    return;

  case 'i':
    append_padded(output, hex_of(filesystem.filesystem_id, allocator).view(),
                  spec, true);
    return;

  case 'l':
    append_padded(output, decimal_of(filesystem.name_max, allocator).view(),
                  spec, true);
    return;

  case 'n': append_padded(output, display_name, spec, false); return;

  case 's':
    append_padded(output, decimal_of(filesystem.block_size, allocator).view(),
                  spec, true);
    return;

  case 'S':
    append_padded(
        output, decimal_of(filesystem.fundamental_block_size, allocator).view(),
        spec, true);
    return;

  case 't':
    append_padded(output, hex_of(filesystem.type_id, allocator).view(), spec,
                  true);
    return;

  case 'T': {
    let const name = StringView{filesystem.type_name};
    if (!name.is_empty()) {
      append_padded(output, name, spec, false);
      return;
    }

    let unknown = String{allocator, "UNKNOWN (0x"};
    unknown += hex_of(filesystem.type_id, allocator).view();
    unknown += ")";
    append_padded(output, unknown.view(), spec, false);
    return;
  }

  default: break;
  }

  output += "?";
}

fn parse_directive(StringView format, usize &position,
                   directive_spec &spec) wontthrow -> bool
{
  spec = directive_spec{};

  loop
  {
    if (position >= format.length) return false;

    switch (format[position]) {
    case '-': spec.is_left_aligned = true; break;
    case '0': spec.is_zero_padded = true; break;
    case '#': spec.is_alternate = true; break;
    case '+':
    case ' ': break;
    default: goto flags_done;
    }

    position++;
  }

flags_done:
  while (position < format.length && format[position] >= '0' &&
         format[position] <= '9')
  {
    spec.width = (spec.width * 10) + static_cast<usize>(format[position] - '0');
    position++;
  }

  if (position < format.length && format[position] == '.') {
    position++;
    spec.has_precision = true;
    while (position < format.length && format[position] >= '0' &&
           format[position] <= '9')
    {
      spec.precision =
          (spec.precision * 10) + static_cast<usize>(format[position] - '0');
      position++;
    }
  }

  if (position + 1 < format.length &&
      (format[position] == 'H' || format[position] == 'L'))
  {
    spec.sub_field = format[position];
    position++;
  }

  if (position >= format.length) return false;

  spec.letter = format[position];
  position++;
  return true;
}

fn append_escapes(String &output, StringView text) throws -> void
{
  usize position = 0;
  while (position < text.length) {
    if (text[position] != '\\' || position + 1 >= text.length) {
      output += text.substring_of_length(position, 1);
      position++;
      continue;
    }

    position++;
    switch (text[position]) {
    case 'n': output += "\n"; break;
    case 't': output += "\t"; break;
    case 'r': output += "\r"; break;
    case 'a': output += "\a"; break;
    case 'b': output += "\b"; break;
    case 'f': output += "\f"; break;
    case 'v': output += "\v"; break;
    case '\\': output += "\\"; break;
    case '"': output += "\""; break;
    default: output += "\\"; output += text.substring_of_length(position, 1);
    }

    position++;
  }
}

fn render_format(String &output, StringView format, file_subject *subject,
                 const os::filesystem_status *filesystem,
                 StringView display_name, EvalContext &cxt) throws -> void
{
  usize position = 0;
  while (position < format.length) {
    if (format[position] != '%') {
      output += format.substring_of_length(position, 1);
      position++;
      continue;
    }

    position++;
    if (position >= format.length) {
      output += "%";
      return;
    }

    if (format[position] == '%') {
      output += "%";
      position++;
      continue;
    }

    directive_spec spec{};
    if (!parse_directive(format, position, spec)) {
      output += "%";
      return;
    }

    if (subject != nullptr) {
      render_file_directive(output, spec, *subject, cxt);
      continue;
    }

    render_filesystem_directive(output, spec, *filesystem, display_name, cxt);
  }
}

fn resolve_format(String &format, bool &should_append_newline,
                  bool is_filesystem_mode, Allocator allocator) throws -> void
{
  if (FLAG_STAT_PRINTF.is_set()) {
    append_escapes(format, FLAG_STAT_PRINTF.value());
    should_append_newline = false;
    return;
  }

  if (FLAG_STAT_FORMAT.is_set()) {
    format += FLAG_STAT_FORMAT.value();
    should_append_newline = true;
    return;
  }

  unused(allocator);
  should_append_newline = false;
  if (FLAG_STAT_TERSE.is_enabled()) {
    format += is_filesystem_mode ? TERSE_FILESYSTEM_FORMAT : TERSE_FILE_FORMAT;
    return;
  }

  if (is_filesystem_mode) {
    format += DEFAULT_FILESYSTEM_FORMAT;
  }
}

} // namespace

Stat::Stat() = default;

pure fn Stat::kind() const wontthrow -> Utility::Kind { return Kind::Stat; }

fn Stat::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  let const allocator = cxt.scratch_allocator();
  let const is_filesystem_mode = FLAG_STAT_FILESYSTEM.is_enabled();
  let const should_follow =
      FLAG_STAT_DEREFERENCE.is_enabled() || is_filesystem_mode;

  let selected_format = String{allocator};
  bool should_append_newline = false;
  resolve_format(selected_format, should_append_newline, is_filesystem_mode,
                 allocator);

  let operand_paths = ArrayList<Path>{allocator};
  let file_statuses = ArrayList<os::file_status>{allocator};
  let batch = os::Batch{allocator};
  let results = ArrayList<os::batch_result>{allocator};
  if (!is_filesystem_mode) {
    operand_paths.reserve(operands.count());
    file_statuses.reserve(operands.count());
    batch.reserve(operands.count());
    for (let const &operand : operands) {
      operand_paths.push(Path{operand.view(), allocator});
      file_statuses.push({});
    }
    for (usize index = 0; index < operands.count(); index++) {
      if (should_follow) {
        batch.add(os::batch_operation::stat(operand_paths[index],
                                            file_statuses[index]));
      } else {
        batch.add(os::batch_operation::lstat(operand_paths[index],
                                             file_statuses[index]));
      }
    }
    results = batch.execute();
  }

  i32 status = 0;
  for (usize index = 0; index < operands.count(); index++) {
    let const &operand = operands[index];
    let output = String{allocator};

    if (is_filesystem_mode) {
      os::filesystem_status filesystem{};
      if (!os::stat_filesystem(operand.view(), filesystem)) {
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "cannot read filesystem of '" + operand +
                "': " + os::last_system_error_message());
        status = 1;
        continue;
      }

      render_format(output, selected_format.view(), nullptr, &filesystem,
                    operand.view(), cxt);
      if (should_append_newline) output += "\n";

      ec.print_to_stdout(output);
      continue;
    }

    file_subject subject{file_statuses[index], operand.view(),
                         String{allocator}, String{allocator}};
    if (results[index].error_number != 0) {
      os::set_last_system_error(results[index].error_number);
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "cannot stat '" + operand + "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let active_format = String{allocator, selected_format.view()};
    if (active_format.is_empty()) {
      let const type_letter = os::file_type_letter(subject.status.mode);
      let const is_device = type_letter == 'b' || type_letter == 'c';
      active_format += is_device ? DEVICE_FILE_FORMAT : DEFAULT_FILE_FORMAT;
    }

    render_format(output, active_format.view(), &subject, nullptr,
                  operand.view(), cxt);
    if (should_append_newline) output += "\n";

    ec.print_to_stdout(output);
  }

  return status;
}

} // namespace koshka::koshkit
