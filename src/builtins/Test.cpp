/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the test and bracket expression grammar, including
 * string, integer, filesystem, option, logical, and parenthesized operators
 * with shell-specific argument-count rules.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("expression", "[ expression ]");

HELP_DESCRIPTION_DECL(
    "The test builtin evaluates an expression and reports true or false.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Test);

namespace koshka {

namespace {

fn parse_integer(StringView text, i64 &out) throws -> bool
{
  bool is_out_of_range = false;
  let const parsed = utils::parse_decimal_i64(text, &is_out_of_range);
  if (parsed.is_error() || is_out_of_range) {
    return false;
  }
  out = parsed.value();
  return true;
}

/* The window is [pos, end), so the argument-count rules can strip a wrapping
   paren pair by narrowing pos and end before the grammar runs. */
class TestEvaluator
{
public:
  const ExecContext &ec;
  const ArrayList<String> &args;
  usize pos;
  usize end;
  bool is_bash_compatible;

  pure const String &current() const wontthrow
  {
    ASSERT(pos < end);
    return args[pos];
  }
  pure bool at_end() const wontthrow { return pos >= end; }

  void fail(StringView message) throws
  {
    let error = make_error_for_arg(ec, pos, message);
    error.set_command_status(2);
    throw error;
  }

  bool evaluate_unary(const String &op, const String &operand) throws
  {
    if (op == "-z") return operand.is_empty();
    if (op == "-n") return !operand.is_empty();

    let const operand_path = Path{operand};

    if (op == "-s") {
      let const size = operand_path.file_size();
      return size.has_value() && size.value() > 0;
    }
    using file_predicate = bool (Path::*)() const;
    static constexpr static_string_entry<file_predicate> ENTRIES[] = {
        {SSK("-e"), &Path::exists                     },
        {SSK("-f"), &Path::is_regular_file            },
        {SSK("-d"), &Path::is_directory               },
        {SSK("-r"), &Path::is_readable                },
        {SSK("-w"), &Path::is_writable                },
        {SSK("-x"), &Path::is_executable              },
        {SSK("-L"), &Path::is_symbolic_link           },
        {SSK("-h"), &Path::is_symbolic_link           },
        {SSK("-b"), &Path::is_block_device            },
        {SSK("-c"), &Path::is_character_device        },
        {SSK("-p"), &Path::is_fifo                    },
        {SSK("-S"), &Path::is_socket                  },
        {SSK("-g"), &Path::has_setgid_bit             },
        {SSK("-u"), &Path::has_setuid_bit             },
        {SSK("-k"), &Path::has_sticky_bit             },
        {SSK("-O"), &Path::is_owned_by_effective_user },
        {SSK("-G"), &Path::is_owned_by_effective_group},
    };
    static constexpr StaticStringMap FILE_TESTS{ENTRIES};

    if (let const predicate = FILE_TESTS.find(op.view()); predicate.has_value())
      return (operand_path.*(*predicate))();

    if (op == "-t") {
      i64 file_descriptor = 0;
      if (!parse_integer(operand.view(), file_descriptor)) return false;
      /* Any descriptor is checked, not only the standard three, since a config
         dups the controlling terminal onto a higher descriptor and tests it. */
      return os::shell_fd_is_a_tty(static_cast<int>(file_descriptor));
    }
    fail(
        StringView{"'"} + op +
        "' is not a known unary operator, expected one of -z -n -e -f -d -s -r "
        "-w -x -L -h -b -c -p -S -g -u -k -O -G -t");
    return false;
  }

  bool evaluate_binary(const String &left, const String &op,
                       const String &right) throws
  {
    /* == is a bashism for string equality. bash accepts it, so the bash mood
       treats it as =, while the default and POSIX moods reject it the way dash
       does. The analysis stage also warns on it as SC3014. */
    if (op == "=" || (op == "==" && is_bash_compatible)) {
      return left == right;
    }
    if (op == "==") {
      fail("'==' is a bashism, use = for string equality in POSIX mode");
      return false;
    }
    if (op == "!=") return left != right;
    if (op == "<") return left < right;
    if (op == ">") return right < left;

    if (op == "-ef") return Path{left}.is_same_file_as(Path{right});
    if (op == "-nt") return Path{left}.is_newer_than(Path{right});
    if (op == "-ot") return Path{left}.is_older_than(Path{right});

    i64 left_number = 0, right_number = 0;
    if (op == "-eq" || op == "-ne" || op == "-lt" || op == "-le" ||
        op == "-gt" || op == "-ge")
    {
      let const left_is_integer = parse_integer(left, left_number);
      let const right_is_integer = parse_integer(right, right_number);
      if (!left_is_integer || !right_is_integer) {
        let const &not_a_number = left_is_integer ? right : left;
        fail(StringView{"Cannot compare with '"} + op + "', '" + not_a_number +
             "' is not an integer");
        return false;
      }
      if (op == "-eq") return left_number == right_number;
      if (op == "-ne") return left_number != right_number;
      if (op == "-lt") return left_number < right_number;
      if (op == "-le") return left_number <= right_number;
      if (op == "-gt") return left_number > right_number;
      return left_number >= right_number;
    }
    fail(StringView{"'"} + op +
         "' is not a known binary operator, expected one of = != < > -eq -ne "
         "-lt -le -gt -ge -ef -nt -ot");
    return false;
  }

  pure fn is_unary_operator(const String &s) const wontthrow -> bool
  {
    static constexpr PackedStringKey KEYS[] = {
        SSK("-z"), SSK("-n"), SSK("-e"), SSK("-f"), SSK("-d"), SSK("-s"),
        SSK("-r"), SSK("-w"), SSK("-x"), SSK("-L"), SSK("-h"), SSK("-b"),
        SSK("-c"), SSK("-p"), SSK("-S"), SSK("-g"), SSK("-u"), SSK("-k"),
        SSK("-O"), SSK("-G"), SSK("-t"),
    };
    static constexpr StaticStringSet UNARY_OPS{KEYS};
    return UNARY_OPS.contains(s.view());
  }

  pure fn is_binary_operator(const String &s) const wontthrow -> bool
  {
    static constexpr PackedStringKey KEYS[] = {
        SSK("="),   SSK("=="),  SSK("!="),  SSK("<"),   SSK(">"),
        SSK("-eq"), SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"),
        SSK("-ge"), SSK("-ef"), SSK("-nt"), SSK("-ot"),
    };
    static constexpr StaticStringSet BINARY_OPS{KEYS};
    return BINARY_OPS.contains(s.view());
  }

  /* A unary operator at index reads as a plain operand rather than an operator
     when nothing follows it, or when two or more tokens follow and the next is
     a binary operator, mirroring dash's isoperand. With exactly one token after
     it the operator is a real unary primary. */
  pure bool is_unary_in_operand_position(usize index) const wontthrow
  {
    if (index + 1 >= end) return true;
    if (index + 2 >= end) return false;
    return is_binary_operator(args[index + 1]);
  }

  /* A bare ( reads as a plain operand rather than a grouping paren when no
     token follows it, mirroring dash's t_lex special case for a trailing paren.
   */
  pure bool is_open_paren_token(usize index) const wontthrow
  {
    return args[index] == "(" && index + 1 < end;
  }

  pure bool is_unary_operator_token(usize index) const wontthrow
  {
    return is_unary_operator(args[index]) &&
           !is_unary_in_operand_position(index);
  }

  bool parse_factor() throws
  {
    if (at_end()) {
      fail("An argument is expected");
      return false;
    }
    if (current() == "!") {
      /* A lone ! with nothing after it is the one-argument test of a non-empty
         string, which is true, not a negation missing its operand. */
      if (pos + 1 >= end) {
        pos++;
        return true;
      }
      pos++;
      return !parse_factor();
    }
    if (is_open_paren_token(pos)) {
      pos++;
      let const result = parse_expression();
      if (at_end() || current() != ")") {
        fail("A ')' is expected");
      } else
        pos++;
      return result;
    }
    if (is_unary_operator_token(pos)) {
      let const &op = args[pos];
      let const &operand = args[pos + 1];
      pos += 2;
      return evaluate_unary(op, operand);
    }
    if (pos + 1 < end && is_binary_operator(args[pos + 1])) {
      if (pos + 2 >= end) {
        fail(StringView{"An argument is expected after '"} + args[pos + 1] +
             "'");
        pos = end;
        return false;
      }
      let const &left = args[pos];
      let const &op = args[pos + 1];
      let const &right = args[pos + 2];
      pos += 3;
      return evaluate_binary(left, op, right);
    }
    let const result = !current().is_empty();
    pos++;
    return result;
  }

  bool parse_term() throws
  {
    let is_true = parse_factor();
    while (!at_end() && current() == "-a") {
      pos++;
      let const right = parse_factor();
      is_true = is_true && right;
    }
    return is_true;
  }

  bool parse_expression() throws
  {
    let is_true = parse_term();
    while (!at_end() && current() == "-o") {
      pos++;
      let const right = parse_term();
      is_true = is_true || right;
    }
    return is_true;
  }

  /* The POSIX argument-count rules disambiguate the short forms before the
     grammar runs, mirroring dash's testcmd. A three-argument form whose middle
     operand is a binary primary is the binary test, so a literal paren in the
     first or third operand stays a plain operand. A three or four argument form
     wrapped in a paren pair strips the pair, and one led by ! strips the bang
     and flips the verdict, then rechecks the shorter window. Whatever remains
     runs through the grammar, where a paren groups and -a and -o connect. The
     window narrows by pos and end so the stripping needs no copy. */
  bool evaluate_top() throws
  {
    let should_negate = false;
    loop
    {
      let const count = end - pos;
      if (count < 1) return !should_negate;

      if (count == 3 && is_binary_operator(args[pos + 1])) {
        let const result =
            evaluate_binary(args[pos], args[pos + 1], args[pos + 2]);
        pos = end;
        return should_negate ? !result : result;
      }

      if (count == 3 || count == 4) {
        if (args[pos] == "(" && args[end - 1] == ")") {
          pos++;
          end--;
          continue;
        }
        if (args[pos] == "!") {
          should_negate = !should_negate;
          pos++;
          continue;
        }
      }
      break;
    }

    let const result = parse_expression();
    return should_negate ? !result : result;
  }
};

} /* namespace */

Test::Test() = default;

pure fn Test::kind() const wontthrow -> Builtin::Kind { return Kind::Test; }

fn Test::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &arguments = ec.args();
  ASSERT(!arguments.is_empty());

  /* Only the kosh default mood answers --help, only as the sole argument,
     and only for the word form, since bash and dash evaluate the word as a
     nonempty string and [ --help ] stays an expression. */
  if (arguments.count() == 2 && arguments[1] == "--help" &&
      ec.program() != "[" && !cxt.is_posix_mode() && !cxt.is_bash_compatible())
  {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  usize expression_end = arguments.count();
  if (ec.program() == "[") {
    if (arguments.count() < 2 || arguments[arguments.count() - 1] != "]") {
      let error = Error{"The closing ']' is missing"};
      error.set_command_status(2);
      throw error;
    }
    expression_end = arguments.count() - 1;
  }

  if (expression_end <= 1) return 1;

  LOG(All, "test evaluating %zu operands", expression_end - 1);

  let evaluator =
      TestEvaluator{ec, arguments, 1, expression_end, cxt.is_bash_compatible()};
  let const result = evaluator.evaluate_top();
  /* A paren pair the argument-count rules stripped narrowed end past the
     closing paren, so the leftover check runs against the narrowed window
     rather than the original operand count. */
  if (evaluator.pos != evaluator.end) {
    ASSERT(evaluator.pos < evaluator.end);
    let error = make_error_for_arg(
        ec, evaluator.pos,
        StringView{"'"} + arguments[evaluator.pos] +
            "' is an unexpected argument",
        "A test takes an operator or an operand at this position");
    error.set_command_status(2);
    throw error;
  }
  return result ? 0 : 1;
}

} /* namespace koshka */
