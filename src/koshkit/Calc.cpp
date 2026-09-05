/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the calc utility in koshkit.
 * The calc utility joins its command-line operands into one arithmetic
 * expression and prints the result. With no expression on a terminal it
 * reads and evaluates expressions interactively, and a name = value line
 * binds a variable for a later expression to read.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../EvalOperations.hpp"
#include "../Koshkit.hpp"
#include "../Lexer.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-i] [-p] [expression ...]");

HELP_DESCRIPTION_DECL(
    "The calc utility joins its command-line operands into one arithmetic "
    "expression and prints the result. With no expression on a terminal it "
    "reads and "
    "evaluates expressions interactively, and a name = value line binds a "
    "variable for a later expression to read.");

FLAG(CALC_INTERACTIVE, Bool, 'i', "interactive",
     "Read and evaluate expressions interactively, even off a pipe.");
FLAG(CALC_PIPE, Bool, 'p', "pipe",
     "Read and evaluate expressions from standard input, one per line, with no "
     "prompt.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Calc);

namespace koshka {

namespace koshkit {

Calc::Calc() = default;

pure fn Calc::kind() const wontthrow -> Utility::Kind { return Kind::Calc; }

namespace {

fn evaluate_one(const ExecContext &ec, EvalContext &cxt, StringView expression,
                const SourceLocation *expression_base = nullptr) throws -> i32
{
  let render_source = expression;
  if (expression_base != nullptr && cxt.current_source() != nullptr)
    render_source = cxt.current_source()->view();

  if (let const operator_position =
          obvious_xor_power_operator_position(expression))
  {
    let warning_location = SourceLocation{*operator_position, 1};
    if (expression_base != nullptr) {
      warning_location.position += expression_base->position;
      warning_location.source_name_index = expression_base->source_name_index;
    }
    let const warning = WarningWithLocationAndDetails{
        warning_location,
        "The `^` operator performs bitwise XOR in Bash arithmetic",
        "Use `**`, the Bash power operator, if you meant exponentiation"};
    show_message(warning.to_string(render_source, &cxt));
  }

  try {
    let result =
        cxt.evaluate_calculator_arithmetic_text(expression, expression_base);
    result += '\n';
    ec.print_to_stdout(result);
    return 0;
  } catch (const ErrorWithLocation &error) {
    show_message(error.to_string(render_source, &cxt));
    return 1;
  } catch (const Error &error) {
    show_message(error.to_string());
    return 1;
  }
}

/* The right side is stored unevaluated, a == comparison is left for the
   evaluator. */
fn try_define(EvalContext &cxt, StringView line) throws -> bool
{
  usize i = 0;
  while (i < line.length && (line[i] == ' ' || line[i] == '\t'))
    i++;
  if (i >= line.length || !lexer::is_variable_name_start(line[i])) {
    return false;
  }

  let const name_start = i;
  while (i < line.length && lexer::is_variable_name(line[i]))
    i++;
  let const name = line.substring_of_length(name_start, i - name_start);

  while (i < line.length && (line[i] == ' ' || line[i] == '\t'))
    i++;

  /* A single = assigns, while == is a comparison the evaluator handles. */
  if (i >= line.length || line[i] != '=') {
    return false;
  }
  if (i + 1 < line.length && line[i + 1] == '=') {
    return false;
  }

  let const value =
      line.substring_of_length(i + 1, line.length - (i + 1)).trim_blanks();

  /* An empty right side reports rather than binding a name that would read as
     zero and defeat the unset error. */
  if (value.is_empty())
    throw ErrorWithLocation{
        SourceLocation{name_start,              name.length},
        "Assignment to '" + String{cxt.scratch_allocator(), name       }
        +
            "' needs a value"
    };

  cxt.set_shell_variable(name, value);
  return true;
}

fn run_repl(const ExecContext &ec, EvalContext &cxt,
            bool should_force_pipe) throws -> i32
{
  let const input_fd = ec.in_fd.value_or(KOSH_STDIN);
  let const is_terminal = !should_force_pipe && os::is_fd_a_tty(input_fd);

  /* When the host shell ran calc off a -c command, it never entered the
     interactive loop, so toiletline was never initialized. Bring it up here so
     the editor path is taken the way it is inside the interactive shell, then
     tear it down fully on the way out. */
  let const did_initialize_editor = is_terminal && !toiletline::is_active();
  if (did_initialize_editor) {
    try {
      toiletline::initialize();
    } catch (const Error &error) {
      show_message(error.to_string());
    }
  }

  let const should_use_editor = is_terminal && toiletline::is_active();

  /* The editor REPL takes raw mode for itself, otherwise the kernel and the
     editor both echo the line. */
  /* Completion is turned off for the REPL and the history swaps to
     ~/.kosh_calc_history. */
  let const was_completion_enabled =
      should_use_editor && toiletline::completion_is_enabled();
  if (should_use_editor) {
    toiletline::enter_raw_mode();
    toiletline::disable_completion();
    toiletline::enter_calc_history();
  }

  defer
  {
    if (should_use_editor) {
      toiletline::leave_calc_history();
      if (was_completion_enabled) toiletline::enable_completion(cxt);

      /* exit_raw_mode throws, so the throw is reported inside this noexcept
         defer. */
      try {
        toiletline::exit_raw_mode();
      } catch (const Error &error) {
        show_message(error.to_string());
      }
    }

    /* A full teardown only runs when calc brought toiletline up itself, so an
       interactive host shell keeps its editor across the call. */
    if (did_initialize_editor) {
      try {
        toiletline::exit();
      } catch (const Error &error) {
        show_message(error.to_string());
      }
    }
  };

  loop
  {
    Maybe<String> line;
    if (should_use_editor) {
      toiletline::input_result result{};
      try {
        result = toiletline::get_input(String{"calc> "});
      } catch (const Error &error) {
        show_message(error.to_string());
        break;
      }

      switch (result.code) {
      case TL_PRESSED_TAB: toiletline::set_input(result.text); continue;
      case TL_PRESSED_EOF:
        if (result.text.view().is_empty()) {
          koshka::print("^D");
          koshka::flush();
          toiletline::emit_newlines(result.text);
          return 0;
        }
        toiletline::set_input(result.text);
        continue;
      case TL_PRESSED_INTERRUPT:
        koshka::print("^C");
        koshka::flush();
        break;
      case TL_PRESSED_SUSPEND:
        koshka::print("^Z");
        koshka::flush();
        break;
      default:;
      }

      toiletline::emit_newlines(result.text);

      if (result.code != TL_PRESSED_ENTER || result.text.view().is_empty()) {
        continue;
      }

      line = steal(result.text);
    } else {
      if (is_terminal) koshka::print_error("calc> ");
      bool was_delimiter_terminated = false;
      line = utils::read_line_from_fd(input_fd, was_delimiter_terminated);
      if (!line.has_value() || os::INTERRUPT_REQUESTED) {
        break;
      }
    }

    if (line->view().is_empty()) continue;

    try {
      if (try_define(cxt, line->view())) continue;
    } catch (const ErrorWithLocation &error) {
      show_message(error.to_string(line->view(), &cxt));
      continue;
    } catch (const Error &error) {
      show_message(error.to_string());
      continue;
    }

    evaluate_one(ec, cxt, line->view());
  }

  if (!should_use_editor && is_terminal) {
    koshka::print_error("\n");
  }
  return 0;
}

} /* namespace */

fn Calc::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  /* calc prints only errors, an unset variable is a calc error instead. */
  let const were_diagnostics_disabled = cxt.diagnostics_disabled();
  cxt.set_diagnostics_disabled(true);
  defer { cxt.set_diagnostics_disabled(were_diagnostics_disabled); };

  /* A piped run with no expression keeps the usage error so it does not hang.
   */
  let const has_expression = !operands.is_empty();
  let const should_pipe = FLAG_CALC_PIPE.is_enabled();
  let const is_interactive =
      !should_pipe &&
      (FLAG_CALC_INTERACTIVE.is_enabled() ||
       (!has_expression && os::is_stdin_a_tty() && os::is_stdout_a_tty()));
  if (should_pipe || is_interactive) {
    return run_repl(ec, cxt, should_pipe);
  }

  if (!has_expression) {
    throw ErrorWithDetails{
        "calc has no expression to evaluate",
        "Pass an expression such as `calc '2 + 2'`, read a pipe with `-p`, or "
        "enter the interactive prompt with `-i`"};
  }

  LOG(Debug, "calc evaluating %zu arithmetic expressions", operands.count());

  String expression{cxt.scratch_allocator()};
  for (usize i = 0; i < operands.count(); i++) {
    if (i > 0) expression += ' ';
    expression += operands[i].view();
  }

  Maybe<SourceLocation> expression_base;
  if (!operand_locations.is_empty() &&
      operand_locations.count() == operands.count() &&
      cxt.current_source() != nullptr)
  {
    let const first_location = operand_locations[0];
    let const last_location = operand_locations[operand_locations.count() - 1];
    let const expression_end =
        static_cast<usize>(last_location.position) + last_location.length;
    let const source = cxt.current_source()->view();
    let const is_one_source =
        first_location.source_name_index == last_location.source_name_index;
    let const is_valid_span = first_location.position <= expression_end &&
                              expression_end <= source.length;
    let const operand_source =
        is_one_source && is_valid_span
            ? source.substring_of_length(first_location.position,
                                         expression_end -
                                             first_location.position)
            : StringView{};
    if (let const expression_offset =
            operand_source.find_substring(expression.view());
        expression_offset.has_value())
    {
      expression_base =
          SourceLocation{first_location.position + *expression_offset,
                         expression.count(), first_location.source_name_index};
    }
  }

  return evaluate_one(ec, cxt, expression.view(),
                      expression_base.has_value() ? &*expression_base
                                                  : nullptr);
}

} /* namespace koshkit */

} /* namespace koshka */
