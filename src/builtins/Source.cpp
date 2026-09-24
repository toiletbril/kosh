/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the source builtin, source path resolution, temporary
 * positional parameters, and Bash source argument frames. These operations
 * stay together because the source invocation owns both the file execution
 * and the call-entry arguments that must be restored when it returns.
 */

#include "../Builtin.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../base/Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("file");

HELP_DESCRIPTION_DECL(
    "The source builtin runs the named file in the current shell.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Source);

namespace koshka {

Source::Source() = default;

pure fn Source::kind() const wontthrow -> Builtin::Kind { return Kind::Source; }

fn Source::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  if (ec.args().count() < 2) return report_usage_error(ec, cxt, ec.program());

  /* A leading -- ends option parsing, the source -- file form ble.sh uses. */
  usize path_index = 1;
  if (ec.args()[1] == "--") path_index = 2;
  if (path_index >= ec.args().count())
    return report_usage_error(ec, cxt, ec.program());

  let const path = ec.args()[path_index].clone();
  cxt.guard_restricted_path(path.view(), ec.arg_location_at(path_index),
                            restricted_path_use::Source);
  LOG(Info, "source running file '%s' in the current shell", path.c_str());

  let resolved_source_path = cxt.resolve_source_path(path.view());
  if (!resolved_source_path.has_value())
    throw ErrorWithLocationAndDetails{
        ec.arg_location_at(path_index),
        "Unable to source the file '" + path + "': not found in PATH",
        "Pass an absolute path or add its directory to PATH"};
  let source_path = resolved_source_path.take();

  let const contents = source_path.read_entire_file();
  if (!contents.has_value())
    throw ErrorWithLocation{ec.arg_location_at(path_index),
                            "Unable to source the file '" + path +
                                "': " + os::last_system_error_message()};

  /* Operands after the file set the sourced $1 upward, a bash extension the
     sh mood ignores. */
  let const has_extra_args =
      !cxt.is_posix_mode() && ec.args().count() > path_index + 1;
  let saved_params = ArrayList<String>{heap_allocator()};
  let params = ArrayList<String>{heap_allocator()};
  if (has_extra_args) {
    for (usize i = path_index + 1; i < ec.args().count(); i++)
      params.push_managed(ec.args()[i]);
  }
  defer
  {
    if (has_extra_args) cxt.set_positional_params(steal(saved_params));
  };

  /* The RETURN action of a sourced file belongs to the frame the file ran in.
     Functrace is required before the caller DEBUG action reaches it. The frame
     is already left where the action runs, and this window covers it. */
  let saved_debug_action = cxt.save_untraced_debug_trap();
  defer { cxt.restore_untraced_debug_trap(steal(saved_debug_action)); };

  /* Bash reports the source invocation in BASH_COMMAND while the RETURN action
     runs. The commands of the file publish their own text over it. */
  let saved_current_command =
      String{heap_allocator(), cxt.get_current_command()};

  i32 status = 0;
  let status_before_return = Maybe<i32>{None};
  {
    let bash_argument_frame_context = EvalContext::BashArgumentFrameContext{};
    cxt.enter_bash_source_argument_frame(bash_argument_frame_context,
                                         has_extra_args ? &params : nullptr,
                                         path.view());
    defer { cxt.leave_bash_argument_frame(bash_argument_frame_context); };

    if (has_extra_args) {
      saved_params = cxt.take_positional_params();
      cxt.set_positional_params(steal(params));
    }

    status =
        cxt.run_source(*contents, "the file '" + path + "'",
                       ec.arg_location_at(path_index), StringView{path},
                       &status_before_return, nullptr,
                       return_handling::Consume);
  }

  /* A sourced file runs in the current scope, and its finish fires the RETURN
     trap with no functrace option. The source frame is already left here, and
     the positional parameters are still the ones the file received. */
  cxt.set_current_command(steal(saved_current_command));

  let const is_exit_pending =
      cxt.has_pending_control_flow() &&
      cxt.pending_control_flow().kind == control_flow::Kind::Exit;

  if (!cxt.is_posix_mode() && !is_exit_pending) {
    if (status_before_return.has_value())
      cxt.run_return_trap(*status_before_return);
    else
      cxt.run_return_trap(status);
  }

  return status;
}

} /* namespace koshka */
