/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file lists builtin names and renders full, descriptive, synopsis, or
 * manpage-style help for the help builtin. It reads immutable help metadata
 * from the builtin registry and delegates full help to the selected builtin.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-dms] [pattern ...]");

HELP_DESCRIPTION_DECL(
    "The help builtin lists the builtins or displays the help for one.");

FLAG(SHORT, Bool, 'd', "\0",
     "Display a short description instead of the full help.");
FLAG(MANPAGE, Bool, 'm', "\0", "Display help in a manpage-style format.");
FLAG(SUMMARY, Bool, 's', "\0", "Display only the usage synopsis.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Help);

namespace koshka {

Help::Help() = default;

pure fn Help::kind() const wontthrow -> Builtin::Kind { return Kind::Help; }

static fn append_help_synopsis(String &out, StringView name,
                               const SynopsisList &synopsis,
                               bool should_prefix_name) throws -> void
{
  if (should_prefix_name) {
    out.append(name);
    out += ": ";
  }

  for (usize index = 0; index < synopsis.count(); index++) {
    if (index > 0) out += should_prefix_name ? " or " : "\n    ";
    out.append(name);
    if (!synopsis[index].is_empty()) {
      out.push(' ');
      out.append(synopsis[index]);
    }
  }
  out.push('\n');
}

fn Help::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (args.count() <= 1) {
    let out = String{cxt.scratch_allocator()};
    for (let const &name : builtin_names()) {
      out.append(name.view());
      out += '\n';
    }
    ec.print_to_stdout(out);
    return 0;
  }

  i32 status = 0;
  for (usize i = 1; i < args.count(); i++) {
    let const &name = args[i];
    let const resolved = search_builtin(name.view());
    if (!resolved.has_value()) {
      report_soft_builtin_error(
          ec, cxt,
          i < operand_locations.count() ? operand_locations[i]
                                        : ec.source_location(),
          StringView{"'"} + name + "' is not a shell builtin",
          "Run `help` with no operand to list every builtin");
      status = 1;
      continue;
    }

    let const description = builtin_help_description(*resolved);
    let const synopsis = builtin_help_synopsis(*resolved);
    if (FLAG_SHORT.is_enabled()) {
      let out = String{cxt.scratch_allocator(), name.view()};
      out += " - ";
      out.append(description);
      out.push('\n');
      ec.print_to_stdout(out);
      continue;
    }
    if (FLAG_MANPAGE.is_enabled() && synopsis != nullptr) {
      let out = String{cxt.scratch_allocator()};
      out += "NAME\n    ";
      out.append(name.view());
      out += " - ";
      out.append(description);
      out += "\n\nSYNOPSIS\n    ";
      append_help_synopsis(out, name.view(), *synopsis, false);
      out += "\nDESCRIPTION\n";
      out += wrap_text(description, HELP_INDENT, HELP_WRAP_WIDTH);
      out += "\n\n";
      if (let const flags = builtin_flag_list(*resolved); flags != nullptr)
        out += make_flag_help(*flags);
      ec.print_to_stdout(out);
      continue;
    }
    if (FLAG_SUMMARY.is_enabled() && synopsis != nullptr) {
      let out = String{cxt.scratch_allocator()};
      append_help_synopsis(out, name.view(), *synopsis, true);
      ec.print_to_stdout(out);
      continue;
    }

    let forwarded = ArrayList<String>{heap_allocator()};
    forwarded.push(String{heap_allocator(), name.view()});
    forwarded.push(String{"--help"});
    let forwarded_locations = ArrayList<SourceLocation>{heap_allocator()};
    let sub = ExecContext::from_resolved(
        ec.source_location(), ResolvedCommand::from_builtin(*resolved),
        steal(forwarded), steal(forwarded_locations));
    execute_builtin(steal(sub), cxt);
  }

  return status;
}

} /* namespace koshka */
