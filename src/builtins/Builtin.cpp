/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the builtin builtin. The
 * builtin builtin runs a shell builtin past a same-named function.
 */

#include "../Builtin.hpp"

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[--list] [name [argument ...]]");

HELP_DESCRIPTION_DECL(
    "The builtin builtin runs a shell builtin past a same-named function.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(BUILTIN_LIST, Bool, '\0', "list", "List every builtin one per line.");

REGISTER_BUILTIN_FLAGS(BuiltinBuiltin);

namespace koshka {

BuiltinBuiltin::BuiltinBuiltin() = default;

pure fn BuiltinBuiltin::kind() const wontthrow -> Builtin::Kind
{
  return Kind::BuiltinBuiltin;
}

static fn sorted_builtin_names(Allocator allocator) throws -> ArrayList<String>
{
  let names = ArrayList<String>{allocator};
  for (let const &builtin_name : builtin_names())
    names.push_managed(builtin_name);
  names.sort();
  return names;
}

static fn print_builtin_columns(ExecContext &ec, Allocator allocator) throws
    -> void
{
  let const sorted = sorted_builtin_names(allocator);

  let out = String{allocator};
  out += "Koshka has ";
  out += String::from(static_cast<i64>(sorted.count()), allocator);
  out += " builtins:\n\n";
  utils::append_name_columns(out, sorted.count(),
                             [&](usize index) { return sorted[index].view(); });
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

  /* The flags are not parsed generically, since every argument after the name
     belongs to the target builtin and passes through untouched. */
  let const &name = ec.args()[1];
  if (name == "--help") SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (name == "--list") {
    let const sorted = sorted_builtin_names(cxt.scratch_allocator());
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

} /* namespace koshka */
