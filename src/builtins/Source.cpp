#include "../Builtin.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
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
  if (has_extra_args) {
    let params = ArrayList<String>{heap_allocator()};
    for (usize i = path_index + 1; i < ec.args().count(); i++)
      params.push_managed(ec.args()[i]);

    saved_params = cxt.take_positional_params();
    cxt.set_positional_params(steal(params));
  }
  defer
  {
    if (has_extra_args) cxt.set_positional_params(steal(saved_params));
  };

  return cxt.run_source(*contents, "the file '" + path + "'",
                        return_handling::Consume,
                        ec.arg_location_at(path_index), StringView{path});
}

} /* namespace koshka */
