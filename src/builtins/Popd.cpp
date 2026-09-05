/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the popd builtin. The popd
 * builtin removes the top directory from the stack and changes to the new
 * top. With +N or -N it removes the Nth entry, counting from the top for +N
 * and from the bottom for -N, and changes directory only when the current
 * entry is removed.
 */

#include "../Builtin.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[+N | -N]");
HELP_DESCRIPTION_DECL(
    "The popd builtin removes the top directory from the stack and changes to "
    "the new top. With +N or -N it removes the Nth entry, counting from the "
    "top "
    "for +N and from the bottom for -N, and changes directory only when the "
    "current entry is removed.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Popd);

namespace koshka {

Popd::Popd() = default;

pure fn Popd::kind() const wontthrow -> Builtin::Kind { return Kind::Popd; }

fn Popd::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args =
      parse_flags_vec(FLAG_LIST, ec.args(), ec.source_location().position,
                      nullptr, &ec.arg_locations(), &operand_locations,
                      builtin_error_context(ec.program()), true);
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let &stack = cxt.directory_stack();
  if (stack.is_empty()) {
    throw ErrorWithLocationAndDetails{
        ec.source_location(), "popd found the directory stack empty",
        "Push a directory first with `pushd DIR`"};
  }

  let const do_pop_top = [&]() throws -> i32 {
    let const target = String{cxt.scratch_allocator(), stack.back().view()};
    let const status = run_cd_to_directory(cxt, ec, target.view());
    if (status != 0) return status;
    stack.pop_back();
    return 0;
  };

  /* With no argument the top of the stack is removed and becomes the current
     directory. */
  if (args.count() <= 1) {
    if (let const status = do_pop_top(); status != 0) return status;
    print_directory_stack(cxt, ec, false, false, false);
    return 0;
  }

  if (usize index = 0; parse_directory_stack_rotation(
          args[1].view(), stack.count() + 1, operand_locations[1], index))
  {
    /* Index zero names the current directory, so removing it pops the top and
       moves there. A deeper index drops a saved entry without a chdir. */
    if (index == 0) {
      if (let const status = do_pop_top(); status != 0) return status;
    } else {
      stack.remove(stack.count() - index);
    }
    print_directory_stack(cxt, ec, false, false, false);
    return 0;
  }

  throw ErrorWithLocationAndDetails{
      operand_locations[1],
      StringView{"popd does not accept the argument '"} + args[1].view() + "'",
      "Pass a +N or a -N stack index, or no argument to pop the top"};
}

} /* namespace koshka */
