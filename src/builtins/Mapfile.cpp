#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-t] [-n count] [array]");

HELP_DESCRIPTION_DECL(
    "The mapfile builtin reads standard input lines into an indexed array.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(MAPFILE_TRIM, Bool, 't', "", "Strip the trailing newline from each line.");
FLAG(MAPFILE_COUNT, String, 'n', "",
     "Read at most count lines, or all of them when count is zero.");
FLAG(MAPFILE_SKIP, String, 's', "", "Discard count lines before reading.");
FLAG(MAPFILE_ORIGIN, String, 'O', "", "Begin storing at the array index.");
FLAG(MAPFILE_DELIMITER, String, 'd', "", "Use the first byte as delimiter.");
FLAG(MAPFILE_FD, String, 'u', "", "Read from the file descriptor.");
FLAG(MAPFILE_CALLBACK, String, 'C', "", "Accept the callback name.");
FLAG(MAPFILE_QUANTUM, String, 'c', "", "Accept the callback interval.");

REGISTER_BUILTIN_FLAGS(Mapfile);

namespace koshka {

Mapfile::Mapfile() = default;

pure fn Mapfile::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Mapfile;
}

fn Mapfile::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = parse_flags_vec(
      FLAG_LIST, ec.args(), ec.source_location().position, nullptr,
      &ec.arg_locations(), nullptr, builtin_error_context(ec.program()));
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let const do_parse_count = [&](const FlagString &flag, StringView label,
                                 StringView note, i64 &value) throws -> bool {
    if (!flag.is_set()) return true;
    let const parsed = flag.value().to<i64>();
    if (!parsed.is_error() && parsed.value() >= 0) {
      value = parsed.value();
      return true;
    }
    report_soft_builtin_error(ec, cxt, flag.value_location(),
                              flag.value() + label, note);
    return false;
  };

  i64 max_lines = 0;
  i64 skip_count = 0;
  i64 origin = 0;
  if (!do_parse_count(FLAG_MAPFILE_COUNT, ": invalid line count",
                      "The -n count must be a whole number", max_lines) ||
      !do_parse_count(FLAG_MAPFILE_SKIP, ": invalid line count",
                      "The -s count must be a whole number", skip_count) ||
      !do_parse_count(FLAG_MAPFILE_ORIGIN, ": invalid array origin",
                      "The -O origin must be a whole number", origin))
  {
    return 1;
  }

  let read_fd = ec.in_fd.value_or(KOSH_STDIN);
  if (FLAG_MAPFILE_FD.is_set()) {
    let const parsed_fd = FLAG_MAPFILE_FD.value().to<i64>();
    if (parsed_fd.is_error() || parsed_fd.value() < 0) {
      report_soft_builtin_error(ec, cxt, FLAG_MAPFILE_FD.value_location(),
                                FLAG_MAPFILE_FD.value() +
                                    ": invalid file descriptor");
      return 1;
    }
    read_fd = os::descriptor_from_fd_number(parsed_fd.value());
  }

  let const array_name =
      args.count() > 1 ? args[1].view() : StringView{"MAPFILE"};
  let const should_strip_newline = FLAG_MAPFILE_TRIM.is_enabled();
  let const has_origin = FLAG_MAPFILE_ORIGIN.is_set();
  let const delimiter = FLAG_MAPFILE_DELIMITER.is_set()
                            ? (FLAG_MAPFILE_DELIMITER.value().is_empty()
                                   ? '\0'
                                   : FLAG_MAPFILE_DELIMITER.value()[0])
                            : '\n';

  LOG(Debug, "mapfile reading lines into array '%.*s'",
      static_cast<int>(array_name.length), array_name.data);

  /* The skipped lines are read and dropped before any line is stored, so -s
     does not count against -n. */
  for (i64 skipped = 0; skipped < skip_count; skipped++) {
    bool was_terminated = false;
    if (!utils::read_line_from_fd(read_fd, was_terminated, delimiter)) break;
  }

  let lines = ArrayList<String>{heap_allocator()};
  loop
  {
    if (max_lines > 0 && static_cast<i64>(lines.count()) >= max_lines) {
      break;
    }

    bool was_newline_terminated = false;
    let const read =
        utils::read_line_from_fd(read_fd, was_newline_terminated, delimiter);
    if (!read) break;

    let element = String{read->view()};
    if (!should_strip_newline && was_newline_terminated) {
      element.push(delimiter);
    }
    lines.push(steal(element));
  }

  LOG(Debug, "mapfile stored %zu lines", lines.count());

  if (has_origin) {
    for (usize element_index = 0; element_index < lines.count();
         element_index++)
    {
      char index_text[24];
      let const subscript =
          utils::int_to_text_into(origin + static_cast<i64>(element_index),
                                  index_text, sizeof(index_text));
      cxt.assign_array_element(array_name, subscript,
                               lines[element_index].view(), false);
    }
  } else {
    cxt.set_indexed_array(array_name, steal(lines));
  }
  return 0;
}

} /* namespace koshka */
