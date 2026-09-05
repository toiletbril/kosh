/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the umask builtin. The umask
 * builtin prints or sets the file creation mask.
 */

#include "../Builtin.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-p] [-S] [mask]");

HELP_DESCRIPTION_DECL(
    "The umask builtin prints or sets the file creation mask.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(UMASK_REUSABLE, Bool, 'p', "",
     "Print the mask in a form reusable as input.");
FLAG(UMASK_SYMBOLIC, Bool, 'S', "",
     "Print the mask in symbolic form, the u=rwx,g=rx,o=rx style.");

REGISTER_BUILTIN_FLAGS(Umask);

namespace koshka {

namespace {

constexpr u32 PERMISSION_BITS = 0777u;

pure fn group_mask(char who) wontthrow -> u32
{
  switch (who) {
  case 'u': return 0700u;
  case 'g': return 0070u;
  case 'o': return 0007u;
  case 'a': return 0777u;
  default: return 0;
  }
}

fn mask_to_symbolic(u32 mask, Allocator allocator) throws -> String
{
  let const allowed = (~mask) & PERMISSION_BITS;
  const char groups[] = {'u', 'g', 'o'};
  const u32 shifts[] = {6, 3, 0};
  let out = String{allocator};
  for (usize g = 0; g < 3; g++) {
    if (g > 0) out.push(',');
    out.push(groups[g]);
    out.push('=');
    let const bits = (allowed >> shifts[g]) & 7u;
    if (bits & 4u) out.push('r');
    if (bits & 2u) out.push('w');
    if (bits & 1u) out.push('x');
  }
  return out;
}

/* The clauses operate on the complement of the mask, then the result is
   complemented back into a mask. */
fn apply_symbolic_mask(StringView spec, u32 current_mask) throws -> Maybe<u32>
{
  u32 allowed = (~current_mask) & PERMISSION_BITS;
  usize i = 0;
  while (i < spec.length) {
    u32 who = 0;
    while (i < spec.length && (spec[i] == 'u' || spec[i] == 'g' ||
                               spec[i] == 'o' || spec[i] == 'a'))
    {
      who |= group_mask(spec[i]);
      i++;
    }
    if (who == 0) who = PERMISSION_BITS;

    if (i >= spec.length) return None;
    let const op = spec[i];
    if (op != '+' && op != '-' && op != '=') {
      return None;
    }
    i++;

    u32 permission = 0;
    while (i < spec.length &&
           (spec[i] == 'r' || spec[i] == 'w' || spec[i] == 'x'))
    {
      if (spec[i] == 'r') permission |= 4u;
      if (spec[i] == 'w') permission |= 2u;
      if (spec[i] == 'x') permission |= 1u;
      i++;
    }

    u32 permission_bits = 0;
    if (who & 0700u) permission_bits |= permission << 6;
    if (who & 0070u) permission_bits |= permission << 3;
    if (who & 0007u) permission_bits |= permission;

    if (op == '=')
      allowed = (allowed & ~who) | permission_bits;
    else if (op == '+')
      allowed |= permission_bits;
    else
      allowed &= ~permission_bits;

    if (i < spec.length) {
      if (spec[i] != ',') return None;
      i++;
    }
  }
  return (~allowed) & PERMISSION_BITS;
}

} /* namespace */

Umask::Umask() = default;

pure fn Umask::kind() const wontthrow -> Builtin::Kind { return Kind::Umask; }

cold i32 Umask::execute(ExecContext &ec, EvalContext &cxt) const throws
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);
  ASSERT(!ec.args().is_empty());

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let const should_print_symbolic = FLAG_UMASK_SYMBOLIC.is_enabled();
  let const should_print_reusable = FLAG_UMASK_REUSABLE.is_enabled();

  if (operands.count() > 2) {
    ErrorWithLocation located{
        ec.arg_location_at(2),
        StringView{"umask accepts at most one mask operand"}};
    located.set_script_fatal();
    located.set_command_status(1);
    throw located;
  }

  if (operands.count() < 2) {
    let const mask = os::get_file_creation_mask();
    let out = String{cxt.scratch_allocator()};
    if (should_print_reusable)
      out += should_print_symbolic ? "umask -S " : "umask ";

    if (should_print_symbolic) {
      out += mask_to_symbolic(mask, cxt.scratch_allocator()).view();
    } else {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "%04o", mask);
      out += buffer;
    }
    out.push('\n');

    ec.print_to_stdout(out);
    return 0;
  }

  let const &requested = operands[1];

  LOG(Debug, "umask setting the file creation mask from '%s'",
      requested.c_str());

  if (!requested.is_empty() && requested[0] >= '0' && requested[0] <= '7') {
    let const parsed = utils::parse_integer_in_base(requested, int_base::octal);
    if (parsed.is_error())
      throw make_error_for_arg(
          ec, 1,
          "Unable to set the file creation mask because '" + requested +
              "' is not a valid octal mask");
    os::set_file_creation_mask(static_cast<u32>(parsed.value()));
    return 0;
  }

  let const new_mask =
      apply_symbolic_mask(requested.view(), os::get_file_creation_mask());
  if (!new_mask.has_value())
    throw make_error_for_arg(ec, 1,
                             "Unable to set the file creation mask because '" +
                                 requested + "' is not a valid symbolic mask");
  os::set_file_creation_mask(*new_mask);

  return 0;
}

} /* namespace koshka */
