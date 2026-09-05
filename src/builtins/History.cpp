/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements history listing, clearing, file reading and writing,
 * appended persistence, operand printing, and accepted compatibility
 * operations for the history builtin.
 */

#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-c] [-d offset] [-r|-n|-a|-w] [-p arg ...] [count]",
                   "-s [arg ...]");
HELP_DESCRIPTION_DECL(
    "The history builtin lists and maintains the interactive command history.");

FLAG(HISTORY_CLEAR, Bool, 'c', "", "Clear the history list.");
FLAG(HISTORY_DELETE, String, 'd', "", "Delete an event or event range.");
FLAG(HISTORY_APPEND, Bool, 'a', "", "Write the history list to the file.");
FLAG(HISTORY_READ_NEW, Bool, 'n', "", "Read the history file into the list.");
FLAG(HISTORY_READ, Bool, 'r', "", "Read the history file into the list.");
FLAG(HISTORY_WRITE, Bool, 'w', "", "Write the history list to the file.");
FLAG(HISTORY_PRINT, Bool, 'p', "", "Print the operands, storing nothing.");
FLAG(HISTORY_STORE, Bool, 's', "", "Store the operands as a history event.");
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
  let events = toiletline::history_events(cxt.scratch_allocator());

  usize first_index = 0;
  if (wanted_count != 0 && wanted_count < events.count()) {
    first_index = events.count() - wanted_count;
  }

  let out = String{cxt.scratch_allocator()};
  for (usize i = first_index; i < events.count(); i++) {
    char number_buffer[24];
    let const number =
        utils::int_to_text_into(static_cast<i64>(events[i].number),
                                number_buffer, sizeof(number_buffer));
    out.append_repeated(' ', number.length < 5 ? 5 - number.length : 0);
    out.append(number);
    out += "  ";
    out.append(events[i].command.view());
    out += '\n';
  }
  ec.print_to_stdout(out);
}

static fn append_contents_into_history(EvalContext &cxt,
                                       StringView source_text) throws -> bool
{
  let const backing = toiletline::history_path();
  if (!backing.has_value()) return false;
  let parent = backing->parent();
  if (parent.text().is_empty()) parent = Path{"."};
  let lock = os::acquire_process_lock(parent.text().view());
  if (!lock.has_value()) return false;
  defer { os::release_process_lock(lock.take()); };

  bool needs_separator = false;
  if (backing->exists()) {
    let opened = os::open_file_descriptor(backing->text().view(),
                                          os::file_open_mode::Read);
    if (!opened.has_value()) return false;
    let const fd = opened.value();
    char buffer[2048];
    bool has_bytes = false;
    char last_byte = '\0';
    for (;;) {
      let const read_count = os::read_fd(fd, buffer, sizeof(buffer));
      if (!read_count.has_value()) {
        os::close_fd(fd);
        return false;
      }
      if (*read_count == 0) break;
      has_bytes = true;
      last_byte = buffer[*read_count - 1];
    }
    os::close_fd(fd);
    needs_separator = has_bytes && last_byte != '\n';
  }

  let payload = String{cxt.scratch_allocator()};
  let contents = source_text;
  if (needs_separator ||
      (!source_text.is_empty() && source_text[source_text.length - 1] != '\n'))
  {
    payload.reserve(source_text.length + (needs_separator ? 1 : 0) + 1);
    if (needs_separator) payload.push('\n');
    payload.append(source_text);
    if (!source_text.is_empty() && source_text[source_text.length - 1] != '\n')
      payload.push('\n');
    contents = payload.view();
  }

  let const opened = os::open_file_descriptor(backing->text().view(),
                                              os::file_open_mode::Append);
  if (!opened.has_value()) return false;

  let const fd = opened.value();
  let const was_written =
      contents.is_empty() || os::write_all(fd, contents.data, contents.length);
  os::close_fd(fd);
  return was_written;
}

static fn write_history_to_file(const Path &target) throws -> bool
{
  let contents = Maybe<String>{None};
  if (let const source_path = toiletline::history_path();
      source_path.has_value() && source_path->exists())
  {
    contents = source_path->read_entire_file();
    if (!contents.has_value()) return false;
  }

  let const opened = os::open_file_descriptor(target.text().view(),
                                              os::file_open_mode::Truncate);
  if (!opened.has_value()) return false;

  let const fd = opened.value();

  bool was_written = true;
  if (contents.has_value() && !contents->is_empty())
    was_written = os::write_all(fd, contents->data(), contents->count());

  os::close_fd(fd);
  return was_written;
}

struct history_selection
{
  usize first_index;
  usize last_index;
};

static fn parse_history_selection(
    StringView specification,
    const ArrayList<toiletline::history_event> &events) throws
    -> Maybe<history_selection>
{
  if (events.is_empty()) return None;

  Maybe<usize> separator{None};
  for (usize position = 1; position < specification.length; position++) {
    if (specification[position] != '-') continue;
    separator = position;
    break;
  }

  let const do_resolve = [&](StringView text) throws -> Maybe<usize> {
    let const parsed = utils::parse_decimal_i64(text);
    if (parsed.is_error() || parsed.value() == 0) return None;

    if (parsed.value() < 0) {
      let const index = static_cast<i64>(events.count()) + parsed.value();
      if (index < 0) return None;
      return static_cast<usize>(index);
    }

    let const number = static_cast<usize>(parsed.value());
    for (usize index = 0; index < events.count(); index++)
      if (events[index].number == number) return index;

    return None;
  };

  if (!separator.has_value()) {
    let const index = do_resolve(specification);
    if (!index.has_value()) return None;
    return history_selection{*index, *index};
  }

  let const first_index =
      do_resolve(specification.substring_of_length(0, *separator));
  let const last_index = do_resolve(specification.substring(*separator + 1));
  if (!first_index.has_value() || !last_index.has_value() ||
      *first_index > *last_index)
  {
    return None;
  }

  return history_selection{*first_index, *last_index};
}

fn History::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  toiletline::set_history_limit(
      cxt.get_history_limit("KOSH_HISTORY_SIZE", 4096));

  let did_maintain_list = false;

  if (FLAG_HISTORY_CLEAR.is_enabled()) {
    LOG(Debug, "history clearing the list");
    if (!toiletline::history_clear()) {
      report_soft_builtin_error(ec, cxt, ec.source_location(),
                                "Unable to clear the history list");
      return 1;
    }

    did_maintain_list = true;
  }

  if (FLAG_HISTORY_READ.is_enabled() || FLAG_HISTORY_READ_NEW.is_enabled()) {
    LOG(Debug, "history reading the file into the list");

    if (args.count() > 1)
      cxt.guard_restricted_path(args[1].view(), ec.arg_location_at(1),
                                restricted_path_use::History);

    if (args.count() > 1) {
      let const source = Path{args[1].view()};
      let const source_text = source.read_entire_file();
      if (!source_text.has_value()) {
        report_soft_builtin_error(
            ec, cxt, ec.arg_location_at(1),
            StringView{"cannot read history from '"} + args[1].view() +
                "': " + os::last_system_error_message(),
            "Pass a readable history file, e.g. `history -r ~/.kosh_history`");
        return 1;
      }
      if (!toiletline::is_history_contents_valid(source_text->view())) {
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                  StringView{"cannot read history from '"} +
                                      args[1].view() +
                                      "': the file contains invalid data",
                                  "Pass a text history file");
        return 1;
      }
      if (!append_contents_into_history(cxt, source_text->view())) {
        report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                  "Unable to append the history file");
        return 1;
      }
    }

    if (!toiletline::history_read()) {
      report_soft_builtin_error(ec, cxt, ec.source_location(),
                                "Unable to read the history file");
      return 1;
    }

    did_maintain_list = true;
  }

  if (FLAG_HISTORY_APPEND.is_enabled() || FLAG_HISTORY_WRITE.is_enabled()) {
    LOG(Debug, "history writing the list to the file");

    if (args.count() > 1) {
      cxt.guard_restricted_path(args[1].view(), ec.arg_location_at(1),
                                restricted_path_use::History);
      let const target = Path{args[1].view()};
      if (!write_history_to_file(target)) {
        report_soft_builtin_error(
            ec, cxt, ec.arg_location_at(1),
            StringView{"cannot write history to '"} + args[1].view() +
                "': " + os::last_system_error_message(),
            "Pass a writable path, e.g. `history -w ~/.kosh_history`");
        return 1;
      }
    } else {
      if (!toiletline::history_write()) {
        report_soft_builtin_error(ec, cxt, ec.source_location(),
                                  "Unable to write the history file");
        return 1;
      }
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

  if (FLAG_HISTORY_DELETE.is_set()) {
    let events = toiletline::history_events(cxt.scratch_allocator());
    let const selection =
        parse_history_selection(FLAG_HISTORY_DELETE.value(), events);
    if (!selection.has_value()) {
      report_soft_builtin_error(ec, cxt, FLAG_HISTORY_DELETE.value_location(),
                                FLAG_HISTORY_DELETE.value() +
                                    ": history position out of range");
      return 1;
    }

    for (usize index = selection->last_index + 1;
         index-- > selection->first_index;)
    {
      let const &event = events[index];
      if (!toiletline::history_rewrite_event(event.number, event.command.view(),
                                             ""))
      {
        report_soft_builtin_error(ec, cxt, FLAG_HISTORY_DELETE.value_location(),
                                  "Unable to delete the history event");
        return 1;
      }
    }

    did_maintain_list = true;
  }

  if (FLAG_HISTORY_STORE.is_enabled()) {
    if (args.count() > 1) {
      let event = String{cxt.scratch_allocator()};
      for (usize index = 1; index < args.count(); index++) {
        if (index > 1) event.push(' ');
        event.append(args[index].view());
      }

      if (!toiletline::history_append_event(event.view()).has_value()) {
        report_soft_builtin_error(ec, cxt, ec.source_location(),
                                  "Unable to store the history event");
        return 1;
      }
    }

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
