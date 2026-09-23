/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evilfiles utility. It lists files held by visible
 * processes and filters them by process, owner, command, and path.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Toiletline.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-itw] [-p pid] [-u user] [-c command] [path ...]");

HELP_DESCRIPTION_DECL(
    "The evilfiles utility lists the files that running processes hold open.");

FLAG(EVILFILES_TERSE, Bool, 't', "terse",
     "Print only the process ids, one on each line.");
FLAG(EVILFILES_PID, String, 'p', "pid", "List only this process id.");
FLAG(EVILFILES_USER, String, 'u', "user",
     "List only the processes of this owner.");
FLAG(EVILFILES_COMMAND, String, 'c', "command",
     "List only the processes whose name starts with this text.");
FLAG(EVILFILES_NETWORK, Bool, 'i', "network", "List only socket descriptors.");
FLAG(EVILFILES_WIDE, Bool, 'w', "wide",
     "Do not truncate names to the terminal width.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilFiles);

namespace koshka::koshkit {

namespace {

struct open_file_row
{
  String command;
  String pid;
  String user;
  String descriptor;
  String type;
  String mode;
  String state;
  String device;
  String size;
  String offset;
  String node;
  String endpoint;
  String name;
};

struct column_widths
{
  usize command{7};
  usize pid{3};
  usize user{4};
  usize descriptor{2};
  usize type{4};
  usize mode{4};
  usize state{5};
  usize device{6};
  usize size{8};
  usize offset{6};
  usize node{4};
  usize endpoint{8};
};

pure fn file_type_label(u32 mode) wontthrow -> StringView
{
  switch (os::file_type_letter(mode)) {
  case 'd': return "DIR";
  case 'l': return "LINK";
  case 'c': return "CHR";
  case 'b': return "BLK";
  case 'p': return "FIFO";
  case 's': return "unix";
  default: break;
  }

  return "REG";
}

pure fn bracketed_type_label(StringView path) wontthrow -> StringView
{
  constexpr static_string_entry<StringView> ENTRIES[] = {
      {SSK("[event]"),  "EVENT" },
      {SSK("[kqueue]"), "KQUEUE"},
      {SSK("[pipe]"),   "FIFO"  },
      {SSK("[socket]"), "unix"  },
  };
  constexpr StaticStringMap TYPES{ENTRIES};
  let const found = TYPES.find(path);
  if (found.has_value()) return *found;

  return "unknown";
}

fn device_label(const os::file_status &status, Allocator allocator) throws
    -> String
{
  let const type_letter = os::file_type_letter(status.mode);
  let const device_id = type_letter == 'c' || type_letter == 'b'
                            ? status.special_device_id
                            : status.device_id;

  let label = String::from(os::device_major(device_id), allocator);
  label += ",";
  label += String::from(os::device_minor(device_id), allocator).view();
  return label;
}

fn descriptor_label(const os::process_open_file &file,
                    Allocator allocator) throws -> String
{
  switch (file.use) {
  case os::process_file_use::Cwd: return String{allocator, "cwd"};
  case os::process_file_use::Root: return String{allocator, "rtd"};
  case os::process_file_use::Executable: return String{allocator, "txt"};
  case os::process_file_use::Mapped: return String{allocator, "mem"};
  case os::process_file_use::File: break;
  }

  if (file.descriptor_number < 0) return String{allocator, "unk"};

  let label = String::from(static_cast<u64>(file.descriptor_number), allocator);
  label += StringView{&file.access, 1};
  return label;
}

fn widen(usize &width, const String &text) wontthrow -> void
{
  if (text.length() > width) width = text.length();
}

fn matches_filters(const os::process_entry &process, i64 wanted_pid,
                   bool has_wanted_pid, Maybe<u32> wanted_owner) wontthrow
    -> bool
{
  if (has_wanted_pid && process.pid != wanted_pid) return false;

  if (wanted_owner.has_value() && process.owner_id != *wanted_owner) {
    return false;
  }

  if (FLAG_EVILFILES_COMMAND.is_set() &&
      !process.name.view().starts_with(FLAG_EVILFILES_COMMAND.value()))
  {
    return false;
  }

  return true;
}

fn matches_path_operands(StringView path,
                         const ArrayList<String> &operands) wontthrow -> bool
{
  if (operands.is_empty()) return true;

  for (let const &operand : operands) {
    let const wanted = operand.view();
    if (path == wanted) return true;

    if (path.starts_with(wanted) && wanted.length > 0 &&
        path.length > wanted.length && path[wanted.length] == '/')
    {
      return true;
    }
  }

  return false;
}

pure fn is_network_file(StringView path) wontthrow -> bool
{
  return path == "[socket]" || path.starts_with("socket:[");
}

} /* namespace */

EvilFiles::EvilFiles() = default;

pure fn EvilFiles::kind() const wontthrow -> Utility::Kind
{
  return Kind::EvilFiles;
}

fn EvilFiles::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = PARSE_KOSHKIT_ARGS(args, arg_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();

  if (!os::has_process_open_file_listing()) {
    report_soft_koshkit_error(ec, cxt,
                              "open file listing is unavailable",
                              "this platform exposes no open file table");
    return 1;
  }

  i64 wanted_pid = 0;
  let const has_wanted_pid = FLAG_EVILFILES_PID.is_set();
  if (has_wanted_pid) {
    let const parsed = utils::parse_integer_in_base(FLAG_EVILFILES_PID.value(),
                                                    int_base::decimal);
    if (parsed.is_error()) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_EVILFILES_PID.value_location(),
          "invalid process id '" +
              String{allocator, FLAG_EVILFILES_PID.value()} + "'",
          "provide a decimal process id");
      return 1;
    }

    wanted_pid = static_cast<i64>(parsed.value());
  }

  Maybe<u32> wanted_owner = None;
  if (FLAG_EVILFILES_USER.is_set()) {
    wanted_owner = os::username_to_uid(FLAG_EVILFILES_USER.value());
    if (!wanted_owner.has_value()) {
      let const parsed = utils::parse_integer_in_base(
          FLAG_EVILFILES_USER.value(), int_base::decimal);
      if (parsed.is_error()) {
        KOSHKIT_REPORT_ERROR_AT(
            FLAG_EVILFILES_USER.value_location(),
            "no such user '" + String{allocator, FLAG_EVILFILES_USER.value()} +
                "'",
            "provide a user name or numeric uid");
        return 1;
      }

      wanted_owner = static_cast<u32>(parsed.value());
    }
  }

  let const processes = os::enumerate_processes();
  if (processes.is_empty()) {
    report_soft_koshkit_error(ec, cxt,
                              "the process listing is unavailable",
                              "this platform exposes no process table");
    return 1;
  }

  ArrayList<open_file_row> rows{allocator};
  column_widths widths{};
  let terse_output = String{allocator};
  bool did_match = false;
  usize inaccessible_process_count = 0;

  for (let const &process : processes) {
    if (!matches_filters(process, wanted_pid, has_wanted_pid, wanted_owner)) {
      continue;
    }

    let const files = os::list_process_open_files(process.pid, allocator);
    if (files.is_empty()) continue;

    if (files.count() == 1 && files[0].is_inaccessible) {
      inaccessible_process_count++;
      continue;
    }

    if (FLAG_EVILFILES_TERSE.is_enabled()) {
      bool did_match_this_process = false;
      for (let const &file : files) {
        if (FLAG_EVILFILES_NETWORK.is_enabled() &&
            !is_network_file(file.path.view()))
        {
          continue;
        }
        if (!matches_path_operands(file.path.view(), operands)) continue;

        did_match = true;
        did_match_this_process = true;
        break;
      }
      if (did_match_this_process) {
        terse_output +=
            String::from(static_cast<u64>(process.pid), allocator).view();
        terse_output += "\n";
      }
      continue;
    }

    let matching_positions = ArrayList<usize>{allocator};
    let matching_paths = ArrayList<Path>{allocator};
    for (usize file_position = 0; file_position < files.count();
         file_position++)
    {
      if (FLAG_EVILFILES_NETWORK.is_enabled() &&
          !is_network_file(files[file_position].path.view()))
      {
        continue;
      }
      if (!matches_path_operands(files[file_position].path.view(), operands))
        continue;

      matching_positions.push(file_position);
      matching_paths.push(Path{files[file_position].path.view()});
    }
    if (matching_positions.is_empty()) continue;

    did_match = true;
    let file_statuses = ArrayList<os::file_status>{allocator};
    let metadata_batch = os::Batch{allocator};
    file_statuses.reserve(matching_positions.count());
    metadata_batch.reserve(matching_positions.count());
    for (usize position = 0; position < matching_positions.count(); position++)
      file_statuses.push({});
    for (usize position = 0; position < matching_positions.count(); position++)
      metadata_batch.add(os::batch_operation::stat(matching_paths[position],
                                                   file_statuses[position]));
    let const metadata_results = metadata_batch.execute();
    let const owner = os::process_owner_name(static_cast<u32>(process.pid),
                                             process.owner_id, allocator);

    for (usize position = 0; position < matching_positions.count(); position++)
    {
      let const &file = files[matching_positions[position]];
      let const &status = file_statuses[position];
      let const did_stat = metadata_results[position].error_number == 0;

      open_file_row row{
          String{allocator, process.name.view()                                                         },
          String::from(static_cast<u64>(process.pid), allocator),
          owner.has_value() ? String{allocator, owner->view()                                                               }
                            : String::from(process.owner_id, allocator),
          descriptor_label(file, allocator),
          String{allocator, did_stat ? file_type_label(status.mode)
                                     : bracketed_type_label(file.path.view())},
          did_stat ? os::format_mode_string(status.mode)
                   : String{allocator, "-"},
          String{allocator, file.is_deleted ? "deleted" : "-"},
          did_stat ? device_label(status, allocator) : String{allocator, "-"                                                                         },
          String::from(file.size != 0 || !did_stat ? file.size : status.size,
                       allocator),
          String::from(file.offset, allocator),
          String::from(file.file_id != 0 || !did_stat ? file.file_id
                                                      : status.file_id,
                       allocator),
          file.socket_endpoint,
          String{allocator, file.path.view()                                                            },
      };

      widen(widths.command, row.command);
      widen(widths.pid, row.pid);
      widen(widths.user, row.user);
      widen(widths.descriptor, row.descriptor);
      widen(widths.type, row.type);
      widen(widths.mode, row.mode);
      widen(widths.state, row.state);
      widen(widths.device, row.device);
      widen(widths.size, row.size);
      widen(widths.offset, row.offset);
      widen(widths.node, row.node);
      widen(widths.endpoint, row.endpoint);
      rows.push(steal(row));
    }
  }

  let warnings = String{allocator};
  if (inaccessible_process_count != 0) {
    warnings += "Skipped ";
    warnings += String::from(inaccessible_process_count, allocator).view();
    warnings += inaccessible_process_count == 1 ? " inaccessible process."
                                                : " inaccessible processes.";
  }

  if (FLAG_EVILFILES_TERSE.is_enabled()) {
    ec.print_to_stdout(terse_output);
    return did_match ? 0 : 1;
  }

  if (rows.is_empty()) {
    if (!warnings.is_empty())
      show_warning(warnings.view());
    return 1;
  }

  let const should_color = koshkit_should_color();
  let output = String{allocator};
  usize line_width_limit = SIZE_MAX;
  if (!FLAG_EVILFILES_WIDE.is_enabled()) {
    u32 terminal_columns = 0;
    u32 terminal_rows = 0;
    if (os::terminal_size(terminal_columns, terminal_rows,
                          ec.out_fd.value_or(KOSH_STDOUT)) &&
        terminal_columns > 8)
      line_width_limit = terminal_columns;
  }
  if (line_width_limit != SIZE_MAX) {
    let const fixed_width = 2 + widths.pid + 2 + widths.user + 2 +
                            widths.descriptor + 2 + widths.type + 2 +
                            widths.mode + 2 + widths.state + 2 +
                            widths.device + 2 + widths.size + 2 +
                            widths.offset + 2 + widths.node + 2 +
                            widths.endpoint + 2 + 3;
    if (line_width_limit > fixed_width + 4) {
      let const command_limit = line_width_limit - fixed_width;
      if (widths.command > command_limit) widths.command = command_limit;
    }
  }
  append_report_text(output, "Open files", colors::ansi::BOLD_BLUE,
                     should_color);
  output += '\n';
  output += "  ";
  append_report_column(output, "COMMAND", widths.command, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "PID", widths.pid, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "USER", widths.user, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "FD", widths.descriptor, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "TYPE", widths.type, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "MODE", widths.mode, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "STATE", widths.state, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "DEVICE", widths.device, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "SIZE/OFF", widths.size, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "OFFSET", widths.offset, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "NODE", widths.node, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "ENDPOINT", widths.endpoint, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_text(output, "NAME", colors::ansi::BOLD_CYAN, should_color);
  output += "\n";

  for (let const &row : rows) {
    output += "  ";
    let command = String{allocator, row.command.view()};
    if (toiletline::display_width(command.view()) > widths.command &&
        widths.command > 3)
    {
      usize actual_cells = 0;
      let const kept_bytes =
          toiletline::byte_offset_at_or_before_display_cell(
              command.view(), widths.command - 3, actual_cells);
      command.truncate(kept_bytes);
      command += "...";
    }
    append_report_column(output, command.view(), widths.command, false,
                         colors::ansi::BOLD_GREEN, should_color);
    output += "  ";
    append_report_column(output, row.pid.view(), widths.pid, true,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, row.user.view(), widths.user, false,
                         colors::ansi::YELLOW, should_color);
    output += "  ";
    append_report_column(output, row.descriptor.view(), widths.descriptor, true,
                         colors::ansi::CYAN, should_color);
    output += "  ";
    append_report_column(output, row.type.view(), widths.type, false,
                         colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_column(output, row.mode.view(), widths.mode, false,
                         colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_column(output, row.state.view(), widths.state, false,
                         row.state == "deleted" ? colors::ansi::BOLD_RED
                                                : colors::ansi::GREEN,
                         should_color);
    output += "  ";
    append_report_column(output, row.device.view(), widths.device, true,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, row.size.view(), widths.size, true,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, row.offset.view(), widths.offset, true,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, row.node.view(), widths.node, true,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, row.endpoint.view(), widths.endpoint, false,
                         colors::ansi::CYAN, should_color);
    output += "  ";
    if (line_width_limit != SIZE_MAX) {
      usize const used_width =
          2 + widths.command + 2 + widths.pid + 2 + widths.user + 2 +
          widths.descriptor + 2 + widths.type + 2 + widths.mode + 2 +
          widths.state + 2 + widths.device + 2 + widths.size + 2 +
          widths.offset + 2 + widths.node + 2 + widths.endpoint + 2;
      if (used_width + toiletline::display_width(row.name.view()) >
          line_width_limit)
      {
        if (line_width_limit > used_width + 4) {
          String name = String{allocator, row.name.view()};
          usize actual_cells = 0;
          let const kept_bytes =
              toiletline::byte_offset_at_or_before_display_cell(
                  name.view(), line_width_limit - used_width - 3,
                  actual_cells);
          name.truncate(kept_bytes);
          name += "...";
          output += name.view();
        } else {
          output += "...";
        }
        output += "\n";
        continue;
      }
    }
    output += row.name.view();
    output += "\n";
  }

  ec.print_to_stdout(output);
  if (!warnings.is_empty())
    show_warning(warnings.view());
  return 0;
}

} /* namespace koshka::koshkit */
