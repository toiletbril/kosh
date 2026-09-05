/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements operand classifiers shared by test, printf,
 * redirection, command-name, variable, and parameter-expansion checks. It
 * centralizes operator and name tables without performing command traversal.
 */

#include "DiagnosticsChecksInternal.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions::internal {

/* The direct test operator a leading ! collapses into, for the SC2335 lint.
   None for an operator with no negated shortcut. */
constexpr static_string_entry<StringView> NEGATED_TEST_OPERATOR_ENTRIES[] = {
    {SSK("-eq"), StringView{"-ne", 3}},
    {SSK("-ne"), StringView{"-eq", 3}},
    {SSK("-lt"), StringView{"-ge", 3}},
    {SSK("-ge"), StringView{"-lt", 3}},
    {SSK("-gt"), StringView{"-le", 3}},
    {SSK("-le"), StringView{"-gt", 3}},
    {SSK("="),   StringView{"!=", 2} },
    {SSK("!="),  StringView{"=", 1}  },
};
constexpr StaticStringMap NEGATED_TEST_OPERATORS{NEGATED_TEST_OPERATOR_ENTRIES};

cold fn negated_test_operator(StringView op) wontthrow -> Maybe<StringView>
{
  return NEGATED_TEST_OPERATORS.find(op);
}

/* The binary operators of test, used to tell a == in the operator slot from a
   literal == operand, so the SC3014 lint does not flag [ x = == ]. */
constexpr PackedStringKey TEST_BINARY_OPERATOR_KEYS[] = {
    SSK("="),   SSK("=="),  SSK("!="),  SSK("<"),   SSK(">"),
    SSK("-eq"), SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"),
    SSK("-ge"), SSK("-ef"), SSK("-nt"), SSK("-ot"),
};
constexpr StaticStringSet TEST_BINARY_OPERATORS{TEST_BINARY_OPERATOR_KEYS};

cold fn is_test_binary_operator_word(StringView op) wontthrow -> bool
{
  return TEST_BINARY_OPERATORS.contains(op);
}

/* The file comparison operators of test, absent from POSIX, for the SC3013
   lint. */
constexpr PackedStringKey TEST_FILE_COMPARISON_KEYS[] = {
    SSK("-ef"),
    SSK("-nt"),
    SSK("-ot"),
};
constexpr StaticStringSet TEST_FILE_COMPARISONS{TEST_FILE_COMPARISON_KEYS};

cold fn is_test_file_comparison_word(StringView op) wontthrow -> bool
{
  return TEST_FILE_COMPARISONS.contains(op);
}

/* The numeric comparison operators of test, for the SC2170 lint. */
constexpr PackedStringKey TEST_NUMERIC_OPERATOR_KEYS[] = {
    SSK("-eq"), SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"), SSK("-ge"),
};
constexpr StaticStringSet TEST_NUMERIC_OPERATORS{TEST_NUMERIC_OPERATOR_KEYS};

cold fn is_test_numeric_operator_word(StringView op) wontthrow -> bool
{
  return TEST_NUMERIC_OPERATORS.contains(op);
}

/* The unary operators of test, gathered from the test builtin and the
   conditional evaluator, for the SC2057 and SC2058 lints. -a and -o are listed
   because they also join two conditions. */
constexpr PackedStringKey TEST_UNARY_OPERATOR_KEYS[] = {
    SSK("-a"), SSK("-b"), SSK("-c"), SSK("-d"), SSK("-e"), SSK("-f"), SSK("-g"),
    SSK("-h"), SSK("-k"), SSK("-n"), SSK("-o"), SSK("-p"), SSK("-r"), SSK("-s"),
    SSK("-t"), SSK("-u"), SSK("-v"), SSK("-w"), SSK("-x"), SSK("-z"), SSK("-G"),
    SSK("-L"), SSK("-N"), SSK("-O"), SSK("-R"), SSK("-S"),
};
constexpr StaticStringSet TEST_UNARY_OPERATORS{TEST_UNARY_OPERATOR_KEYS};

cold fn is_known_test_operator_word(StringView op) wontthrow -> bool
{
  return TEST_UNARY_OPERATORS.contains(op) ||
         TEST_BINARY_OPERATORS.contains(op);
}

/* The unary operators that take a path, for the SC2245 lint. -a is left out
   because it also joins two conditions, -t takes a descriptor, and -n and -z
   belong to SC2157. */
constexpr PackedStringKey TEST_PATH_UNARY_OPERATOR_KEYS[] = {
    SSK("-b"), SSK("-c"), SSK("-d"), SSK("-e"), SSK("-f"), SSK("-g"), SSK("-h"),
    SSK("-k"), SSK("-p"), SSK("-r"), SSK("-s"), SSK("-u"), SSK("-w"), SSK("-x"),
    SSK("-G"), SSK("-L"), SSK("-N"), SSK("-O"), SSK("-S"),
};
constexpr StaticStringSet TEST_PATH_UNARY_OPERATORS{
    TEST_PATH_UNARY_OPERATOR_KEYS};

cold fn is_test_path_unary_operator_word(StringView op) wontthrow -> bool
{
  return TEST_PATH_UNARY_OPERATORS.contains(op);
}

cold fn is_test_unary_operator_word(StringView op) wontthrow -> bool
{
  return TEST_UNARY_OPERATORS.contains(op);
}

/* The words that open a fresh condition, so the word after one of them sits in
   the unary operator slot. */
constexpr PackedStringKey TEST_CONDITION_OPENER_KEYS[] = {
    SSK("!"),
    SSK("("),
    SSK("-a"),
    SSK("-o"),
};
constexpr StaticStringSet TEST_CONDITION_OPENERS{TEST_CONDITION_OPENER_KEYS};

cold fn is_test_condition_opener_word(StringView word) wontthrow -> bool
{
  return TEST_CONDITION_OPENERS.contains(word);
}

/* The words a bracketed constant condition can hold, whose diagnostic names the
   builtin the author meant. */
constexpr static_string_entry<bracketed_constant_kind>
    BRACKETED_CONSTANT_ENTRIES[] = {
        {SSK("0"),     bracketed_constant_kind::Zero },
        {SSK("1"),     bracketed_constant_kind::One  },
        {SSK("false"), bracketed_constant_kind::False},
        {SSK("true"),  bracketed_constant_kind::True },
};
constexpr StaticStringMap BRACKETED_CONSTANTS{BRACKETED_CONSTANT_ENTRIES};

cold fn get_bracketed_constant_kind(StringView word) wontthrow
    -> Maybe<bracketed_constant_kind>
{
  return BRACKETED_CONSTANTS.find(word);
}

/* The left operand of an X != Y triple centered on operator_index, absent when
   the words there do not form one. The raw view is compared, so "$name" and
   $name stay distinct. */
cold fn test_inequality_left_operand(const ArrayList<const Token *> &args,
                                     usize operator_index,
                                     usize operand_end) wontthrow
    -> Maybe<StringView>
{
  if (operator_index == 0 || operator_index + 1 >= operand_end) {
    return None;
  }

  let const op = args[operator_index]->raw_view();
  if (!op.has_value() || *op != StringView{"!="}) {
    return None;
  }

  return args[operator_index - 1]->raw_view();
}

/* An operator name carries a letter after the dash, which keeps a negative
   number such as -5 out of the unknown-operator lints. */
cold fn view_looks_like_test_operator(StringView view) wontthrow -> bool
{
  if (view.length < 2 || view[0] != '-') {
    return false;
  }

  let const byte = view[1];

  return lexer::is_variable_name_start(byte) && byte != '_';
}

cold fn view_has_decimal_fraction(StringView view) wontthrow -> bool
{
  usize position = 0;
  if (position < view.length) {
    switch (view[position]) {
    case '-':
    case '+': position++; break;
    default: break;
    }
  }

  let const integer_start = position;
  while (position < view.length && lexer::is_number(view[position]))
    position++;

  if (position == integer_start) return false;

  if (position >= view.length || view[position] != '.') {
    return false;
  }

  position++;

  let const fraction_start = position;
  while (position < view.length && lexer::is_number(view[position]))
    position++;

  return position > fraction_start && position == view.length;
}

static pure fn is_arithmetic_operand_byte(char byte) wontthrow -> bool
{
  if (lexer::is_variable_name(byte)) return true;

  switch (byte) {
  case '$':
  case '}':
  case ')': return true;
  default: return false;
  }
}

/* A multiplicative or additive operator between two operand bytes, which reads
   as arithmetic the test builtin never evaluates. The minus sign is left out
   because a date such as 2019-01-01 carries the same shape. */
cold fn view_has_arithmetic_operator(StringView view) wontthrow -> bool
{
  for (usize position = 1; position + 1 < view.length; position++) {
    switch (view[position]) {
    case '+':
    case '*':
    case '/':
    case '%': break;
    default: continue;
    }

    if (!is_arithmetic_operand_byte(view[position - 1])) continue;

    if (!is_arithmetic_operand_byte(view[position + 1])) continue;

    return true;
  }

  return false;
}

/* A letter that appears twice in a tr set, which reads as a word rather than as
   a set of characters. Case is kept apart because tr keeps it apart. */
cold fn view_repeats_a_letter(StringView view) wontthrow -> bool
{
  u32 seen_lowercase = 0;
  u32 seen_uppercase = 0;

  for (usize position = 0; position < view.length; position++) {
    let const byte = view[position];
    if (byte >= 'a' && byte <= 'z') {
      let const letter_bit = 1u << static_cast<u32>(byte - 'a');
      if ((seen_lowercase & letter_bit) != 0) return true;
      seen_lowercase |= letter_bit;
    } else if (byte >= 'A' && byte <= 'Z') {
      let const letter_bit = 1u << static_cast<u32>(byte - 'A');
      if ((seen_uppercase & letter_bit) != 0) return true;
      seen_uppercase |= letter_bit;
    }
  }

  return false;
}

/* A pattern whose only regular expression byte is a star that follows an
   ordinary byte. The author wrote a glob, where the star repeats one character
   instead of matching any text. */
cold fn view_is_glob_shaped_pattern(StringView view) wontthrow -> bool
{
  bool has_repetition = false;

  for (usize position = 0; position < view.length; position++) {
    switch (view[position]) {
    case '*':
      if (position == 0) return false;
      has_repetition = true;
      break;
    case '.':
    case '^':
    case '$':
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case '+':
    case '?':
    case '|':
    case '\\': return false;
    default: break;
    }
  }

  return has_repetition;
}

/* A sed script of the s<delimiter>search<delimiter>replacement<delimiter> shape
   whose fields hold no regular expression byte, so a parameter expansion
   replaces the text without a fork. */
cold fn view_is_plain_substitution_script(StringView view) wontthrow -> bool
{
  if (view.length < 4 || view[0] != 's') {
    return false;
  }

  let const delimiter = view[1];
  if (lexer::is_variable_name(delimiter) || delimiter == '\\') {
    return false;
  }

  usize field_count = 1;
  for (usize position = 2; position < view.length; position++) {
    let const byte = view[position];
    if (byte == delimiter) {
      field_count++;
      continue;
    }

    if (field_count == 3) {
      if (byte != 'g') return false;
      continue;
    }

    switch (byte) {
    case '.':
    case '*':
    case '^':
    case '$':
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case '+':
    case '?':
    case '|':
    case '&':
    case '\\': return false;
    default: break;
    }
  }

  return field_count == 3;
}

/* The escape sequence a shell echo prints as written, for the printf
   suggestion. The returned view spans the backslash and the letter behind
   it. */
cold fn find_echo_escape_sequence(StringView view) wontthrow -> StringView
{
  for (usize position = 0; position + 1 < view.length; position++) {
    if (view[position] != '\\') continue;

    switch (view[position + 1]) {
    case 'a':
    case 'b':
    case 'e':
    case 'f':
    case 'n':
    case 'r':
    case 't':
    case 'v':
    case '0': return view.substring_of_length(position, 2);
    default: position++;
    }
  }

  return {};
}

/* An echo option bundle that names the escape handling, so the operand text is
   written the way the author intends. */
cold fn view_settles_echo_escapes(StringView view) wontthrow -> bool
{
  if (view.length < 2 || view[0] != '-') {
    return false;
  }

  bool has_escape_letter = false;
  for (usize position = 1; position < view.length; position++) {
    switch (view[position]) {
    case 'e':
    case 'E': has_escape_letter = true; break;
    case 'n': break;
    default: return false;
    }
  }

  return has_escape_letter;
}

/* The command that produces the output of a substitution body. The body is read
   back to its last pipe, since that stage writes what the caller collects. */
cold fn substitution_runs_pattern_matcher(StringView body) throws -> bool
{
  usize position = 0;
  for (usize scan_position = 0; scan_position < body.length; scan_position++)
    if (body[scan_position] == '|') position = scan_position + 1;

  let const command = body.next_ascii_whitespace_word(position);
  if (command.is_empty()) return false;

  return get_analysis_command_info(command).is_in_group(
      COMMAND_GROUP_PATTERN_MATCHER);
}

cold fn word_is_fully_literal(const Word &word) wontthrow -> bool
{
  for (let const &segment : word.segments)
    if (segment.kind != WordSegment::Kind::LiteralText &&
        segment.kind != WordSegment::Kind::UnquotedText &&
        segment.kind != WordSegment::Kind::DoubleQuotedText)
    {
      return false;
    }
  return true;
}

cold fn token_has_command_substitution(const Token *token) wontthrow -> bool
{
  if (token->kind() != Token::Kind::Word) return false;

  for (let const &segment :
       static_cast<const tokens::WordToken *>(token)->word().segments)
    if (segment.kind == WordSegment::Kind::CommandSubstitution) return true;

  return false;
}

cold fn token_has_ansi_c_quote(const Token *token) wontthrow -> bool
{
  if (token->kind() != Token::Kind::Word) return false;

  for (let const &segment :
       static_cast<const tokens::WordToken *>(token)->word().segments)
    if (segment.was_ansi_c_quoted) return true;

  return false;
}

cold fn printf_consumed_argument_count(StringView format,
                                       bool &has_quote_conversion) wontthrow
    -> usize
{
  usize count = 0;
  for (usize i = 0; i < format.length; i++) {
    if (format[i] != '%' || i + 1 >= format.length) {
      continue;
    }
    i++;
    if (format[i] == '%') continue;

    while (i < format.length &&
           (format[i] == '-' || format[i] == '+' || format[i] == ' ' ||
            format[i] == '#' || format[i] == '0'))
      i++;
    if (i >= format.length) break;
    if (format[i] == '*') {
      count++;
      i++;
    } else
      while (i < format.length && (format[i] >= '0' && format[i] <= '9'))
        i++;
    if (i < format.length && format[i] == '.') {
      i++;
      if (i < format.length && format[i] == '*') {
        count++;
        i++;
      } else
        while (i < format.length && (format[i] >= '0' && format[i] <= '9'))
          i++;
    }
    while (i < format.length &&
           (format[i] == 'h' || format[i] == 'l' || format[i] == 'L' ||
            format[i] == 'j' || format[i] == 'z' || format[i] == 't'))
      i++;
    if (i < format.length && format[i] == '(') {
      while (i < format.length && format[i] != ')')
        i++;
      if (i + 1 < format.length) i++;
    }
    if (i < format.length && format[i] == 'q') {
      has_quote_conversion = true;
    }

    count++;
  }

  return count;
}

cold pure fn view_is_integer_literal(StringView view) wontthrow -> bool
{
  usize start = view.length >= 1 && view[0] == '-' ? 1 : 0;
  return start < view.length && view.substring(start).is_all_decimal_digits();
}

cold fn args_have_short_flag(const ArrayList<const Token *> &args,
                             char letter) throws -> bool
{
  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const literal = static_cast<const tokens::WordToken *>(args[i])
                            ->word()
                            .to_literal_string();
    let const view = literal.view();
    if (view.length >= 2 && view[0] == '-' && view[1] != '-' &&
        view.find_character(letter).has_value())
    {
      return true;
    }
  }
  return false;
}

/* The lone operand of a move, a copy or a link, for the missing destination
   lints. None is returned when a destination is named, when a flag supplies the
   destination, or when an operand carries an expansion that could bring more
   words with it. */
cold fn single_literal_file_operand(const ArrayList<const Token *> &args) throws
    -> Maybe<const Token *>
{
  const Token *lone_operand = nullptr;

  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) return None;

    let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
    if (!word_is_fully_literal(word)) return None;

    let const literal = word.to_literal_string();
    let const view = literal.view();
    if (view.is_empty()) return None;

    if (view[0] == '-') {
      if (view == "--" || view == "-t" ||
          view.starts_with(StringView{"--target-directory"}))
      {
        return None;
      }

      continue;
    }

    if (lone_operand != nullptr) return None;
    lone_operand = args[i];
  }

  if (lone_operand == nullptr) return None;

  return lone_operand;
}

/* The top-level system directories rm -r must never aim at, the SC2114
   table. */
constexpr PackedStringKey SYSTEM_DIRECTORY_KEYS[] = {
    SSK("/"),     SSK("/bin"), SSK("/boot"), SSK("/dev"),  SSK("/etc"),
    SSK("/home"), SSK("/lib"), SSK("/proc"), SSK("/root"), SSK("/sbin"),
    SSK("/sys"),  SSK("/usr"), SSK("/var"),
};
constexpr StaticStringSet SYSTEM_DIRECTORIES{SYSTEM_DIRECTORY_KEYS};

pure fn is_system_directory(StringView path) wontthrow -> bool
{
  return SYSTEM_DIRECTORIES.contains(path);
}

constexpr PackedStringKey FIND_ACTION_KEYS[] = {
    SSK("-delete"), SSK("-exec"),    SSK("-execdir"), SSK("-fls"),
    SSK("-fprint"), SSK("-fprint0"), SSK("-fprintf"), SSK("-ls"),
    SSK("-ok"),     SSK("-okdir"),   SSK("-print"),   SSK("-print0"),
    SSK("-printf"), SSK("-prune"),   SSK("-quit"),    SSK("-used")};
constexpr StaticStringSet FIND_ACTIONS{FIND_ACTION_KEYS};

pure fn is_find_action(StringView word) wontthrow -> bool
{
  return FIND_ACTIONS.contains(word);
}

/* The find options that stand before the search paths, so the path lint keeps
   reading past them. -f takes the path itself and counts as a path. */
constexpr PackedStringKey FIND_LEADING_OPTION_KEYS[] = {
    SSK("-E"), SSK("-H"), SSK("-L"), SSK("-P"),
    SSK("-d"), SSK("-s"), SSK("-x"), SSK("-X")};
constexpr StaticStringSet FIND_LEADING_OPTIONS{FIND_LEADING_OPTION_KEYS};

pure fn is_find_leading_option(StringView word) wontthrow -> bool
{
  return FIND_LEADING_OPTIONS.contains(word);
}

/* The names the shell or the environment gives a value without the script
   assigning one, so a read of them is not an unassigned read. */
constexpr PackedStringKey SHELL_MAINTAINED_VARIABLE_KEYS[] = {
    SSK("BASH"),
    SSK("BASHOPTS"),
    SSK("BASH_ALIASES"),
    SSK("BASH_ARGC"),
    SSK("BASH_ARGV"),
    SSK("BASH_ENV"),
    SSK("BASH_REMATCH"),
    SSK("BASH_VERSINFO"),
    SSK("BASH_VERSION"),
    SSK("CDPATH"),
    SSK("COLUMNS"),
    SSK("COMP_CWORD"),
    SSK("COMP_LINE"),
    SSK("COMP_POINT"),
    SSK("COMP_WORDS"),
    SSK("DIRSTACK"),
    SSK("DISPLAY"),
    SSK("EDITOR"),
    SSK("ENV"),
    SSK("FCEDIT"),
    SSK("GLOBIGNORE"),
    SSK("HOME"),
    SSK("LANG"),
    SSK("LC_ALL"),
    SSK("LC_COLLATE"),
    SSK("LC_CTYPE"),
    SSK("LC_MESSAGES"),
    SSK("LC_NUMERIC"),
    SSK("LC_TIME"),
    SSK("LINES"),
    SSK("LOGNAME"),
    SSK("MAIL"),
    SSK("MAILCHECK"),
    SSK("MAILPATH"),
    SSK("OLDPWD"),
    SSK("OPTARG"),
    SSK("OPTERR"),
    SSK("OPTIND"),
    SSK("PAGER"),
    SSK("PATH"),
    SSK("PIPESTATUS"),
    SSK("POSIXLY_CORRECT"),
    SSK("PROMPT_COMMAND"),
    SSK("PS1"),
    SSK("PS2"),
    SSK("PS3"),
    SSK("PS4"),
    SSK("PWD"),
    SSK("REPLY"),
    SSK("SHELL"),
    SSK("SHLVL"),
    SSK("TERM"),
    SSK("TIMEFORMAT"),
    SSK("TMPDIR"),
    SSK("TZ"),
    SSK("USER"),
    SSK("VISUAL"),
};
constexpr StaticStringSet SHELL_MAINTAINED_VARIABLES{
    SHELL_MAINTAINED_VARIABLE_KEYS};

pure fn is_shell_maintained_variable(StringView name) wontthrow -> bool
{
  if (is_runtime_dynamic_variable_name(name)) return true;

  static constexpr PackedStringKey KOSH_KEYS[] = {
      SSK("KOSH"),
      SSK("KOSH_BUILD_MODE"),
      SSK("KOSH_CALC_HISTORY"),
      SSK("KOSH_COMMIT"),
      SSK("KOSH_DIRECTORY_HISTORY"),
      SSK("KOSH_FAREWELL"),
      SSK("KOSH_FLAGS"),
      SSK("KOSH_HISTORY_FILE"),
      SSK("KOSH_HISTORY_SIZE"),
      SSK("KOSH_OS"),
      SSK("KOSH_VERSION"),
      SSK("KOSH_WELCOME"),
  };
  static constexpr StaticStringSet KOSH_VARIABLES{KOSH_KEYS};
  if (KOSH_VARIABLES.contains(name)) return true;

  return SHELL_MAINTAINED_VARIABLES.contains(name);
}

constexpr PackedStringKey BASH_ONLY_VARIABLE_KEYS[] = {
    SSK("BASHOPTS"),     SSK("BASH_ALIASES"),   SSK("BASH_ARGC"),
    SSK("BASH_ARGV"),    SSK("BASH_REMATCH"),   SSK("BASH_VERSINFO"),
    SSK("BASH_VERSION"), SSK("COMP_CWORD"),     SSK("COMP_LINE"),
    SSK("COMP_POINT"),   SSK("COMP_WORDS"),     SSK("DIRSTACK"),
    SSK("PIPESTATUS"),   SSK("PROMPT_COMMAND"), SSK("SHLVL"),
};
constexpr StaticStringSet BASH_ONLY_VARIABLES{BASH_ONLY_VARIABLE_KEYS};

/* The builtins that have no external program of the same name, so an external
   launcher such as sudo never finds them. echo, printf, test, pwd, true and
   false are left out because a real program exists for each. */
constexpr PackedStringKey SHELL_ONLY_BUILTIN_KEYS[] = {
    SSK("."),    SSK("alias"),    SSK("cd"),      SSK("declare"), SSK("eval"),
    SSK("exec"), SSK("export"),   SSK("getopts"), SSK("let"),     SSK("local"),
    SSK("read"), SSK("readonly"), SSK("set"),     SSK("shift"),   SSK("source"),
    SSK("trap"), SSK("ulimit"),   SSK("umask"),   SSK("unalias"), SSK("unset"),
};
constexpr StaticStringSet SHELL_ONLY_BUILTINS{SHELL_ONLY_BUILTIN_KEYS};

pure fn is_shell_only_builtin(StringView name) wontthrow -> bool
{
  return SHELL_ONLY_BUILTINS.contains(name);
}

/* An ssh short option that consumes the operand behind it, so the host lint
   does not read that operand as the remote host. */
pure fn ssh_option_takes_value(char letter) wontthrow -> bool
{
  switch (letter) {
  case 'B':
  case 'b':
  case 'c':
  case 'D':
  case 'E':
  case 'e':
  case 'F':
  case 'I':
  case 'i':
  case 'J':
  case 'L':
  case 'l':
  case 'm':
  case 'O':
  case 'o':
  case 'p':
  case 'Q':
  case 'R':
  case 'S':
  case 'W':
  case 'w': return true;
  default: return false;
  }
}

pure fn leading_command_word(StringView text) wontthrow -> StringView
{
  usize position = 0;
  return text.next_ascii_whitespace_word(position);
}

fn check_posix_parameter_expansion(AnalysisContext &actx,
                                   const WordSegment &segment, StringView text,
                                   SourceLocation fallback_location) throws
    -> void
{
  if (text.is_empty()) return;

  let const do_get_location = [&]() -> SourceLocation {
    return expansion_location_with_sigil(
        actx, segment.get_source_location(fallback_location.source_name_index)
                  .value_or(fallback_location));
  };

  if (text[0] == '!') {
    if (text.length < 2) return;

    if (text.find_character('[').has_value()) {
      actx.report_diagnostic(diagnostic_id::sc3055, do_get_location(), {text});
      return;
    }

    let const last = text[text.length - 1];
    if (last == '*' || last == '@') {
      actx.report_diagnostic(diagnostic_id::sc3056, do_get_location(), {text});
      return;
    }

    actx.report_diagnostic(diagnostic_id::sc3053, do_get_location(), {text});
    return;
  }

  const usize name_start = text[0] == '#' ? 1 : 0;
  usize position = name_start;
  while (position < text.length && lexer::is_variable_name(text[position]))
    position++;

  if (position == name_start) return;

  let const name = text.substring_of_length(name_start, position - name_start);
  if (BASH_ONLY_VARIABLES.contains(name) ||
      is_bash_only_dynamic_variable_name(name))
  {
    actx.report_diagnostic(diagnostic_id::sc3028, do_get_location(), {name});
    return;
  }

  if (position >= text.length) return;

  switch (text[position]) {
  case '[':
    actx.report_diagnostic(diagnostic_id::sc3054, do_get_location(), {text});
    break;

  case '/':
    actx.report_diagnostic(diagnostic_id::sc3060, do_get_location(), {text});
    break;

  case ':': {
    let const modifier = position + 1 < text.length ? text[position + 1] : '\0';
    if (modifier == '-' || modifier == '=' || modifier == '?' ||
        modifier == '+')
    {
      break;
    }
    actx.report_diagnostic(diagnostic_id::sc3057, do_get_location(), {text});
    break;
  }

  default: break;
  }
}

/* The name an arithmetic assignment writes, read backwards from the '=' the
   scan stands on. The view is empty when that '=' closes a comparison. */
pure fn arithmetic_assignment_target(StringView expression,
                                     usize equals_position) wontthrow
    -> StringView
{
  if (equals_position == 0) return {};
  if (equals_position + 1 < expression.length &&
      expression[equals_position + 1] == '=')
  {
    return {};
  }

  usize at = equals_position;

  switch (expression[at - 1]) {
  case '=':
  case '!': return {};

  case '<':
  case '>':
    if (at < 2 || expression[at - 2] != expression[at - 1]) {
      return {};
    }
    at -= 2;
    break;

  case '+':
  case '-':
  case '*':
  case '/':
  case '%':
  case '&':
  case '|':
  case '^': at--; break;

  default: break;
  }

  while (at > 0 && lexer::is_whitespace(expression[at - 1]))
    at--;

  let const end = at;
  while (at > 0 && lexer::is_variable_name(expression[at - 1]))
    at--;

  if (at == end) return {};
  if (!lexer::is_variable_name_start(expression[at])) return {};

  return expression.substring_of_length(at, end - at);
}

} /* namespace expressions::internal */

} /* namespace koshka */
