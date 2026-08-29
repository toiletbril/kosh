#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-hL] [file ...]");

HELP_DESCRIPTION_DECL("The file utility classifies file operands.");

FLAG(FILE_NO_FOLLOW, Bool, 'h', "no-dereference", "Classify symbolic links.");
FLAG(FILE_FOLLOW, Bool, 'L', "dereference", "Follow symbolic links.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(File);

namespace koshka::koshkit {

static pure fn file_content_description(StringView bytes) wontthrow
    -> StringView
{
  if (bytes.is_empty()) return "empty";
  if (bytes.length >= 4 && static_cast<u8>(bytes[0]) == 0x7f &&
      bytes.substring_of_length(1, 3) == "ELF")
    return "ELF executable";
  if (bytes.length >= 2 && bytes[0] == '#' && bytes[1] == '!')
    return "script text executable";

  bool is_text = true;
  for (usize position = 0; position < bytes.length; position++) {
    let const byte = static_cast<u8>(bytes[position]);
    if (byte == 0 || (byte < 0x20 && byte != '\n' && byte != '\r' &&
                      byte != '\t' && byte != '\f' && byte != '\b'))
    {
      is_text = false;
      break;
    }
  }

  return is_text ? StringView{"text"} : StringView{"data"};
}

File::File() = default;

pure fn File::kind() const wontthrow -> Utility::Kind { return Kind::File; }

fn File::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const should_follow =
      FLAG_FILE_FOLLOW.position() > FLAG_FILE_NO_FOLLOW.position();
  i32 status = 0;

  for (let const &operand : operands) {
    os::file_status file_status{};
    let const did_stat =
        should_follow ? os::stat_path_following(operand.view(), file_status)
                      : os::stat_path(operand.view(), file_status);
    if (!did_stat) {
      report_soft_koshkit_error(ec, cxt,
                                "file: cannot open '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    StringView description;
    switch (os::file_type_letter(file_status.mode)) {
    case 'd': description = "directory"; break;
    case 'l': description = "symbolic link"; break;
    case 'p': description = "fifo"; break;
    case 's': description = "socket"; break;
    case 'b': description = "block special file"; break;
    case 'c': description = "character special file"; break;
    default: {
      let const descriptor =
          os::open_file_descriptor(operand.view(), os::file_open_mode::Read);
      if (!descriptor.has_value()) {
        report_soft_koshkit_error(ec, cxt,
                                  "file: cannot read '" + operand +
                                      "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      defer { os::close_fd(*descriptor); };

      char buffer[8192];
      let const read_count = os::read_fd(*descriptor, buffer, sizeof(buffer));
      if (!read_count.has_value()) {
        report_soft_koshkit_error(ec, cxt,
                                  "file: cannot read '" + operand +
                                      "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      description = file_content_description(StringView{buffer, *read_count});
      break;
    }
    }

    ec.print_to_stdout(operand + ": " + description + "\n");
  }

  return status;
}

} // namespace koshka::koshkit
