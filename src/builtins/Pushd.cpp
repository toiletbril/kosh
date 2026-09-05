/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the pushd builtin. The pushd
 * builtin saves the current directory on the stack and changes to dir. With
 * no argument it swaps the top two directories. With +N it rotates the stack
 * so the Nth entry from the top becomes the current directory, and with -N
 * it counts from the bottom.
 */

#include "../Builtin.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[dir | +N | -N]");
HELP_DESCRIPTION_DECL(
    "The pushd builtin saves the current directory on the stack and changes to "
    "dir. With no argument it swaps the top two directories. With +N it "
    "rotates "
    "the stack so the Nth entry from the top becomes the current directory, "
    "and "
    "with -N it counts from the bottom.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Pushd);

namespace koshka {

Pushd::Pushd() = default;

pure fn Pushd::kind() const wontthrow -> Builtin::Kind { return Kind::Pushd; }

fn Pushd::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args =
      parse_flags_vec(FLAG_LIST, ec.args(), ec.source_location().position,
                      nullptr, &ec.arg_locations(), &operand_locations,
                      builtin_error_context(ec.program()), true);
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let &stack = cxt.directory_stack();
  let const pwd = logical_working_directory(cxt).text().clone();

  /* With no directory the top two entries swap, so the current directory and
     the top of the stack trade places. */
  if (args.count() <= 1) {
    if (stack.is_empty()) {
      throw ErrorWithLocationAndDetails{
          ec.source_location(), "pushd has no other directory to swap to",
          "Push a directory first with `pushd DIR`"};
    }
    let const target = String{cxt.scratch_allocator(), stack.back().view()};
    let const status = run_cd_to_directory(cxt, ec, target.view());
    if (status != 0) return status;
    stack.back() = String{heap_allocator(), pwd.view()};
    print_directory_stack(cxt, ec, false, false, false);
    return 0;
  }

  let const arg = args[1].view();

  /* The ring is the current directory at index zero, then the saved stack from
     the top down, which drives a rotation. */
  if (usize index = 0; parse_directory_stack_rotation(
          arg, stack.count() + 1, operand_locations[1], index))
  {
    ArrayList<String> ring{cxt.scratch_allocator()};
    ring.push(String{cxt.scratch_allocator(), pwd.view()});
    for (usize i = stack.count(); i > 0; i--)
      ring.push(String{cxt.scratch_allocator(), stack[i - 1].view()});

    let const status = run_cd_to_directory(cxt, ec, ring[index].view());
    if (status != 0) return status;

    stack.clear();
    for (usize offset = ring.count() - 1; offset >= 1; offset--) {
      stack.push(String{heap_allocator(),
                        ring[(index + offset) % ring.count()].view()});
      if (offset == 1) break;
    }
    print_directory_stack(cxt, ec, false, false, false);
    return 0;
  }

  let const status = run_cd_to_directory(cxt, ec, arg);
  if (status != 0) return status;
  stack.push(String{heap_allocator(), pwd.view()});
  print_directory_stack(cxt, ec, false, false, false);
  return 0;
}

} /* namespace koshka */
