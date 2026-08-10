#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-c] [-r|-n|-a|-w] [-p arg ...] [count]");
HELP_DESCRIPTION_DECL(
    "The history builtin lists and maintains the interactive command history.");

FLAG(HISTORY_CLEAR, Bool, 'c', "", "Clear the history list.");
FLAG(HISTORY_DELETE, Bool, 'd', "",
     "Accept a delete offset operand, leaving the list unchanged.");
FLAG(HISTORY_APPEND, Bool, 'a', "", "Write the history list to the file.");
FLAG(HISTORY_READ_NEW, Bool, 'n', "", "Read the history file into the list.");
FLAG(HISTORY_READ, Bool, 'r', "", "Read the history file into the list.");
FLAG(HISTORY_WRITE, Bool, 'w', "", "Write the history list to the file.");
FLAG(HISTORY_PRINT, Bool, 'p', "", "Print the operands, storing nothing.");
FLAG(HISTORY_STORE, Bool, 's', "",
     "Accept operands to store, leaving the list unchanged.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(History);

namespace koshka {

History::History() = default;

pure fn History::kind() const wontthrow -> Builtin::Kind
{
  return Kind::History;
}

static fn print_history_list(const ExecContext &ec, EvalContext &cxt,
                             usize wanted_count) throws -> void
{
  let const path = toiletline::history_path();
  if (!path.has_value()) return;

  let const contents = path->read_entire_file();
  if (!contents.has_value()) return;

  let const text = contents->view();
  let lines = ArrayList<StringView>{cxt.scratch_allocator()};
  usize line_start = 0;
  for (usize i = 0; i < text.length; i++)
    if (text[i] == '\n') {
      lines.push(text.substring_of_length(line_start, i - line_start));
      line_start = i + 1;
    }

  if (line_start < text.length) lines.push(text.substring(line_start));

  usize first_index = 0;
  if (wanted_count != 0 && wanted_count < lines.count()) {
    first_index = lines.count() - wanted_count;
  }

  let out = String{cxt.scratch_allocator()};
  for (usize i = first_index; i < lines.count(); i++) {
    char number_buffer[24];
    let const number = utils::int_to_text_into(
        static_cast<i64>(i + 1), number_buffer, sizeof(number_buffer));
    for (usize pad = number.length; pad < 5; pad++)
      out += ' ';
    out.append(number);
    out += "  ";
    out.append(lines[i]);
    out += '\n';
  }
  ec.print_to_stdout(out);
}

static fn append_file_into_history(EvalContext &cxt, const Path &source) throws
    -> bool
{
  let const backing = toiletline::history_path();
  if (!backing.has_value()) return false;

  let const source_text = source.read_entire_file();
  if (!source_text.has_value()) return false;
  if (source_text->is_empty()) return true;

  let const opened = os::open_file_descriptor(backing->text().view(),
                                              os::file_open_mode::Append);
  if (!opened.has_value()) return false;

  let const fd = opened.value();
  let payload = String{cxt.scratch_allocator(), source_text->view()};
  if (payload[payload.count() - 1] != '\n') payload += '\n';

  let const written = os::write_fd(fd, payload.data(), payload.count());
  os::close_fd(fd);
  return written.has_value() && written.value() == payload.count();
}

static fn write_history_to_file(EvalContext &cxt, const Path &target) throws
    -> bool
{
  let contents = String{cxt.scratch_allocator()};

  if (let const source_path = toiletline::history_path();
      source_path.has_value())
  {
    let const source_text = source_path->read_entire_file();
    if (source_text.has_value()) contents.append(source_text->view());
  }

  let const opened = os::open_file_descriptor(target.text().view(),
                                              os::file_open_mode::Truncate);
  if (!opened.has_value()) return false;

  let const fd = opened.value();

  bool was_written = true;
  if (!contents.is_empty()) {
    let const written = os::write_fd(fd, contents.data(), contents.count());
    was_written = written.has_value() && written.value() == contents.count();
  }

  os::close_fd(fd);
  return was_written;
}

fn History::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let did_maintain_list = false;

  if (FLAG_HISTORY_CLEAR.is_enabled()) {
    LOG(Debug, "history clearing the list");
    toiletline::history_clear();
    did_maintain_list = true;
  }

  if (FLAG_HISTORY_READ.is_enabled() || FLAG_HISTORY_READ_NEW.is_enabled()) {
    LOG(Debug, "history reading the file into the list");

    if (args.count() > 1)
      cxt.guard_restricted_path(args[1].view(), ec.arg_location_at(1),
                                restricted_path_use::History);

    if (args.count() > 1 &&
        !append_file_into_history(cxt, Path{args[1].view()}))
    {
      report_soft_builtin_error(
          ec, cxt, ec.arg_location_at(1),
          StringView{"cannot read history from '"} + args[1].view() +
              "': " + os::last_system_error_message(),
          "Pass a readable history file, e.g. `history -r ~/.kosh_history`");
      return 1;
    }

    toiletline::history_read();
    did_maintain_list = true;
  }

  if (FLAG_HISTORY_APPEND.is_enabled() || FLAG_HISTORY_WRITE.is_enabled()) {
    LOG(Debug, "history writing the list to the file");

    if (args.count() > 1) {
      cxt.guard_restricted_path(args[1].view(), ec.arg_location_at(1),
                                restricted_path_use::History);
      let const target = Path{args[1].view()};
      if (!write_history_to_file(cxt, target)) {
        report_soft_builtin_error(
            ec, cxt, ec.arg_location_at(1),
            StringView{"cannot write history to '"} + args[1].view() +
                "': " + os::last_system_error_message(),
            "Pass a writable path, e.g. `history -w ~/.kosh_history`");
        return 1;
      }
    } else {
      toiletline::history_write();
    }

    did_maintain_list = true;
  }

  if (FLAG_HISTORY_PRINT.is_enabled()) {
    let out = String{cxt.scratch_allocator()};
    for (usize i = 1; i < args.count(); i++) {
      out.append(args[i].view());
      out += '\n';
    }
    ec.print_to_stdout(out);
    did_maintain_list = true;
  }

  if (FLAG_HISTORY_DELETE.is_enabled() || FLAG_HISTORY_STORE.is_enabled()) {
    did_maintain_list = true;
  }

  if (did_maintain_list) return 0;

  usize wanted_count = 0;
  if (args.count() > 1) {
    let const parsed = utils::parse_decimal_i64(args[1].view());
    if (parsed.is_error()) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                StringView{"'"} + args[1].view() +
                                    "' is not a valid count");
      return 2;
    }

    if (parsed.value() > 0) wanted_count = static_cast<usize>(parsed.value());
  }

  print_history_list(ec, cxt, wanted_count);
  return 0;
}

} /* namespace koshka */
