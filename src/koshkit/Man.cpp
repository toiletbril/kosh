#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-k keyword] | [section] name ...");

HELP_DESCRIPTION_DECL(
    "The man utility displays manual pages found through MANPATH.");

FLAG(MAN_KEYWORD, String, 'k', "keyword", "Search manual page descriptions.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Man);

namespace koshka::koshkit {

static pure fn is_manual_section(StringView value) wontthrow -> bool
{
  if (value.is_empty()) return false;
  for (usize position = 0; position < value.length; position++)
    if ((value[position] < '0' || value[position] > '9') &&
        value[position] != 'p' && value[position] != 'n')
      return false;
  return true;
}

static fn find_manual_page(StringView paths, StringView section,
                           StringView name, Allocator allocator) throws
    -> Maybe<String>
{
  usize path_start = 0;
  while (path_start <= paths.length) {
    let const remaining = paths.substring(path_start);
    let const path_length =
        remaining.find_character(':').value_or(remaining.length);
    let const directory = path_length == 0
                              ? StringView{"."}
                              : remaining.substring_of_length(0, path_length);
    for (usize candidate_section = 1; candidate_section <= 9;
         candidate_section++)
    {
      let current_section = section;
      char section_byte = '\0';
      if (current_section.is_empty()) {
        section_byte = static_cast<char>('0' + candidate_section);
        current_section = StringView{&section_byte, 1};
      }
      let path = String{allocator, directory};
      if (!path.is_empty() && path.back() != '/') path += '/';
      path += "man";
      path += current_section;
      path += '/';
      path += name;
      path += '.';
      path += current_section;
      os::file_status status{};
      if (os::stat_path_following(path.view(), status)) return path;
      if (!section.is_empty()) break;
    }
    if (path_length == remaining.length) break;
    path_start += path_length + 1;
  }
  return None;
}

Man::Man() = default;

pure fn Man::kind() const wontthrow -> Utility::Kind { return Kind::Man; }

fn Man::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const configured_paths = cxt.get_variable_value("MANPATH");
  let const paths =
      configured_paths.has_value() && !configured_paths->is_empty()
          ? configured_paths->view()
          : StringView{"/usr/share/man:/usr/local/share/man"};
  if (FLAG_MAN_KEYWORD.is_set()) {
    if (!operands.is_empty())
      return report_usage_error(ec, cxt, args[0].view());
    let keyword = String{cxt.scratch_allocator(), FLAG_MAN_KEYWORD.value()};
    keyword.lowercase_ascii();
    let output = String{cxt.scratch_allocator()};
    usize path_start = 0;
    while (path_start <= paths.length) {
      let const remaining = paths.substring(path_start);
      let const path_length =
          remaining.find_character(':').value_or(remaining.length);
      let const directory = path_length == 0
                                ? StringView{"."}
                                : remaining.substring_of_length(0, path_length);
      for (u32 section_number = 1; section_number <= 9; section_number++) {
        let manual_directory = String{cxt.scratch_allocator(), directory};
        if (!manual_directory.is_empty() && manual_directory.back() != '/')
          manual_directory += '/';
        manual_directory += "man";
        manual_directory += static_cast<char>('0' + section_number);
        let entries = os::list_directory(manual_directory.view());
        if (!entries.has_value()) continue;
        entries->sort();
        for (let const &entry : *entries) {
          if (entry.length() < 3 || entry[entry.length() - 2] != '.' ||
              entry[entry.length() - 1] !=
                  static_cast<char>('0' + section_number))
            continue;
          let path = String{cxt.scratch_allocator(), manual_directory.view()};
          path += '/';
          path += entry.view();
          let content = Path{path.view()}.read_entire_file();
          if (!content.has_value()) continue;
          let searchable = content->clone();
          searchable.lowercase_ascii();
          let name = entry.view().substring_of_length(0, entry.length() - 2);
          let lowered_name = String{cxt.scratch_allocator(), name};
          lowered_name.lowercase_ascii();
          if (std::strstr(searchable.c_str(), keyword.c_str()) == NULL &&
              std::strstr(lowered_name.c_str(), keyword.c_str()) == NULL)
            continue;
          let const newline = content->view().find_character('\n');
          let const summary = content->view().substring_of_length(
              0, newline.value_or(content->length()));
          output += name;
          output += " (";
          output += static_cast<char>('0' + section_number);
          output += ") - ";
          output += summary;
          output += '\n';
        }
      }
      if (path_length == remaining.length) break;
      path_start += path_length + 1;
    }
    ec.print_to_stdout(output);
    return output.is_empty() ? 1 : 0;
  }

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  usize name_position = 0;
  StringView section;
  if (operands.count() > 1 && is_manual_section(operands[0].view())) {
    section = operands[0].view();
    name_position = 1;
  }
  i32 status = 0;
  for (; name_position < operands.count(); name_position++) {
    let const page =
        find_manual_page(paths, section, operands[name_position].view(),
                         cxt.scratch_allocator());
    if (!page.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "man: no manual entry for '" +
                                    operands[name_position] + "'");
      status = 1;
      continue;
    }
    let const content = Path{page->view()}.read_entire_file();
    if (!content.has_value()) {
      status = 1;
      continue;
    }
    ec.print_to_stdout(content->view());
  }
  return status;
}

} // namespace koshka::koshkit
