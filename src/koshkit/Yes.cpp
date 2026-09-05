/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the yes utility in koshkit.
 * The yes utility writes the given string on its own line over and over.
 */

#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[string ...]");

HELP_DESCRIPTION_DECL(
    "The yes utility writes the given string on its own line over and over.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Yes);

namespace koshka {

namespace koshkit {

Yes::Yes() = default;

pure fn Yes::kind() const wontthrow -> Utility::Kind { return Kind::Yes; }

fn Yes::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  String line{cxt.scratch_allocator()};
  if (operands.is_empty())
    line += "y";
  else
    for (usize i = 0; i < operands.count(); i++) {
      if (i > 0) line += ' ';
      line += operands[i].view();
    }
  line += '\n';

  let const out_fd = ec.out_fd.value_or(KOSH_STDOUT);
  loop
  {
    if (os::INTERRUPT_REQUESTED) return 130;

    /* written_count accumulates across passes, so a short write advances
       through the line rather than re-emitting the whole line and corrupting
       the stream. */
    usize written_count = 0;
    while (written_count < line.count()) {
      let const chunk = os::write_fd(out_fd, line.view().data + written_count,
                                     line.count() - written_count);
      if (!chunk.has_value())
        return errno == EPIPE ? KOSH_BROKEN_PIPE_EXIT_STATUS : 0;
      if (*chunk == 0) return 0;
      written_count += *chunk;
    }
  }
}

} /* namespace koshkit */

} /* namespace koshka */
