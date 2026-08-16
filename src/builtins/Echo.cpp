#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-neE] [arg ...]");

HELP_DESCRIPTION_DECL(
    "The echo builtin prints its arguments followed by a newline.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(ECHO_NO_NEWLINE, Bool, 'n', "", "Do not print the trailing newline.");
FLAG(ECHO_ESCAPES, Bool, 'e', "", "Interpret backslash escapes.");
FLAG(ECHO_NO_ESCAPES, Bool, 'E', "", "Leave backslash escapes literal.");

REGISTER_BUILTIN_FLAGS(Echo);

namespace koshka {

Echo::Echo() = default;

pure fn Echo::kind() const wontthrow -> Builtin::Kind { return Kind::Echo; }

fn Echo::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  /* Only the kosh default mood answers --help, since bash and dash print the
     literal text a script may depend on. */
  if (args.count() == 2 && args[1] == "--help" && !cxt.is_posix_mode() &&
      !cxt.is_bash_compatible())
  {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  LOG(All, "echo printing %zu arguments", args.count() - 1);

  usize start = 1;
  let should_suppress_newline = false;
  let const is_windows_default =
      cxt.mood() == mimic_mood::Default && os::DIRECTORY_SEPARATOR == '\\';
  let should_interpret_escapes =
      !cxt.is_bash_compatible() && !is_windows_default;

  if (!cxt.is_posix_mode()) {
    while (start < args.count()) {
      let const arg = args[start].view();
      if (arg.length < 2 || arg[0] != '-') {
        break;
      }
      let is_all_option_letters = true;
      for (usize k = 1; k < arg.length; k++)
        if (arg[k] != 'n' && arg[k] != 'e' && arg[k] != 'E') {
          is_all_option_letters = false;
          break;
        }
      if (!is_all_option_letters) break;
      for (usize k = 1; k < arg.length; k++) {
        if (arg[k] == 'n')
          should_suppress_newline = true;
        else if (arg[k] == 'e')
          should_interpret_escapes = true;
        else if (arg[k] == 'E')
          should_interpret_escapes = false;
      }
      start++;
    }
  } else {
    while (start < args.count() && args[start] == "-n") {
      should_suppress_newline = true;
      start++;
    }
  }

  let output = String{cxt.scratch_allocator()};
  let should_stop = false;

  for (usize i = start; i < args.count() && !should_stop; i++) {
    if (i > start) output += ' ';

    let const &arg = args[i];
    for (usize j = 0; j < arg.length(); j++) {
      if (!should_interpret_escapes || arg[j] != '\\' || j + 1 >= arg.length())
      {
        output += arg[j];
        continue;
      }

      let const escaped = arg[j + 1];
      j++;
      switch (escaped) {
      case 'a': output += '\a'; break;
      case 'b': output += '\b'; break;
      case 'f': output += '\f'; break;
      case 'n': output += '\n'; break;
      case 'r': output += '\r'; break;
      case 't': output += '\t'; break;
      case 'v': output += '\v'; break;
      case 'e':
      case 'E':
        if (cxt.is_posix_mode()) {
          output += '\\';
          output += escaped;
        } else {
          output += '\x1b';
        }
        break;
      case '\\': output += '\\'; break;
      case 'c': should_stop = true; break;
      case 'x': {
        if (cxt.is_posix_mode()) {
          output += '\\';
          output += escaped;
          break;
        }

        let const do_hex_value = [](char digit) -> i32 {
          if (digit >= '0' && digit <= '9') {
            return digit - '0';
          }
          if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
          }
          if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
          }
          return -1;
        };

        i32 value = 0;
        usize digit_count = 0;
        while (digit_count < 2 && j + 1 < arg.length()) {
          let const digit = do_hex_value(arg[j + 1]);
          if (digit < 0) break;
          value = value * 16 + digit;
          j++;
          digit_count++;
        }

        if (digit_count == 0) {
          output += '\\';
          output += escaped;
        } else {
          output += static_cast<char>(value);
        }
      } break;
      case '0': {
        i32 value = 0;
        usize digit_count = 0;
        while (digit_count < 3 && j + 1 < arg.length() && arg[j + 1] >= '0' &&
               arg[j + 1] <= '7')
        {
          value = value * 8 + (arg[j + 1] - '0');
          j++;
          digit_count++;
        }
        output += static_cast<char>(value);
      } break;
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7': {
        if (cxt.is_bash_compatible()) {
          output += '\\';
          output += escaped;
          break;
        }
        i32 value = escaped - '0';
        usize digit_count = 1;
        while (digit_count < 3 && j + 1 < arg.length() && arg[j + 1] >= '0' &&
               arg[j + 1] <= '7')
        {
          value = value * 8 + (arg[j + 1] - '0');
          j++;
          digit_count++;
        }
        output += static_cast<char>(value);
      } break;
      default:
        output += '\\';
        output += escaped;
        break;
      }

      if (should_stop) break;
    }
  }

  if (!should_suppress_newline && !should_stop) {
    output += '\n';
  }

  ec.print_to_stdout(output);

  return 0;
}

} /* namespace koshka */
