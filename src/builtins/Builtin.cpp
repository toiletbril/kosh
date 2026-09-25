/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the builtin command, including sorted listing and direct
 * dispatch that bypasses functions and PATH lookup.
 */

#include "../Builtin.hpp"

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Utils.hpp"
#include "../base/Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[--list] [name [argument ...]]");

HELP_DESCRIPTION_DECL(
    "The builtin builtin runs the builtin even when a function has the same "
    "name.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(BUILTIN_LIST, Bool, '\0', "list", "List every builtin one per line.");

REGISTER_BUILTIN_FLAGS(BuiltinBuiltin);

namespace koshka {

BuiltinBuiltin::BuiltinBuiltin() = default;

pure fn BuiltinBuiltin::kind() const wontthrow -> Builtin::Kind
{
  return Kind::BuiltinBuiltin;
}

using sorted_builtin_name_list =
    SortedArrayList<String, order_comparator<String>>;

static fn sorted_builtin_names() throws -> const sorted_builtin_name_list &
{
  static sorted_builtin_name_list names = [] throws {
    let collected = ArrayList<String>{heap_allocator()};
    for (let const &builtin_name : builtin_names())
      collected.push_managed(builtin_name);
    return steal(collected).make_sorted(sort_order::ascending);
  }();
  return names;
}

enum class builtin_section : u8
{
  Posix,
  Bash,
  Koshka,
};

static pure fn get_builtin_section(Builtin::Kind kind,
                                   StringView name) wontthrow -> builtin_section
{
  if (name == "source") return builtin_section::Bash;

  switch (kind) {
  case Builtin::Kind::Assimilate:
  case Builtin::Kind::Bench:
  case Builtin::Kind::Koshkit:
  case Builtin::Kind::Z: return builtin_section::Koshka;
  case Builtin::Kind::Alias:
  case Builtin::Kind::Bg:
  case Builtin::Kind::Break:
  case Builtin::Kind::Cd:
  case Builtin::Kind::CommandBuiltin:
  case Builtin::Kind::Continue:
  case Builtin::Kind::Echo:
  case Builtin::Kind::Eval:
  case Builtin::Kind::Exec:
  case Builtin::Kind::Exit:
  case Builtin::Kind::Export:
  case Builtin::Kind::False:
  case Builtin::Kind::Fc:
  case Builtin::Kind::Fg:
  case Builtin::Kind::Getopts:
  case Builtin::Kind::Hash:
  case Builtin::Kind::Jobs:
  case Builtin::Kind::Kill:
  case Builtin::Kind::Newgrp:
  case Builtin::Kind::Printf:
  case Builtin::Kind::Pwd:
  case Builtin::Kind::Read:
  case Builtin::Kind::Readonly:
  case Builtin::Kind::Return:
  case Builtin::Kind::Set:
  case Builtin::Kind::Shift:
  case Builtin::Kind::Source:
  case Builtin::Kind::Test:
  case Builtin::Kind::Times:
  case Builtin::Kind::Trap:
  case Builtin::Kind::True:
  case Builtin::Kind::Type:
  case Builtin::Kind::Ulimit:
  case Builtin::Kind::Umask:
  case Builtin::Kind::Unalias:
  case Builtin::Kind::Unset:
  case Builtin::Kind::Wait: return builtin_section::Posix;
  default: return builtin_section::Bash;
  }
}

static fn print_builtin_columns(ExecContext &ec, Allocator allocator) throws
    -> void
{
  let const &sorted = sorted_builtin_names();
  let posix_names = ArrayList<StringView>{allocator};
  let bash_names = ArrayList<StringView>{allocator};
  let koshka_names = ArrayList<StringView>{allocator};

  for (let const &name : sorted) {
    let const kind = search_builtin(name.view());
    ASSERT(kind.has_value());
    switch (get_builtin_section(*kind, name.view())) {
    case builtin_section::Posix: posix_names.push(name.view()); break;
    case builtin_section::Bash: bash_names.push(name.view()); break;
    case builtin_section::Koshka: koshka_names.push(name.view()); break;
    }
  }

  let out = String{allocator};
  out += "Koshka has ";
  out += String::from(static_cast<i64>(sorted.count()), allocator);
  out += " builtins:\n\n";
  let const should_color = colors::stdout_wants_color();
  append_report_name_section(out, "POSIX", posix_names, should_color);
  append_report_name_section(out, "BASH", bash_names, should_color);
  append_report_name_section(out, "Koshka", koshka_names, should_color);
  ec.print_to_stdout(out.view());
}

fn BuiltinBuiltin::execute(ExecContext &ec, EvalContext &cxt) const throws
    -> i32
{
  if (ec.args().count() < 2) {
    if (cxt.mood() == mimic_mood::Default)
      print_builtin_columns(ec, cxt.scratch_allocator());
    return 0;
  }

  let const &name = ec.args()[1];
  if (name == "--help") SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (name == "--list") {
    let const &sorted = sorted_builtin_names();
    let out = String{cxt.scratch_allocator()};
    for (let const &builtin_name : sorted) {
      out += builtin_name.view();
      out += "\n";
    }
    ec.print_to_stdout(out.view());
    return 0;
  }

  LOG(Debug, "builtin forwarding to '%s' past functions and PATH",
      name.c_str());

  let const target = search_builtin(name.view());
  if (!target.has_value()) {
    report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                              StringView{"'"} + name +
                                  "' is not a shell builtin");
    return 1;
  }

  let forwarded = ArrayList<String>{heap_allocator()};
  let forwarded_locations = ArrayList<SourceLocation>{heap_allocator()};
  for (usize i = 1; i < ec.args().count(); i++) {
    forwarded.push_managed(ec.args()[i]);
    forwarded_locations.push(ec.arg_location_at(i));
  }
  let sub = ExecContext::from_resolved(
      ec.source_location(), ResolvedCommand::from_builtin(*target),
      steal(forwarded), steal(forwarded_locations));
  return execute_builtin(steal(sub), cxt);
}

} // namespace koshka
