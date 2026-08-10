#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[n]");

HELP_DESCRIPTION_DECL("The break builtin exits an enclosing loop.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Break);

namespace koshka {

Break::Break() = default;

pure fn Break::kind() const wontthrow -> Builtin::Kind { return Kind::Break; }

static fn report_break_out_of_range(const ExecContext &ec, EvalContext &cxt,
                                    StringView count) throws -> void
{
  let message = String{cxt.scratch_allocator()};
  message += "break count '";
  message.append(count);
  message += "' is out of range";

  const ErrorWithLocation located{ec.arg_location_at(1), message.view()};
  if (const String *source = cxt.current_source(); source != nullptr) {
    show_message(located.to_string(source->view(), &cxt));
  } else {
    let fallback = String{cxt.scratch_allocator()};
    fallback += "kosh: ";
    fallback.append(message.view());
    fallback += "\n";
    print_error(fallback.view());
  }
}

fn Break::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help")
    SHOW_BUILTIN_HELP_AND_RETURN(ec);

  i64 level = 1;
  if (ec.args().count() > 1) {
    let const parsed_level = ec.args()[1].to<i64>();
    if (parsed_level.is_error()) {
      let message = String{cxt.scratch_allocator()};
      message += "Unable to break because '";
      message.append(ec.args()[1].view());
      message += "' is not a valid integer";

      ErrorWithLocation located{ec.arg_location_at(1), message.view()};
      located.set_script_fatal();
      located.set_command_status(2);
      throw located;
    }
    level = parsed_level.value();
  }

  if (ec.args().count() > 2) {
    ErrorWithLocation located{
        ec.arg_location_at(2),
        StringView{"break accepts at most one loop count"}};
    located.set_script_fatal();
    located.set_command_status(1);
    throw located;
  }

  if (level < 1) {
    report_break_out_of_range(ec, cxt, ec.args()[1].view());
    cxt.request_break(1, ec.source_location());
    return 1;
  }

  LOG(All, "break leaving %lld enclosing loops", static_cast<long long>(level));
  cxt.request_break(level, ec.source_location());
  return 0;
}

} // namespace koshka
