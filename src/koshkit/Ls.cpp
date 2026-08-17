#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-aA1lh] [path ...]");

HELP_DESCRIPTION_DECL("The ls utility lists the names in each directory.");

FLAG(LS_ALL, Bool, 'a', "", "List entries whose name starts with a dot.");
FLAG(LS_ALMOST_ALL, Bool, 'A', "",
     "List dot entries but not the . and .. directory entries.");
FLAG(LS_ONE, Bool, '1', "", "List one entry per line.");
FLAG(LS_LONG, Bool, 'l', "",
     "Print the mode, owner, group, size, and time before each name.");
FLAG(LS_HUMAN, Bool, 'h', "",
     "With -l, print the size in a human-readable form such as 4.0K.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Ls);

namespace koshka {

namespace koshkit {

static constexpr usize COLUMN_GAP = 2;

/* The blocks ride along so the total line sums them without a second stat. */
struct long_entry
{
  explicit long_entry(Allocator allocator)
      : mode_string(allocator), link_count(allocator), owner(allocator),
        group(allocator), size(allocator), time(allocator), name(allocator)
  {}
  String mode_string;
  String link_count;
  String owner;
  String group;
  String size;
  String time;
  String name;
  u64 blocks{0};
};

struct id_name_entry
{
  id_name_entry(u32 id, String name) : id(id), name(steal(name)) {}
  u32 id;
  String name;
};

enum class id_name_kind : u8
{
  Owner,
  Group,
};

static fn cached_id_name(u32 id, id_name_kind kind,
                         ArrayList<id_name_entry> &cache,
                         Allocator allocator) throws -> StringView
{
  for (const id_name_entry &entry : cache)
    if (entry.id == id) return entry.name.view();
  let const looked_up = kind == id_name_kind::Owner ? os::uid_to_username(id)
                                                    : os::gid_to_groupname(id);
  let name = looked_up.has_value() ? String{allocator, looked_up->view()}
                                   : String::from(id, allocator);
  cache.push(id_name_entry{id, steal(name)});
  return cache.back().name.view();
}

static fn append_padded(String &output, StringView field, usize width,
                        bool should_pad_on_left) throws -> void
{
  if (should_pad_on_left) output.append_repeated(' ', width - field.length);
  output += field;
  if (!should_pad_on_left) output.append_repeated(' ', width - field.length);
}

/* A path that cannot be stat'd renders a sparse row so the listing still names
   it. */
static fn build_long_entry(const Path &path, StringView name,
                           ArrayList<id_name_entry> &uid_cache,
                           ArrayList<id_name_entry> &gid_cache,
                           Allocator allocator) throws -> long_entry
{
  long_entry entry{allocator};
  entry.name = String{allocator, name};

  os::file_status status{};
  if (!os::stat_path(path.text().view(), status)) {
    entry.mode_string = "??????????";
    entry.link_count = "?";
    entry.owner = "?";
    entry.group = "?";
    entry.size = "?";
    entry.time = "?";
    return entry;
  }

  entry.mode_string = os::format_mode_string(status.mode);
  entry.link_count = String::from(status.link_count, allocator);
  entry.owner = cached_id_name(status.owner_id, id_name_kind::Owner, uid_cache,
                               allocator);
  entry.group = cached_id_name(status.group_id, id_name_kind::Group, gid_cache,
                               allocator);
  entry.size = FLAG_LS_HUMAN.is_enabled()
                   ? format_human_size(status.size, allocator)
                   : String::from(status.size, allocator);
  entry.time =
      utils::format_unix_timestamp(status.modification_time, "%b %e %H:%M");
  entry.blocks = status.blocks;
  return entry;
}

static fn render_long_entries(const ArrayList<long_entry> &entries,
                              String &output) throws -> void
{
  usize link_width = 0;
  usize owner_width = 0;
  usize group_width = 0;
  usize size_width = 0;
  for (const long_entry &entry : entries) {
    if (entry.link_count.count() > link_width)
      link_width = entry.link_count.count();
    if (entry.owner.count() > owner_width) owner_width = entry.owner.count();
    if (entry.group.count() > group_width) group_width = entry.group.count();
    if (entry.size.count() > size_width) size_width = entry.size.count();
  }

  for (const long_entry &entry : entries) {
    output += entry.mode_string.view();
    output += ' ';
    append_padded(output, entry.link_count.view(), link_width, true);
    output += ' ';
    append_padded(output, entry.owner.view(), owner_width, false);
    output += ' ';
    append_padded(output, entry.group.view(), group_width, false);
    output += ' ';
    append_padded(output, entry.size.view(), size_width, true);
    output += ' ';
    output += entry.time.view();
    output += ' ';
    output += entry.name.view();
    output += '\n';
  }
}

static fn column_width(const ArrayList<usize> &widths, usize column_index,
                       usize rows) wontthrow -> usize
{
  let const count = widths.count();
  usize widest = 0;
  for (usize r = 0; r < rows; r++) {
    let const index = column_index * rows + r;
    if (index < count && widths[index] > widest) {
      widest = widths[index];
    }
  }
  return widest;
}

static fn render_columns(const ArrayList<StringView> &names, String &output,
                         Allocator allocator) throws -> void
{
  let const count = names.count();
  if (count == 0) return;

  u32 terminal_columns = 0;
  u32 terminal_rows = 0;
  let const is_terminal = os::terminal_size(terminal_columns, terminal_rows);
  if (FLAG_LS_ONE.is_enabled() || !is_terminal) {
    for (const StringView &name : names) {
      output += name;
      output += '\n';
    }
    return;
  }
  const usize terminal_width = terminal_columns;

  ArrayList<usize> widths{allocator};
  widths.reserve(count);
  for (const StringView &name : names)
    widths.push(name.length);

  /* A column-major grid puts the entry at column*rows+row. */
  usize shortest_name_length = names.front().length;
  for (let const name : names)
    if (name.length < shortest_name_length) shortest_name_length = name.length;
  let const width_limited_columns =
      terminal_width / (shortest_name_length + COLUMN_GAP);
  let const bounded_columns =
      count < width_limited_columns ? count : width_limited_columns;
  let const max_columns = bounded_columns == 0 ? 1 : bounded_columns;
  usize best_columns = 1;
  for (usize columns = max_columns;; columns--) {
    let const rows = (count + columns - 1) / columns;
    usize total = 0;
    for (usize c = 0; c < columns; c++) {
      total += column_width(widths, c, rows);
      if (c + 1 < columns) total += COLUMN_GAP;
    }
    if (total <= terminal_width) {
      best_columns = columns;
      break;
    }
    if (columns == 1) break;
  }

  let const rows = (count + best_columns - 1) / best_columns;
  ArrayList<usize> column_widths{allocator};
  column_widths.reserve(best_columns);
  for (usize c = 0; c < best_columns; c++)
    column_widths.push(column_width(widths, c, rows));

  for (usize r = 0; r < rows; r++) {
    for (usize c = 0; c < best_columns; c++) {
      let const index = c * rows + r;
      if (index >= count) continue;
      output += names[index];
      let const has_next =
          (c + 1 < best_columns) && ((c + 1) * rows + r < count);
      if (has_next)
        output.append_repeated(' ',
                               column_widths[c] + COLUMN_GAP - widths[index]);
    }
    output += '\n';
  }
}

/* coreutils prints the total in 1K blocks, summed from the 512-byte block
   counts the entries carry, hence the divide by two. */
static fn long_total_blocks(const ArrayList<long_entry> &entries,
                            Allocator allocator) throws -> String
{
  u64 total_512_blocks = 0;
  for (const long_entry &entry : entries)
    total_512_blocks += entry.blocks;
  return "total " + String::from(total_512_blocks / 2, allocator);
}

Ls::Ls() = default;

pure fn Ls::kind() const wontthrow -> Utility::Kind { return Kind::Ls; }

fn Ls::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  ArrayList<StringView> targets{cxt.scratch_allocator()};
  if (operands.is_empty())
    targets.push(StringView{"."});
  else
    for (const String &operand : operands)
      targets.push(operand.view());

  targets.sort();

  ArrayList<StringView> file_targets{cxt.scratch_allocator()};
  ArrayList<StringView> dir_targets{cxt.scratch_allocator()};
  ArrayList<id_name_entry> uid_cache{cxt.scratch_allocator()};
  ArrayList<id_name_entry> gid_cache{cxt.scratch_allocator()};
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  for (const StringView &target : targets) {
    let const path = Path{target};
    if (!path.exists()) {
      report_soft_koshkit_error(ec, cxt,
                                "ls: cannot access '" +
                                    String{cxt.scratch_allocator(), target} +
                                    "': no such file or directory");
      status = 2;
      continue;
    }

    if (path.is_directory())
      dir_targets.push(target);
    else
      file_targets.push(target);
  }

  let const should_print_headers =
      file_targets.count() + dir_targets.count() > 1;

  if (!file_targets.is_empty()) {
    if (FLAG_LS_LONG.is_enabled()) {
      ArrayList<long_entry> file_entries{cxt.scratch_allocator()};
      file_entries.reserve(file_targets.count());
      for (const StringView &target : file_targets)
        file_entries.push(build_long_entry(Path{target}, target, uid_cache,
                                           gid_cache, cxt.scratch_allocator()));
      render_long_entries(file_entries, output);
    } else {
      render_columns(file_targets, output, cxt.scratch_allocator());
    }
  }

  let const is_showing_all =
      FLAG_LS_ALL.is_enabled() &&
      (!FLAG_LS_ALMOST_ALL.is_enabled() ||
       FLAG_LS_ALL.position() > FLAG_LS_ALMOST_ALL.position());
  let const is_showing_dot_names =
      is_showing_all || FLAG_LS_ALMOST_ALL.is_enabled();

  bool has_printed_block = !file_targets.is_empty();
  for (const StringView &target : dir_targets) {
    let const path = Path{target};
    Maybe<ArrayList<String>> names = Path::read_directory(path);
    if (!names.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "ls: cannot open directory '" +
                                    String{cxt.scratch_allocator(), target} +
                                    "'");
      status = 2;
      continue;
    }

    if (should_print_headers) {
      if (has_printed_block) output += '\n';
      output += target;
      output += ":\n";
    }
    has_printed_block = true;

    ArrayList<StringView> visible_names{cxt.scratch_allocator()};
    if (is_showing_all) {
      visible_names.push(StringView{"."});
      visible_names.push(StringView{".."});
    }
    for (const String &name : *names) {
      if (!is_showing_dot_names && name.starts_with(StringView{"."})) {
        continue;
      }
      visible_names.push(name.view());
    }
    visible_names.sort();

    if (FLAG_LS_LONG.is_enabled()) {
      ArrayList<long_entry> entries{cxt.scratch_allocator()};
      entries.reserve(visible_names.count());
      for (const StringView &name : visible_names) {
        let const child = PathBuilder{target}.append(name).build();
        entries.push(build_long_entry(child, name, uid_cache, gid_cache,
                                      cxt.scratch_allocator()));
      }
      output += long_total_blocks(entries, cxt.scratch_allocator());
      output += '\n';
      render_long_entries(entries, output);
    } else {
      render_columns(visible_names, output, cxt.scratch_allocator());
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
