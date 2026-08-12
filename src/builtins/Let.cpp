#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("expression ...");

HELP_DESCRIPTION_DECL(
    "The let builtin evaluates each argument as an arithmetic expression.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Let);

namespace koshka {

Let::Let() = default;

pure fn Let::kind() const wontthrow -> Builtin::Kind { return Kind::Let; }

fn Let::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  /* An empty let reports status 1, not usage status 2, matching bash. */
  if (ec.args().count() < 2) {
    report_soft_builtin_error(ec, cxt, ec.arg_location_at(0),
                              "expression expected");
    return 1;
  }

  LOG(Debug, "let evaluating %zu arithmetic expressions",
      ec.args().count() - 1);

  i64 last_value = 0;
  for (usize i = 1; i < ec.args().count(); i++) {
    let expression_base = ec.arg_location_at(i);
    if (let const source = cxt.current_source(); source != nullptr) {
      let const raw_expression =
          expression_base.get_source_text(source->view());
      if (raw_expression.has_value()) {
        let const decoded_expression = utils::decode_shell_word(
            *raw_expression, cxt.scratch_allocator(), true);
        if (decoded_expression.text == ec.args()[i] &&
            !decoded_expression.raw_positions.is_empty())
        {
          expression_base.position += decoded_expression.raw_positions[0];
        }
      }
    }

    try {
      last_value =
          cxt.evaluate_arithmetic(ec.args()[i].view(), &expression_base);
    } catch (const ErrorWithLocation &error) {
      let const message = builtin_error_message(ec.program(), error.message());
      if (cxt.is_bash_compatible()) {
        if (error.detail_message().is_empty())
          report_soft_builtin_error(ec, cxt, error.location(),
                                    error.message().view());
        else
          report_soft_builtin_error(ec, cxt, error.location(),
                                    error.message().view(),
                                    error.detail_message());
        return 1;
      }

      if (error.detail_message().is_empty())
        throw ErrorWithLocation{error.location(), message.view()};
      throw ErrorWithLocationAndDetails{error.location(), message.view(),
                                        error.detail_message()};
    } catch (const Error &error) {
      if (cxt.is_bash_compatible()) {
        if (error.detail_message().is_empty())
          report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                    error.message().view());
        else
          report_soft_builtin_error(ec, cxt, ec.arg_location_at(i),
                                    error.message().view(),
                                    error.detail_message());
        return 1;
      }

      if (error.detail_message().is_empty())
        throw make_error_for_arg(ec, i, error.message().view());
      throw make_error_for_arg(ec, i, error.message().view(),
                               error.detail_message());
    }
  }

  return last_value != 0 ? 0 : 1;
}

} // namespace koshka
