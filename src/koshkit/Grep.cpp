/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the grep utility. It compiles the requested regular
 * expression and streams matching or inverted lines with optional ASCII case
 * folding.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ivrnh] pattern [file ...]");

HELP_DESCRIPTION_DECL(
    "The grep utility prints the lines of each file that match a pattern.");

FLAG(GREP_IGNORE_CASE, Bool, 'i', "", "Match without regard to letter case.");
FLAG(GREP_INVERT, Bool, 'v', "", "Print the lines that do not match.");
FLAG(GREP_RECURSIVE, Bool, 'r', "recursive", "Search directories recursively.");
FLAG(GREP_LINE_NUMBER, Bool, 'n', "line-number", "Prefix matching lines with numbers.");
FLAG(GREP_NO_FILENAME, Bool, 'h', "no-filename", "Suppress file-name prefixes.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Grep);

namespace koshka {

namespace koshkit {

constexpr usize GREP_UNKNOWN_BATCH_COUNT = 512;

static pure fn is_literal_search_pattern(StringView pattern) wontthrow -> bool
{
  for (usize index = 0; index < pattern.length; index++) {
    switch (pattern[index]) {
      case '.':
      case '^':
      case '$':
      case '*':
      case '+':
      case '?':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '|':
      case '\\': return false;
      default: break;
    }
  }

  return true;
}

static pure fn is_ascii_pattern(StringView pattern) wontthrow -> bool
{
  for (usize index = 0; index < pattern.length; index++)
    if (static_cast<unsigned char>(pattern[index]) > 0x7f) return false;
  return true;
}

static fn collect_recursive_sources(const ExecContext &ec, EvalContext &cxt,
                                    const Path &path,
                                    Path::entry_kind path_kind,
                                    Allocator allocator,
                                    ArrayList<String> &storage,
                                    i32 &status) throws -> void
{
  if (path_kind == Path::entry_kind::Unknown) {
    os::file_status file_status{};
    if (!os::stat_path(path.view(), file_status)) {
      report_soft_koshkit_util_error(
          ec, cxt, "grep",
          path.text() + ": " + os::last_system_error_message());
      status = 2;
      return;
    }

    switch (os::file_type_letter(file_status.mode)) {
    case 'd': path_kind = Path::entry_kind::Directory; break;
    case '-': path_kind = Path::entry_kind::Regular; break;
    default: path_kind = Path::entry_kind::Other; break;
    }
  }

  if (path_kind != Path::entry_kind::Directory) {
    if (path_kind == Path::entry_kind::Regular)
      storage.push(path.text().clone());
    return;
  }

  let children = Path::read_directory_typed(path, allocator);
  if (!children.has_value()) {
    report_soft_koshkit_util_error(
        ec, cxt, "grep",
        path.text() + ": " + os::last_system_error_message());
    status = 2;
    return;
  }
  children->sort([](const Path::directory_child &left,
                    const Path::directory_child &right) {
    return left.name.view() < right.name.view();
  });

  if (os::INTERRUPT_REQUESTED) return;

  let child_paths = ArrayList<Path>{allocator};
  child_paths.reserve(children->count());
  usize unknown_count = 0;
  for (let const &child : *children) {
    let child_path = path.clone();
    child_path.push_component(child.name.view());
    child_paths.push(steal(child_path));
    if (child.kind == Path::entry_kind::Unknown) unknown_count++;
  }

  if (unknown_count != 0) {
    let unknown_statuses = ArrayList<os::file_status>{allocator};
    let unknown_indices = ArrayList<usize>{allocator};
    let results = ArrayList<os::batch_result>{allocator};
    let batch = os::Batch{allocator};
    let const wave_count = unknown_count < GREP_UNKNOWN_BATCH_COUNT
                               ? unknown_count
                               : GREP_UNKNOWN_BATCH_COUNT;
    unknown_statuses.reserve(wave_count);
    unknown_indices.reserve(wave_count);
    results.reserve(wave_count);
    batch.reserve(wave_count);

    let const do_flush_unknown = [&]() throws -> void {
      if (unknown_indices.is_empty()) return;

      batch.clear();
      for (usize index = 0; index < unknown_indices.count(); index++)
        batch.add(os::batch_operation::stat(
            child_paths[unknown_indices[index]], unknown_statuses[index]));

      batch.execute(results);
      for (usize index = 0; index < unknown_indices.count(); index++) {
        let &kind = (*children)[unknown_indices[index]].kind;
        if (results[index].error_number != 0) {
          kind = Path::entry_kind::Other;
          continue;
        }

        switch (os::file_type_letter(unknown_statuses[index].mode)) {
        case 'd': kind = Path::entry_kind::Directory; break;
        case '-': kind = Path::entry_kind::Regular; break;
        default: kind = Path::entry_kind::Other; break;
        }
      }
      unknown_statuses.clear();
      unknown_indices.clear();
    };

    for (usize index = 0; index < children->count(); index++) {
      if ((*children)[index].kind != Path::entry_kind::Unknown) continue;

      unknown_statuses.push({});
      unknown_indices.push(index);
      if (unknown_indices.count() == GREP_UNKNOWN_BATCH_COUNT)
        do_flush_unknown();
    }
    do_flush_unknown();
  }

  for (usize index = 0; index < children->count(); index++) {
    if (os::INTERRUPT_REQUESTED) return;
    let const kind = (*children)[index].kind;
    if (kind == Path::entry_kind::Directory)
      collect_recursive_sources(ec, cxt, child_paths[index], kind, allocator,
                                storage, status);
    else if (kind == Path::entry_kind::Regular)
      storage.push(child_paths[index].text().clone());
  }
}

Grep::Grep() = default;

pure fn Grep::kind() const wontthrow -> Utility::Kind { return Kind::Grep; }

fn Grep::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  let const pattern = operands[0].view();
  let const should_ignore_case = FLAG_GREP_IGNORE_CASE.is_enabled();
  let const should_invert = FLAG_GREP_INVERT.is_enabled();
  let const should_recurse = FLAG_GREP_RECURSIVE.is_enabled();
  let const should_print_line_numbers = FLAG_GREP_LINE_NUMBER.is_enabled();
  let const should_suppress_names = FLAG_GREP_NO_FILENAME.is_enabled();
  let const should_use_literal_search =
      is_literal_search_pattern(pattern) &&
      (!should_ignore_case || is_ascii_pattern(pattern));

  let folded_pattern = String{cxt.scratch_allocator()};
  if (should_use_literal_search && should_ignore_case) {
    for (usize index = 0; index < pattern.length; index++)
      folded_pattern.push(utils::ascii_to_lower(pattern[index]));
  }

  os::compiled_regex compiled;
  if (!should_use_literal_search) {
    if (os::compile_search_regex(
            pattern,
            should_ignore_case ? os::case_sensitivity::Insensitive
                               : os::case_sensitivity::Sensitive,
            compiled) != os::regex_compile_result::Ok) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[0], args[0].view(),
          "the pattern '" + operands[0] + "' is not a valid regex");
      return 2;
    }
  }

  defer {
    if (!should_use_literal_search) os::free_regex(compiled);
  };

  let const allocator = cxt.scratch_allocator();
  let const operand_sources = source_list_from_operands(operands, allocator, 1);
  ArrayList<String> recursive_storage{allocator};
  ArrayList<StringView> sources{allocator};
  i32 status = 0;
  if (should_recurse) {
    for (let const source : operand_sources) {
      if (source == "-") {
        sources.push(source);
        continue;
      }
      let const source_path = Path{source, allocator};
      collect_recursive_sources(ec, cxt, source_path,
                                Path::entry_kind::Unknown, allocator,
                                recursive_storage, status);
    }
    sources.reserve(recursive_storage.count() + 1);
    for (let const &source : recursive_storage) sources.push(source.view());
    if (sources.is_empty() && operand_sources.count() == 1 &&
        operand_sources[0] == "-")
      sources.push("-");
  } else {
    sources = steal(operand_sources);
  }

  let const should_print_names = !should_suppress_names && sources.count() > 1;
  let output = String{allocator};
  let line = String{allocator};
  let reader = SourceBatchReader{ec, sources, allocator, 64 * 1024, true,
                                 should_recurse};
  let chunks = ArrayList<SourceBatchReader::Chunk>{allocator};
  ArrayList<usize> source_line_numbers{allocator};
  source_line_numbers.reserve(sources.count());
  for (usize index = 0; index < sources.count(); index++)
    source_line_numbers.push(1);
  bool has_any_match = false;
  let const do_process_line = [&](usize source_index, StringView source,
                                  StringView value) throws -> void {
    let const is_match = should_use_literal_search
                             ? (should_ignore_case
                                    ? utils::contains_case_insensitive_ascii(
                                          value, folded_pattern.view())
                                    : value.find_substring(pattern).has_value())
                             : os::regex_matches_null_terminated(compiled, value);
    if (is_match != should_invert) {
      has_any_match = true;
      if (should_print_names) {
        output += source == "-" ? StringView{"(standard input)"} : source;
        output += ':';
      }
      if (should_print_line_numbers) {
        output += String::from(source_line_numbers[source_index], allocator);
        output += ':';
      }
      output += value;
      output += '\n';
      if (output.count() >= 65536) {
        ec.print_to_stdout(output);
        output.clear();
      }
    }
    line.clear();
  };

  loop
  {
    let const read_result = reader.read_next_ordered(chunks);
    if (read_result == SourceBatchReader::ReadResult::Complete) break;
    if (read_result == SourceBatchReader::ReadResult::Interrupted) return 130;

    for (let const &chunk : chunks) {
      let const source = sources[chunk.source_index];
      usize position = 0;
      while (position < chunk.content.length) {
        let delimiter_position = position;
        while (delimiter_position < chunk.content.length &&
               chunk.content[delimiter_position] != '\n')
        {
          delimiter_position++;
        }

        let const segment = chunk.content.substring_of_length(
            position, delimiter_position - position);
        if (delimiter_position == chunk.content.length) {
          line.append(segment);
          break;
        }

        if (should_use_literal_search && line.is_empty()) {
          do_process_line(chunk.source_index, source, segment);
          source_line_numbers[chunk.source_index]++;
        } else {
          line.append(segment);
          do_process_line(chunk.source_index, source, line.view());
          source_line_numbers[chunk.source_index]++;
        }
        position = delimiter_position;
        position++;
      }

      if (!chunk.is_complete) continue;
      if (chunk.error_number != 0) {
        line.clear();
        os::set_last_system_error(chunk.error_number);
        let const source_location =
            chunk.source_index + 1 < operand_locations.count()
                ? operand_locations[chunk.source_index + 1]
                : ec.source_location();
        report_soft_koshkit_util_error(ec, cxt, source_location, args[0].view(),
                                       String{cxt.scratch_allocator(), source} +
                                           ": " +
                                           os::last_system_error_message());
        status = 2;
        continue;
      }

      if (!line.is_empty()) {
        do_process_line(chunk.source_index, source, line.view());
        source_line_numbers[chunk.source_index]++;
      }
    }
  }

  ec.print_to_stdout(output);
  if (status == 2) return 2;

  return has_any_match ? 0 : 1;
}

} /* namespace koshkit */

} /* namespace koshka */
