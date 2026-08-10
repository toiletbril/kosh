#include "../Builtin.hpp"

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../ResolvedCommand.hpp"
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

  usize longest_length = 0;
  for (let const &builtin_name : sorted)
    if (builtin_name.length() > longest_length)
      longest_length = builtin_name.length();
  let const column_width = longest_length + 2;
  let const columns = column_width >= 78 ? usize{1} : 78 / column_width;

  let out = String{allocator};
  out += "Koshka has ";
  out += String::from(static_cast<i64>(sorted.count()), allocator);
  out += " builtins:\n\n";
  for (usize i = 0; i < sorted.count(); i++) {
    if (i % columns == 0) out += "  ";
    out += sorted[i].view();
    let const is_last_in_row =
        i % columns == columns - 1 || i + 1 == sorted.count();
    if (is_last_in_row) {
      out += "\n";
    } else {
      for (usize pad = sorted[i].length(); pad < column_width; pad++)
        out += " ";
    }
  }
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

} // namespace koshka
