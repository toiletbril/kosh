/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the paste utility. It decodes delimiter escapes and
 * joins corresponding lines across files or serial lines within each file.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-s] [-d list] [file ...]");

HELP_DESCRIPTION_DECL(
    "The paste utility merges corresponding or serial lines.");

FLAG(PASTE_DELIMITERS, String, 'd', "delimiters",
     "Cycle through these delimiters.");
FLAG(PASTE_SERIAL, Bool, 's', "serial", "Paste one file at a time.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Paste);

namespace koshka::koshkit {

static fn parse_paste_delimiters(StringView text, Allocator allocator) throws
    -> String
{
  if (text.is_empty()) throw Error{"paste: delimiter list is empty"};

  String delimiters{allocator};
  for (usize position = 0; position < text.length; position++) {
    let byte = text[position];
    if (byte == '\\' && position + 1 < text.length) {
      byte = text[++position];
      if (byte == 'n')
        byte = '\n';
      else if (byte == 't')
        byte = '\t';
      else if (byte == '0')
        byte = '\0';
    }
    delimiters += byte;
  }

  return delimiters;
}

static fn append_paste_delimiter(String &output, StringView delimiters,
                                 usize delimiter_position) throws -> void
{
  let const delimiter = delimiters[delimiter_position % delimiters.length];
  if (delimiter != '\0') output += delimiter;
}

Paste::Paste() = default;

pure fn Paste::kind() const wontthrow -> Utility::Kind { return Kind::Paste; }

fn Paste::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const delimiters = parse_paste_delimiters(
      FLAG_PASTE_DELIMITERS.is_set() ? FLAG_PASTE_DELIMITERS.value()
                                     : StringView{"\t"},
      cxt.scratch_allocator());
  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let contents = ArrayList<String>{cxt.scratch_allocator()};
  contents.reserve(sources.count());
  let lines = ArrayList<ArrayList<StringView>>{cxt.scratch_allocator()};
  lines.reserve(sources.count());
  i32 status = 0;

  for (let const source : sources) {
    let content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "paste: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      contents.push(String{cxt.scratch_allocator()});
      lines.push(ArrayList<StringView>{cxt.scratch_allocator()});
      continue;
    }

    contents.push(steal(*content));
    let source_lines = utils::split_lines(contents.back().view(),
                                          cxt.scratch_allocator(), true);
    for (let &line : source_lines)
      line = line.without_trailing_newline();
    lines.push(steal(source_lines));
  }

  let output = String{cxt.scratch_allocator()};
  if (FLAG_PASTE_SERIAL.is_enabled()) {
    for (let const &source_lines : lines) {
      for (usize line_index = 0; line_index < source_lines.count();
           line_index++)
      {
        if (line_index != 0)
          append_paste_delimiter(output, delimiters.view(), line_index - 1);
        output += source_lines[line_index];
      }
      output += '\n';
    }
  } else {
    usize max_line_count = 0;
    for (let const &source_lines : lines)
      if (source_lines.count() > max_line_count)
        max_line_count = source_lines.count();

    for (usize line_index = 0; line_index < max_line_count; line_index++) {
      for (usize source_index = 0; source_index < lines.count(); source_index++)
      {
        if (source_index != 0)
          append_paste_delimiter(output, delimiters.view(), source_index - 1);
        if (line_index < lines[source_index].count())
          output += lines[source_index][line_index];
      }
      output += '\n';
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
