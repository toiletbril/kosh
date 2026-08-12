#include "Diagnostics.hpp"

#include "ExpressionsInternal.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"

namespace koshka {

#define D(code, slug, summary, message, suggestion, related, tier, delivery)   \
  {slug,                                                                       \
   summary,                                                                    \
   message,                                                                    \
   suggestion,                                                                 \
   related,                                                                    \
   code,                                                                       \
   diagnostic_tier::tier,                                                      \
   diagnostic_delivery::delivery}

const diagnostic_definition DIAGNOSTIC_DEFINITIONS[] = {
    D(1000, "literal-dollar-in-quotes",
      "a literal dollar sign is written with a backslash",
      "The double-quoted string ends right after `$`, so the shell reads the "
      "dollar sign as ordinary text",
      "Write `\\$` inside the quotes and keep the string in one piece", None,
      Annoying, Policy),
    D(1001, "ineffective-escape", "the backslash before a letter is dropped",
      "The escape '{0}' has no meaning here, so the shell reads it as a plain "
      "'{1}'",
      "Quote the backslash, or build the byte with `printf`", None, Annoying,
      Policy),
    D(1003, "escaped-single-quote",
      "a single-quoted string cannot hold a single quote",
      "A backslash carries no meaning inside single quotes, so this one closes "
      "the string and stays in the text",
      "Write the quote as `'\\''` outside the single-quoted run", None,
      Annoying, Policy),
    D(1004, "escaped-newline-in-quotes",
      "a backslash inside single quotes does not continue the line",
      "The backslash and the line ending are both kept in the single-quoted "
      "text",
      "Close the quote, break the line, and reopen the quote", None, Annoying,
      Policy),
    D(1008, "unrecognized-shebang", "the shebang names an unknown interpreter",
      "The shebang names '{0}', which is not a shell this analysis knows",
      "Name a shell such as `sh` or `bash` in the shebang", None, Annoying,
      Policy),
    D(1012, "literal-control-escape",
      "the backslash before a control letter is dropped",
      "The escape '{0}' has no meaning here, so the shell reads it as a plain "
      "'{1}'",
      "Build the byte with `printf`, or write it inside `$'...'`", None,
      Annoying, Policy),
    D(1014, "direct-command-in-test",
      "if runs a command directly, not inside test brackets",
      "Test brackets do not run the command written inside them",
      "Run the command directly as the if condition", None, Strict, Policy),
    D(1017, "literal-carriage-return", "the script holds a carriage return",
      "A carriage return in the script text is read as data, not as a line "
      "ending",
      "Convert the file to Unix line endings with `tr -d '\\r'`", None, Lenient,
      Policy),
    D(1018, "unicode-space", "a Unicode space does not separate words",
      "The Unicode space '{0}' does not separate words, the shell reads it as "
      "part of one word",
      "Delete it and retype an ASCII space", None, Strict, Policy),
    D(1019, "missing-unary-operand", "a unary test operator takes one operand",
      "The '{0}' operator expects an operand, '{1}' is another operator",
      "Give the operator its operand before the comparison", None, Strict,
      Policy),
    D(1026, "brace-group-in-conditional",
      "grouping inside double brackets uses parentheses",
      "A brace does not group a `[[ ]]` expression",
      "Group the expression with `( )`", None, Strict, Policy),
    D(1029, "escaped-conditional-parenthesis",
      "double brackets take unescaped parentheses",
      "A `[[ ]]` expression takes `(` and `)` without a backslash",
      "Remove the backslash before the parenthesis", None, Strict, Policy),
    D(1035, "test-bracket-spacing",
      "test brackets and operands require separating spaces",
      "Test brackets and operands require separating spaces",
      "Add spaces after the opening bracket and before the close", None, Strict,
      Policy),
    D(1037, "positional-parameter-braces",
      "a positional parameter above nine needs braces",
      "A positional parameter above nine needs braces",
      "Write ${10} to select positional parameter 10", None, Strict, Policy),
    D(1039, "indented-heredoc-terminator",
      "the here-document terminator carries indentation",
      "The line holding '{0}' is indented, so it does not close the "
      "here-document and the rest of the file is read as body text",
      "Move the terminator to the first column", None, Strict, Policy),
    D(1040, "tab-indented-heredoc-terminator",
      "the here-document terminator carries indentation",
      "The line holding '{0}' is indented with tabs, so it does not close the "
      "here-document and the rest of the file is read as body text",
      "Write `<<-` in place of `<<` to strip the leading tabs", None, Strict,
      Policy),
    D(1082, "byte-order-mark", "a byte-order mark precedes the script",
      "A UTF-8 byte-order mark precedes the script text",
      "Save the script as UTF-8 without a byte-order mark", None, Strict,
      Policy),
    D(1084, "swapped-shebang", "the shebang characters are swapped",
      "The first line begins with `!#`, so the shell reads it as a command",
      "Write `#!` at the start of the line", None, Strict, Policy),
    D(1100, "unicode-dash", "a Unicode dash is not the ASCII minus",
      "The Unicode dash '{0}' is not the ASCII minus, so the shell reads it as "
      "ordinary text",
      "Delete it and retype an ASCII minus", None, Strict, Policy),
    D(1101, "blank-after-continuation",
      "a line continuation ends the line at the backslash",
      "A blank follows the backslash, so the backslash escapes that blank and "
      "the line does not continue",
      "Delete the blanks between the backslash and the line ending", None,
      Annoying, Policy),
    D(1104, "shebang-missing-hash", "the shebang is missing its hash",
      "The first line begins with `!`, a shebang begins with `#!`",
      "Write `#!` at the start of the line", None, Strict, Policy),
    D(1106, "arithmetic-test-operator",
      "arithmetic uses the symbolic comparison operators",
      "The '{0}' operator belongs to test, arithmetic reads it as a "
      "subtraction",
      "Use the symbolic form such as `<` or `>` inside arithmetic", None,
      Strict, Policy),
    D(1107, "unknown-shellcheck-directive",
      "the directive names an unknown key",
      "The directive names '{0}', which is not a directive key, so the "
      "directive is ignored",
      "Use `disable`, `enable`, `shell`, `source`, `source-path`, or "
      "`external-sources`",
      None, Annoying, Policy),
    D(1109, "html-entity", "an HTML entity is not shell syntax",
      "The command '{0}' is the tail of the HTML entity `&{0};`, which the "
      "shell reads as an operator followed by a command",
      "Write the character the entity names", None, Strict, Policy),
    D(1110, "unicode-quote", "a Unicode quote does not quote anything",
      "The Unicode quote '{0}' is ordinary text, so it opens no quoted string",
      "Delete it and retype an ASCII quote", None, Strict, Policy),
    D(1111, "unicode-quote-in-double-quotes",
      "a Unicode double quote inside a quoted string is literal",
      "The Unicode quote '{0}' inside this double-quoted string is printed as "
      "text",
      "Delete it and retype an ASCII quote, or single-quote the string to keep "
      "it literal",
      None, Annoying, Policy),
    D(1112, "unicode-quote-in-single-quotes",
      "a Unicode single quote inside a quoted string is literal",
      "The Unicode quote '{0}' inside this single-quoted string is printed as "
      "text",
      "Delete it and retype an ASCII quote, or double-quote the string to keep "
      "it literal",
      None, Annoying, Policy),
    D(1113, "shebang-missing-bang", "the shebang is missing its bang",
      "The first line is an ordinary comment, a shebang begins with `#!`",
      "Write `#!` at the start of the line", None, Strict, Policy),
    D(1114, "shebang-leading-space", "whitespace precedes the shebang",
      "Whitespace precedes the shebang, so the first two bytes are not `#!`",
      "Move `#!` to the start of the line", None, Strict, Policy),
    D(1115, "shebang-inner-space",
      "the shebang holds a space between its characters",
      "A space separates `#` from `!`, so the line is an ordinary comment",
      "Write `#!` with nothing between them", None, Strict, Policy),
    D(1118, "blank-after-heredoc-terminator",
      "the here-document terminator stands alone on its line",
      "A blank follows '{0}', so the line does not close the here-document and "
      "the rest of the file is read as body text",
      "Delete the blanks that follow the terminator", None, Strict, Policy),
    D(1123, "directive-before-clause",
      "a directive belongs before a complete command",
      "The directive sits before '{0}', which continues the command above it, "
      "so nothing is suppressed",
      "Move the directive above the complete command", None, Annoying, Policy),
    D(1124, "directive-before-case-branch",
      "a directive belongs before a complete command",
      "The directive sits before a case branch, so nothing is suppressed",
      "Move the directive above the `case` command", None, Annoying, Policy),
    D(1125, "malformed-shellcheck-directive",
      "a directive takes a key and a value",
      "The directive holds '{0}', which is not a `key=value` pair, so the "
      "directive is ignored",
      "Write the directive as `key=value`, such as `disable=SC2034`", None,
      Annoying, Policy),
    D(1126, "directive-after-command",
      "a directive belongs before the command it covers",
      "The directive follows a command on the same line, so nothing is "
      "suppressed",
      "Move the directive to its own line above the command", None, Annoying,
      Policy),
    D(1128, "shebang-not-first-line", "the shebang is not on the first line",
      "The shebang is not on the first line, so it is an ordinary comment",
      "Move the shebang to the first line and keep the comments below it", None,
      Strict, Policy),
    D(1135, "quote-break-for-dollar",
      "a literal dollar sign is written with a backslash",
      "The double-quoted string is closed right after `$` so the dollar sign "
      "stays literal, and the word continues after it",
      "Write `\\$` inside the quotes and keep the string in one piece", None,
      Annoying, Policy),
    D(1143, "continuation-in-comment",
      "a comment does not continue onto the next line",
      "The backslash closes a comment, so the command above it ends here",
      "Move the comment above the command, or wrap it in `` `# ...` ``", None,
      Annoying, Policy),
    D(2000, "echo-piped-into-wc",
      "the shell knows a string length without a pipeline",
      "The `echo` output is counted by `wc`, where the shell knows the length "
      "on its own",
      "Use `${#variable}` instead", None, Annoying, Policy),
    D(2001, "sed-plain-substitution", "the sed script is a plain substitution",
      "The `sed` script '{0}' replaces plain text, which the shell does "
      "without a fork",
      "Use `${variable//search/replace}` instead", None, Annoying, Policy),
    D(2002, "useless-cat",
      "a useless cat can pass its file to the next command", "A useless cat",
      "Give the file to the next command directly instead of piping cat", None,
      Annoying, Policy),
    D(2003, "expr-arithmetic",
      "expr forks for arithmetic the shell handles directly",
      "An expr forks for arithmetic the shell does natively",
      "Use $((...)) for the calculation", None, Strict, Policy),
    D(2004, "arithmetic-dollar",
      "arithmetic variables do not need a dollar sign",
      "Arithmetic variables do not need a dollar sign",
      "Use the variable name directly inside arithmetic", None, Annoying,
      Policy),
    D(2005, "useless-echo-substitution",
      "echo around a command substitution is redundant",
      "An echo of a command substitution prints what the command already "
      "prints",
      "Run the command on its own instead", None, Strict, Policy),
    D(2006, "backticks",
      "backticks are harder to nest than command substitutions",
      "Backticks are harder to nest than command substitutions",
      "Use $(...) for command substitution", None, Annoying, Policy),
    D(2007, "obsolete-arithmetic-expansion",
      "$[...] is the obsolete arithmetic expansion spelling",
      "$[...] is the obsolete arithmetic expansion spelling",
      "Use $((...)) for arithmetic expansion", None, Annoying, Policy),
    D(2008, "pipeline-into-echo", "a pipeline feeds echo, which ignores stdin",
      "The pipe feeds `echo`, which never reads stdin, so the upstream output "
      "is discarded",
      "Remove the pipe, or forward the input with `cat`", None, Lenient,
      Policy),
    D(2009, "grep-ps", "grepping ps output races the process table",
      "Grepping the ps output races the process table and matches the grep "
      "itself",
      "Use pgrep to match a process by name", None, Annoying, Policy),
    D(2010, "grep-ls", "grepping ls output mangles names",
      "Grepping the ls listing mangles a name with a space or a newline",
      "Match the names with a glob or with find instead", None, Lenient,
      Policy),
    D(2011, "xargs-ls", "piping ls into xargs mangles names",
      "The `ls` listing splits a name with a space or a newline before `xargs` "
      "reads it",
      "Use `find -print0` with `xargs -0`, or `find -exec`", None, Lenient,
      Policy),
    D(2012, "ls-output-as-data", "the ls listing is read as data",
      "The `ls` listing mangles a name with a space or a newline when it is "
      "read as data",
      "Use `find`, or match the names with a glob", None, Annoying, Policy),
    D(2013, "for-command-output", "for over command output iterates words",
      "A for over the cat output iterates IFS-split words rather than lines",
      "Read the lines with 'while IFS= read -r line' instead", None, Strict,
      Policy),
    D(2014, "find-exec-substitution",
      "a substitution in the find action expands once",
      "The substitution in the `find` action expands before `find` runs, not "
      "once for each file",
      "Move the substitution into `-exec sh -c` so it runs for each file", None,
      Lenient, Policy),
    D(2015, "and-or-else", "A && B || C also runs C when B fails",
      "A && B || C also runs C when B fails",
      "Use an if statement when C is the else branch", None, Annoying, Policy),
    D(2016, "single-quoted-expansion", "single quotes prevent expansion",
      "Single quotes prevent the expansion written inside them",
      "Use double quotes if the value should expand", None, Lenient, Policy),
    D(2017, "integer-division-first",
      "an integer division before a multiplication truncates",
      "The division truncates before the multiplication, so the result loses "
      "precision",
      "Write `a*c/b` to multiply first", None, Annoying, Policy),
    D(2018, "tr-lowercase-range", "the a-z range covers ASCII only",
      "The `a-z` range in `tr` leaves an accented letter untranslated",
      "Use `[:lower:]` instead", None, Annoying, Policy),
    D(2019, "tr-uppercase-range", "the A-Z range covers ASCII only",
      "The `A-Z` range in `tr` leaves an accented letter untranslated",
      "Use `[:upper:]` instead", None, Annoying, Policy),
    D(2020, "tr-word-set", "tr translates characters, not words",
      "The `tr` set '{0}' repeats a letter, and `tr` translates one character "
      "at a time",
      "Use `sed` to replace a word", None, Lenient, Policy),
    D(2021, "tr-bracket-range", "brackets around tr ranges add literal bytes",
      "Brackets around a tr range add literal bracket bytes",
      "Use a quoted range without brackets", None, Annoying, Policy),
    D(2022, "grep-repetition-pattern", "the grep pattern repeats one character",
      "In the regular expression '{0}' the star repeats the preceding "
      "character, where a glob would match any text",
      "Write `.*` for any text, or match the names with a glob", None, Lenient,
      Policy),
    D(2024, "sudo-glob", "sudo does not elevate shell glob expansion",
      "The shell expands this glob before sudo changes privileges",
      "Run the glob expansion inside a shell under sudo", None, Strict, Policy),
    D(2024, "sudo-redirection", "sudo does not elevate shell redirections",
      "The shell opens this redirection before sudo changes privileges",
      "Run a shell under sudo or pipe through sudo tee", None, Strict, Policy),
    D(2025, "prompt-display-guards", "PS1 control escapes need display guards",
      "PS1 control escapes need balanced display guards",
      "Wrap nonprinting prompt escapes in \\[ and \\]", None, Annoying, Policy),
    D(2028, "echo-escape-sequence", "echo prints an escape sequence literally",
      "The `echo` operand holds '{0}', which `echo` prints as written",
      "Use `printf` instead", None, Lenient, Policy),
    D(2029, "ssh-client-side-expansion",
      "the expansion happens on the client side",
      "The expansion in this `ssh` operand runs on the client, not on the "
      "remote host",
      "Quote the operand in single quotes so the remote shell expands it", None,
      Lenient, Policy),
    D(2030, "pipeline-assignment",
      "a pipeline assignment is lost when its stage exits",
      "This pipeline assignment is lost when the stage exits",
      "Move the assignment outside the pipeline", None, Strict, Policy),
    D(2030, "pipeline-read-assignment",
      "a pipeline read assignment is lost when its stage exits",
      "This pipeline read assignment is lost when the stage exits",
      "Feed the loop with a redirection or process substitution", None, Strict,
      Policy),
    D(2031, "pipeline-later-read",
      "a later read sees the value from before the pipeline",
      "This read sees the value from before the pipeline",
      "Move the assignment outside the pipeline or avoid the pipeline subshell",
      None, Strict, Policy),
    D(2032, "su-runs-no-function", "su cannot run a shell function",
      "The `su` command starts a new shell, where the function '{0}' is not "
      "defined",
      "Move the body into its own script, or pass it with `sh -c`", None,
      Strict, Policy),
    D(2033, "function-passed-to-command",
      "an external command cannot run a shell function",
      "The function '{0}' is handed to an external command, which never sees a "
      "shell function",
      "Wrap the call in `sh -c`, or move the body into its own script", None,
      Strict, Policy),
    D(2035, "option-shaped-glob",
      "a bare glob can expand to an option-shaped filename",
      "A bare glob can expand to a filename that begins with '-'",
      "Prefix the glob with a directory or place -- before it", None, Annoying,
      Policy),
    D(2036, "piped-assignment-stage",
      "an assignment feeding a pipe assigns nothing from the pipeline",
      "The assignment to '{0}' runs as the first stage, so the pipeline output "
      "is not assigned",
      "Write `{0}=$(...)` around the complete pipeline", None, Strict, Policy),
    D(2037, "assignment-ate-command",
      "an assignment prefix swallowed the command name",
      "The value of '{0}' is read as a name, and '{1}' becomes the command",
      "Write `{0}=$(...)` to assign the command output", None, Strict, Policy),
    D(2038, "find-xargs-names", "find piped to xargs breaks on special names",
      "An xargs splits the find output on whitespace and quotes",
      "Pair find -print0 with xargs -0 or use find -exec", None, Strict,
      Policy),
    D(2043, "for-over-constant-word", "the loop runs once over a constant word",
      "The word list holds the one constant word '{0}', so the body runs once",
      "Loop over a glob, a variable, or a command substitution", None, Lenient,
      Policy),
    D(2044, "for-find-output",
      "for over find output breaks names with whitespace",
      "A for over the find output breaks a name with whitespace apart",
      "Use find -exec or a 'while read -r' loop over find -print0", None,
      Strict, Policy),
    D(2045, "for-ls-output", "for over ls output breaks filenames",
      "A for loop over ls output breaks filenames at whitespace",
      "Iterate over a glob or read a delimited filename stream", None, Strict,
      Policy),
    D(2046, "unquoted-command-substitution",
      "an unquoted command substitution splits its output",
      "An unquoted command substitution splits its output",
      "Quote it to keep one argument", None, Strict, Policy),
    D(2048, "unquoted-star", "$* splits positional parameters",
      "$* splits positional parameters and loses their boundaries",
      "Use quoted \"$@\" to preserve each argument", None, Strict, Policy),
    D(2049, "regex-operator-takes-glob",
      "the regex operator was given a glob pattern",
      "The `=~` operator takes a regular expression, '{0}' reads as a glob",
      "Compare with `=` for a glob, or write the pattern as a regex", None,
      Strict, Policy),
    D(2050, "constant-comparison", "a conditional compares two constant values",
      "The '{0}' comparison has literal operands on both sides",
      "Remove the constant condition or compare runtime data", None, Annoying,
      Policy),
    D(2051, "variable-brace-range", "brace ranges expand before variables",
      "Brace ranges are expanded before variables",
      "Use an arithmetic loop for a variable limit", None, Lenient, Policy),
    D(2053, "unquoted-conditional-right-operand",
      "an unquoted right operand is matched as a glob",
      "The right side of `{0}` inside `[[ ]]` is matched as a glob, not "
      "compared",
      "Quote '{1}' so the comparison stays literal", None, Strict, Policy),
    D(2055, "or-between-inequalities",
      "an or between two inequalities is always true",
      "The '{0}' operand fails only one of the two `!=` tests, so the `||` is "
      "always true",
      "Join the two inequalities with `&&`", None, Strict, Policy),
    D(2056, "test-or-between-inequalities",
      "a test -o between two inequalities is always true",
      "The '{0}' operand fails only one of the two `!=` tests, so the `-o` is "
      "always true",
      "Join the two inequalities with `-a`", None, Strict, Policy),
    D(2057, "unknown-binary-test-operator",
      "the operand slot holds an unknown binary operator",
      "The '{0}' operator is not a known test operator",
      "Check the operator spelling or quote the operand", None, Strict, Policy),
    D(2058, "unknown-unary-test-operator",
      "the operator slot holds an unknown unary operator",
      "The '{0}' operator is not a known test operator",
      "Check the operator spelling or quote the operand", None, Strict, Policy),
    D(2059, "variable-printf-format",
      "a variable printf format can inject directives",
      "The printf format comes from a variable, the data can inject format "
      "directives",
      "Use printf '%s' to print it", None, Strict, Policy),
    D(2060, "unquoted-tr-range", "an unquoted tr range can expand as a glob",
      "The unquoted tr range can expand as a filename glob", "Quote the range",
      None, Strict, Policy),
    D(2061, "unquoted-find-pattern",
      "an unquoted find pattern expands before find sees it",
      "The unquoted find pattern expands before find sees it",
      "Quote the pattern so find performs the match", None, Strict, Policy),
    D(2062, "unquoted-grep-pattern",
      "an unquoted grep pattern can glob against local files",
      "The unquoted grep pattern can glob against the local files before grep "
      "sees it",
      "Quote the pattern", None, Strict, Policy),
    D(2063, "grep-glob-pattern", "a grep pattern looks like a glob",
      "A grep reads a regular expression, where a leading * has nothing to "
      "repeat, this pattern looks like a glob",
      None, None, Lenient, Policy),
    D(2064, "early-trap-expansion",
      "a double-quoted trap action expands when set",
      "The double-quoted trap action expands now, when the trap is set, not "
      "when it fires",
      "Single-quote it so it expands as the signal arrives", None, Lenient,
      Policy),
    D(2065, "test-redirection-comparison",
      "a redirection in a test is not a comparison",
      "The `{0}` is a file redirection, not a comparison",
      "Escape it as `\\>`, or use the `[[ ]]` form", None, Strict, Policy),
    D(2066, "quoted-for-glob", "a quoted for-loop glob stays literal",
      "A quoted for-loop glob remains one literal word",
      "Leave the glob unquoted so it expands into loop values", None, Lenient,
      Policy),
    D(2067, "find-exec-terminator", "find -exec needs a terminator",
      "The find -exec action has no terminating ';' or '+'",
      "Terminate the action with an escaped semicolon or plus", None, Strict,
      Policy),
    D(2068, "unquoted-at", "an unquoted $@ splits and globs each argument",
      "An unquoted $@ word-splits and globs each argument",
      "Quote it as \"$@\" to pass the arguments through unchanged", None,
      Strict, Policy),
    D(2069, "redirect-order",
      "2>&1 before a file redirect leaves stderr on the terminal",
      "2>&1 before the file redirect duplicates the terminal, so stderr stays "
      "on the terminal",
      "Put the file redirect first as in '>file 2>&1'", None, Strict, Policy),
    D(2070, "unquoted-nonempty-test",
      "an unquoted operand breaks the nonempty test",
      "The `-n` operand is unquoted, so an empty value leaves `-n` with no "
      "operand",
      "Quote the operand, or use the `[[ ]]` form", None, Strict, Policy),
    D(2071, "string-numeric-operator",
      "a string operator compares lexicographically",
      "The {0} operator compares strings lexicographically",
      "Use an arithmetic command or a numeric -lt or -gt operator", None,
      Lenient, Policy),
    D(2072, "decimal-numeric-comparison",
      "a numeric operator compares integers only",
      "The '{0}' operator compares integers, '{1}' carries a fraction",
      "Compare the decimals with `bc` or `awk`", None, Strict, Policy),
    D(2073, "test-input-redirection", "a less-than in a test redirects input",
      "The `<` reads a file into the test, it does not compare",
      "Escape it as `\\<`, or use the `[[ ]]` form", None, Strict, Policy),
    D(2074, "test-regex-operator",
      "the test builtin does not support regex matching",
      "The test builtin does not support the =~ regular expression operator",
      "Use [[ value =~ expression ]]", None, Strict, Policy),
    D(2076, "quoted-regex", "a quoted regular expression is matched literally",
      "A quoted regular expression is matched literally",
      "Store the expression in a variable or leave it unquoted", None, Lenient,
      Policy),
    D(2077, "conditional-operator-spacing",
      "a conditional operator needs surrounding spaces",
      "A conditional operator needs surrounding spaces",
      "Place spaces around the comparison operator", None, Strict, Policy),
    D(2078, "constant-test-operand",
      "a bare literal operand makes the test constant",
      "The '{0}' operand is a literal, so the test never changes",
      "Add the `$` when a variable was meant", None, Strict, Policy),
    D(2079, "arithmetic-decimal", "shell arithmetic works on integers only",
      "The arithmetic evaluates integers, so the decimal '{0}' is a syntax "
      "error",
      "Compute the value with `bc` or `awk`", None, Strict, Policy),
    D(2080, "octal-literal", "a leading zero selects octal",
      "The literal '{0}' begins with a zero, so it is read in base eight",
      "Drop the leading zero, or write `10#{0}` for base ten", None, Lenient,
      Policy),
    D(2081, "test-glob-match", "test cannot glob-match strings",
      "[ and test compare strings byte for byte and never glob-match",
      "Use a case or the [[ ]] form for the pattern", None, Lenient, Policy),
    D(2084, "arithmetic-expansion-as-command",
      "the arithmetic result runs as a command",
      "The expansion produces a number and the shell then runs it as a command",
      "Drop the `$` for `(( ))`, or write `_=$(( ))` to keep the value", None,
      Strict, Policy),
    D(2086, "unquoted-expansion", "an unquoted variable can split and glob",
      "An unquoted variable can split into words and expand globs",
      "Quote the expansion to keep one argument", None, Strict, Policy),
    D(2086, "unquoted-test-expansion",
      "an unquoted test variable can split or vanish",
      "A test reads an unquoted variable",
      "Quote it to avoid an empty or split argument", None, Strict, Policy),
    D(2087, "unquoted-heredoc-delimiter",
      "an unquoted here document delimiter expands on this host",
      "The here document body is expanded here before `ssh` sends it",
      "Quote the delimiter to send the body unexpanded", None, Lenient, Policy),
    D(2088, "quoted-tilde", "a quoted tilde remains literal",
      "A quoted tilde stays literal instead of expanding to the home directory",
      "Leave the tilde unquoted or use a quoted $HOME expansion", None, Lenient,
      Policy),
    D(2089, "quoted-value-assignment", "quotes stored in a value stay literal",
      "The value of '{0}' carries quote bytes, and the expansion keeps them as "
      "text",
      "Use an array, or set the positional parameters with `set --`", None,
      Lenient, Policy),
    D(2090, "quoted-value-expansion",
      "an expanded value keeps its quotes literal",
      "The expansion of '{0}' keeps its quote bytes, so the shell does not "
      "regroup the words",
      "Use an array, or set the positional parameters with `set --`",
      "'{0}' is assigned here", Lenient, Policy),
    D(2091, "substitution-command-position",
      "a command substitution in command position executes its output",
      "A command substitution in command position executes its output",
      "Run the command inside the substitution directly", None, Lenient,
      Policy),
    D(2093, "commands-after-exec",
      "commands after exec do not run when exec succeeds",
      "Commands after exec do not run when exec succeeds",
      "Remove exec or remove the unreachable commands",
      "this is the first command skipped after exec", Lenient, Policy),
    D(2094, "read-write-same-file",
      "reading and writing the same file truncates the input",
      "The command reads and truncates '{0}' at once, the truncation empties "
      "the input before it is read",
      "Write to a temporary and move it over",
      "this redirect reads the file that is later truncated", Strict, Policy),
    D(2095, "while-read-stdin-consumer",
      "a command in a while-read loop can consume loop input",
      "An ssh command in a while-read loop can consume the loop input",
      "Redirect ssh input from /dev/null or pass -n", None, Annoying, Policy),
    D(2096, "shebang-parameter-count", "a shebang takes at most one parameter",
      "The shebang passes more than one parameter, most systems hand the rest "
      "over as one argument",
      "Keep one parameter and move the options into the script", None, Lenient,
      Policy),
    D(2099, "arithmetic-assignment-expansion",
      "an assignment concatenates instead of adding",
      "The value of '{0}' is joined as text, and the arithmetic is not "
      "evaluated",
      "Write `{0}=$(( ... ))` to evaluate the arithmetic", None, Strict,
      Policy),
    D(2100, "arithmetic-assignment-literal",
      "an assignment stores the expression instead of its value",
      "The value of '{0}' is stored as text, and the arithmetic is not "
      "evaluated",
      "Write `{0}=$(( ... ))` to evaluate the arithmetic", None, Strict,
      Policy),
    D(2103, "directory-change-and-back", "a subshell avoids the return trip",
      "A later `cd -` undoes this directory change, and a subshell restores "
      "the directory on its own",
      "Run the commands inside `( )` instead",
      "the working directory is restored here", Annoying, Policy),
    D(2104, "break-outside-loop-in-function",
      "a function leaves through return",
      "The `{0}` command stands in a "
      "function outside any loop, where there is nothing to leave",
      "Use `return` instead", None, Strict, Policy),
    D(2105, "break-outside-loop", "break is valid only inside a loop",
      "The `{0}` command stands outside any loop, where there is nothing to "
      "leave",
      "Remove it, or move it into the loop it belongs to", None, Strict,
      Policy),
    D(2106, "loop-control-in-pipeline",
      "a loop control inside a pipeline leaves only its own subshell",
      "Each pipeline stage runs in its own subshell, so the `{0}` leaves that "
      "subshell and the loop keeps running",
      "Write `||` in place of the pipe", None, Strict, Policy),
    D(2107, "and-inside-single-bracket",
      "a double ampersand does not join one test",
      "A `[ ]` test does not take `&&`, the bracket closes before it",
      "Write `[ a ] && [ b ]`", None, Strict, Policy),
    D(2108, "conditional-dash-a", "double brackets take a double ampersand",
      "A `[[ ]]` expression joins its parts with `&&`",
      "Write `&&` in place of `-a`", None, Strict, Policy),
    D(2109, "or-inside-single-bracket", "a double pipe does not join one test",
      "A `[ ]` test does not take `||`, the bracket closes before it",
      "Write `[ a ] || [ b ]`", None, Strict, Policy),
    D(2110, "conditional-dash-o", "double brackets take a double pipe",
      "A `[[ ]]` expression joins its parts with `||`",
      "Write `||` in place of `-o`", None, Strict, Policy),
    D(2114, "recursive-rm-system-directory", "rm -r targets a system directory",
      "A rm -r targets the system directory '{0}'",
      "double-check the path before running this", None, Strict, Policy),
    D(2115, "recursive-rm-empty-variable",
      "rm -r can target root when a variable is empty",
      "A rm -r on \"${0}/\" deletes '/' when the variable is empty",
      "write ${{0}:?} so an empty value aborts the command instead", None,
      Strict, Policy),
    D(2116, "useless-echo-in-substitution",
      "an echo in a command substitution is redundant",
      "A command substitution wraps a useless echo",
      "The text can be used directly without the subshell", None, Annoying,
      Policy),
    D(2117, "su-without-command-flag", "su takes its command behind -c",
      "The operand '{0}' is handed to `su` as a command without `-c`, which "
      "not "
      "every `su` accepts",
      "Write `su -c` instead, or use `sudo`", None, Lenient, Policy),
    D(2119, "function-call-without-arguments",
      "a call passes no arguments to a function that reads them",
      "The function '{0}' reads the arguments it is called with, and this call "
      "passes none",
      "Pass the values '{0}' expects, or write `\"$@\"` to forward the script "
      "arguments",
      "the body reads its arguments here", Strict, Policy),
    D(2120, "function-arguments-never-passed",
      "a function reads arguments that no call passes",
      "The function '{0}' reads the arguments it is called with, and no call "
      "passes any",
      "Pass the arguments at the call, or read a named variable in place of "
      "`$1`",
      None, Strict, Policy),
    D(2121, "set-as-assignment", "set does not assign a variable",
      "The `set` builtin changes the shell options and the positional "
      "parameters, so '{0}' is not assigned",
      "Write `{0}=value` on its own", None, Strict, Policy),
    D(2122, "invalid-comparison-operator",
      "the comparison operator does not exist in test",
      "The '{0}' operator does not exist in test",
      "Negate the opposite comparison, as in `! a < b`", None, Strict, Policy),
    D(2123, "search-path-overwritten",
      "an assignment replaces the command search path",
      "The PATH variable holds the command search path, and this assignment "
      "replaces it with '{0}'",
      "Name the variable something else, or keep `$PATH` in the value", None,
      Lenient, Policy),
    D(2124, "scalar-at-assignment",
      "a scalar assignment from $@ loses boundaries",
      "A scalar assignment from $@ loses argument boundaries",
      "Assign one value or use an array", None, Lenient, Policy),
    D(2125, "literal-glob-assignment",
      "a glob in an assignment value stays literal",
      "The value of '{0}' keeps the pattern as text, because an assignment "
      "does not expand it",
      "Quote the value when the pattern is data, or use an array", None,
      Lenient, Policy),
    D(2126, "grep-wc-count", "wc -l on grep output runs an extra process",
      "Counting grep output with wc -l runs an extra process",
      "Use grep -c to count the matching lines directly", None, Annoying,
      Policy),
    D(2128, "array-without-subscript",
      "an array without a subscript expands to one element",
      "The name '{0}' holds an array, and an expansion without a subscript "
      "reads the first element alone",
      "Write `${{0}[@]}` to expand every element", None, Strict, Policy),
    D(2129, "repeated-append", "several appends can share one redirection",
      "Several commands append to the same file separately",
      "Apply one append redirection to a grouped command",
      "this later append belongs under the same redirection", Annoying, Policy),
    D(2130, "integer-comparison-on-text",
      "the `-eq` operator compares integers only",
      "The `-eq` operator compares integers, '{0}' is not an integer",
      "Use `=` to compare the text", None, Strict, Policy),
    D(2140, "quote-sandwich", "a bare word sits between two quoted runs",
      "The word closes a quote, leaves '{0}' bare, and opens a quote again",
      "Quote the complete word, or escape the inner quotes", None, Lenient,
      Policy),
    D(2141, "literal-escape-separator",
      "an escape written as text stays two bytes",
      "The value of '{0}' carries a backslash and a letter, not the control "
      "byte",
      "Write `$'\\t'` to place the control byte", None, Lenient, Policy),
    D(2142, "alias-positional-arguments",
      "an alias body cannot receive positional arguments",
      "An alias body cannot receive positional arguments",
      "Use a function when the wrapper needs arguments", None, Annoying,
      Policy),
    D(2143, "tested-grep-output", "the grep output is tested for a match",
      "The complete `grep` output is collected to learn whether one line "
      "matched",
      "Use `grep -q` and test its exit status", None, Lenient, Policy),
    D(2144, "glob-file-test", "a file test on a glob checks one expanded path",
      "A file test on a glob checks only one expanded path",
      "Expand the glob in a loop and test each path", None, Strict, Policy),
    D(2145, "at-inside-word", "$@ inside a longer word concatenates text",
      "$@ inside a longer word concatenates the surrounding text onto the "
      "first and last argument",
      "Use $* for one joined string or a separate \"$@\" word", None, Strict,
      Policy),
    D(2146, "find-or-grouping", "find actions after -o need grouping",
      "The find expression uses -o without grouping its actions",
      "Group each side with escaped parentheses", None, Lenient, Policy),
    D(2147, "literal-tilde-path", "a tilde inside PATH remains literal",
      "A tilde inside PATH remains literal",
      "Expand HOME before assigning PATH", None, Lenient, Policy),
    D(2148, "missing-shebang", "the script names no interpreter",
      "The script names no interpreter, so the analysis cannot tell which "
      "shell it targets",
      "Add a shebang such as `#!/bin/sh` on the first line", None, Annoying,
      Policy),
    D(2151, "return-extra-operands", "return takes one status",
      "The `return` builtin takes one number from 0 to 255, so the operands "
      "after the first are ignored",
      "Write the other data to standard output", None, Strict, Policy),
    D(2152, "return-substitution-value", "return takes a status, not output",
      "The `return` operand is command output, and a status is one number from "
      "0 to 255",
      "Print the value and return a status separately", None, Strict, Policy),
    D(2153, "misspelled-variable-name",
      "a read name resembles an assigned name",
      "The variable '{0}' is never assigned, and '{1}' is",
      "Correct the spelling or assign '{0}'",
      "the assignment that gives '{1}' a value runs here", Strict, Policy),
    D(2154, "unassigned-variable-read",
      "a name is read that the script never assigns",
      "The variable '{0}' is read before any assignment gives it a value",
      "Assign '{0}' before it is read, or export it from the environment", None,
      Strict, Policy),
    D(2155, "declare-substitution-status",
      "declare and assignment can mask command status",
      "Declaring and assigning from a command substitution in one command "
      "masks the command's exit status",
      "Split the declaration and the assignment so a failure is seen", None,
      Lenient, Policy),
    D(2156, "find-exec-source-injection",
      "find -exec shell text must receive filenames as arguments",
      "The find result is inserted into shell source and can execute filename "
      "text",
      "Pass the result as an argument and reference it as $1", None, Strict,
      Policy),
    D(2157, "literal-string-test",
      "a literal string test operand makes the test constant",
      "The operand is literal, so this string test is constant",
      "Test a variable or drop the check", None, Annoying, Policy),
    D(2157, "literal-test", "a literal test operand makes the test constant",
      "The operand is a literal, so this {0} test is constant",
      "Test a variable or drop the check", None, Annoying, Policy),
    D(2158, "bracketed-false",
      "the brackets around `false` invert what the word means",
      "`[ false ]` tests the nonempty word `false`, so it succeeds",
      "Drop the brackets and write `false`", None, Strict, Policy),
    D(2159, "bracketed-zero",
      "the brackets around `0` invert what the digit means",
      "`[ 0 ]` tests the nonempty word `0`, so it succeeds",
      "Write `false` for a failing condition", None, Strict, Policy),
    D(2160, "bracketed-true", "the brackets around `true` add nothing",
      "`[ true ]` tests the nonempty word `true`, the brackets add nothing",
      "Drop the brackets and write `true`", None, Annoying, Policy),
    D(2161, "bracketed-one", "the brackets around `1` add nothing",
      "`[ 1 ]` tests the nonempty word `1`, the brackets add nothing",
      "Write `true` for a succeeding condition", None, Annoying, Policy),
    D(2162, "read-without-r", "read without -r mangles backslashes",
      "A read without -r mangles a backslash in the input",
      "Add -r to read the line literally", None, Lenient, Policy),
    D(2163, "export-expanded-name", "export takes a name, not a value",
      "The `export` operand expands '{0}', so the exported name is the value "
      "of "
      "'{0}'",
      "Drop the dollar sign to export '{0}' itself", None, Strict, Policy),
    D(2164, "unchecked-cd",
      "an unchecked cd can leave commands in the wrong directory",
      "This cd is unchecked, so later commands can run in the wrong directory",
      "Stop or return when cd fails", None, Annoying, Policy),
    D(2165, "nested-loop-reuse", "a nested loop reuses the outer variable",
      "The nested inner loop reuses the outer loop variable '{0}'",
      "Use a distinct variable for the nested loop",
      "the outer loop first binds '{0}' here", Annoying, Policy),
    D(2166, "obsolescent-test-and-or", "test with -a or -o is obsolescent",
      "A test with -a or -o is obsolescent",
      "Join two tests with && or || instead", None, Annoying, Policy),
    D(2167, "nested-loop-overwrite",
      "a nested loop overwrites the outer variable",
      "The outer loop variable '{0}' is overwritten by the nested inner loop",
      "Use a distinct variable for the nested loop",
      "the nested inner loop binds '{0}' again here", Annoying, Policy),
    D(2168, "local-outside-function", "local outside a function has no scope",
      "A local outside a function has no scope to bind",
      "Declare the variable plainly or move it into a function", None, Strict,
      Policy),
    D(2170, "non-numeric-test-operand",
      "a numeric test operator has a non-numeric literal",
      "The numeric comparison {0} reads '{1}', which is not a number, so the "
      "test errors at run time",
      None, None, Strict, Policy),
    D(2171, "unmatched-closing-bracket",
      "a trailing bracket sits outside a test",
      "The trailing `]` has no opening `[`",
      "Add the opening `[`, or quote the bracket when it is data", None,
      Lenient, Policy),
    D(2172, "trap-signal-number", "a trapped signal is named by its number",
      "The trap condition '{0}' is a signal number, and the numbering is not "
      "the same on every system",
      "Name the signal, such as `INT` or `TERM`", None, Lenient, Policy),
    D(2173, "trap-unblockable-signal",
      "the KILL and STOP signals cannot be trapped",
      "The signal '{0}' cannot be trapped, so this handler never runs",
      "Remove the condition, since the kernel delivers this signal "
      "unconditionally",
      None, Strict, Policy),
    D(2174, "mkdir-parent-mode",
      "mkdir -pm applies mode only to the deepest directory",
      "A mkdir -pm applies the mode only to the deepest directory, the created "
      "parents keep the umask default",
      None, None, Lenient, Policy),
    D(2176, "posix-timed-pipeline",
      "timing a pipeline is undefined in POSIX sh",
      "The `time` keyword times only a simple command in POSIX sh",
      "Time one stage, or run the pipeline through `bash -c` and time that",
      None, Strict, Policy),
    D(2177, "posix-timed-compound",
      "timing a compound command is undefined in POSIX sh",
      "The `time` keyword times only a simple command in POSIX sh",
      "Run the compound command through `sh -c` and time that", None, Strict,
      Policy),
    D(2178, "array-assigned-string", "an array name is reassigned as a scalar",
      "The name '{0}' holds an array, and this assignment replaces it with one "
      "string in the first element",
      "Write `{0}=( ... )` to keep the array", None, Strict, Policy),
    D(2179, "array-appended-string",
      "a scalar append on an array name adds nothing",
      "The name '{0}' holds an array, and a scalar append changes the first "
      "element",
      "Write `{0}+=( ... )` to append an element", None, Strict, Policy),
    D(2180, "multidimensional-array", "a shell array has one dimension",
      "The name '{0}' carries several subscripts, and the shell has no "
      "multidimensional array",
      "Flatten the index, or use an associative array", None, Strict, Policy),
    D(2181, "indirect-exit-status-test",
      "test the command instead of testing $?",
      "Testing $? checks the exit status indirectly",
      "Test the command directly with if or && so an intervening command "
      "cannot clobber the status",
      None, Annoying, Policy),
    D(2182, "printf-ignored-arguments",
      "the printf format consumes no arguments",
      "The printf format consumes no argument, so the remaining operands are "
      "ignored",
      "Add a format directive, or remove the extra operands", None, Lenient,
      Policy),
    D(2183, "printf-missing-arguments",
      "printf has fewer arguments than its format consumes",
      "The printf format consumes more arguments than the command supplies",
      "Add the missing arguments or remove format directives", None, Lenient,
      Policy),
    D(2184, "unquoted-unset-index",
      "an unquoted unset array index can expand as a glob",
      "An unquoted unset array index can expand as a filename glob",
      "Quote the complete array element name", None, Strict, Policy),
    D(2185, "find-without-path", "find is given no path to search",
      "Some `find` implementations have no default path, so this search walks "
      "nothing",
      "Write `.` as the path before the expression", None, Lenient, Policy),
    D(2188, "redirection-without-command", "a redirection has no command",
      "The redirection is attached to no command, so no data is read or "
      "written",
      "Attach the redirection to its command, or write `: > file` for the "
      "truncation",
      None, Lenient, Policy),
    D(2189, "pipeline-stage-without-command", "a pipeline stage has no command",
      "The pipeline stage holds a redirection and no command, so it moves no "
      "data",
      "Attach the redirection to the command that reads it", None, Strict,
      Policy),
    D(2193, "impossible-comparison", "the compared literals can never be equal",
      "The literals '{0}' and '{1}' differ, so the comparison never succeeds",
      "Compare a variable, or correct the operand spelling", None, Strict,
      Policy),
    D(2194, "constant-case-word", "the case word is constant",
      "The case word '{0}' is constant, so the same branch is chosen every "
      "time",
      "Add the missing `$`, or drop the case", None, Lenient, Policy),
    D(2195, "unmatchable-case-pattern",
      "the pattern never matches the case word",
      "The pattern '{0}' cannot match the case word '{1}', so its branch is "
      "dead",
      "Correct the pattern or the case word", None, Strict, Policy),
    D(2196, "deprecated-egrep", "egrep is deprecated",
      "The egrep command is deprecated",
      "Use grep -E for the extended regular expression match", None, Annoying,
      Policy),
    D(2197, "deprecated-fgrep", "fgrep is deprecated",
      "The fgrep command is deprecated",
      "Use grep -F for the fixed string match", None, Annoying, Policy),
    D(2198, "array-operand-in-test", "an array does not fit one test operand",
      "The `[ ]` test takes one word per operand, '{0}' expands to every "
      "element",
      "Loop over the elements, or join them with `*`", None, Strict, Policy),
    D(2199, "array-operand-in-conditional",
      "an array concatenates inside double brackets",
      "A `[[ ]]` expression concatenates '{0}' into one word",
      "Loop over the elements, or write `*` in place of `@`", None, Strict,
      Policy),
    D(2200, "brace-expansion-in-test",
      "a brace expansion does not fit one test operand",
      "The `[ ]` test takes one word per operand, '{0}' expands to several",
      "Loop over the expanded words", None, Strict, Policy),
    D(2201, "brace-expansion-in-conditional",
      "a brace expansion stays literal inside double brackets",
      "A `[[ ]]` expression leaves '{0}' unexpanded",
      "Loop over the expanded words", None, Strict, Policy),
    D(2202, "glob-operand-in-test", "a glob does not fit one test operand",
      "The `[ ]` test takes one word per operand, '{0}' expands to every "
      "match",
      "Loop over the matches", None, Strict, Policy),
    D(2203, "glob-operand-in-conditional",
      "a glob stays literal inside double brackets",
      "A `[[ ]]` expression matches a glob only to the right of `=` or `!=`, "
      "so '{0}' stays literal",
      "Loop over the matches", None, Strict, Policy),
    D(2204, "parentheses-subshell",
      "parentheses start a subshell instead of a test",
      "Parentheses start a subshell rather than a file or string test",
      "Use [[ ... ]] or [ ... ] for the condition", None, Strict, Policy),
    D(2205, "subshell-as-condition", "a subshell is not a test expression",
      "A `( )` subshell runs its contents, it does not test them",
      "Write the condition as `[ ... ]`", None, Strict, Policy),
    D(2206, "array-element-splitting",
      "an unquoted element splits into several array entries",
      "The expansion in this array element splits on whitespace and expands "
      "globs",
      "Quote the element, or split the value with `mapfile` or `read -a`", None,
      Lenient, Policy),
    D(2207, "array-from-command-output",
      "an array from command output splits and globs",
      "An array built from command output splits words and expands globs",
      "Use mapfile or readarray to preserve output records", None, Strict,
      Policy),
    D(2208, "unquoted-set-variable-operand", "the -v operand expands as a glob",
      "The `-v` operand '{0}' carries glob characters, so the shell expands it "
      "before the test runs",
      "Quote the name, or use the `[[ ]]` form", None, Strict, Policy),
    D(2209, "command-name-as-value",
      "an assigned command name stores the name, it does not run",
      "The value of '{0}' is the command name '{1}', and the command never "
      "runs",
      "Write `{0}=$({1})` to store its output, or quote the name", None,
      Lenient, Policy),
    D(2210, "numeric-redirection-target",
      "a redirection target of digits is not a descriptor",
      "The redirect writes to the file named '{0}', it does not name a "
      "descriptor",
      "Write `>&{0}` to reach the descriptor, or compare the numbers in a test",
      None, Lenient, Policy),
    D(2212, "empty-test", "an empty test always fails",
      "A test with no operand always fails",
      "Write `false` for a failing condition", None, Strict, Policy),
    D(2213, "getopts-option-unhandled",
      "a declared option reaches no case branch",
      "The optstring declares '{0}', which no branch of this case handles",
      "Add a `{0})` branch, or drop the letter from the optstring",
      "this optstring declares the option", Lenient, Policy),
    D(2214, "case-branch-outside-optstring",
      "a case branch names an option the optstring omits",
      "The optstring does not declare '{0}', so getopts never stores that "
      "letter",
      "Add the letter to the optstring, or remove the branch",
      "this optstring omits the option", Lenient, Policy),
    D(2215, "option-command-position", "an option-shaped word is not a command",
      "An option-shaped word in command position is not a command",
      "Place the option after its command", None, Strict, Policy),
    D(2216, "pipeline-non-reader",
      "a pipeline feeds a command that ignores stdin",
      "The pipe feeds '{0}', which never reads stdin, so the upstream output "
      "is discarded",
      None, None, Lenient, Policy),
    D(2217, "redirect-non-reader",
      "an input redirect feeds a command that ignores stdin",
      "The input redirect feeds '{0}', which never reads stdin, so the data is "
      "discarded",
      None, None, Annoying, Policy),
    D(2218, "function-called-before-definition",
      "a call runs before the function is defined",
      "The function '{0}' is called here, and the definition that gives it a "
      "body runs later in the file",
      "Move the definition of '{0}' above this call",
      "the definition runs after the call", Strict, Policy),
    D(2219, "let-arithmetic-command",
      "an arithmetic command states the intent that let hides",
      "The `let` builtin evaluates its argument as arithmetic",
      "Write `(( ... ))`, which needs no quoting and reads as arithmetic", None,
      Annoying, Policy),
    D(2220, "getopts-invalid-option-unhandled",
      "an invalid option reaches no case branch",
      "getopts stores a question mark for an unknown option, which no branch "
      "of this case handles",
      "Add a `*)` branch that prints the usage and exits",
      "this loop reads the options", Annoying, Policy),
    D(2221, "duplicate-case-pattern",
      "an earlier case pattern makes this pattern unreachable",
      "An earlier case pattern makes this pattern unreachable",
      "Remove the duplicate pattern", "this identical pattern matches first",
      Strict, Policy),
    D(2222, "shadowed-case-pattern",
      "an earlier case pattern shadows this pattern",
      "An earlier case pattern shadows this pattern",
      "Move the specific pattern before the broader pattern",
      "this broader pattern shadows the later pattern", Annoying, Policy),
    D(2223, "unquoted-default-assignment",
      "an unquoted default assignment is split and globbed",
      "The default value given to '{0}' is split into words and matched "
      "against "
      "the filenames",
      "Quote the expansion to keep the default intact", None, Annoying, Policy),
    D(2224, "move-without-destination", "the move has no destination",
      "The `mv` command is given '{0}' alone, so no destination is named",
      "Add the destination path", None, Strict, Policy),
    D(2225, "copy-without-destination", "the copy has no destination",
      "The `cp` command is given '{0}' alone, so no destination is named",
      "Add the destination path", None, Strict, Policy),
    D(2226, "link-without-destination", "the link has no destination",
      "The `ln` command is given '{0}' alone, so no link name is named",
      "Add the link name, or write `.` explicitly", None, Strict, Policy),
    D(2229, "read-variable-dollar",
      "read expects a variable name without a dollar sign",
      "A read operand is a variable name, not a variable value",
      "Drop the dollar sign from the variable name", None, Strict, Policy),
    D(2230, "which-is-nonstandard", "which is not a shell builtin",
      "The `which` program is not standard and is missing on some systems",
      "Use `command -v` instead", None, Annoying, Policy),
    D(2231, "unquoted-expansion-in-glob",
      "an unquoted expansion beside a glob splits on whitespace",
      "The expansion in '{0}' is unquoted, so the path splits on whitespace "
      "before the glob runs",
      "Quote the expansion and leave the glob characters outside the quotes",
      None, Annoying, Policy),
    D(2232, "sudo-runs-no-builtin", "sudo cannot run a shell builtin",
      "The `sudo` command starts an external program, so the builtin '{0}' is "
      "not found",
      "Wrap the builtin in `sudo sh -c` instead", None, Strict, Policy),
    D(2233, "redundant-condition-parentheses",
      "the parentheses around the condition add nothing",
      "The `( )` around the condition starts a subshell and changes nothing "
      "else",
      "Remove the parentheses", None, Annoying, Policy),
    D(2234, "redundant-test-parentheses",
      "the parentheses around the test add nothing",
      "The `( )` around the test starts a subshell and changes nothing else",
      "Remove the parentheses", None, Annoying, Policy),
    D(2235, "subshell-grouping", "a subshell groups what braces group for free",
      "The `( )` group starts a subshell, which costs a process on every run",
      "Write `{ ...; }` to group the commands in the current shell", None,
      Annoying, Policy),
    D(2236, "negated-z", "a negated -z is -n", "A negated -z is just -n",
      "Test with -n instead", None, Annoying, Policy),
    D(2237, "negated-n", "a negated -n is -z", "A negated -n is just -z",
      "Test with -z instead", None, Annoying, Policy),
    D(2238, "redirection-to-command-name",
      "a redirection target names a command",
      "The redirect opens a file named '{0}', and the command never runs",
      "Feed `{0}` through a pipe, or quote the name to write the file", None,
      Strict, Policy),
    D(2239, "shebang-relative-interpreter", "the shebang names a relative path",
      "The shebang names '{0}', which is not an absolute path",
      "Write an absolute path, or use `#!/usr/bin/env` to search PATH", None,
      Strict, Policy),
    D(2240, "posix-dot-arguments", "the POSIX dot command takes no argument",
      "The POSIX `.` command reads a file and takes no argument, so '{0}' "
      "never "
      "reaches it",
      "Set the values as variables before the file is read", None, Strict,
      Policy),
    D(2241, "exit-extra-operands", "exit takes one status",
      "The `exit` builtin takes one number from 0 to 255, so the operands "
      "after "
      "the first are ignored",
      "Write the other data to standard output", None, Strict, Policy),
    D(2242, "invalid-status-code",
      "an exit or return code is outside 0 through 255",
      "The code '{0}' is not a number from 0 to 255, {1} either rejects it or "
      "wraps it modulo 256",
      None, None, Lenient, Policy),
    D(2243, "one-operand-output-test",
      "a one-operand test on output checks for printed bytes",
      "A one-operand test on command output only checks that something was "
      "printed",
      "Write it with `-n`, or drop the brackets to check the exit status", None,
      Annoying, Policy),
    D(2244, "one-operand-test", "a one-operand test is a nonempty-string test",
      "A one-operand test is the nonempty-string test",
      "Write it with -n to read clearer", None, Annoying, Policy),
    D(2245, "file-test-on-glob-in-test",
      "a file test reaches only the first expanded path",
      "The `{0}` test runs against the first path '{1}' expands to",
      "Loop over the matches to test each one", None, Lenient, Policy),
    D(2246, "shebang-directory-interpreter", "the shebang names a directory",
      "The shebang names '{0}', which ends in a slash and names a directory",
      "Remove the trailing slash so the shebang names the interpreter file",
      None, Strict, Policy),
    D(2249, "case-without-default", "a case without a default can miss input",
      "This case has no default *) branch, a value no pattern matches is "
      "silently ignored",
      None, None, Annoying, Policy),
    D(2251, "negation-skips-errexit",
      "a negated command is not a condition and skips errexit",
      "The `!` inhibits errexit whether the command succeeds or fails, and "
      "nothing reads the status",
      "Write `|| exit 1` after it, or check `$?`", None, Lenient, Policy),
    D(2252, "or-between-bracket-inequalities",
      "an or between two bracketed inequalities is always true",
      "The '{0}' operand fails only one of the two `!=` tests, so the `||` is "
      "always true",
      "Join the two tests with `&&`", None, Strict, Policy),
    D(2254, "unquoted-case-pattern",
      "an unquoted expansion in a case pattern matches as a glob",
      "The expansion in the pattern '{0}' is matched as a glob, not compared "
      "literally",
      "Quote the expansion so the pattern matches literally", None, Lenient,
      Policy),
    D(2255, "test-arithmetic-operand",
      "test brackets do not evaluate arithmetic",
      "Test brackets compare '{0}' as text, the arithmetic is not evaluated",
      "Evaluate the numbers with `$((...))` before the comparison", None,
      Lenient, Policy),
    D(2257, "redirection-mutation", "redirection expansion can lose mutation",
      "A redirection expansion can run in a child and lose its mutation",
      "Update the variable before forming the redirect path", None, Strict,
      Policy),
    D(2259, "redirection-overrides-input-pipe",
      "a redirection overrides the piped input",
      "The input redirection takes precedence, so the pipe into '{0}' is "
      "closed",
      "Pass the file as an operand, or merge the inputs into one stream", None,
      Strict, Policy),
    D(2260, "redirection-overrides-output-pipe",
      "a redirection overrides the piped output",
      "The output redirection takes precedence, so the pipe out of '{0}' is "
      "closed",
      "Use `tee` to write the file and feed the pipe", None, Strict, Policy),
    D(2261, "competing-redirections",
      "several redirections compete for one descriptor",
      "The redirect to '{0}' competes with an earlier redirect for the same "
      "descriptor",
      "Pass the files as operands, or use `tee` for several outputs",
      "this earlier redirect claims the same descriptor", Strict, Policy),
    D(2264, "recursive-wrapper", "a function wrapper calls itself recursively",
      "The '{0}' function calls itself recursively",
      "Use `command` before the wrapped command name", "'{0}' is defined here",
      Strict, Policy),
    D(2267, "deprecated-xargs-replace", "the xargs -i flag is deprecated",
      "The `xargs -i` flag is deprecated",
      "Use `-I` with an explicit replacement string instead", None, Annoying,
      Policy),
    D(2268, "x-prefix-test", "the x-prefix test workaround is obsolete",
      "The x-prefix test workaround is obsolete", "Quote the variable directly",
      None, Annoying, Policy),
    D(2269, "self-assignment", "a variable is assigned its own value",
      "The value assigned to '{0}' is the value '{0}' already holds",
      "Remove the assignment, or name the variable the value comes from", None,
      Lenient, Policy),
    D(2270, "positional-assignment-name",
      "a positional parameter cannot be assigned",
      "A positional parameter cannot be assigned, so '{0}' runs as a command",
      "Assign to a named variable, or compare with `[ x = y ]`", None, Strict,
      Policy),
    D(2271, "expanded-assignment-name",
      "an assignment name cannot come from an expansion",
      "An assignment name cannot come from an expansion, so '{0}' runs as a "
      "command",
      "Use an array, or assign through `read`", None, Strict, Policy),
    D(2272, "equals-in-command-name", "a command name carries a double equals",
      "The word '{0}' holds `==` and runs as a command name",
      "Write `[ x = y ]` to compare the values", None, Strict, Policy),
    D(2273, "triple-equals-run", "a run of equals signs runs as a command",
      "The word '{0}' is a run of equals signs and runs as a command name",
      "Remove the separator line, or comment it out", None, Strict, Policy),
    D(2274, "command-name-starts-triple-equals",
      "a command name starts with a run of equals signs",
      "The word '{0}' starts with `===` and is neither an assignment nor a "
      "comparison",
      "Write `[ x = y ]` to compare the values", None, Strict, Policy),
    D(2275, "command-name-starts-equals", "a command name starts with equals",
      "An assignment needs a name before the equals sign, so '{0}' runs as a "
      "command",
      "Add the variable name before the equals sign", None, Strict, Policy),
    D(2276, "quoted-assignment-name", "a quoted assignment name is a command",
      "The quotes make '{0}' one command name, and no assignment is performed",
      "Remove the quotes to assign, or write `[ x = y ]` to compare", None,
      Strict, Policy),
    D(2277, "positional-zero-assignment",
      "the zeroth positional parameter cannot be assigned",
      "The `$0` parameter cannot be assigned, so '{0}' runs as a command",
      "Assign to `BASH_ARGV0` instead", None, Strict, Policy),
    D(2279, "positional-zero-assignment-posix",
      "the zeroth positional parameter cannot be assigned in POSIX sh",
      "The `$0` parameter cannot be assigned in POSIX sh, so '{0}' runs as a "
      "command",
      "Assign the program name to a named variable", None, Strict, Policy),
    D(2281, "dollar-assignment-name",
      "an assignment name must not start with a dollar sign",
      "An assignment name must not start with a dollar sign",
      "Remove the dollar sign from the assignment name", None, Strict, Policy),
    D(2282, "numeric-assignment-name",
      "a variable name cannot start with a digit",
      "A variable name cannot start with a digit, so '{0}' runs as a command",
      "Rename the variable to start with a letter or an underscore", None,
      Strict, Policy),
    D(2283, "assignment-equals-spacing",
      "an assignment cannot contain spaces around equals",
      "An assignment cannot contain spaces around equals",
      "Write NAME=value without spaces", None, Strict, Policy),
    D(2284, "comparison-as-command",
      "a comparison written as a command runs the left operand",
      "The `==` comparison runs '{0}' as a command",
      "Write `[ x = y ]` to compare the values", None, Strict, Policy),
    D(2285, "append-equals-spacing",
      "an append cannot contain spaces around the operator",
      "An append cannot contain spaces around `+=`, so '{0}' runs as a command",
      "Write NAME+=value without spaces, or quote the operator to pass it "
      "through",
      None, Strict, Policy),
    D(2335, "negated-numeric-comparison",
      "a negated numeric comparison has a direct operator",
      "A negated {0} is just {1}", "Drop the ! and use {1}", None, Annoying,
      Policy),
    D(3001, "posix-process-substitution",
      "process substitution is absent from POSIX sh",
      "The <(...) process substitution is a bash extension absent from POSIX "
      "sh",
      "Use a named pipe or a temporary file under a sh shebang", None, Strict,
      Policy),
    D(3002, "posix-extglob", "extended globs are absent from POSIX sh",
      "The extended glob {0} is a bash extension absent from POSIX sh",
      "Use a case statement or a plain glob under a sh shebang", None, Strict,
      Policy),
    D(3003, "posix-ansi-c-quoting", "$'...' quoting is absent from POSIX sh",
      "The $'...' quoting form is a bash extension absent from POSIX sh",
      "Use printf to produce the escapes under a sh shebang", None, Strict,
      Policy),
    D(3006, "posix-standalone-arithmetic",
      "a standalone ((...)) is undefined in POSIX sh",
      "The standalone ((...)) command is a bash extension absent from POSIX sh",
      "Use the : $((...)) form under a sh shebang", None, Strict, Policy),
    D(3012, "posix-test-lexicographical",
      "a lexicographical comparison is undefined in POSIX test",
      "The {0} comparison in test is undefined in POSIX sh",
      "Compare with expr or a case statement under a sh shebang", None, Strict,
      Policy),
    D(3013, "posix-test-file-comparison",
      "a file comparison operator is undefined in POSIX test",
      "The {0} operator in test is undefined in POSIX sh",
      "Compare the timestamps with find or ls under a sh shebang", None, Strict,
      Policy),
    D(3014, "posix-test-equals", "== is undefined in POSIX test",
      "== is undefined in POSIX test", "Use = for string equality", None,
      Strict, Policy),
    D(3017, "posix-test-unary-a", "a unary -a is undefined in POSIX test",
      "The unary -a in test is undefined in POSIX sh", "Use -e instead", None,
      Strict, Policy),
    D(3018, "posix-arithmetic-increment",
      "the increment operators are undefined in POSIX sh",
      "The {0} operator in arithmetic is undefined in POSIX sh",
      "Write the assignment out under a sh shebang", None, Strict, Policy),
    D(3019, "posix-arithmetic-exponent",
      "the exponent operator is undefined in POSIX sh",
      "The ** operator in arithmetic is undefined in POSIX sh",
      "Multiply the value out under a sh shebang", None, Strict, Policy),
    D(3020, "posix-both-streams-redirect",
      "the &> redirection is absent from POSIX sh",
      "The &> redirection is a bash extension absent from POSIX sh",
      "Write the file redirect and 2>&1 separately under a sh shebang", None,
      Strict, Policy),
    D(3021, "posix-dup-to-filename",
      "a >& with a filename is absent from POSIX sh",
      "The >& redirection to {0} is a bash extension absent from POSIX sh",
      "Write the file redirect and 2>&1 separately under a sh shebang", None,
      Strict, Policy),
    D(3022, "posix-named-descriptor",
      "a named file descriptor is absent from POSIX sh",
      "The named file descriptor {0} is a bash extension absent from POSIX sh",
      "Name a fixed descriptor number under a sh shebang", None, Strict,
      Policy),
    D(3023, "posix-descriptor-range",
      "a file descriptor above nine is undefined in POSIX sh",
      "The file descriptor {0} is outside the range POSIX sh defines",
      "Use a descriptor from 0 to 9 under a sh shebang", None, Strict, Policy),
    D(3024, "posix-append-assignment",
      "the += assignment is absent from POSIX sh",
      "The += assignment to {0} is a bash extension absent from POSIX sh",
      "Write NAME=\"$NAME\"value under a sh shebang", None, Strict, Policy),
    D(3025, "posix-network-device",
      "the network devices are absent from POSIX sh",
      "The path {0} is a bash network device that POSIX sh leaves undefined",
      "Open the connection with a network client under a sh shebang", None,
      Strict, Policy),
    D(3026, "posix-glob-caret",
      "a ^ in a bracket expression is not a POSIX negation",
      "A ^ opens the bracket expression {0} where POSIX sh negates with !",
      "Use [! ...] under a sh shebang", None, Strict, Policy),
    D(3028, "posix-bash-variable",
      "the variable is defined by bash and not by POSIX sh",
      "{0} is a bash variable that POSIX sh leaves undefined",
      "Compute the value another way under a sh shebang", None, Strict, Policy),
    D(3030, "posix-array-reader",
      "mapfile and readarray are absent from POSIX sh",
      "{0} is a bash array builtin absent from POSIX sh",
      "read the input with a while read loop or switch the shebang to bash",
      None, Strict, Policy),
    D(3031, "posix-glob-redirect",
      "a glob redirection target is undefined in POSIX sh",
      "The redirection target {0} is a glob that POSIX sh leaves undefined",
      "Name the file explicitly under a sh shebang", None, Strict, Policy),
    D(3034, "posix-file-substitution", "$(<file) is absent from POSIX sh",
      "The $(<file) form is a bash shorthand absent from POSIX sh",
      "Use $(cat file) under a sh shebang", None, Strict, Policy),
    D(3035, "posix-backtick-file-substitution",
      "`<file` is absent from POSIX sh",
      "The `<file` form is a bash shorthand absent from POSIX sh",
      "Use $(cat file) under a sh shebang", None, Strict, Policy),
    D(3037, "posix-echo-flag", "echo flags are not in POSIX echo",
      "An echo {0} relies on a bash builtin, the POSIX echo prints the flag as "
      "text",
      "Use printf instead under a sh shebang", None, Strict, Policy),
    D(3038, "posix-exec-flag", "exec flags are absent from POSIX sh",
      "The exec {0} flag is a bash extension absent from POSIX sh",
      "Drop the flag under a sh shebang", None, Strict, Policy),
    D(3039, "posix-let", "let is absent from POSIX sh",
      "The let builtin is a bash extension absent from POSIX sh",
      "Use the : $((...)) form under a sh shebang", None, Strict, Policy),
    D(3043, "posix-local", "local is absent from POSIX sh",
      "The local builtin is not in POSIX sh, the value stays global",
      "rework the function or switch the shebang to bash", None, Strict,
      Policy),
    D(3044, "posix-declare", "declare is absent from POSIX sh",
      "The {0} builtin is not in POSIX",
      "Assign the variable plainly under a sh shebang, or switch the shebang "
      "to bash",
      None, Strict, Policy),
    D(3045, "posix-printf-v", "printf -v is absent from POSIX printf",
      "The printf -v form is a bash extension, the POSIX printf has no -v",
      "capture the output with a command substitution under a sh shebang", None,
      Strict, Policy),
    D(3046, "posix-source",
      "source is the bash spelling of the POSIX dot command",
      "The name source is the bash spelling, the POSIX dot command is '.'",
      "Use '.' under a sh shebang", None, Strict, Policy),
    D(3047, "posix-trap-err", "the ERR trap is absent from POSIX sh",
      "The ERR condition is a bash extension absent from POSIX sh",
      "Check the status after each command under a sh shebang", None, Strict,
      Policy),
    D(3048, "posix-trap-sig-prefix",
      "a SIG prefixed signal name is undefined in POSIX sh",
      "The SIG prefix on {0} is undefined in POSIX sh",
      "Name the signal without the SIG prefix under a sh shebang", None, Strict,
      Policy),
    D(3049, "posix-trap-signal-case",
      "a lowercase signal name is undefined in POSIX sh",
      "The signal name {0} is undefined in POSIX sh because it is not "
      "uppercase",
      "Name the signal in uppercase under a sh shebang", None, Strict, Policy),
    D(3050, "posix-printf-quote",
      "the printf %q directive is absent from POSIX sh",
      "The printf %q directive is a bash extension absent from POSIX sh",
      "Quote the value another way under a sh shebang", None, Strict, Policy),
    D(3053, "posix-indirect-expansion",
      "indirect expansion is absent from POSIX sh",
      "The indirect expansion of {0} is a bash extension absent from POSIX sh",
      "Use eval or a case dispatch under a sh shebang", None, Strict, Policy),
    D(3054, "posix-array-reference", "arrays are absent from POSIX sh",
      "The array reference {0} is a bash extension absent from POSIX sh",
      "Use the positional parameters under a sh shebang", None, Strict, Policy),
    D(3055, "posix-array-key-expansion",
      "array key expansion is absent from POSIX sh",
      "The array key expansion of {0} is a bash extension absent from POSIX sh",
      "Use the positional parameters under a sh shebang", None, Strict, Policy),
    D(3056, "posix-name-prefix-expansion",
      "name matching prefixes are absent from POSIX sh",
      "The name prefix expansion of {0} is a bash extension absent from POSIX "
      "sh",
      "List the names explicitly under a sh shebang", None, Strict, Policy),
    D(3057, "posix-string-indexing", "string indexing is absent from POSIX sh",
      "The substring expansion of {0} is a bash extension absent from POSIX sh",
      "Use expr or cut under a sh shebang", None, Strict, Policy),
    D(3060, "posix-string-replacement",
      "string replacement is absent from POSIX sh",
      "The string replacement in {0} is a bash extension absent from POSIX sh",
      "Use sed or a case dispatch under a sh shebang", None, Strict, Policy),
    D(0, "arith-assign", "array syntax was used for arithmetic assignment",
      "The assignment of '{0}' uses array syntax instead of arithmetic",
      "Use `let '{0}={1}'` to evaluate and assign it", None, Strict, Policy),
    D(0, "assignment-prefix-read",
      "an assignment prefix is read before it takes effect",
      "The assignment prefix does not affect this command, '{0}' is read "
      "before it is set",
      None, None, Lenient, Policy),
    D(0, "exported-cdpath", "an exported CDPATH can redirect child scripts",
      "An exported CDPATH can redirect cd commands in child scripts",
      "Keep CDPATH unexported or clear it before running scripts", None, Strict,
      Policy),
    D(0, "arith-external-input",
      "external input is evaluated as arithmetic code",
      "External input is evaluated as arithmetic code",
      "Validate the value as decimal digits before arithmetic", None, Strict,
      Policy),
    D(0, "array-subscript-external-input",
      "an external array subscript is evaluated as arithmetic code",
      "An array subscript from external input is evaluated as arithmetic code",
      "Validate the subscript as decimal digits before using it", None, Strict,
      Policy),
    D(0, "malformed-glob", "a glob contains an unterminated bracket class",
      "Malformed glob pattern, unterminated '['", None, None, Strict, Policy),
    D(0, "no-local", "a function assignment without local leaks globally",
      "This assignment to '{0}' in a function has no local, so the value leaks "
      "to the global scope",
      "Declare it with local to keep it inside the function", None, Annoying,
      Policy),
    D(0, "typeset-spelling", "typeset is the ksh spelling of declare",
      "The typeset builtin is the ksh spelling of declare",
      "Write declare for the clearer bash name", None, Annoying, Policy),
    D(0, "unresolved-command", "a command cannot be resolved during analysis",
      "Command '{0}' was not found", "Did you mean '{1}'?", None, Lenient,
      Policy),
    D(0, "unresolved-command-uncertain",
      "a command may be defined by unseen runtime code",
      "Command '{0}' was not found", "Did you mean '{1}'?", None, Lenient,
      Warning),
    D(0, "use-before-assign", "a variable is read before a later assignment",
      "The variable '{0}' is read before it is assigned", None,
      "the assignment that gives '{0}' a value runs here", Lenient, Policy),
};

#undef D

static_assert(countof(DIAGNOSTIC_DEFINITIONS) ==
              static_cast<usize>(diagnostic_id::Count));

pure fn get_diagnostic_definition(diagnostic_id id) wontthrow
    -> const diagnostic_definition &
{
  let const index = static_cast<usize>(id);
  ASSERT(index < get_diagnostic_count());
  return DIAGNOSTIC_DEFINITIONS[index];
}

pure fn get_diagnostic_count() wontthrow -> usize
{
  return countof(DIAGNOSTIC_DEFINITIONS);
}

fn format_diagnostic_template(
    const char *text_template,
    std::initializer_list<StringView> arguments) throws -> String
{
  let result = String{heap_allocator()};
  let const text = StringView{text_template};

  for (usize position = 0; position < text.length; position++) {
    if (text[position] != '{' || position + 2 >= text.length ||
        text[position + 2] != '}')
    {
      result += text[position];
      continue;
    }

    usize argument_index = 0;
    switch (text[position + 1]) {
    case '0': argument_index = 0; break;
    case '1': argument_index = 1; break;
    default: result += text[position]; continue;
    }

    if (argument_index >= arguments.size()) return String{heap_allocator()};
    result += *(arguments.begin() + argument_index);
    position += 2;
  }

  return result;
}

static pure fn parse_diagnostic_code(StringView text) wontthrow -> Maybe<u16>
{
  if (text.length >= 2 && text[0] == 'S' && text[1] == 'C') {
    text = text.substring(2);
  }
  if (!text.is_all_decimal_digits()) return None;

  u32 value = 0;
  for (usize position = 0; position < text.length; position++) {
    value = value * 10 + static_cast<u32>(text[position] - '0');
    if (value > UINT16_MAX) return None;
  }

  return static_cast<u16>(value);
}

pure fn shellcheck_selector_disables(const shellcheck_selector &selector,
                                     StringView source,
                                     diagnostic_id id) wontthrow -> bool
{
  let const &definition = get_diagnostic_definition(id);

  switch (selector.kind) {
  case shellcheck_selector_kind::All: return true;
  case shellcheck_selector_kind::Slug:
    return source.substring_of_length(selector.slug.position,
                                      selector.slug.length) ==
           StringView{definition.slug};
  case shellcheck_selector_kind::Code:
    return definition.shellcheck_code != 0 &&
           definition.shellcheck_code == selector.code_start;
  case shellcheck_selector_kind::CodeRange:
    return definition.shellcheck_code != 0 &&
           definition.shellcheck_code >= selector.code_start &&
           definition.shellcheck_code < selector.code_end;
  }

  return false;
}

static fn
push_resolved_selector(StringView source, StringView text,
                       ArrayList<shellcheck_selector> &selectors) throws -> void
{
  if (text == StringView{"all"}) {
    selectors.push(shellcheck_selector{shellcheck_selector_kind::All});
    return;
  }

  if (let const code = parse_diagnostic_code(text); code.has_value()) {
    selectors.push(shellcheck_selector{
        shellcheck_selector_kind::Code, {0, 0},
         *code, 0
    });
    return;
  }

  if (let const separator = text.find_character('-'); separator.has_value()) {
    let const range_start =
        parse_diagnostic_code(text.substring_of_length(0, *separator));
    let const range_end = parse_diagnostic_code(text.substring(*separator + 1));
    if (range_start.has_value() && range_end.has_value()) {
      selectors.push(shellcheck_selector{
          shellcheck_selector_kind::CodeRange,
          {0, 0},
          *range_start,
          *range_end
      });
      return;
    }
  }

  let const slug_position = static_cast<usize>(text.data - source.data);
  selectors.push(shellcheck_selector{
      shellcheck_selector_kind::Slug, {slug_position, text.length},
       0, 0
  });
}

static fn collect_comma_separated_selectors(
    StringView source, StringView value,
    ArrayList<shellcheck_selector> &selectors) throws -> void
{
  usize component_start = 0;
  while (component_start <= value.length) {
    usize component_end = component_start;
    while (component_end < value.length && value[component_end] != ',') {
      component_end++;
    }
    push_resolved_selector(
        source,
        value.substring_of_length(component_start,
                                  component_end - component_start),
        selectors);
    if (component_end == value.length) break;
    component_start = component_end + 1;
  }
}

fn collect_shellcheck_selectors(
    StringView source, shellcheck_directive_span comment_span,
    ArrayList<shellcheck_selector> &selectors) throws -> void
{
  if (comment_span.position + comment_span.length > source.length) return;

  let const comment =
      source.substring_of_length(comment_span.position, comment_span.length);
  usize position = 1;
  while (position < comment.length &&
         (comment[position] == ' ' || comment[position] == '\t'))
  {
    position++;
  }
  let const directive_text = comment.substring(position);
  if (!directive_text.starts_with(StringView{"shellcheck"}) ||
      (directive_text.length > 10 && directive_text[10] != ' ' &&
       directive_text[10] != '\t'))
  {
    return;
  }
  position += 10;

  while (position < comment.length) {
    while (position < comment.length &&
           (comment[position] == ' ' || comment[position] == '\t'))
    {
      position++;
    }
    if (position >= comment.length || comment[position] == '#') break;
    if (!comment.substring(position).starts_with(StringView{"disable="})) {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
      continue;
    }
    position += 8;

    char quote = '\0';
    if (position < comment.length &&
        (comment[position] == '\'' || comment[position] == '"'))
    {
      quote = comment[position++];
    }
    let const value_start = position;
    if (quote != '\0') {
      while (position < comment.length && comment[position] != quote) {
        position++;
      }
      if (position == comment.length) return;
    } else {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
    }
    collect_comma_separated_selectors(
        source,
        comment.substring_of_length(value_start, position - value_start),
        selectors);
    if (quote != '\0' && position < comment.length) position++;
  }
}

namespace {

#define C(name, id, groups)                                                    \
  {                                                                            \
    SSK(name), { command_name_id::id, groups }                                 \
  }

constexpr u32 NO_COMMAND_GROUP = 0;
constexpr u32 DECLARATION_GROUPS = COMMAND_GROUP_DECLARATION_BUILTIN |
                                   COMMAND_GROUP_ASSIGNMENT_BUILTIN |
                                   COMMAND_GROUP_VARIABLE_TARGET;
constexpr u32 EXPORT_GROUPS =
    COMMAND_GROUP_ASSIGNMENT_BUILTIN | COMMAND_GROUP_VARIABLE_TARGET;
constexpr u32 NEUTRAL_READER_GROUPS =
    COMMAND_GROUP_NON_STDIN_READER | COMMAND_GROUP_ENVIRONMENT_NEUTRAL;
constexpr u32 BRACKET_GROUPS =
    COMMAND_GROUP_TEST | COMMAND_GROUP_VARIABLE_PROBE;

constexpr static_string_entry<analysis_command_info>
    ANALYSIS_COMMAND_ENTRIES[] = {
        C(".", Dot, COMMAND_GROUP_RUNTIME_DEFINER),
        C(":", Colon, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("[", SingleBracket,
          BRACKET_GROUPS | COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("[[", DoubleBracket, BRACKET_GROUPS),
        C("alias", Alias, COMMAND_GROUP_RUNTIME_DEFINER),
        C("amp", Unknown, COMMAND_GROUP_HTML_ENTITY_TAIL),
        C("arch", Arch, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("basename", Basename, NEUTRAL_READER_GROUPS),
        C("break", Break, NO_COMMAND_GROUP),
        C("builtin", Builtin, NO_COMMAND_GROUP),
        C("cd", Cd, NO_COMMAND_GROUP),
        C("chmod", Chmod, COMMAND_GROUP_NON_STDIN_READER),
        C("chown", Chown, COMMAND_GROUP_NON_STDIN_READER),
        C("command", Command, NO_COMMAND_GROUP),
        C("continue", Continue, NO_COMMAND_GROUP),
        C("cp", Cp, COMMAND_GROUP_NON_STDIN_READER),
        C("date", Date, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("declare", Declare, DECLARATION_GROUPS),
        C("dirname", Dirname, NEUTRAL_READER_GROUPS),
        C("echo", Echo, NEUTRAL_READER_GROUPS),
        C("egrep", Egrep, COMMAND_GROUP_PATTERN_MATCHER),
        C("eval", Eval,
          COMMAND_GROUP_RUNTIME_DEFINER | COMMAND_GROUP_VARIABLE_PROBE),
        C("exec", Exec, NO_COMMAND_GROUP),
        C("exit", Exit, NO_COMMAND_GROUP),
        C("export", Export, EXPORT_GROUPS),
        C("expr", Expr, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("false", False, NEUTRAL_READER_GROUPS),
        C("fgrep", Fgrep, COMMAND_GROUP_PATTERN_MATCHER),
        C("find", Find, NO_COMMAND_GROUP),
        C("getopts", Getopts, COMMAND_GROUP_VARIABLE_TARGET),
        C("grep", Grep, COMMAND_GROUP_PATTERN_MATCHER),
        C("gt", Unknown, COMMAND_GROUP_HTML_ENTITY_TAIL),
        C("hostname", Hostname, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("id", Id, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("kill", Kill, COMMAND_GROUP_NON_STDIN_READER),
        C("let", Let, COMMAND_GROUP_VARIABLE_PROBE),
        C("ln", Ln, COMMAND_GROUP_NON_STDIN_READER),
        C("local", Local, DECLARATION_GROUPS),
        C("ls", Ls, COMMAND_GROUP_NON_STDIN_READER),
        C("lt", Unknown, COMMAND_GROUP_HTML_ENTITY_TAIL),
        C("mapfile", Mapfile, COMMAND_GROUP_VARIABLE_TARGET),
        C("mkdir", Mkdir, COMMAND_GROUP_NON_STDIN_READER),
        C("mv", Mv, COMMAND_GROUP_NON_STDIN_READER),
        C("printf", Printf, COMMAND_GROUP_NON_STDIN_READER),
        C("ps", Ps, COMMAND_GROUP_NON_STDIN_READER),
        C("pwd", Pwd, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("read", Read, COMMAND_GROUP_VARIABLE_TARGET),
        C("readarray", Readarray, COMMAND_GROUP_VARIABLE_TARGET),
        C("readonly", Readonly, EXPORT_GROUPS),
        C("return", Return, NO_COMMAND_GROUP),
        C("rm", Rm, COMMAND_GROUP_NON_STDIN_READER),
        C("rmdir", Rmdir, COMMAND_GROUP_NON_STDIN_READER),
        C("sed", Sed, NO_COMMAND_GROUP),
        C("seq", Seq, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("set", Set, NO_COMMAND_GROUP),
        C("sleep", Sleep, COMMAND_GROUP_NON_STDIN_READER),
        C("source", Source, COMMAND_GROUP_RUNTIME_DEFINER),
        C("ssh", Ssh, NO_COMMAND_GROUP),
        C("su", Su, NO_COMMAND_GROUP),
        C("sudo", Sudo, NO_COMMAND_GROUP),
        C("test", Test, BRACKET_GROUPS | COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("touch", Touch, COMMAND_GROUP_NON_STDIN_READER),
        C("tr", Tr, NO_COMMAND_GROUP),
        C("trap", Trap, NO_COMMAND_GROUP),
        C("true", True, NEUTRAL_READER_GROUPS),
        C("tty", Tty, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("typeset", Typeset, DECLARATION_GROUPS),
        C("uname", Uname, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("unlink", Unlink, COMMAND_GROUP_NON_STDIN_READER),
        C("unset", Unset, COMMAND_GROUP_VARIABLE_PROBE),
        C("wc", Wc, NO_COMMAND_GROUP),
        C("which", Which, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("whoami", Whoami, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("xargs", Xargs, NO_COMMAND_GROUP),
};

#undef C

constexpr StaticStringMap ANALYSIS_COMMANDS{ANALYSIS_COMMAND_ENTRIES};

} /* namespace */

fn get_analysis_command_info(StringView name) throws -> analysis_command_info
{
  if (let const found = ANALYSIS_COMMANDS.find(name); found.has_value())
    return *found;

  return analysis_command_info{command_name_id::Unknown, NO_COMMAND_GROUP};
}

namespace expressions {

namespace {

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

} /* namespace */

cold fn is_test_unary_operator_word(StringView op) wontthrow -> bool
{
  return TEST_UNARY_OPERATORS.contains(op);
}

namespace {

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
enum class bracketed_constant_kind : u8
{
  False,
  Zero,
  True,
  One,
};

constexpr static_string_entry<bracketed_constant_kind>
    BRACKETED_CONSTANT_ENTRIES[] = {
        {SSK("0"),     bracketed_constant_kind::Zero },
        {SSK("1"),     bracketed_constant_kind::One  },
        {SSK("false"), bracketed_constant_kind::False},
        {SSK("true"),  bracketed_constant_kind::True },
};
constexpr StaticStringMap BRACKETED_CONSTANTS{BRACKETED_CONSTANT_ENTRIES};

/* The left operand of an X != Y triple centered on operator_index, absent when
   the words there do not form one. The raw view is compared, so "$name" and
   $name stay distinct. */
cold fn test_inequality_left_operand(const ArrayList<const Token *> &args,
                                     usize operator_index,
                                     usize operand_end) wontthrow
    -> Maybe<StringView>
{
  if (operator_index == 0 || operator_index + 1 >= operand_end) return None;

  let const op = args[operator_index]->raw_view();
  if (!op.has_value() || *op != StringView{"!="}) return None;

  return args[operator_index - 1]->raw_view();
}

/* An operator name carries a letter after the dash, which keeps a negative
   number such as -5 out of the unknown-operator lints. */
cold fn view_looks_like_test_operator(StringView view) wontthrow -> bool
{
  if (view.length < 2 || view[0] != '-') return false;

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

  if (position >= view.length || view[position] != '.') return false;

  position++;

  let const fraction_start = position;
  while (position < view.length && lexer::is_number(view[position]))
    position++;

  return position > fraction_start && position == view.length;
}

pure fn is_arithmetic_operand_byte(char byte) wontthrow -> bool
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
  if (view.length < 4 || view[0] != 's') return false;

  let const delimiter = view[1];
  if (lexer::is_variable_name(delimiter) || delimiter == '\\') return false;

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
  if (view.length < 2 || view[0] != '-') return false;

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
  usize start = 0;
  for (usize position = 0; position < body.length; position++)
    if (body[position] == '|') start = position + 1;

  while (start < body.length && (body[start] == ' ' || body[start] == '\t'))
    start++;

  usize end = start;
  while (end < body.length && body[end] != ' ' && body[end] != '\t' &&
         body[end] != '\n')
    end++;

  if (end == start) return false;

  return get_analysis_command_info(body.substring_of_length(start, end - start))
      .is_in_group(COMMAND_GROUP_PATTERN_MATCHER);
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
    if (format[i] != '%' || i + 1 >= format.length) continue;
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
    if (i < format.length && format[i] == 'q') has_quote_conversion = true;

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

constexpr PackedStringKey FIND_ACTION_KEYS[] = {
    SSK("-delete"), SSK("-exec"),    SSK("-execdir"), SSK("-fls"),
    SSK("-fprint"), SSK("-fprint0"), SSK("-fprintf"), SSK("-ls"),
    SSK("-ok"),     SSK("-okdir"),   SSK("-print"),   SSK("-print0"),
    SSK("-printf"), SSK("-prune"),   SSK("-quit"),    SSK("-used")};
constexpr StaticStringSet FIND_ACTIONS{FIND_ACTION_KEYS};

/* The find options that stand before the search paths, so the path lint keeps
   reading past them. -f takes the path itself and counts as a path. */
constexpr PackedStringKey FIND_LEADING_OPTION_KEYS[] = {
    SSK("-E"), SSK("-H"), SSK("-L"), SSK("-P"),
    SSK("-d"), SSK("-s"), SSK("-x"), SSK("-X")};
constexpr StaticStringSet FIND_LEADING_OPTIONS{FIND_LEADING_OPTION_KEYS};

/* The names the shell or the environment gives a value without the script
   assigning one, so a read of them is not an unassigned read. Every KOSH_ name
   is covered by its prefix and is left out here. */
constexpr PackedStringKey SHELL_MAINTAINED_VARIABLE_KEYS[] = {
    SSK("BASH"),
    SSK("BASHOPTS"),
    SSK("BASHPID"),
    SSK("BASH_ALIASES"),
    SSK("BASH_ARGC"),
    SSK("BASH_ARGV"),
    SSK("BASH_ARGV0"),
    SSK("BASH_COMMAND"),
    SSK("BASH_ENV"),
    SSK("BASH_EXECUTION_STRING"),
    SSK("BASH_LINENO"),
    SSK("BASH_MONOSECONDS"),
    SSK("BASH_REMATCH"),
    SSK("BASH_SOURCE"),
    SSK("BASH_SUBSHELL"),
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
    SSK("EPOCHREALTIME"),
    SSK("EPOCHSECONDS"),
    SSK("EUID"),
    SSK("FCEDIT"),
    SSK("FUNCNAME"),
    SSK("GLOBIGNORE"),
    SSK("GROUPS"),
    SSK("HISTFILE"),
    SSK("HISTSIZE"),
    SSK("HOME"),
    SSK("HOSTNAME"),
    SSK("HOSTTYPE"),
    SSK("IFS"),
    SSK("LANG"),
    SSK("LC_ALL"),
    SSK("LC_COLLATE"),
    SSK("LC_CTYPE"),
    SSK("LC_MESSAGES"),
    SSK("LC_NUMERIC"),
    SSK("LC_TIME"),
    SSK("LINENO"),
    SSK("LINES"),
    SSK("LOGNAME"),
    SSK("MACHTYPE"),
    SSK("MAIL"),
    SSK("MAILCHECK"),
    SSK("MAILPATH"),
    SSK("OLDPWD"),
    SSK("OPTARG"),
    SSK("OPTERR"),
    SSK("OPTIND"),
    SSK("OSTYPE"),
    SSK("PAGER"),
    SSK("PATH"),
    SSK("PIPESTATUS"),
    SSK("POSIXLY_CORRECT"),
    SSK("PPID"),
    SSK("PROMPT_COMMAND"),
    SSK("PS1"),
    SSK("PS2"),
    SSK("PS3"),
    SSK("PS4"),
    SSK("PWD"),
    SSK("RANDOM"),
    SSK("REPLY"),
    SSK("SECONDS"),
    SSK("SHELL"),
    SSK("SHELLOPTS"),
    SSK("SHLVL"),
    SSK("SRANDOM"),
    SSK("TERM"),
    SSK("TIMEFORMAT"),
    SSK("TMPDIR"),
    SSK("TZ"),
    SSK("UID"),
    SSK("USER"),
    SSK("VISUAL"),
};
constexpr StaticStringSet SHELL_MAINTAINED_VARIABLES{
    SHELL_MAINTAINED_VARIABLE_KEYS};

constexpr PackedStringKey BASH_ONLY_VARIABLE_KEYS[] = {
    SSK("BASHOPTS"),      SSK("BASHPID"),
    SSK("BASH_ALIASES"),  SSK("BASH_ARGC"),
    SSK("BASH_ARGV"),     SSK("BASH_COMMAND"),
    SSK("BASH_LINENO"),   SSK("BASH_REMATCH"),
    SSK("BASH_SOURCE"),   SSK("BASH_SUBSHELL"),
    SSK("BASH_VERSINFO"), SSK("BASH_VERSION"),
    SSK("COMP_CWORD"),    SSK("COMP_LINE"),
    SSK("COMP_POINT"),    SSK("COMP_WORDS"),
    SSK("DIRSTACK"),      SSK("EPOCHREALTIME"),
    SSK("EPOCHSECONDS"),  SSK("EUID"),
    SSK("FUNCNAME"),      SSK("GROUPS"),
    SSK("HOSTNAME"),      SSK("HOSTTYPE"),
    SSK("MACHTYPE"),      SSK("OSTYPE"),
    SSK("PIPESTATUS"),    SSK("PROMPT_COMMAND"),
    SSK("RANDOM"),        SSK("SECONDS"),
    SSK("SHELLOPTS"),     SSK("SHLVL"),
    SSK("SRANDOM"),       SSK("UID"),
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

/* The first word of a command line, used where a builtin hands a whole command
   string to another shell. */
pure fn leading_command_word(StringView text) wontthrow -> StringView
{
  usize start = 0;
  while (start < text.length && (text[start] == ' ' || text[start] == '\t'))
    start++;

  usize end = start;
  while (end < text.length && text[end] != ' ' && text[end] != '\t' &&
         text[end] != '\n')
    end++;

  return text.substring_of_length(start, end - start);
}

fn check_posix_parameter_expansion(AnalysisContext &actx,
                                   const WordSegment &segment, StringView text,
                                   SourceLocation fallback_location) throws
    -> void
{
  if (text.is_empty()) return;

  let const do_get_location = [&]() -> SourceLocation {
    return expansion_location_with_sigil(
        actx, segment.get_source_location(fallback_location.filename)
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
  if (BASH_ONLY_VARIABLES.contains(name)) {
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
    if (at < 2 || expression[at - 2] != expression[at - 1]) return {};
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

} /* namespace */

fn check_posix_arithmetic_operators(AnalysisContext &actx,
                                    StringView expression,
                                    SourceLocation location) throws -> void
{
  let has_increment = false;
  let has_decrement = false;
  let has_exponent = false;

  for (usize position = 0; position + 1 < expression.length; position++) {
    let const byte = expression[position];
    if (expression[position + 1] != byte) continue;

    switch (byte) {
    case '+': has_increment = true; break;
    case '-': has_decrement = true; break;
    case '*': has_exponent = true; break;
    default: continue;
    }

    position++;
  }

  if (has_increment)
    actx.report_diagnostic(diagnostic_id::sc3018, location, {"++"});
  if (has_decrement)
    actx.report_diagnostic(diagnostic_id::sc3018, location, {"--"});
  if (has_exponent) actx.report_diagnostic(diagnostic_id::sc3019, location);
}

fn check_arithmetic_expression_lints(AnalysisContext &actx,
                                     StringView expression,
                                     SourceLocation location) throws -> void
{
  let has_reported_test_operator = false;
  let has_reported_decimal = false;
  let has_reported_octal = false;
  let has_reported_precision_loss = false;
  /* A division truncates its result, so a multiplication that follows it in the
     same term multiplies the truncated value, shellcheck SC2017. Any operator
     that ends the term clears the flag. */
  let has_pending_division = false;

  for (usize position = 0; position < expression.length; position++) {
    switch (expression[position]) {
    case '/':
      if (position + 1 < expression.length &&
          (expression[position + 1] == '/' || expression[position + 1] == '='))
      {
        position++;
        has_pending_division = false;
        break;
      }
      has_pending_division = true;
      break;

    case '*':
      if (position + 1 < expression.length && expression[position + 1] == '*') {
        position++;
        has_pending_division = false;
        break;
      }
      if (has_pending_division && !has_reported_precision_loss) {
        actx.report_diagnostic(diagnostic_id::sc2017, location);
        has_reported_precision_loss = true;
      }
      has_pending_division = false;
      break;

    case '-': {
      has_pending_division = false;
      if (has_reported_test_operator) break;
      if (position + 3 > expression.length) break;
      if (position > 0 && !lexer::is_whitespace(expression[position - 1]))
        break;

      let const after = position + 3;
      if (after < expression.length && !lexer::is_whitespace(expression[after]))
        break;

      let const candidate = expression.substring_of_length(position, 3);
      if (is_test_numeric_operator_word(candidate)) {
        actx.report_diagnostic(diagnostic_id::sc1106, location, {candidate});
        has_reported_test_operator = true;
      }
      break;
    }

    case '+':
    case '(':
    case ')':
    case ',':
    case ';':
    case '?':
    case ':':
    case '|':
    case '&':
    case '^':
    case '<':
    case '>':
    case '%': has_pending_division = false; break;

    case '=': {
      has_pending_division = false;

      let const target = arithmetic_assignment_target(expression, position);
      if (!target.is_empty()) actx.note_variable_assignment(target, location);
      break;
    }

    /* An arithmetic expression is never quoted, so a $ here introduces a real
       expansion and a positional name behind it is a positional read. */
    case '$': {
      has_pending_division = false;

      usize name_start = position + 1;
      let const is_braced =
          name_start < expression.length && expression[name_start] == '{';
      if (is_braced) name_start++;

      usize name_end = name_start;
      while (name_end < expression.length &&
             lexer::is_variable_name(expression[name_end]))
        name_end++;

      if (name_end == name_start && name_end < expression.length) {
        switch (expression[name_end]) {
        case '@':
        case '*':
        case '#': name_end++; break;

        default: break;
        }
      }

      position = name_end - 1;

      if (is_braced &&
          (name_end >= expression.length || expression[name_end] != '}'))
      {
        break;
      }

      actx.note_positional_reference(
          expression.substring_of_length(name_start, name_end - name_start));
      break;
    }

    default: {
      if (!lexer::is_variable_name(expression[position])) break;

      /* The whole name or number is consumed so the scan never restarts inside
         one and reads a suffix as a fresh literal. */
      let const start = position;
      while (position + 1 < expression.length &&
             (lexer::is_variable_name(expression[position + 1]) ||
              expression[position + 1] == '.'))
      {
        position++;
      }

      let const word =
          expression.substring_of_length(start, position + 1 - start);
      if (!lexer::is_number(word[0])) break;

      let const dot = word.find_character('.');
      if (dot.has_value() && *dot + 1 < word.length &&
          lexer::is_number(word[*dot + 1]) && !has_reported_decimal)
      {
        actx.report_diagnostic(diagnostic_id::sc2079, location, {word});
        has_reported_decimal = true;
        break;
      }

      if (word[0] == '0' && word.length > 1 && word.is_all_decimal_digits() &&
          !has_reported_octal)
      {
        actx.report_diagnostic(diagnostic_id::sc2080, location, {word});
        has_reported_octal = true;
      }
      break;
    }
    }
  }
}

fn check_numeric_comparison_operand(AnalysisContext &actx,
                                    StringView operator_view,
                                    const Token *operand_token,
                                    bool should_prefer_string_comparison) throws
    -> void
{
  if (operand_token == nullptr || operand_token->kind() != Token::Kind::Word) {
    return;
  }

  if (!is_test_numeric_operator_word(operator_view)) return;

  let const location = operand_token->source_location();
  let const &word =
      static_cast<const tokens::WordToken *>(operand_token)->word();
  if (!word_is_fully_literal(word)) return;

  let const literal = word.to_literal_string();
  if (view_is_integer_literal(literal.view())) return;

  if (view_has_arithmetic_operator(literal.view())) {
    actx.report_diagnostic(diagnostic_id::sc2255, location, {literal.view()});
  } else if (view_has_decimal_fraction(literal.view())) {
    actx.report_diagnostic(diagnostic_id::sc2072, location,
                           {operator_view, literal.view()});
  } else if (should_prefer_string_comparison) {
    actx.report_diagnostic(diagnostic_id::sc2130, location, {literal.view()});
  } else {
    actx.report_diagnostic(diagnostic_id::sc2170, location,
                           {operator_view, literal.view()});
  }
}

fn check_posix_word_portability(AnalysisContext &actx,
                                const WordSegment &segment,
                                SourceLocation fallback_location) throws -> void
{
  let const text = segment.text.view();
  let const do_get_location = [&]() -> SourceLocation {
    return expansion_location_with_sigil(
        actx, segment.get_source_location(fallback_location.filename)
                  .value_or(fallback_location));
  };

  switch (segment.kind) {
  case WordSegment::Kind::ProcessSubstitution:
    actx.report_diagnostic(diagnostic_id::sc3001, do_get_location());
    break;

  case WordSegment::Kind::CommandSubstitution: {
    usize position = 0;
    while (position < text.length &&
           (text[position] == ' ' || text[position] == '\t'))
      position++;

    if (position >= text.length || text[position] != '<') break;

    let const location = do_get_location();
    let const source_text = analysis_source_text(actx, location);
    let const spelling = !source_text.is_empty() && source_text[0] == '`'
                             ? diagnostic_id::sc3035
                             : diagnostic_id::sc3034;
    actx.report_diagnostic(spelling, location);
    break;
  }

  case WordSegment::Kind::VariableReference:
    check_posix_parameter_expansion(actx, segment, text, fallback_location);
    break;

  case WordSegment::Kind::ArithmeticExpansion:
    check_posix_arithmetic_operators(actx, text, do_get_location());
    break;

  case WordSegment::Kind::LiteralText:
    if (segment.was_ansi_c_quoted)
      actx.report_diagnostic(diagnostic_id::sc3003, do_get_location());
    break;

  case WordSegment::Kind::UnquotedText: {
    let has_extended_glob = false;
    let has_caret_bracket = false;
    for (usize position = 0; position + 1 < text.length; position++) {
      let const following = text[position + 1];
      if (following != '(' && following != '^') continue;

      if (following == '(') {
        has_extended_glob |= lexer::is_extglob_operator(text[position]);
      } else {
        has_caret_bracket |= text[position] == '[';
      }

      if (has_extended_glob && has_caret_bracket) break;
    }

    if (!has_extended_glob && !has_caret_bracket) break;

    let const location = do_get_location();
    if (has_extended_glob)
      actx.report_diagnostic(diagnostic_id::sc3002, location, {text});
    if (has_caret_bracket)
      actx.report_diagnostic(diagnostic_id::sc3026, location, {text});
    break;
  }

  default: break;
  }
}

fn check_operand_lints_before_scan(AnalysisContext &actx,
                                   const command_lint_input &input) throws
    -> void
{
  if (input.command_is_shadowed) return;

  let const &args = input.args;

  if (input.is_in_group(COMMAND_GROUP_TEST)) {
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      if (literal.view() == "=~")
        actx.report_diagnostic(diagnostic_id::sc2074,
                               args[i]->source_location());
    }

    if (args.count() >= 4) {
      let const first_operand = args[1]->raw_string();
      if (first_operand.view().starts_with(StringView{"x$"}) ||
          first_operand.view().starts_with(StringView{"x\"$"}))
      {
        actx.report_diagnostic(diagnostic_id::sc2268,
                               args[1]->source_location());
      }
    }

    return;
  }

  switch (input.command_id()) {
  case command_name_id::Find: {
    bool has_exec = false;
    bool has_exec_terminator = false;
    bool has_or = false;
    bool has_group = false;
    bool has_action = false;
    bool has_path_operand = false;
    bool is_before_path_operand = true;
    bool is_inside_exec_action = false;
    Maybe<SourceLocation> exec_location{};
    Maybe<SourceLocation> or_location{};
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();

      if (is_before_path_operand &&
          !FIND_LEADING_OPTIONS.contains(literal.view()))
      {
        is_before_path_operand = false;
        has_path_operand = literal.view() == "-f" ||
                           (!literal.is_empty() && literal[0] != '-' &&
                            literal.view() != "(" && literal.view() != "!");
      }

      if (literal.view() == "-exec" || literal.view() == "-execdir") {
        has_exec = true;
        is_inside_exec_action = true;
        exec_location = args[i]->source_location();
      } else if (has_exec && (literal.view() == ";" || literal.view() == "+")) {
        has_exec_terminator = true;
        is_inside_exec_action = false;
      } else if (is_inside_exec_action &&
                 token_has_command_substitution(args[i]))
      {
        actx.report_diagnostic(diagnostic_id::sc2014,
                               args[i]->source_location());
      } else if (literal.view() == "-o") {
        has_or = true;
        or_location = args[i]->source_location();
      } else if (literal.view() == "(" || literal.view() == ")") {
        has_group = true;
      }
      if (FIND_ACTIONS.contains(literal.view())) has_action = true;
    }
    if (has_exec && !has_exec_terminator && exec_location.has_value())
      actx.report_diagnostic(diagnostic_id::sc2067, *exec_location);
    if (has_or && has_action && !has_group && or_location.has_value())
      actx.report_diagnostic(diagnostic_id::sc2146, *or_location);
    if (!has_path_operand)
      actx.report_diagnostic(diagnostic_id::sc2185, input.command_location());
    break;
  }

  case command_name_id::Alias:
    for (usize i = 1; i < args.count(); i++) {
      let const raw = args[i]->raw_string();
      if (view_contains(raw.view(), StringView{"$1"}) ||
          view_contains(raw.view(), StringView{"$@"}) ||
          view_contains(raw.view(), StringView{"$*"}))
      {
        actx.report_diagnostic(diagnostic_id::sc2142,
                               args[i]->source_location());
      }
    }
    break;

  case command_name_id::Tr:
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      let const set_view = literal.view();

      if (i <= 2 && literal.length() >= 5 && literal[0] == '[' &&
          literal[literal.length() - 1] == ']' &&
          set_view.find_character('-').has_value())
      {
        actx.report_diagnostic(diagnostic_id::sc2021,
                               args[i]->source_location());
      }

      if (set_view == "a-z") {
        actx.report_diagnostic(diagnostic_id::sc2018,
                               args[i]->source_location());
      } else if (set_view == "A-Z") {
        actx.report_diagnostic(diagnostic_id::sc2019,
                               args[i]->source_location());
      } else if (!set_view.is_empty() && set_view[0] != '-' &&
                 set_view[0] != '[' && view_repeats_a_letter(set_view))
      {
        actx.report_diagnostic(diagnostic_id::sc2020,
                               args[i]->source_location(), {set_view});
      }
    }
    break;

  case command_name_id::Echo: {
    if (args.count() == 2 && args[1]->kind() == Token::Kind::Word) {
      let const &word = static_cast<const tokens::WordToken *>(args[1])->word();
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::CommandSubstitution)
      {
        actx.report_diagnostic(diagnostic_id::sc2005,
                               args[0]->source_location());
      }
    }

    StringView escape_sequence{};
    Maybe<SourceLocation> escape_location{};
    for (usize i = 1; i < args.count(); i++) {
      let const operand_text =
          analysis_source_text(actx, args[i]->source_location());

      if (view_settles_echo_escapes(operand_text)) {
        escape_location = None;
        break;
      }

      if (escape_location.has_value()) continue;
      if (token_has_ansi_c_quote(args[i])) continue;

      escape_sequence = find_echo_escape_sequence(operand_text);
      if (!escape_sequence.is_empty())
        escape_location = args[i]->source_location();
    }

    if (escape_location.has_value())
      actx.report_diagnostic(diagnostic_id::sc2028, *escape_location,
                             {escape_sequence});
    break;
  }

  case command_name_id::Sed:
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      let const script_view = literal.view();

      if (script_view.starts_with(StringView{"-f"})) break;

      if (!script_view.is_empty() && script_view[0] == '-') {
        continue;
      }

      if (view_is_plain_substitution_script(script_view)) {
        actx.report_diagnostic(diagnostic_id::sc2001,
                               args[i]->source_location(), {script_view});
      }

      break;
    }
    break;

  default: break;
  }
}

/* The parser refuses a malformed assignment and hands the word on as a command
   name, so the leading byte of the source text decides which of the shellcheck
   assignment shapes was written. */
fn check_equals_bearing_command_name(AnalysisContext &actx,
                                     StringView command_literal,
                                     usize equals_position,
                                     const SourceLocation &location) throws
    -> void
{
  switch (command_literal[0]) {
  case '$': {
    if (command_literal[1] == '0' && equals_position == 2) {
      let const id = actx.shebang_is_posix_sh ? diagnostic_id::sc2279
                                              : diagnostic_id::sc2277;
      actx.report_diagnostic(id, location, {command_literal});

      return;
    }

    if (command_literal[1] >= '0' && command_literal[1] <= '9') {
      actx.report_diagnostic(diagnostic_id::sc2270, location,
                             {command_literal});

      return;
    }

    if (lexer::is_variable_name_start(command_literal[1]))
      actx.report_diagnostic(diagnostic_id::sc2281, location);

    return;
  }

  case '=': {
    bool is_all_equals = true;
    for (usize i = 0; i < command_literal.length; i += 1) {
      if (command_literal[i] != '=') {
        is_all_equals = false;
        break;
      }
    }

    if (is_all_equals && command_literal.length >= 3) {
      actx.report_diagnostic(diagnostic_id::sc2273, location,
                             {command_literal});

      return;
    }

    let const id = command_literal.starts_with(StringView{"==="})
                       ? diagnostic_id::sc2274
                       : diagnostic_id::sc2275;
    actx.report_diagnostic(id, location, {command_literal});

    return;
  }

  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    actx.report_diagnostic(diagnostic_id::sc2282, location, {command_literal});

    return;

  default: break;
  }

  if (equals_position + 1 < command_literal.length &&
      command_literal[equals_position + 1] == '=')
  {
    actx.report_diagnostic(diagnostic_id::sc2272, location, {command_literal});

    return;
  }

  for (usize i = 0; i < equals_position; i += 1) {
    if (command_literal[i] != '$') continue;

    actx.report_diagnostic(diagnostic_id::sc2271, location, {command_literal});

    return;
  }
}

/* A name like [ holds a glob metacharacter that static_command_name rejects,
   so the literal text is taken separately for the test recognition. */
fn check_command_word_shape(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void
{
  let const &args = input.args;
  let const command_literal = input.command_literal;
  let const location = input.command_location();

  if (!command_literal.is_empty() && command_literal[0] == '-')
    actx.report_diagnostic(diagnostic_id::sc2215, location);

  let const equals_position = command_literal.find_character('=');

  if (equals_position.has_value() && command_literal.length > 1 &&
      !command_literal.starts_with(StringView{"[["}))
  {
    /* The word literal drops the quotes, so the source text tells a quoted
       assignment name apart from a bare one, shellcheck SC2276. */
    let const command_source = location.get_source_text(actx.source);
    if (command_source.has_value() && !command_source->is_empty() &&
        ((*command_source)[0] == '"' || (*command_source)[0] == '\''))
    {
      actx.report_diagnostic(diagnostic_id::sc2276, location,
                             {*command_source});
    } else {
      check_equals_bearing_command_name(actx, command_literal, *equals_position,
                                        location);
    }
  }

  if ((command_literal.starts_with(StringView{"["}) &&
       command_literal != "[") ||
      (command_literal.starts_with(StringView{"[["}) &&
       command_literal != "[["))
    actx.report_diagnostic(diagnostic_id::sc1035, location);

  if (command_literal.starts_with(StringView{"[["}) &&
      command_literal.find_character('=').has_value())
    actx.report_diagnostic(diagnostic_id::sc2077, location);

  if (command_literal.starts_with(StringView{"["}) && command_literal != "[" &&
      command_literal != "[[" && args.count() > 1)
  {
    String last_storage{heap_allocator()};
    let const last_raw = borrowed_token_text(args.back(), last_storage);
    if (!last_raw.is_empty() && last_raw[last_raw.length - 1] == ']')
      actx.report_diagnostic(diagnostic_id::sc1014, location);
  }

  /* The word literal drops the quotes, so the source text decides whether the
     bracket was written as syntax or as data. */
  if (args.count() >= 2 && command_literal != "[" && command_literal != "[[" &&
      args.back()->source_location().get_source_text(actx.source) ==
          StringView{"]"})
  {
    actx.report_diagnostic(diagnostic_id::sc2171,
                           args.back()->source_location());
  }

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"="})
    actx.report_diagnostic(diagnostic_id::sc2283, args[1]->source_location());

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"=="}) {
    actx.report_diagnostic(diagnostic_id::sc2284, args[1]->source_location(),
                           {command_literal});
  }

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"+="}) {
    actx.report_diagnostic(diagnostic_id::sc2285, args[1]->source_location(),
                           {command_literal});
  }
}

fn check_operand_lints_after_scan(AnalysisContext &actx,
                                  const command_lint_input &input) throws
    -> void
{
  if (input.command_is_shadowed) return;

  let const &args = input.args;

  switch (input.command_id()) {
  case command_name_id::Read: {
    let should_skip_option_operand = false;
    let should_take_array_target = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      if (should_skip_option_operand) {
        should_skip_option_operand = false;
        if (should_take_array_target) {
          should_take_array_target = false;
          let const target = operand_target_name(literal.view());
          if (!target.is_empty()) {
            actx.array_valued_names.add(target);
            actx.external_input_names.add(target);
          }
        }

        continue;
      }
      if (literal.view().length == 2 && literal.view()[0] == '-') {
        switch (literal.view()[1]) {
        case 'a':
          should_skip_option_operand = true;
          should_take_array_target = true;
          continue;

        case 'd':
        case 'i':
        case 'n':
        case 'N':
        case 'p':
        case 't':
        case 'u': should_skip_option_operand = true; continue;

        default: break;
        }
      }
      if (literal.view().starts_with("-")) continue;
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::VariableReference)
        actx.report_diagnostic(diagnostic_id::sc2229,
                               args[i]->source_location());

      if (actx.is_direct_pipeline_stage && !literal.view().starts_with("-")) {
        let const target = operand_target_name(literal.view());
        if (!target.is_empty()) {
          actx.report_diagnostic(diagnostic_id::sc2030_read,
                                 args[i]->source_location());
          actx.pipeline_lost_names.add(target);
        }
      }
      if (!literal.view().starts_with("-")) {
        let const target = operand_target_name(literal.view());
        if (!target.is_empty()) actx.external_input_names.add(target);
      }
    }
    break;
  }

  /* The client expands an ssh operand before the remote shell ever sees it,
     shellcheck SC2029. The host is the first plain operand, so the command that
     follows it is the part that runs remotely. */
  case command_name_id::Ssh: {
    let has_seen_host = false;
    let should_skip_option_value = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (should_skip_option_value) {
        should_skip_option_value = false;
        continue;
      }
      if (!view.is_empty() && view[0] == '-') {
        if (view.length >= 2 && ssh_option_takes_value(view[view.length - 1]))
          should_skip_option_value = true;
        continue;
      }
      if (!has_seen_host) {
        has_seen_host = true;
        continue;
      }

      for (let const &segment : word.segments)
        if (segment.kind == WordSegment::Kind::VariableReference ||
            segment.kind == WordSegment::Kind::CommandSubstitution)
        {
          actx.report_diagnostic(diagnostic_id::sc2029,
                                 args[i]->source_location());
          break;
        }
    }
    break;
  }

  /* su starts a fresh shell, so a function name never resolves there,
     shellcheck SC2032. A bare command operand without -c is SC2117. */
  case command_name_id::Su: {
    let is_command_value_next = false;
    let has_seen_user = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const literal = static_cast<const tokens::WordToken *>(args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      if (is_command_value_next) {
        let const command_word = leading_command_word(view);
        if (actx.defined_functions.contains(command_word)) {
          actx.report_diagnostic(diagnostic_id::sc2032,
                                 args[i]->source_location(), {command_word});
        }
        break;
      }
      if (!view.is_empty() && view[0] == '-') {
        if (view == "-c" || view == "--command") is_command_value_next = true;
        continue;
      }
      if (!has_seen_user) {
        has_seen_user = true;
        continue;
      }

      if (actx.defined_functions.contains(view)) {
        actx.report_diagnostic(diagnostic_id::sc2032,
                               args[i]->source_location(), {view});
      } else {
        actx.report_diagnostic(diagnostic_id::sc2117,
                               args[i]->source_location(), {view});
      }
      break;
    }
    break;
  }

  case command_name_id::Export: {
    let const should_check_cdpath = !args_have_short_flag(args, 'n');
    for (usize i = 1; i < args.count(); i++) {
      let const raw = args[i]->raw_string();
      if (should_check_cdpath &&
          (raw.view().starts_with(StringView{"CDPATH="}) ||
           raw.view() == "CDPATH"))
      {
        actx.report_diagnostic(diagnostic_id::exported_cdpath,
                               args[i]->source_location());
      }

      if (args[i]->kind() != Token::Kind::Word) continue;

      /* export $name exports whatever the value spells, shellcheck SC2163. A
         modifier or a brace form is left alone, since the name no longer spans
         the whole segment. */
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word.segments.count() != 1 ||
          word.segments[0].kind != WordSegment::Kind::VariableReference)
        continue;

      let const text = word.segments[0].text.view();
      let const name = operand_target_name(text);
      if (!name.is_empty() && name.length == text.length)
        actx.report_diagnostic(diagnostic_id::sc2163,
                               args[i]->source_location(), {name});
    }
    break;
  }

  case command_name_id::Unset:
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const raw = args[i]->raw_string();
      let const source_text =
          analysis_source_text(actx, args[i]->source_location());
      if (raw.view().find_character('[').has_value() &&
          raw.view().find_character(']').has_value() &&
          (source_text.is_empty() ||
           (source_text[0] != '\'' && source_text[0] != '"')))
        actx.report_diagnostic(diagnostic_id::sc2184,
                               args[i]->source_location());
    }
    break;

  case command_name_id::Find:
    for (usize i = 1; i + 1 < args.count(); i++) {
      let const predicate = args[i]->raw_string();
      if (predicate.view() == "-name" || predicate.view() == "-iname" ||
          predicate.view() == "-path" || predicate.view() == "-ipath" ||
          predicate.view() == "-regex")
      {
        if (args[i + 1]->kind() == Token::Kind::Word &&
            word_is_bare_glob(
                static_cast<const tokens::WordToken *>(args[i + 1])->word()))
          actx.report_diagnostic(diagnostic_id::sc2061,
                                 args[i + 1]->source_location());
      }

      if (predicate.view() == "-exec" || predicate.view() == "-execdir") {
        /* find launches the action itself, so a shell function is never found,
           shellcheck SC2033. */
        let const action = args[i + 1]->raw_string();
        if (actx.defined_functions.contains(action.view())) {
          actx.report_diagnostic(diagnostic_id::sc2033,
                                 args[i + 1]->source_location(),
                                 {action.view()});
        }

        if (i + 3 < args.count()) {
          let const shell_flag = args[i + 2]->raw_string();
          let const script = args[i + 3]->raw_string();
          if ((action.view() == "sh" || action.view() == "bash") &&
              shell_flag.view() == "-c" &&
              view_contains(script.view(), StringView{"{}"}))
            actx.report_diagnostic(diagnostic_id::sc2156,
                                   args[i + 3]->source_location());
        }
      }
    }
    break;

  case command_name_id::Tr:
    for (usize i = 1; i < args.count() && i <= 2; i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word_is_bare_glob(word))
        actx.report_diagnostic(diagnostic_id::sc2060,
                               args[i]->source_location());
    }
    break;

  case command_name_id::Let:
    for (usize i = 1; i < args.count(); i++) {
      let const expression = args[i]->raw_string();
      if (arithmetic_reads_external_input(actx, expression.view()))
        actx.report_diagnostic(diagnostic_id::external_arithmetic_input,
                               args[i]->source_location());

      check_arithmetic_expression_lints(actx, expression.view(),
                                        args[i]->source_location());

      if (actx.shebang_is_posix_sh) {
        check_posix_arithmetic_operators(actx, expression.view(),
                                         args[i]->source_location());
      }
    }
    break;

  case command_name_id::Printf: {
    usize format_index = 1;
    if (format_index < args.count()) {
      let const leading = args[format_index]->raw_string();
      if (leading.view() == "-v") {
        format_index += 2;
      } else if (leading.view() == "--") {
        format_index++;
      }
    }

    if (format_index < args.count() &&
        args[format_index]->kind() == Token::Kind::Word)
    {
      let const &format_word =
          static_cast<const tokens::WordToken *>(args[format_index])->word();
      if (word_is_fully_literal(format_word)) {
        let const format = format_word.to_literal_string();
        let has_quote_conversion = false;
        let const consumed =
            printf_consumed_argument_count(format.view(), has_quote_conversion);
        let const available = args.count() - format_index - 1;
        if (consumed > available) {
          actx.report_diagnostic(diagnostic_id::sc2183,
                                 args[format_index]->source_location());
        } else if (consumed == 0 && available > 0) {
          actx.report_diagnostic(diagnostic_id::sc2182,
                                 args[format_index]->source_location());
        }

        if (has_quote_conversion && actx.shebang_is_posix_sh) {
          actx.report_diagnostic(diagnostic_id::sc3050,
                                 args[format_index]->source_location());
        }
      }
    }
    break;
  }

  case command_name_id::Sudo: {
    for (let const &redirection : input.redirections)
      if (redirection.target != nullptr)
        actx.report_diagnostic(diagnostic_id::sc2024_redirection,
                               redirection.target->source_location());

    let has_seen_command_word = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word_is_bare_glob(word)) {
        actx.report_diagnostic(diagnostic_id::sc2024_glob,
                               args[i]->source_location());
      }

      if (has_seen_command_word) continue;

      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (view.is_empty() || view[0] == '-') continue;

      has_seen_command_word = true;

      /* sudo starts an external program, so a builtin with no program of the
         same name is never reached, shellcheck SC2232. */
      if (SHELL_ONLY_BUILTINS.contains(view))
        actx.report_diagnostic(diagnostic_id::sc2232,
                               args[i]->source_location(), {view});
    }
    break;
  }

  default: break;
  }
}

namespace {

pure fn signal_name_is_unblockable(StringView bare) wontthrow -> bool
{
  return bare == "KILL" || bare == "STOP";
}

fn check_trap_condition_operands(AnalysisContext &actx,
                                 const ArrayList<const Token *> &args,
                                 bool is_posix) throws -> void
{
  for (usize i = 2; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;

    let const literal = static_cast<const tokens::WordToken *>(args[i])
                            ->word()
                            .to_literal_string();
    let const view = literal.view();
    if (view.is_empty()) continue;

    if (view == "ERR") {
      if (is_posix) {
        actx.report_diagnostic(diagnostic_id::sc3047,
                               args[i]->source_location());
      }
      continue;
    }

    /* A numeric condition is a signal number, and only the first few numbers
       are fixed by POSIX, shellcheck SC2172. */
    if (view.is_all_decimal_digits() && view.length <= 3) {
      usize number = 0;
      for (usize position = 0; position < view.length; position++)
        number = number * 10 + static_cast<usize>(view[position] - '0');

      actx.report_diagnostic(diagnostic_id::sc2172, args[i]->source_location(),
                             {view});

      let const name = os::signal_name_from_number(static_cast<i32>(number));
      if (name.has_value() && signal_name_is_unblockable(name->view())) {
        actx.report_diagnostic(diagnostic_id::sc2173,
                               args[i]->source_location(), {view});
      }

      continue;
    }

    let uppercase = String{heap_allocator()};
    let has_lowercase = false;

    for (usize position = 0; position < view.length; position++) {
      let const byte = view[position];
      if (byte >= 'a' && byte <= 'z') {
        has_lowercase = true;
        uppercase.push(static_cast<char>(byte - 'a' + 'A'));
      } else {
        uppercase.push(byte);
      }
    }

    let const has_sig_prefix =
        uppercase.view().starts_with(StringView{"SIG"}) && view.length > 3;
    let const bare =
        has_sig_prefix ? uppercase.view().substring(3) : uppercase.view();
    let const names_a_signal =
        bare == "EXIT" || os::signal_number_from_name(bare).has_value();

    /* The kernel delivers KILL and STOP without consulting the handler table,
       shellcheck SC2173. */
    if (names_a_signal && signal_name_is_unblockable(bare)) {
      actx.report_diagnostic(diagnostic_id::sc2173, args[i]->source_location(),
                             {view});
    }

    if (!is_posix) continue;

    if (has_sig_prefix && names_a_signal) {
      actx.report_diagnostic(diagnostic_id::sc3048, args[i]->source_location(),
                             {view});
    }

    if (has_lowercase && names_a_signal) {
      actx.report_diagnostic(diagnostic_id::sc3049, args[i]->source_location(),
                             {view});
    }
  }
}

} /* namespace */

fn check_command_name_lints(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void
{
  let const &args = input.args;
  let const location = input.command_location();

  /* An unquoted variable inside a test silently breaks when it is empty or
     splits. This stays a warning even at the strict default, since the split
     may be intended. */
  if (input.is_in_group(COMMAND_GROUP_TEST)) {
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      for (let const &segment : word.segments) {
        if (segment.kind == WordSegment::Kind::VariableReference &&
            segment.is_split_eligible())
        {
          let const operand_location = args[i]->source_location();
          actx.report_diagnostic(
              diagnostic_id::sc2086_test,
              expansion_location_with_sigil(
                  actx, segment.get_source_location(operand_location.filename)
                            .value_or(operand_location)));
          break;
        }
      }
    }
  }

  if (input.command_is_shadowed) return;

  let const is_posix = actx.shebang_is_posix_sh;

  switch (input.command_id()) {
  case command_name_id::Read:
    /* read without -r lets a backslash escape the next byte, mangling a line,
       shellcheck SC2162. */
    if (!args_have_short_flag(args, 'r'))
      actx.report_diagnostic(diagnostic_id::sc2162,
                             input.command_source_location);
    break;

  case command_name_id::Echo:
    if (is_posix && args.count() >= 2 && args[1]->kind() == Token::Kind::Word) {
      let const flag = static_cast<const tokens::WordToken *>(args[1])
                           ->word()
                           .to_literal_string();
      let const view = flag.view();
      if (view == "-e" || view == "-n" || view == "-E" || view == "-ne" ||
          view == "-en")
        actx.report_diagnostic(diagnostic_id::sc3037,
                               args[1]->source_location(), {view});
    }
    break;

  case command_name_id::Declare:
    if (is_posix)
      actx.report_diagnostic(diagnostic_id::sc3044, location,
                             {input.command_literal});
    break;

  case command_name_id::Typeset:
    if (is_posix) {
      actx.report_diagnostic(diagnostic_id::sc3044, location,
                             {input.command_literal});
    } else {
      actx.report_diagnostic(diagnostic_id::typeset_spelling, location);
    }
    break;

  case command_name_id::Source:
    if (is_posix) actx.report_diagnostic(diagnostic_id::sc3046, location);
    break;

  case command_name_id::Local:
    if (is_posix) actx.report_diagnostic(diagnostic_id::sc3043, location);
    if (actx.function_scope_depth == 0 && !actx.is_command_status_observed)
      actx.report_diagnostic(diagnostic_id::sc2168, location);
    break;

  /* mapfile and its readarray alias are bash array builtins, shellcheck
     SC3030. */
  case command_name_id::Mapfile:
  case command_name_id::Readarray: {
    if (is_posix)
      actx.report_diagnostic(diagnostic_id::sc3030, location,
                             {input.command_literal});

    /* The filled array is the last plain operand, so the index is kept and its
       text is read once the loop has settled on it. */
    let filled_name_index = args.count();
    let should_skip_option_operand = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const literal = static_cast<const tokens::WordToken *>(args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      if (should_skip_option_operand) {
        should_skip_option_operand = false;
        continue;
      }
      if (view.length == 2 && view[0] == '-') {
        switch (view[1]) {
        case 'C':
        case 'c':
        case 'd':
        case 'n':
        case 'O':
        case 's':
        case 'u': should_skip_option_operand = true; continue;

        default: continue;
        }
      }
      if (view.length >= 2 && view[0] == '-') continue;
      if (!operand_target_name(view).is_empty()) filled_name_index = i;
    }

    if (filled_name_index == args.count()) {
      actx.array_valued_names.add(StringView{"MAPFILE"});
      break;
    }

    let const filled =
        static_cast<const tokens::WordToken *>(args[filled_name_index])
            ->word()
            .to_literal_string();
    actx.array_valued_names.add(operand_target_name(filled.view()));
    break;
  }

  /* The optstring and the name reach the case in the loop body, so the call is
     recorded for shellcheck SC2213, SC2214 and SC2220. The views point into the
     syntax tree, which outlives the analysis. */
  case command_name_id::Getopts: {
    if (args.count() < 3) break;
    if (args[1]->kind() != Token::Kind::Word) break;
    if (args[2]->kind() != Token::Kind::Word) break;

    let const &optstring_word =
        static_cast<const tokens::WordToken *>(args[1])->word();
    let const &name_word =
        static_cast<const tokens::WordToken *>(args[2])->word();
    if (optstring_word.segments.count() != 1) break;
    if (name_word.segments.count() != 1) break;
    if (!word_is_fully_literal(optstring_word)) break;
    if (!word_is_fully_literal(name_word)) break;

    actx.active_getopts.optstring = optstring_word.segments[0].text.view();
    actx.active_getopts.variable_name = name_word.segments[0].text.view();
    actx.active_getopts.location = args[1]->source_location();
    break;
  }

  /* Nothing surrounds a break outside a loop, shellcheck SC2104 and SC2105. A
     function body starts its own loop depth, so a call from inside a loop does
     not count. */
  case command_name_id::Break:
  case command_name_id::Continue:
    if (actx.loop_body_depth == 0) {
      actx.report_diagnostic(actx.function_scope_depth > 0
                                 ? diagnostic_id::sc2104
                                 : diagnostic_id::sc2105,
                             location, {input.command_literal});
    } else if (actx.is_direct_pipeline_stage) {
      actx.report_diagnostic(diagnostic_id::sc2106, location,
                             {input.command_literal});
    }
    break;

  /* set changes the options and the positional parameters, so a name=value
     operand assigns nothing, shellcheck SC2121. */
  case command_name_id::Set:
    if (args.count() >= 2) {
      /* The operand arrives as an Assignment token, since name=value keeps that
         shape wherever it stands. */
      let const literal = args[1]->raw_string();
      let const view = literal.view();
      if (!view.is_empty() && view[0] != '-' && view[0] != '+') {
        let const target = operand_target_name(view);
        if (!target.is_empty() && target.length < view.length &&
            view[target.length] == '=')
        {
          actx.report_diagnostic(diagnostic_id::sc2121,
                                 args[1]->source_location(), {target});
        }
      }
    }
    break;

  /* The POSIX dot command reads a file and takes nothing else, shellcheck
     SC2240. The source spelling is already reported as SC3046. */
  case command_name_id::Dot:
    if (is_posix && args.count() > 2) {
      let const operand = args[2]->raw_string();
      actx.report_diagnostic(diagnostic_id::sc2240, args[2]->source_location(),
                             {operand.view()});
    }
    break;

  case command_name_id::Which:
    actx.report_diagnostic(diagnostic_id::sc2230, location);
    break;

  case command_name_id::Egrep:
    actx.report_diagnostic(diagnostic_id::sc2196, location);
    break;

  case command_name_id::Fgrep:
    actx.report_diagnostic(diagnostic_id::sc2197, location);
    break;

  case command_name_id::Expr:
    actx.report_diagnostic(diagnostic_id::sc2003, location);
    break;

  /* A double-quoted trap action expands at set time, not when it fires,
     shellcheck SC2064. The action is the first operand. */
  case command_name_id::Trap:
    if (args.count() >= 2 && args[1]->kind() == Token::Kind::Word) {
      let const &action =
          static_cast<const tokens::WordToken *>(args[1])->word();
      let action_expands_now = false;
      for (let const &segment : action.segments)
        if (segment.is_in_double_quotes &&
            (segment.kind == WordSegment::Kind::VariableReference ||
             segment.kind == WordSegment::Kind::CommandSubstitution))
        {
          action_expands_now = true;
          break;
        }
      if (action_expands_now)
        actx.report_diagnostic(diagnostic_id::sc2064,
                               args[1]->source_location());
    }
    check_trap_condition_operands(actx, args, is_posix);
    break;

  case command_name_id::Exec:
    if (is_posix && args.count() >= 2 && args[1]->kind() == Token::Kind::Word) {
      let const flag = static_cast<const tokens::WordToken *>(args[1])
                           ->word()
                           .to_literal_string();
      let const view = flag.view();
      if (view.length >= 2 && view[0] == '-' && view != "--")
        actx.report_diagnostic(diagnostic_id::sc3038,
                               args[1]->source_location(), {view});
    }
    break;

  case command_name_id::Let:
    if (is_posix) actx.report_diagnostic(diagnostic_id::sc3039, location);

    if (args.count() >= 2)
      actx.report_diagnostic(diagnostic_id::sc2219, location);

    break;

  case command_name_id::Printf: {
    if (is_posix && args.count() >= 2 && args[1]->kind() == Token::Kind::Word &&
        static_cast<const tokens::WordToken *>(args[1])
                ->word()
                .to_literal_string()
                .view() == "-v")
    {
      actx.report_diagnostic(diagnostic_id::sc3045, args[1]->source_location());
    }

    /* A variable or command substitution in the printf format lets the data
       control the directives, shellcheck SC2059. The format is the first
       non-option word, and a -- forces the next word as the format. */
    usize format_index = 0;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) {
        format_index = i;
        break;
      }
      let const literal = static_cast<const tokens::WordToken *>(args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      if (view == "--") {
        if (i + 1 < args.count()) format_index = i + 1;
        break;
      }
      if (!(view.length >= 1 && view[0] == '-')) {
        format_index = i;
        break;
      }
    }

    if (format_index != 0 && args[format_index]->kind() == Token::Kind::Word) {
      let const &format =
          static_cast<const tokens::WordToken *>(args[format_index])->word();
      bool format_has_expansion = false;
      for (let const &segment : format.segments) {
        if (segment.kind == WordSegment::Kind::VariableReference ||
            segment.kind == WordSegment::Kind::CommandSubstitution)
        {
          format_has_expansion = true;
          break;
        }
      }
      if (format_has_expansion)
        actx.report_diagnostic(diagnostic_id::sc2059,
                               args[format_index]->source_location());
    }
    break;
  }

  default: break;
  }
}

fn check_command_value_lints(AnalysisContext &actx,
                             const command_lint_input &input) throws -> void
{
  if (input.command_is_shadowed) return;

  let const &args = input.args;

  /* A declaration builtin that assigns from a command substitution, such as
     local x=$(cmd), reports its own success rather than the command's status,
     shellcheck SC2155. The value rides an Assignment token. */
  if (input.is_in_group(COMMAND_GROUP_ASSIGNMENT_BUILTIN)) {
    let has_reported_substitution_value = false;
    let does_declare_an_array = false;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() == Token::Kind::Assignment) {
        let const *assignment =
            static_cast<const tokens::Assignment *>(args[i]);
        if (does_declare_an_array)
          actx.array_valued_names.add(assignment->key().view());

        if (has_reported_substitution_value) continue;

        let value_has_substitution = false;
        for (let const &segment : assignment->value_word().segments)
          if (segment.kind == WordSegment::Kind::CommandSubstitution) {
            value_has_substitution = true;
            break;
          }
        if (!value_has_substitution) continue;

        actx.report_diagnostic(diagnostic_id::sc2155,
                               args[i]->source_location());
        has_reported_substitution_value = true;
        continue;
      }

      if (args[i]->kind() != Token::Kind::Word) continue;

      let const literal = static_cast<const tokens::WordToken *>(args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();

      /* A grouped -aA flag list declares an array, and every other letter in
         the group changes an unrelated attribute. */
      if (view.length >= 2 && view[0] == '-') {
        if (view.find_character('a').has_value() ||
            view.find_character('A').has_value())
        {
          does_declare_an_array = true;
        }

        continue;
      }

      if (!does_declare_an_array) continue;

      let const target = operand_target_name(view);
      if (!target.is_empty()) actx.array_valued_names.add(target);
    }
  }

  switch (input.command_id()) {
  /* rm -r with a "$var/" operand deletes / when the variable is empty,
     shellcheck SC2115. A literal top-level system directory is SC2114. */
  case command_name_id::Rm:
    if (!args_have_short_flag(args, 'r')) break;

    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      if (word.segments.count() >= 2 &&
          word.segments[0].kind == WordSegment::Kind::VariableReference &&
          !word.segments[0].text.view().find_character(':').has_value() &&
          !word.segments[1].text.is_empty() && word.segments[1].text[0] == '/')
      {
        actx.report_diagnostic(diagnostic_id::sc2115,
                               args[i]->source_location(),
                               {word.segments[0].text.view()});
      }
      if (word_is_fully_literal(word)) {
        let const literal = word.to_literal_string();
        if (SYSTEM_DIRECTORIES.contains(literal.view()))
          actx.report_diagnostic(diagnostic_id::sc2114,
                                 args[i]->source_location(), {literal.view()});
      }
    }
    break;

  /* The grep pattern lints. An unquoted pattern with a glob metacharacter is
     SC2062, a pattern with a leading * that has nothing to repeat is SC2063,
     and a glob-shaped pattern whose star repeats one character is SC2022.
     The pattern is the first word past the options. */
  case command_name_id::Grep:
  case command_name_id::Egrep:
  case command_name_id::Fgrep: {
    let is_fixed_string_mode = input.command_id() == command_name_id::Fgrep;
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (view.length >= 1 && view[0] == '-') {
        if (view.find_character('F').has_value() ||
            view.starts_with(StringView{"--fixed-strings"}))
        {
          is_fixed_string_mode = true;
        }
        continue;
      }
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::UnquotedText &&
          word.segments[0].has_glob_metacharacter())
      {
        actx.report_diagnostic(diagnostic_id::sc2062,
                               args[i]->source_location());
      } else if (!view.is_empty() && view[0] == '*') {
        actx.report_diagnostic(diagnostic_id::sc2063,
                               args[i]->source_location());
      } else if (!is_fixed_string_mode && view_is_glob_shaped_pattern(view)) {
        actx.report_diagnostic(diagnostic_id::sc2022,
                               args[i]->source_location(), {view});
      }
      break;
    }
    break;
  }

  /* mkdir -pm applies the mode only to the deepest directory, shellcheck
     SC2174. */
  case command_name_id::Mkdir:
    if (args_have_short_flag(args, 'p') && args_have_short_flag(args, 'm'))
      actx.report_diagnostic(diagnostic_id::sc2174, input.command_location());
    break;

  /* An exit or return code outside the literal 0-255 shape errors or wraps
     modulo 256, shellcheck SC2242. */
  case command_name_id::Exit:
  case command_name_id::Return: {
    let const is_return = input.command_id() == command_name_id::Return;

    /* One status is all either builtin reads, shellcheck SC2151 and SC2241. */
    if (args.count() > 2) {
      actx.report_diagnostic(is_return ? diagnostic_id::sc2151
                                       : diagnostic_id::sc2241,
                             args[2]->source_location());
    }

    if (args.count() >= 2 && args[1]->kind() == Token::Kind::Word) {
      let const &operand =
          static_cast<const tokens::WordToken *>(args[1])->word();
      if (word_is_fully_literal(operand)) {
        let const literal = operand.to_literal_string();
        let const view = literal.view();
        let is_in_range = view_is_integer_literal(view) && view[0] != '-';
        if (is_in_range) {
          let const parsed_code = view.to<i64>();
          is_in_range = !parsed_code.is_error() && parsed_code.value() <= 255;
        }
        if (!is_in_range)
          actx.report_diagnostic(diagnostic_id::sc2242,
                                 args[1]->source_location(),
                                 {view, input.command_literal});
      } else if (is_return && operand.segments.count() == 1 &&
                 operand.segments[0].kind ==
                     WordSegment::Kind::CommandSubstitution)
      {
        /* Command output stands where a status belongs, shellcheck SC2152. */
        actx.report_diagnostic(diagnostic_id::sc2152,
                               args[1]->source_location());
      }
    }
    break;
  }

  /* A move, a copy or a link given one operand names no destination,
     shellcheck SC2224, SC2225 and SC2226. */
  case command_name_id::Cp:
  case command_name_id::Ln:
  case command_name_id::Mv: {
    let const operand = single_literal_file_operand(args);
    if (!operand.has_value()) break;

    let missing_destination = diagnostic_id::sc2224;
    switch (input.command_id()) {
    case command_name_id::Cp:
      missing_destination = diagnostic_id::sc2225;
      break;
    case command_name_id::Ln:
      missing_destination = diagnostic_id::sc2226;
      break;
    default: break;
    }

    actx.report_diagnostic(missing_destination, (*operand)->source_location(),
                           {(*operand)->raw_string().view()});
    break;
  }

  /* GNU xargs kept -i for compatibility and documents -I in its place,
     shellcheck SC2267. */
  case command_name_id::Xargs:
    if (args_have_short_flag(args, 'i'))
      actx.report_diagnostic(diagnostic_id::sc2267, input.command_location());

    /* xargs launches the program itself, so a shell function is never found,
       shellcheck SC2033. */
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;

      let const literal = static_cast<const tokens::WordToken *>(args[i])
                              ->word()
                              .to_literal_string();
      let const view = literal.view();
      if (!view.is_empty() && view[0] == '-') continue;

      if (actx.defined_functions.contains(view)) {
        actx.report_diagnostic(diagnostic_id::sc2033,
                               args[i]->source_location(), {view});
      }
      break;
    }
    break;

  default: break;
  }
}

namespace {

fn check_posix_redirection_portability(AnalysisContext &actx,
                                       const Redirection &redirection,
                                       SourceLocation fallback_location) throws
    -> void
{
  let const do_get_location = [&]() -> SourceLocation {
    return redirection.target != nullptr ? redirection.target->source_location()
                                         : fallback_location;
  };

  if (redirection.fd_allocation_name_token != nullptr) {
    let const name = redirection.fd_allocation_name_token->raw_view();
    actx.report_diagnostic(
        diagnostic_id::sc3022,
        redirection.fd_allocation_name_token->source_location(),
        {name.value_or(StringView{})});
  }

  let const descriptor =
      redirection.fd > redirection.dup_fd ? redirection.fd : redirection.dup_fd;
  if (descriptor > 9) {
    let const descriptor_text = String::from(descriptor, heap_allocator());
    actx.report_diagnostic(diagnostic_id::sc3023, do_get_location(),
                           {descriptor_text.view()});
  }

  if (redirection.is_both_streams_spelling)
    actx.report_diagnostic(diagnostic_id::sc3020, do_get_location());

  if (redirection.target == nullptr) return;

  let const target_text = redirection.target->raw_view();
  if (!target_text.has_value()) return;

  let const view = *target_text;
  if (redirection.kind == Redirection::Kind::DuplicateOutput &&
      redirection.can_dup_be_filename)
  {
    actx.report_diagnostic(diagnostic_id::sc3021, do_get_location(), {view});
  }

  if (view.starts_with(StringView{"/dev/tcp/"}) ||
      view.starts_with(StringView{"/dev/udp/"}))
  {
    actx.report_diagnostic(diagnostic_id::sc3025, do_get_location(), {view});
  }
}

/* A bare word naming one of these programs is almost always a missing command
   substitution or a missing pipe, shellcheck SC2209 and SC2238. Words that read
   naturally as data, such as test, id, set, true, and echo, are left out. */
constexpr PackedStringKey COMMAND_NAME_VALUE_KEYS[] = {
    SSK("awk"),   SSK("cat"),    SSK("chmod"), SSK("chown"), SSK("cp"),
    SSK("curl"),  SSK("docker"), SSK("git"),   SSK("grep"),  SSK("hostname"),
    SSK("ln"),    SSK("ls"),     SSK("mkdir"), SSK("mv"),    SSK("printf"),
    SSK("pwd"),   SSK("rm"),     SSK("rmdir"), SSK("sed"),   SSK("ssh"),
    SSK("sudo"),  SSK("touch"),  SSK("tr"),    SSK("uname"), SSK("whoami"),
    SSK("xargs"),
};
constexpr StaticStringSet COMMAND_NAME_VALUES{COMMAND_NAME_VALUE_KEYS};

cold fn plain_output_redirection_spelling(Redirection::Kind kind) wontthrow
    -> Maybe<StringView>
{
  switch (kind) {
  case Redirection::Kind::TruncateOutput: return StringView{">"};
  case Redirection::Kind::TruncateOutputOverride: return StringView{">|"};
  case Redirection::Kind::AppendOutput: return StringView{">>"};
  default: return None;
  }
}

} /* namespace */

/* The redirection lints. 2>&1 before the stdout file redirect is SC2069,
   reading and truncating the same file is SC2094, an input redirect into a
   non-stdin command is SC2217. */
fn check_redirection_lints(AnalysisContext &actx,
                           const command_lint_input &input) throws -> void
{
  let saw_stderr_to_stdout = false;
  /* An owned String, since the view of a to_literal_string() temporary would
     dangle past the statement. */
  String read_target{heap_allocator()};
  const Token *read_token = nullptr;
  let const is_test_command =
      input.is_in_group(COMMAND_GROUP_TEST) && !input.command_is_shadowed;
  /* Descriptors 0 through 9 are the ones a script writes, and the location of
     the first claim is kept so the second claim can point back at it. */
  SourceLocation claimed_fd_locations[10]{};
  u16 claimed_fd_mask = 0;

  for (let const &redirection : input.redirections) {
    if (redirection.kind == Redirection::Kind::DuplicateOutput &&
        redirection.fd == 2 && redirection.dup_fd == 1)
    {
      saw_stderr_to_stdout = true;
      continue;
    }

    let const is_posix = actx.shebang_is_posix_sh;
    if (is_posix) {
      check_posix_redirection_portability(actx, redirection,
                                          input.command_location());
    }

    /* A descriptor points at one file, so a second claim silently wins and the
       first is lost, shellcheck SC2261. */
    if (redirection.claims_descriptor() && redirection.fd >= 0 &&
        redirection.fd < 10 && redirection.target != nullptr)
    {
      let const fd_bit = static_cast<u16>(1u << redirection.fd);
      if ((claimed_fd_mask & fd_bit) != 0) {
        actx.report_diagnostic(
            diagnostic_id::sc2261, redirection.target->source_location(),
            {redirection.target->raw_view().value_or(StringView{})},
            claimed_fd_locations[redirection.fd]);
      } else {
        claimed_fd_mask |= fd_bit;
        claimed_fd_locations[redirection.fd] =
            redirection.target->source_location();
      }
    }

    /* The local shell expands an unquoted here document body before ssh sends
       it, so the remote host receives values from this host, shellcheck
       SC2087. */
    if (redirection.kind == Redirection::Kind::Heredoc &&
        redirection.should_expand_heredoc &&
        input.command_id() == command_name_id::Ssh &&
        !input.command_is_shadowed)
    {
      actx.report_diagnostic(diagnostic_id::sc2087,
                             redirection.target != nullptr
                                 ? redirection.target->source_location()
                                 : input.command_location());
    }

    let const is_file_output =
        redirection.kind == Redirection::Kind::TruncateOutput ||
        redirection.kind == Redirection::Kind::TruncateOutputOverride;
    if (is_file_output && redirection.fd == 1 && saw_stderr_to_stdout &&
        redirection.target != nullptr)
    {
      actx.report_diagnostic(diagnostic_id::sc2069,
                             redirection.target->source_location());
    }

    if (redirection.target != nullptr) {
      let const output_spelling =
          plain_output_redirection_spelling(redirection.kind);
      let const is_input_redirection =
          redirection.kind == Redirection::Kind::ReadInput;

      if (is_test_command) {
        if (output_spelling.has_value()) {
          actx.report_diagnostic(diagnostic_id::sc2065,
                                 redirection.target->source_location(),
                                 {*output_spelling});
        }
        if (is_input_redirection) {
          actx.report_diagnostic(diagnostic_id::sc2073,
                                 redirection.target->source_location());
        }
      } else if (output_spelling.has_value() || is_input_redirection) {
        let const digits = redirection.target->raw_view();
        if (digits.has_value() && !digits->is_empty() &&
            digits->is_all_decimal_digits())
        {
          actx.report_diagnostic(diagnostic_id::sc2210,
                                 redirection.target->source_location(),
                                 {*digits});
        }

        /* Quoting the name states that a file is meant, and the word literal
           drops the quotes, so the source text decides, shellcheck SC2238. */
        if (digits.has_value() && COMMAND_NAME_VALUES.contains(*digits)) {
          let const target_source =
              redirection.target->source_location().get_source_text(
                  actx.source);
          if (target_source.has_value() && !target_source->is_empty() &&
              (*target_source)[0] != '"' && (*target_source)[0] != '\'')
          {
            actx.report_diagnostic(diagnostic_id::sc2238,
                                   redirection.target->source_location(),
                                   {*digits});
          }
        }
      }
    }

    if (redirection.target != nullptr &&
        redirection.target->kind() == Token::Kind::Word)
    {
      let const &target_word =
          static_cast<const tokens::WordToken *>(redirection.target)->word();
      let has_glob_target = false;

      for (let const &segment : target_word.segments) {
        if (is_posix && !has_glob_target) {
          has_glob_target =
              segment.has_live_glob_chars() && segment.has_glob_metacharacter();
        }

        if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
            (view_contains(segment.text.view(), StringView{"++"}) ||
             view_contains(segment.text.view(), StringView{"--"}) ||
             segment.text.view().find_character('=').has_value()))
        {
          actx.report_diagnostic(diagnostic_id::sc2257,
                                 redirection.target->source_location());
          break;
        }
      }

      if (has_glob_target) {
        actx.report_diagnostic(
            diagnostic_id::sc3031, redirection.target->source_location(),
            {redirection.target->raw_view().value_or(StringView{})});
      }
    }
    if (redirection.kind == Redirection::Kind::ReadInput &&
        redirection.target != nullptr &&
        redirection.target->kind() == Token::Kind::Word)
    {
      read_target = static_cast<const tokens::WordToken *>(redirection.target)
                        ->word()
                        .to_literal_string();
      read_token = redirection.target;
    }
    if (is_file_output && redirection.target != nullptr &&
        redirection.target->kind() == Token::Kind::Word &&
        read_token != nullptr)
    {
      let const write_target =
          static_cast<const tokens::WordToken *>(redirection.target)
              ->word()
              .to_literal_string();
      if (!read_target.is_empty() && write_target.view() == read_target.view())
      {
        actx.report_diagnostic(
            diagnostic_id::sc2094, redirection.target->source_location(),
            {read_target.view()}, read_token->source_location());
      }
    }
  }

  if (input.redirections.is_empty() || input.command_is_shadowed) return;
  if (!input.is_in_group(COMMAND_GROUP_NON_STDIN_READER)) return;
  if (args_have_stdin_operand(input.args)) return;

  for (let const &redirection : input.redirections)
    if (redirection.kind == Redirection::Kind::ReadInput ||
        redirection.kind == Redirection::Kind::Heredoc ||
        redirection.kind == Redirection::Kind::HereString)
    {
      actx.report_diagnostic(diagnostic_id::sc2217, input.command_location(),
                             {input.command_literal});
      break;
    }
}

fn check_test_operand_lints(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void
{
  if (!input.is_in_group(COMMAND_GROUP_TEST) || input.command_is_shadowed)
    return;

  let const &args = input.args;
  let const is_posix = actx.shebang_is_posix_sh;

  /* The operand range excludes the closing bracket, so the operator loop and
     the operand loop share one bound. */
  usize operand_end = args.count();
  bool bracket_form_is_closed = true;
  if (input.command_id() == command_name_id::SingleBracket ||
      input.command_id() == command_name_id::DoubleBracket)
  {
    bracket_form_is_closed =
        args.count() >= 2 &&
        args[args.count() - 1]->kind() == Token::Kind::Word &&
        static_cast<const tokens::WordToken *>(args[args.count() - 1])
                ->word()
                .to_literal_string()
                .view() ==
            (input.command_id() == command_name_id::SingleBracket ? "]" : "]]");
    if (bracket_form_is_closed) operand_end = args.count() - 1;
  }

  /* Obsolescent or redundant test forms. -a or -o joining two conditions is
     SC2166, warned only past the first operand and not after a !. A negated -z
     or -n is SC2236 and SC2237. */
  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const literal = static_cast<const tokens::WordToken *>(args[i])
                            ->word()
                            .to_literal_string();
    let const view = literal.view();
    /* The literal of the previous word, empty for a non-word predecessor. */
    let const previous_literal =
        args[i - 1]->kind() == Token::Kind::Word
            ? static_cast<const tokens::WordToken *>(args[i - 1])
                  ->word()
                  .to_literal_string()
            : String{heap_allocator()};
    /* == is a bashism in test, shellcheck SC3014, warned only when == sits in
       the operator slot so [ x = == ] comparing the literal == is left
       alone. */
    if (view == "==" && i >= 2 &&
        !is_test_binary_operator_word(previous_literal.view()))
    {
      actx.report_diagnostic(diagnostic_id::sc3014, args[i]->source_location());
    }
    let const previous_is_bang = previous_literal.view() == "!";

    /* A dash-led word that names no operator, shellcheck SC2057 and SC2058.
       The word after a condition opener sits in the unary slot, the word after
       a plain operand sits in the binary slot, and the word after a known
       operator is an operand. */
    if (view_looks_like_test_operator(view) && view[1] != '-' &&
        !is_known_test_operator_word(view))
    {
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const is_unary_slot =
          i == 1 || is_test_condition_opener_word(previous_literal.view());
      let is_string_operand = false;
      if (is_unary_slot && i + 1 < args.count() &&
          args[i + 1]->kind() == Token::Kind::Word)
      {
        /* A three-word test compares strings when the middle word is a binary
           operator, so [ -verbose = "$1" ] holds an operand. */
        let const next = static_cast<const tokens::WordToken *>(args[i + 1])
                             ->word()
                             .to_literal_string();
        is_string_operand = is_test_binary_operator_word(next.view());
      }

      if (!is_string_operand && word_is_fully_literal(word)) {
        if (is_unary_slot) {
          actx.report_diagnostic(diagnostic_id::sc2058,
                                 args[i]->source_location(), {view});
        } else if (!is_known_test_operator_word(previous_literal.view())) {
          actx.report_diagnostic(diagnostic_id::sc2057,
                                 args[i]->source_location(), {view});
        }
      }
    }

    if (view == ">=" || view == "<=") {
      actx.report_diagnostic(diagnostic_id::sc2122, args[i]->source_location(),
                             {view});
    }

    /* Both sides of a comparison are written out, so the answer is fixed,
       shellcheck SC2050. The file comparisons read the filesystem and are left
       alone. */
    if (i >= 2 && i + 1 < operand_end && is_test_binary_operator_word(view) &&
        !is_test_file_comparison_word(view) &&
        !is_test_binary_operator_word(previous_literal.view()) &&
        args[i - 1]->kind() == Token::Kind::Word &&
        args[i + 1]->kind() == Token::Kind::Word)
    {
      let const &left =
          static_cast<const tokens::WordToken *>(args[i - 1])->word();
      let const &right =
          static_cast<const tokens::WordToken *>(args[i + 1])->word();
      if (word_is_fully_literal(left) && word_is_fully_literal(right)) {
        /* A bracket test receives its operands already expanded, so a glob or a
           brace list on either side is not the text that is compared. */
        let const left_shape = classify_test_operand(left);
        let const right_shape = classify_test_operand(right);
        let const is_expanded_before_test =
            left_shape.has_unquoted_glob || right_shape.has_unquoted_glob ||
            left_shape.has_brace_expansion || right_shape.has_brace_expansion;

        if (!is_expanded_before_test) {
          actx.report_diagnostic(
              diagnostic_id::sc2050,
              location_spanning(args[i - 1]->source_location(),
                                args[i + 1]->source_location()),
              {view});
        }
      }
    }

    /* The bracket test takes one word per operand, so an operand that expands
       to several words leaves the test with a stray argument. The shape is read
       once from the segments the word already holds. */
    if (i < operand_end) {
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const shape = classify_test_operand(word);
      if (shape.has_positional_reference) actx.mark_positional_reference();

      let const written =
          analysis_source_text(actx, args[i]->source_location());
      let const previous = previous_literal.view();

      if (shape.has_array_spread) {
        actx.report_diagnostic(diagnostic_id::sc2198,
                               args[i]->source_location(), {written});
      }

      if (shape.has_brace_expansion) {
        actx.report_diagnostic(diagnostic_id::sc2200,
                               args[i]->source_location(), {written});
      }

      if (shape.has_unquoted_glob) {
        if (previous == "-v") {
          actx.report_diagnostic(diagnostic_id::sc2208,
                                 args[i]->source_location(), {written});
        } else if (is_test_path_unary_operator_word(previous)) {
          actx.report_diagnostic(diagnostic_id::sc2245,
                                 args[i]->source_location(),
                                 {previous, written});
        } else if (!is_test_binary_operator_word(previous)) {
          actx.report_diagnostic(diagnostic_id::sc2202,
                                 args[i]->source_location(), {written});
        }
      }

      if (shape.has_unquoted_expansion && previous == "-n") {
        actx.report_diagnostic(diagnostic_id::sc2070,
                               args[i]->source_location());
      }
    }

    if (is_posix) {
      let const is_operator_slot =
          i >= 2 && !is_test_binary_operator_word(previous_literal.view());
      if ((view == "<" || view == ">") && is_operator_slot) {
        actx.report_diagnostic(diagnostic_id::sc3012,
                               args[i]->source_location(), {view});
      } else if (is_test_file_comparison_word(view) && is_operator_slot) {
        actx.report_diagnostic(diagnostic_id::sc3013,
                               args[i]->source_location(), {view});
      } else if (view == "-a" && (i == 1 || previous_is_bang)) {
        actx.report_diagnostic(diagnostic_id::sc3017,
                               args[i]->source_location());
      }
    }

    /* Two inequalities on the same operand joined by -o hold for every value,
       shellcheck SC2056. */
    if (view == "-o" && i >= 3) {
      let const before = test_inequality_left_operand(args, i - 2, operand_end);
      let const after = test_inequality_left_operand(args, i + 2, operand_end);
      if (before.has_value() && after.has_value() && *before == *after) {
        actx.report_diagnostic(diagnostic_id::sc2056,
                               args[i]->source_location(), {*before});
      }
    }

    if (i >= 2 && !previous_is_bang && (view == "-a" || view == "-o")) {
      actx.report_diagnostic(diagnostic_id::sc2166, args[i]->source_location());
    } else if (view == "!" && i + 1 < args.count() &&
               args[i + 1]->kind() == Token::Kind::Word)
    {
      let const next = static_cast<const tokens::WordToken *>(args[i + 1])
                           ->word()
                           .to_literal_string();
      if (next.view() == "-z") {
        actx.report_diagnostic(diagnostic_id::sc2236,
                               args[i]->source_location());
      } else if (next.view() == "-n") {
        actx.report_diagnostic(diagnostic_id::sc2237,
                               args[i]->source_location());
      } else if (i + 2 < args.count() &&
                 args[i + 2]->kind() == Token::Kind::Word)
      {
        /* The ! X OP Y shape where OP has a direct negated form, shellcheck
           SC2335. */
        let const op = static_cast<const tokens::WordToken *>(args[i + 2])
                           ->word()
                           .to_literal_string();
        let const inverse = negated_test_operator(op.view());
        if (inverse.has_value()) {
          actx.report_diagnostic(diagnostic_id::sc2335,
                                 args[i]->source_location(),
                                 {op.view(), inverse.value()});
        }
      }
    }
  }

  /* A test with no operand always fails, shellcheck SC2212. */
  if (bracket_form_is_closed && operand_end == 1)
    actx.report_diagnostic(diagnostic_id::sc2212, input.command_location());

  /* A single-operand test with no operator is the nonempty-string test. A
     bracketed true, false, 0 or 1 reads as the builtin and is SC2158 through
     SC2161, another literal is the constant condition SC2078, command output is
     SC2243, and a variable is SC2244. A flag-shaped operand is left alone so
     [ -n ] is not told to use -n. */
  if (bracket_form_is_closed && operand_end == 2 &&
      args[1]->kind() == Token::Kind::Word)
  {
    let const &word = static_cast<const tokens::WordToken *>(args[1])->word();
    let const operand = word.to_literal_string();
    let const view = operand.view();
    let const location = args[1]->source_location();
    let const is_literal = word_is_fully_literal(word);
    let const constant = is_literal ? BRACKETED_CONSTANTS.find(view) : None;

    if (constant.has_value()) {
      switch (*constant) {
      case bracketed_constant_kind::False:
        actx.report_diagnostic(diagnostic_id::sc2158, location);
        break;
      case bracketed_constant_kind::Zero:
        actx.report_diagnostic(diagnostic_id::sc2159, location);
        break;
      case bracketed_constant_kind::True:
        actx.report_diagnostic(diagnostic_id::sc2160, location);
        break;
      case bracketed_constant_kind::One:
        actx.report_diagnostic(diagnostic_id::sc2161, location);
        break;
      }
    } else if (view.length == 0 || view[0] != '-') {
      if (is_literal) {
        actx.report_diagnostic(diagnostic_id::sc2078, location, {view});
      } else if (word.segments.count() == 1 &&
                 word.segments[0].kind ==
                     WordSegment::Kind::CommandSubstitution)
      {
        actx.report_diagnostic(diagnostic_id::sc2243, location);
      } else {
        actx.report_diagnostic(diagnostic_id::sc2244, location);
      }
    }
  }

  /* The operand-shape lints over the closed operand range. A -z or -n on a
     literal operand is SC2157, the same test on collected matcher output is
     SC2143, a numeric comparison against a non-numeric literal is SC2170, and a
     = or == against a glob literal is SC2081. */
  for (usize i = 1; i < operand_end; i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
    let const literal = word.to_literal_string();
    let const view = literal.view();

    if ((view == "-z" || view == "-n") && i + 1 < operand_end &&
        args[i + 1]->kind() == Token::Kind::Word)
    {
      let const &next =
          static_cast<const tokens::WordToken *>(args[i + 1])->word();
      if (word_is_fully_literal(next)) {
        actx.report_diagnostic(diagnostic_id::sc2157,
                               args[i + 1]->source_location(), {view});
      } else if (next.segments.count() == 1 &&
                 next.segments[0].kind ==
                     WordSegment::Kind::CommandSubstitution &&
                 substitution_runs_pattern_matcher(
                     next.segments[0].text.view()))
      {
        actx.report_diagnostic(diagnostic_id::sc2143,
                               args[i + 1]->source_location());
      }
    }

    if (is_test_numeric_operator_word(view)) {
      for (usize side = i - 1; side <= i + 1; side += 2) {
        /* Index zero is the command word, never an operand. */
        if (side == 0 || side >= operand_end ||
            args[side]->kind() != Token::Kind::Word)
          continue;
        check_numeric_comparison_operand(actx, view, args[side], false);
      }
    }

    if (input.command_id() != command_name_id::DoubleBracket &&
        (view == "=" || view == "==") && i + 1 < operand_end &&
        args[i + 1]->kind() == Token::Kind::Word)
    {
      let const &right =
          static_cast<const tokens::WordToken *>(args[i + 1])->word();
      if (word_is_fully_literal(right)) {
        let const right_literal = right.to_literal_string();
        if (right_literal.view().find_character('*').has_value() ||
            right_literal.view().find_character('?').has_value())
        {
          actx.report_diagnostic(diagnostic_id::sc2081,
                                 args[i + 1]->source_location());
        } else if (i >= 2 && args[i - 1]->kind() == Token::Kind::Word) {
          /* Two differing literals never compare equal, shellcheck SC2193. */
          let const &left =
              static_cast<const tokens::WordToken *>(args[i - 1])->word();
          let const left_literal = left.to_literal_string();
          if (word_is_fully_literal(left) &&
              left_literal.view() != right_literal.view())
          {
            actx.report_diagnostic(diagnostic_id::sc2193,
                                   args[i]->source_location(),
                                   {left_literal.view(), right_literal.view()});
          }
        }
      }
    }

    /* A test against $? checks the exit status indirectly, shellcheck
       SC2181. */
    if (word.segments.count() == 1 &&
        word.segments[0].kind == WordSegment::Kind::VariableReference &&
        word.segments[0].text.view() == "?")
    {
      actx.report_diagnostic(diagnostic_id::sc2181, args[i]->source_location());
    }
  }
}

namespace {

/* The variables whose value is a command name by convention, so an ordinary
   prefix assignment to one of them is not a swallowed command. */
constexpr PackedStringKey COMMAND_VALUED_VARIABLE_KEYS[] = {
    SSK("BROWSER"), SSK("CC"),     SSK("CXX"),      SSK("DIFFPROG"),
    SSK("EDITOR"),  SSK("FCEDIT"), SSK("MANPAGER"), SSK("PAGER"),
    SSK("SHELL"),   SSK("VISUAL"),
};
constexpr StaticStringSet COMMAND_VALUED_VARIABLES{
    COMMAND_VALUED_VARIABLE_KEYS};

} /* namespace */

/* A prefix assignment does not affect the expansion on the same command, so a
   reference to one of its names reads the old value. */
fn check_prefix_assignment_reads(AnalysisContext &actx,
                                 const command_lint_input &input) throws -> void
{
  if (input.local_vars.is_empty()) return;

  let const &args = input.args;

  /* One prefix assignment whose value names a command leaves the next word as
     the command name, shellcheck SC2037. A variable that holds a command name
     by convention keeps its ordinary use. */
  if (input.local_vars.count() == 1 && !input.command_literal.is_empty()) {
    let const &value = input.local_vars[0].value;
    let const value_is_bare_word =
        value.segments.count() == 1 &&
        (value.segments[0].kind == WordSegment::Kind::UnquotedText ||
         value.segments[0].kind == WordSegment::Kind::LiteralText);
    if (value_is_bare_word) {
      let const assigned = value.segments[0].text.view();
      let const name = input.local_vars[0].name.view();
      let const value_names_a_command =
          !COMMAND_VALUED_VARIABLES.contains(name) &&
          get_analysis_command_info(assigned).id != command_name_id::Unknown &&
          get_analysis_command_info(input.command_literal).id ==
              command_name_id::Unknown;
      if (input.command_literal[0] == '-' || value_names_a_command) {
        actx.report_diagnostic(diagnostic_id::sc2037,
                               input.local_vars[0].location,
                               {name, input.command_literal});
      }
    }
  }

  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;
      const StringView referenced{segment.text.data(), segment.text.count()};
      bool does_name_a_prefix = false;
      for (let const &var : input.local_vars) {
        if (var.name.view() == referenced) {
          does_name_a_prefix = true;
          break;
        }
      }
      if (does_name_a_prefix) {
        actx.report_diagnostic(diagnostic_id::assignment_prefix_read,
                               args[i]->source_location(),
                               {segment.text.view()});
        break;
      }
    }
  }
}

namespace {

/* An arithmetic value assigns text unless it is wrapped, so the operand shape
   is read from the raw assignment. */
pure fn value_is_self_arithmetic(StringView name, StringView value) wontthrow
    -> bool
{
  usize position = 0;
  if (position < value.length && value[position] == '$') position++;

  if (position + name.length > value.length) return false;
  if (value.substring_of_length(position, name.length) != name) return false;
  position += name.length;

  if (position >= value.length) return false;

  switch (value[position]) {
  case '+':
  case '-':
  case '*':
  case '/': position++; break;

  default: return false;
  }

  if (position >= value.length) return false;

  return value.substring(position).is_all_decimal_digits();
}

/* The value keeps its quote bytes, so the surrounding pair is dropped before
   the name is compared. */
pure fn assignment_value_is_own_name(StringView name,
                                     StringView value) wontthrow -> bool
{
  StringView inner = value;
  if (inner.length >= 2 && inner[0] == '"' && inner[inner.length - 1] == '"')
    inner = inner.substring_of_length(1, inner.length - 2);

  if (inner.length < 2 || inner[0] != '$') return false;

  if (inner.length >= 4 && inner[1] == '{' && inner[inner.length - 1] == '}')
    return inner.substring_of_length(2, inner.length - 3) == name;

  return inner.substring(1) == name;
}

pure fn value_has_written_escape(StringView value) wontthrow -> bool
{
  for (usize i = 0; i + 1 < value.length; i += 1) {
    if (value[i] != '\\') continue;

    switch (value[i + 1]) {
    case 'n':
    case 'r':
    case 't': return true;

    default: break;
    }
  }

  return false;
}

} /* namespace */

/* A brace expansion needs a comma between the braces, so a lone brace stays
   data. A question mark is common inside a plain URL, so only the star counts
   as a glob here. */
static pure fn segment_holds_literal_pattern(StringView text) wontthrow -> bool
{
  if (text.find_character('*').has_value()) return true;

  let const open = text.find_character('{');
  if (!open.has_value()) return false;

  let const comma = text.substring(*open).find_character(',');
  if (!comma.has_value()) return false;

  return text.substring(*open + *comma).find_character('}').has_value();
}

fn scan_assignment_value(AnalysisContext &actx, const Word &value_word,
                         SourceLocation location) throws
    -> assignment_value_shape
{
  assignment_value_shape shape{};

  for (let const &segment : value_word.segments) {
    if (segment.kind != WordSegment::Kind::UnquotedText)
      shape.has_bare_literal_value = false;

    if (actx.shebang_is_posix_sh)
      check_posix_word_portability(actx, segment, location);

    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::DoubleQuotedText:
      if (segment.text.view().find_character('"').has_value())
        shape.has_quoted_literal_value = true;
      break;

    case WordSegment::Kind::UnquotedText: break;

    default: shape.has_only_literal_segments = false; break;
    }

    if (segment.kind == WordSegment::Kind::UnquotedText &&
        segment_holds_literal_pattern(segment.text.view()))
    {
      shape.has_unquoted_pattern = true;
    }

    if (segment.kind == WordSegment::Kind::VariableReference &&
        segment.text.view() == "@")
    {
      actx.report_diagnostic(diagnostic_id::sc2124, location);
      break;
    }

    if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
        segment.text.view().find_character('$').has_value())
    {
      actx.report_diagnostic(diagnostic_id::sc2004, location);
    }
  }

  return shape;
}

fn check_assignment_value_shape(AnalysisContext &actx,
                                const assignment_lint_input &input) throws
    -> void
{
  let const equals = input.raw_assignment.find_character('=');
  if (!equals.has_value()) return;

  let const value = input.raw_assignment.substring(*equals + 1);

  if (input.shape.has_unquoted_pattern) {
    actx.report_diagnostic(diagnostic_id::sc2125, input.location, {input.name});
  }

  /* PATH without a separator and without its own value replaces the search
     path, shellcheck SC2123. An expanded value may already hold a path list. */
  if (input.name == "PATH" && !input.is_append && !value.is_empty() &&
      input.shape.has_only_literal_segments &&
      !value.find_character(':').has_value() &&
      !view_contains(value, StringView{"PATH"}))
  {
    actx.report_diagnostic(diagnostic_id::sc2123, input.location, {value});
  }

  if (value_is_self_arithmetic(input.name, value)) {
    let const id =
        value[0] == '$' ? diagnostic_id::sc2099 : diagnostic_id::sc2100;
    actx.report_diagnostic(id, input.location, {input.name});
  }

  /* A prefix repeating the value the name already holds exports it into the
     environment of the command, which an ordinary assignment does not do. */
  if (!input.is_append && !input.is_command_prefix &&
      assignment_value_is_own_name(input.name, value))
  {
    actx.report_diagnostic(diagnostic_id::sc2269, input.location, {input.name});
  }

  /* A separator written as two text bytes never becomes the control byte,
     shellcheck SC2141. */
  if (input.name == "IFS" && input.shape.has_only_literal_segments &&
      value_has_written_escape(value))
  {
    actx.report_diagnostic(diagnostic_id::sc2141, input.location, {input.name});
  }

  /* A prefix naming the program another tool is meant to start, such as
     `PAGER=cat cmd`, hands the name to that tool and is deliberate. */
  let const is_deliberate_command_prefix =
      input.is_command_prefix && COMMAND_VALUED_VARIABLES.contains(input.name);

  if (!input.is_append && !is_deliberate_command_prefix &&
      input.shape.has_bare_literal_value &&
      COMMAND_NAME_VALUES.contains(value) &&
      actx.should_report(diagnostic_id::sc2209))
  {
    actx.command_name_assignments.push(command_name_assignment_record{
        String{input.name}, String{value}, input.location});
  }

  let const first_bracket = input.name.find_character('[');
  if (input.shape.has_quoted_literal_value &&
      input.shape.has_only_literal_segments && !first_bracket.has_value())
  {
    actx.quoted_literal_assignments.set(input.name, input.location);
  }

  if (!first_bracket.has_value()) {
    /* A scalar assignment to an array name touches the first element alone,
       shellcheck SC2178 and SC2179. */
    if (actx.array_valued_names.count() != 0 &&
        actx.array_valued_names.contains(input.name))
    {
      let const id =
          input.is_append ? diagnostic_id::sc2179 : diagnostic_id::sc2178;
      actx.report_diagnostic(id, input.location, {input.name});
    }

    return;
  }

  /* A second subscript makes the name a multidimensional array, which the shell
     does not have, shellcheck SC2180. */
  let const after_first = input.name.substring(*first_bracket + 1);
  let const closer = after_first.find_character(']');
  if (closer.has_value() &&
      after_first.substring(*closer + 1).find_character('[').has_value())
  {
    actx.report_diagnostic(diagnostic_id::sc2180, input.location, {input.name});
  }
}

namespace {

pure fn option_letter_index(char letter) wontthrow -> u32
{
  if (letter >= 'a' && letter <= 'z') return static_cast<u32>(letter - 'a');
  if (letter >= 'A' && letter <= 'Z')
    return static_cast<u32>(letter - 'A') + 26;

  return 64;
}

pure fn segment_is_literal(const WordSegment &segment) wontthrow -> bool
{
  return segment.kind == WordSegment::Kind::LiteralText ||
         segment.kind == WordSegment::Kind::UnquotedText ||
         segment.kind == WordSegment::Kind::DoubleQuotedText;
}

pure fn literal_run_length(const Word &case_word, usize start,
                           usize end) wontthrow -> usize
{
  usize length = 0;
  for (usize i = start; i < end; i++)
    length += case_word.segments[i].text.view().length;

  return length;
}

pure fn literal_run_matches_at(const Word &case_word, usize start, usize end,
                               StringView pattern, usize position) wontthrow
    -> bool
{
  for (usize i = start; i < end; i++) {
    let const text = case_word.segments[i].text.view();
    if (position + text.length > pattern.length) return false;
    if (pattern.substring_of_length(position, text.length) != text)
      return false;
    position += text.length;
  }

  return true;
}

/* Whether the literal chunks of the case word leave room for the pattern. An
   expansion matches anything, so only a literal chunk can refute a match, and
   an unproven case answers true. The leading run is held to the start of the
   pattern and the trailing run to its end, since an expansion cannot move
   either. */
pure fn case_pattern_can_match_word(const Word &case_word,
                                    StringView pattern) wontthrow -> bool
{
  usize first = 0;
  usize last = case_word.segments.count();
  usize front = 0;
  usize back = pattern.length;

  while (first < last && segment_is_literal(case_word.segments[first])) {
    let const text = case_word.segments[first].text.view();
    if (front + text.length > back) return false;
    if (pattern.substring_of_length(front, text.length) != text) return false;
    front += text.length;
    first++;
  }

  if (first == last) return front == back;

  while (last > first && segment_is_literal(case_word.segments[last - 1])) {
    let const text = case_word.segments[last - 1].text.view();
    if (back < front + text.length) return false;
    if (pattern.substring_of_length(back - text.length, text.length) != text)
      return false;
    back -= text.length;
    last--;
  }

  usize at = first;
  while (at < last) {
    if (!segment_is_literal(case_word.segments[at])) {
      at++;
      continue;
    }

    usize run_end = at;
    while (run_end < last && segment_is_literal(case_word.segments[run_end]))
      run_end++;

    let const run_length = literal_run_length(case_word, at, run_end);
    let is_found = false;
    for (usize position = front; position + run_length <= back; position++) {
      if (!literal_run_matches_at(case_word, at, run_end, pattern, position))
        continue;
      front = position + run_length;
      is_found = true;
      break;
    }

    if (!is_found) return false;

    at = run_end;
  }

  return true;
}

} /* namespace */

fn check_case_word_shape(AnalysisContext &actx,
                         const case_lint_input &input) throws -> void
{
  ASSERT(input.case_word != nullptr);

  if (word_is_fully_literal(*input.case_word) &&
      !input.case_word_source.is_empty())
  {
    actx.report_diagnostic(diagnostic_id::sc2194, input.case_location,
                           {input.case_word_source});
  }
}

fn check_case_pattern_shape(AnalysisContext &actx, const case_lint_input &input,
                            const Word &pattern_word,
                            StringView pattern_literal,
                            StringView pattern_source,
                            SourceLocation pattern_location,
                            case_arm_tally &tally) throws -> void
{
  let const is_bare_pattern =
      pattern_word.segments.count() == 1 &&
      pattern_word.segments[0].kind == WordSegment::Kind::UnquotedText;

  if (is_bare_pattern && pattern_word.segments[0].text.view() == "*") {
    tally.has_default_arm = true;
    return;
  }

  if (pattern_literal == "?") tally.has_question_arm = true;

  let has_glob_metacharacter = false;
  let is_literal_pattern = true;
  let has_unquoted_expansion = false;

  for (let const &segment : pattern_word.segments) {
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
      if (segment.has_live_glob_chars() && segment.has_glob_metacharacter())
        has_glob_metacharacter = true;
      break;

    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::ArithmeticExpansion:
      is_literal_pattern = false;
      if (!segment.is_in_double_quotes) has_unquoted_expansion = true;
      break;

    default: is_literal_pattern = false; break;
    }
  }

  /* An expanded pattern is matched as a glob, so its bytes never compare
     literally, shellcheck SC2254. */
  if (has_unquoted_expansion && !pattern_source.is_empty()) {
    actx.report_diagnostic(diagnostic_id::sc2254, pattern_location,
                           {pattern_source});
  }

  /* A literal pattern with no metacharacter matches one string, so the literal
     chunks of the case word decide it, shellcheck SC2195. */
  if (input.case_word != nullptr && is_literal_pattern &&
      !has_glob_metacharacter && !pattern_source.is_empty() &&
      !input.case_word_source.is_empty() &&
      input.case_word->segments.count() != 0 &&
      !case_pattern_can_match_word(*input.case_word, pattern_literal))
  {
    actx.report_diagnostic(diagnostic_id::sc2195, pattern_location,
                           {pattern_source, input.case_word_source},
                           input.case_location);
  }

  if (!input.is_getopts_case) return;
  if (pattern_literal.length != 1) return;

  let const letter_index = option_letter_index(pattern_literal[0]);
  if (letter_index == 64) return;

  tally.handled_option_letters |= u64{1} << letter_index;

  if (!input.getopts_optstring.find_character(pattern_literal[0]).has_value()) {
    actx.report_diagnostic(diagnostic_id::sc2214, pattern_location,
                           {pattern_literal}, input.getopts_location);
  }
}

fn check_case_option_coverage(AnalysisContext &actx,
                              const case_lint_input &input,
                              const case_arm_tally &tally) throws -> void
{
  if (!input.is_getopts_case) return;

  for (usize i = 0; i < input.getopts_optstring.length; i++) {
    let const letter = input.getopts_optstring[i];
    let const letter_index = option_letter_index(letter);
    if (letter_index == 64) continue;
    if ((tally.handled_option_letters & (u64{1} << letter_index)) != 0)
      continue;

    actx.report_diagnostic(diagnostic_id::sc2213, input.case_location,
                           {input.getopts_optstring.substring_of_length(i, 1)},
                           input.getopts_location);
  }

  /* getopts stores a question mark for an unknown option, so a case without a
     catch-all silently skips it, shellcheck SC2220. */
  if (!tally.has_default_arm && !tally.has_question_arm) {
    actx.report_diagnostic(diagnostic_id::sc2220, input.case_location, {},
                           input.getopts_location);
  }
}

namespace {

/* Which quoting a byte sits inside, which decides whether a homoglyph is read
   as syntax or as text. */
enum class source_scan_state : u8
{
  Normal,
  SingleQuoted,
  DoubleQuoted,
  Comment,
};

constexpr u8 SCAN_ACTS_NORMAL = 1U << 0U;
constexpr u8 SCAN_ACTS_SINGLE_QUOTED = 1U << 1U;
constexpr u8 SCAN_ACTS_DOUBLE_QUOTED = 1U << 2U;
constexpr u8 SCAN_ACTS_COMMENT = 1U << 3U;
constexpr u8 SCAN_ACTS_EVERYWHERE = SCAN_ACTS_NORMAL | SCAN_ACTS_SINGLE_QUOTED |
                                    SCAN_ACTS_DOUBLE_QUOTED | SCAN_ACTS_COMMENT;

struct source_scan_table
{
  u8 acting_states[256];
};

/* The states in which a byte changes the scan below. A run of bytes that acts
   in no state is stepped over at once, which keeps a long word, a long comment,
   and a long quoted string off the per-byte dispatch. */
consteval fn build_source_scan_table() -> source_scan_table
{
  source_scan_table table{};

  for (usize byte = 0x80; byte < 256; byte++)
    table.acting_states[byte] = SCAN_ACTS_EVERYWHERE;

  table.acting_states[static_cast<u8>('\r')] = SCAN_ACTS_EVERYWHERE;
  table.acting_states[static_cast<u8>('\n')] = SCAN_ACTS_EVERYWHERE;
  table.acting_states[static_cast<u8>('\\')] =
      SCAN_ACTS_NORMAL | SCAN_ACTS_SINGLE_QUOTED | SCAN_ACTS_DOUBLE_QUOTED;
  table.acting_states[static_cast<u8>('\'')] =
      SCAN_ACTS_NORMAL | SCAN_ACTS_SINGLE_QUOTED;
  table.acting_states[static_cast<u8>('"')] =
      SCAN_ACTS_NORMAL | SCAN_ACTS_DOUBLE_QUOTED;

  for (let const byte : "#< \t;&|()")
    table.acting_states[static_cast<u8>(byte)] |= SCAN_ACTS_NORMAL;

  return table;
}

constexpr source_scan_table SOURCE_SCAN = build_source_scan_table();

alwaysinline pure fn skip_plain_bytes(StringView source, usize at,
                                      u8 acting_state) wontthrow -> usize
{
  while (at + 1 < source.length &&
         (SOURCE_SCAN.acting_states[static_cast<u8>(source[at + 1])] &
          acting_state) == 0)
  {
    at++;
  }

  return at;
}

enum class homoglyph_kind : u8
{
  None,
  SingleQuote,
  DoubleQuote,
  Dash,
  Space,
};

struct decoded_codepoint
{
  u32 value;
  usize length;
};

constexpr u64 HIGH_BITS = 0x8080808080808080ULL;
constexpr u64 LOW_BITS = 0x0101010101010101ULL;
constexpr u64 CARRIAGE_RETURNS = 0x0d0d0d0d0d0d0d0dULL;
constexpr u64 BACKSLASHES = 0x5c5c5c5c5c5c5c5cULL;

alwaysinline pure fn chunk_holds_byte(u64 chunk, u64 repeated) wontthrow -> u64
{
  let const differences = chunk ^ repeated;
  return (differences - LOW_BITS) & ~differences & HIGH_BITS;
}

alwaysinline pure fn chunk_holds_scanned_byte(u64 chunk) wontthrow -> bool
{
  return ((chunk & HIGH_BITS) | chunk_holds_byte(chunk, CARRIAGE_RETURNS) |
          chunk_holds_byte(chunk, BACKSLASHES)) != 0;
}

/* Whether the source holds a byte the classification below could report. A
   script is almost always plain ASCII, so this eight-byte-at-a-time answer
   keeps the classifying walk off the common path. */
pure fn source_holds_scanned_byte(StringView source) wontthrow -> bool
{
  usize at = 0;
  while (at + sizeof(u64) <= source.length) {
    u64 chunk = 0;
    __builtin_memcpy(&chunk, source.data + at, sizeof(u64));
    if (chunk_holds_scanned_byte(chunk)) return true;
    at += sizeof(u64);
  }

  for (; at < source.length; at++) {
    let const byte = static_cast<u8>(source[at]);
    if (byte >= 0x80 || byte == '\r' || byte == '\\') return true;
  }

  return false;
}

pure fn decode_utf8(StringView source, usize at) wontthrow -> decoded_codepoint
{
  let const first = static_cast<u8>(source[at]);
  usize length = 0;
  u32 value = 0;

  switch (first & 0xf8) {
  case 0xc0:
  case 0xc8:
  case 0xd0:
  case 0xd8:
    length = 2;
    value = first & 0x1fu;
    break;

  case 0xe0:
  case 0xe8:
    length = 3;
    value = first & 0x0fu;
    break;

  case 0xf0:
    length = 4;
    value = first & 0x07u;
    break;

  default: return decoded_codepoint{0, 1};
  }

  if (at + length > source.length) return decoded_codepoint{0, 1};

  for (usize i = 1; i < length; i++) {
    let const continuation = static_cast<u8>(source[at + i]);
    if ((continuation & 0xc0) != 0x80) return decoded_codepoint{0, 1};
    value = (value << 6) | (continuation & 0x3fu);
  }

  return decoded_codepoint{value, length};
}

pure fn classify_codepoint(u32 codepoint) wontthrow -> homoglyph_kind
{
  switch (codepoint) {
  case 0x2018:
  case 0x2019:
  case 0x201a:
  case 0x201b:
  case 0x2032:
  case 0x2035: return homoglyph_kind::SingleQuote;

  case 0x201c:
  case 0x201d:
  case 0x201e:
  case 0x201f:
  case 0x2033:
  case 0x2036: return homoglyph_kind::DoubleQuote;

  case 0x2010:
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2015:
  case 0x2043:
  case 0x2212:
  case 0xfe58:
  case 0xfe63:
  case 0xff0d: return homoglyph_kind::Dash;

  case 0x00a0:
  case 0x2000:
  case 0x2001:
  case 0x2002:
  case 0x2003:
  case 0x2004:
  case 0x2005:
  case 0x2006:
  case 0x2007:
  case 0x2008:
  case 0x2009:
  case 0x200a:
  case 0x200b:
  case 0x202f:
  case 0x205f:
  case 0x3000:
  case 0xfeff: return homoglyph_kind::Space;

  default: return homoglyph_kind::None;
  }
}

/* A slanted single quote inside a double-quoted string, and a slanted double
   quote inside a single-quoted string, are the literal typography upstream
   allows, so they answer None. */
pure fn homoglyph_diagnostic(homoglyph_kind kind,
                             source_scan_state state) wontthrow
    -> Maybe<diagnostic_id>
{
  if (state == source_scan_state::Comment) return None;

  switch (kind) {
  case homoglyph_kind::SingleQuote:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1110;
    if (state == source_scan_state::SingleQuoted) return diagnostic_id::sc1112;
    return None;

  case homoglyph_kind::DoubleQuote:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1110;
    if (state == source_scan_state::DoubleQuoted) return diagnostic_id::sc1111;
    return None;

  case homoglyph_kind::Dash:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1100;
    return None;

  case homoglyph_kind::Space:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1018;
    return None;

  default: return None;
  }
}

fn codepoint_spelling(u32 codepoint) throws -> String
{
  constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

  let spelling = String{"U+"};
  for (let shift = codepoint > 0xffff ? 20 : 12; shift >= 0; shift -= 4)
    spelling.push(HEX_DIGITS[(codepoint >> shift) & 0xf]);

  return spelling;
}

pure fn byte_precedes_comment(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
  case ';':
  case '&':
  case '|':
  case '(':
  case ')': return true;

  default: return false;
  }
}

pure fn byte_ends_here_document_delimiter(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
  case ';':
  case '&':
  case '|':
  case '<':
  case '>':
  case '(':
  case ')': return true;

  default: return false;
  }
}

/* The offset just past the here-document terminator. A body holds prose, where
   a slanted quote or a Unicode dash is ordinary text. */
pure fn skip_here_document(StringView source, usize at) wontthrow -> usize
{
  usize cursor = at + 2;
  if (cursor < source.length && source[cursor] == '-') cursor++;

  while (cursor < source.length &&
         (source[cursor] == ' ' || source[cursor] == '\t'))
    cursor++;

  usize delimiter_start = cursor;
  usize delimiter_end = cursor;

  if (cursor < source.length &&
      (source[cursor] == '\'' || source[cursor] == '"'))
  {
    let const quote = source[cursor];
    cursor++;
    delimiter_start = cursor;
    while (cursor < source.length && source[cursor] != quote)
      cursor++;
    delimiter_end = cursor;
  } else {
    while (cursor < source.length &&
           !byte_ends_here_document_delimiter(source[cursor]))
      cursor++;
    delimiter_end = cursor;
  }

  if (delimiter_end == delimiter_start) return at + 2;

  let const delimiter = source.substring_of_length(
      delimiter_start, delimiter_end - delimiter_start);

  while (cursor < source.length && source[cursor] != '\n')
    cursor++;

  while (cursor < source.length) {
    cursor++;

    usize line_start = cursor;
    while (line_start < source.length && source[line_start] == '\t')
      line_start++;

    usize line_end = line_start;
    while (line_end < source.length && source[line_end] != '\n')
      line_end++;

    if (line_end - line_start == delimiter.length &&
        source.substring_of_length(line_start, delimiter.length) == delimiter)
    {
      return line_end;
    }

    cursor = line_end;
  }

  return source.length;
}

pure fn byte_is_ascii_letter(char byte) wontthrow -> bool
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/* The letters whose escape reads as a control byte in other languages. */
pure fn escape_names_control_byte(char byte) wontthrow -> bool
{
  switch (byte) {
  case 'n':
  case 'r':
  case 't': return true;

  default: return false;
  }
}

fn escape_spelling(char escaped) throws -> String
{
  let spelling = String{"\\"};
  spelling.push(escaped);

  return spelling;
}

/* Whether the line ending just above the given line start carries a
   continuation, which is what makes a commented-out backslash matter. */
pure fn line_above_continues(StringView source, usize line_start) wontthrow
    -> bool
{
  if (line_start < 2) return false;
  if (source[line_start - 1] != '\n') return false;

  usize ending = line_start - 1;
  if (source[ending - 1] == '\r') ending--;

  return ending > 0 && source[ending - 1] == '\\';
}

} /* namespace */

fn check_source_bytes(AnalysisContext &actx, StringView source) throws -> void
{
  if (!source_holds_scanned_byte(source)) return;

  let state = source_scan_state::Normal;
  let was_carriage_return_reported = false;
  let is_command_position = true;
  usize comment_line_start = 0;
  usize line_start = 0;
  usize at = 0;

  while (at < source.length) {
    let const byte = static_cast<u8>(source[at]);

    if (byte >= 0x80) {
      let const decoded = decode_utf8(source, at);
      let const id =
          homoglyph_diagnostic(classify_codepoint(decoded.value), state);

      /* A leading byte-order mark is already reported as its own finding. */
      if (id.has_value() && at != 0) {
        let const spelling = codepoint_spelling(decoded.value);
        actx.report_diagnostic(*id, SourceLocation{at, decoded.length},
                               {spelling.view()});
      }

      at += decoded.length;
      continue;
    }

    if (byte == '\r' && !was_carriage_return_reported) {
      was_carriage_return_reported = true;
      actx.report_diagnostic(diagnostic_id::sc1017, SourceLocation{at, 1});
    }

    switch (state) {
    case source_scan_state::Normal:
      switch (byte) {
      case '\'':
        state = source_scan_state::SingleQuoted;
        is_command_position = false;
        break;

      case '"':
        state = source_scan_state::DoubleQuoted;
        is_command_position = false;
        break;

      case '\\': {
        let const escaped = at + 1 < source.length ? source[at + 1] : '\0';

        if (escaped == ' ' || escaped == '\t') {
          usize blank_end = at + 1;
          while (blank_end < source.length &&
                 (source[blank_end] == ' ' || source[blank_end] == '\t'))
          {
            blank_end++;
          }

          if (blank_end < source.length && source[blank_end] == '\n') {
            actx.report_diagnostic(diagnostic_id::sc1101,
                                   SourceLocation{at, blank_end - at});
          }
        } else if (byte_is_ascii_letter(escaped) && !is_command_position) {
          let const spelling = escape_spelling(escaped);
          actx.report_diagnostic(
              escape_names_control_byte(escaped) ? diagnostic_id::sc1012
                                                 : diagnostic_id::sc1001,
              SourceLocation{at, 2},
              {spelling.view(), source.substring_of_length(at + 1, 1)});
        }

        is_command_position = false;
        at++;
        break;
      }

      case '#':
        if (at == 0 || byte_precedes_comment(source[at - 1])) {
          state = source_scan_state::Comment;
          comment_line_start = line_start;
        }
        break;

      case '<':
        is_command_position = false;
        if (at + 2 < source.length && source[at + 1] == '<' &&
            source[at + 2] != '<')
        {
          at = skip_here_document(source, at);
          line_start = at;
          while (line_start > 0 && source[line_start - 1] != '\n')
            line_start--;
          continue;
        }
        break;

      case ' ':
      case '\t':
      case '\r': break;

      case '\n':
      case ';':
      case '&':
      case '|':
      case '(':
      case ')': is_command_position = true; break;

      default:
        is_command_position = false;
        at = skip_plain_bytes(source, at, SCAN_ACTS_NORMAL);
        break;
      }
      break;

    case source_scan_state::SingleQuoted:
      switch (byte) {
      case '\'': state = source_scan_state::Normal; break;

      /* The backslash carries no meaning here, so the byte behind it is read
         as source and the scan does not step over it. */
      case '\\':
        if (at + 1 < source.length) {
          if (source[at + 1] == '\'') {
            actx.report_diagnostic(diagnostic_id::sc1003,
                                   SourceLocation{at, 2});
          } else if (source[at + 1] == '\n' && at > 0) {
            /* The caret reaches back one byte because a span that opens on a
               continuation is rendered against the line below it. */
            actx.report_diagnostic(diagnostic_id::sc1004,
                                   SourceLocation{at - 1, 2});
          }
        }
        break;

      default:
        at = skip_plain_bytes(source, at, SCAN_ACTS_SINGLE_QUOTED);
        break;
      }
      break;

    case source_scan_state::DoubleQuoted:
      switch (byte) {
      case '"': state = source_scan_state::Normal; break;
      case '\\': at++; break;

      default:
        at = skip_plain_bytes(source, at, SCAN_ACTS_DOUBLE_QUOTED);
        break;
      }
      break;

    case source_scan_state::Comment:
      if (byte != '\n') {
        at = skip_plain_bytes(source, at, SCAN_ACTS_COMMENT);
        break;
      }

      if (at > 1 && source[at - 1] == '\\' &&
          line_above_continues(source, comment_line_start))
      {
        actx.report_diagnostic(diagnostic_id::sc1143,
                               SourceLocation{at - 2, 2});
      }

      state = source_scan_state::Normal;
      is_command_position = true;
      break;
    }

    /* An escape case consumes its escaped byte, so the last consumed byte is
       read rather than the byte the state switch dispatched on. */
    at++;
    if (source[at - 1] == '\n') line_start = at;
  }
}

namespace {

constexpr PackedStringKey KNOWN_SHELL_KEYS[] = {
    SSK("ash"),  SSK("bash"), SSK("bosh"),  SSK("busybox"), SSK("dash"),
    SSK("kosh"), SSK("ksh"),  SSK("ksh88"), SSK("ksh93"),   SSK("mksh"),
    SSK("posh"), SSK("sh"),   SSK("yash"),  SSK("zsh"),
};
constexpr StaticStringSet KNOWN_SHELLS{KNOWN_SHELL_KEYS};

pure fn byte_separates_shebang_words(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t': return true;

  default: return false;
  }
}

pure fn path_base_name(StringView path) wontthrow -> StringView
{
  usize at = path.length;
  while (at > 0 && path[at - 1] != '/')
    at--;

  return path.substring(at);
}

/* One left-to-right reader over the shebang line, so the interpreter, its
   parameters, and their spans come from one walk. */
struct shebang_word_reader
{
  StringView line;
  usize at{0};
  usize word_start{0};

  fn read_next_word() wontthrow -> StringView
  {
    while (at < line.length && byte_separates_shebang_words(line[at]))
      at++;

    word_start = at;
    while (at < line.length && !byte_separates_shebang_words(line[at]))
      at++;

    return line.substring_of_length(word_start, at - word_start);
  }

  pure fn word_location() const wontthrow -> SourceLocation
  {
    return SourceLocation{word_start, at - word_start};
  }
};

/* Whether a later line in the leading comment block opens with the shebang
   bytes. A misplaced shebang sits under a copyright header, so the walk stops
   at the first line that is neither blank nor a comment. */
pure fn header_holds_shebang(StringView source, usize first_line_end) wontthrow
    -> bool
{
  usize at = first_line_end;

  while (at < source.length) {
    at++;

    usize line_start = at;
    while (line_start < source.length &&
           byte_separates_shebang_words(source[line_start]))
      line_start++;

    if (line_start + 1 < source.length && source[line_start] == '#' &&
        source[line_start + 1] == '!')
    {
      return true;
    }

    if (line_start < source.length && source[line_start] != '#' &&
        source[line_start] != '\n')
    {
      return false;
    }

    while (at < source.length && source[at] != '\n')
      at++;
  }

  return false;
}

} /* namespace */

fn check_shebang(AnalysisContext &actx, StringView source,
                 bool is_named_script_file) throws -> void
{
  usize line_end = 0;
  while (line_end < source.length && source[line_end] != '\n')
    line_end++;

  let const first_line = source.substring_of_length(0, line_end);

  usize at = 0;
  while (at < first_line.length && byte_separates_shebang_words(first_line[at]))
    at++;

  let const indent_length = at;

  /* A leading `!` is the negation operator, so only a following path reads as
     a mistyped shebang. */
  if (at < first_line.length && first_line[at] == '!') {
    let const is_swapped = at + 2 < first_line.length &&
                           first_line[at + 1] == '#' &&
                           first_line[at + 2] == '/';

    if (is_swapped) {
      actx.report_diagnostic(diagnostic_id::sc1084, SourceLocation{at, 2});
      return;
    }

    if (at + 1 < first_line.length && first_line[at + 1] == '/')
      actx.report_diagnostic(diagnostic_id::sc1104, SourceLocation{at, 1});

    return;
  }

  let const has_hash = at < first_line.length && first_line[at] == '#';
  usize bang_at = has_hash ? at + 1 : at;
  while (bang_at < first_line.length &&
         byte_separates_shebang_words(first_line[bang_at]))
    bang_at++;

  let const has_bang =
      has_hash && bang_at < first_line.length && first_line[bang_at] == '!';

  if (!has_bang) {
    if (header_holds_shebang(source, line_end)) {
      actx.report_diagnostic(diagnostic_id::sc1128, SourceLocation{at, 1});
      return;
    }

    /* A comment naming an absolute path is the shebang written without its
       bang. */
    if (has_hash && bang_at < first_line.length && first_line[bang_at] == '/') {
      actx.report_diagnostic(diagnostic_id::sc1113, SourceLocation{at, 1});
      return;
    }

    /* A script without a shebang runs correctly, so the missing interpreter is
       reported only when diagnostics were asked for. */
    if (is_named_script_file && actx.warning_level != 0)
      actx.report_diagnostic(diagnostic_id::sc2148, SourceLocation{0, 1});

    return;
  }

  if (indent_length != 0)
    actx.report_diagnostic(diagnostic_id::sc1114,
                           SourceLocation{0, indent_length});

  if (bang_at != at + 1) {
    actx.report_diagnostic(diagnostic_id::sc1115,
                           SourceLocation{at + 1, bang_at - at - 1});
  }

  let reader = shebang_word_reader{first_line, bang_at + 1, bang_at + 1};
  let const interpreter = reader.read_next_word();
  let const interpreter_location = reader.word_location();

  if (interpreter.is_empty()) return;

  if (interpreter[0] != '/') {
    actx.report_diagnostic(diagnostic_id::sc2239, interpreter_location,
                           {interpreter});
  }

  if (interpreter[interpreter.length - 1] == '/') {
    actx.report_diagnostic(diagnostic_id::sc2246, interpreter_location,
                           {interpreter});
  }

  let const interpreter_name = path_base_name(interpreter);
  let const names_env = interpreter_name == StringView{"env"};

  usize parameter_count = 0;
  let shell_argument = StringView{};
  let shell_argument_location = interpreter_location;
  let has_split_string_flag = false;

  for (;;) {
    let const word = reader.read_next_word();
    if (word.is_empty()) break;

    if (shell_argument.is_empty() && word[0] != '-') {
      shell_argument = word;
      shell_argument_location = reader.word_location();
    }

    if (word == StringView{"-S"} ||
        word.starts_with(StringView{"--split-string"}))
      has_split_string_flag = true;

    parameter_count++;
  }

  /* `env -S` splits its own argument, so the words after it are not separate
     shebang parameters. */
  if (parameter_count > 1 && !(names_env && has_split_string_flag))
    actx.report_diagnostic(diagnostic_id::sc2096, interpreter_location);

  let const shell_name =
      names_env ? path_base_name(shell_argument) : interpreter_name;
  let const shell_name_location =
      names_env ? shell_argument_location : interpreter_location;

  if (shell_name.is_empty()) return;

  if (!KNOWN_SHELLS.contains(shell_name)) {
    actx.report_diagnostic(diagnostic_id::sc1008, shell_name_location,
                           {shell_name});
    return;
  }

  if (shell_name == StringView{"dash"} || shell_name == StringView{"sh"})
    actx.shebang_is_posix_sh = true;
}

namespace {

constexpr PackedStringKey DIRECTIVE_KEY_KEYS[] = {
    SSK("disable"), SSK("enable"), SSK("external-sources"),
    SSK("shell"),   SSK("source"), SSK("source-path"),
};
constexpr StaticStringSet DIRECTIVE_KEYS{DIRECTIVE_KEY_KEYS};

/* A word that continues the command above it, so a directive placed before it
   covers nothing. */
constexpr PackedStringKey CLAUSE_KEYWORD_KEYS[] = {
    SSK("do"),   SSK("done"), SSK("elif"), SSK("else"),
    SSK("esac"), SSK("fi"),   SSK("then"), SSK("}"),
};
constexpr StaticStringSet CLAUSE_KEYWORDS{CLAUSE_KEYWORD_KEYS};

constexpr usize DIRECTIVE_KEYWORD_LENGTH = 10;

pure fn byte_is_blank(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t': return true;

  default: return false;
  }
}

pure fn find_line_start(StringView source, usize position) wontthrow -> usize
{
  usize at = position;
  while (at > 0 && source[at - 1] != '\n')
    at--;

  return at;
}

pure fn only_blanks_precede(StringView source, usize line_start,
                            usize position) wontthrow -> bool
{
  for (usize at = line_start; at < position; at++) {
    if (!byte_is_blank(source[at])) return false;
  }

  return true;
}

/* The first word below the directive, with blank lines and further comments
   skipped. */
pure fn read_word_below_directive(StringView source, usize after) wontthrow
    -> StringView
{
  usize at = after;

  loop
  {
    while (at < source.length &&
           (byte_is_blank(source[at]) || source[at] == '\n'))
    {
      at++;
    }

    if (at >= source.length) return {};

    if (source[at] != '#') break;

    while (at < source.length && source[at] != '\n')
      at++;
  }

  let const word_start = at;
  while (at < source.length && !byte_is_blank(source[at]) && source[at] != '\n')
  {
    at++;
  }

  return source.substring_of_length(word_start, at - word_start);
}

/* The last line above the directive that carries a command, with blank lines
   and further comments skipped. Trailing blanks are dropped so the terminator
   is the final byte. */
pure fn read_line_above_directive(StringView source, usize line_start) wontthrow
    -> StringView
{
  usize end = line_start;

  while (end > 0) {
    end--;

    let const start = find_line_start(source, end);
    usize content_start = start;
    while (content_start < end && byte_is_blank(source[content_start]))
      content_start++;

    usize content_end = end;
    while (content_end > content_start &&
           byte_is_blank(source[content_end - 1]))
      content_end--;

    if (content_start < content_end && source[content_start] != '#')
      return source.substring_of_length(content_start,
                                        content_end - content_start);

    end = start;
  }

  return {};
}

pure fn line_opens_case_branch(StringView line) wontthrow -> bool
{
  if (line.length >= 2 && line[line.length - 1] == ';' &&
      line[line.length - 2] == ';')
  {
    return true;
  }

  if (line.length >= 2 && line[line.length - 1] == '&' &&
      line[line.length - 2] == ';')
  {
    return true;
  }

  if (line.length >= 2 && line[line.length - 1] == 'n' &&
      line[line.length - 2] == 'i' &&
      (line.length == 2 || byte_is_blank(line[line.length - 3])))
  {
    return true;
  }

  return false;
}

pure fn word_holds_case_pattern(StringView word) wontthrow -> bool
{
  for (usize at = 0; at < word.length; at++) {
    if (word[at] == '(') return false;
    if (word[at] == ')') return true;
  }

  return false;
}

/* SC1107 and SC1125, read from the tokens after the directive keyword. One
   finding closes the scan, since a malformed directive is usually followed by
   prose that would report again on every word. */
fn check_directive_body(AnalysisContext &actx, StringView source,
                        shellcheck_directive_span span,
                        usize body_position) throws -> void
{
  let const comment_end = span.position + span.length;
  usize at = span.position + body_position;

  while (at < comment_end) {
    while (at < comment_end && byte_is_blank(source[at]))
      at++;

    if (at >= comment_end) return;

    let const token_start = at;
    while (at < comment_end && !byte_is_blank(source[at]))
      at++;

    let const token = source.substring_of_length(token_start, at - token_start);

    usize separator_position = 0;
    while (separator_position < token.length &&
           token[separator_position] != '=')
      separator_position++;

    if (separator_position == token.length) {
      actx.report_diagnostic(diagnostic_id::sc1125,
                             SourceLocation{token_start, token.length},
                             {token});
      return;
    }

    let const key = token.substring_of_length(0, separator_position);

    if (!DIRECTIVE_KEYS.contains(key)) {
      actx.report_diagnostic(diagnostic_id::sc1107,
                             SourceLocation{token_start, key.length}, {key});
      return;
    }
  }
}

} /* namespace */

fn check_shellcheck_directives(
    AnalysisContext &actx, StringView source,
    const ArrayList<shellcheck_directive_span> &directives) throws -> void
{
  usize previous_position = static_cast<usize>(-1);

  for (let const &directive : directives) {
    if (directive.position == previous_position) continue;
    previous_position = directive.position;

    usize body_position = 1;
    while (body_position < directive.length &&
           byte_is_blank(source[directive.position + body_position]))
    {
      body_position++;
    }
    body_position += DIRECTIVE_KEYWORD_LENGTH;

    check_directive_body(actx, source, directive, body_position);

    let const line_start = find_line_start(source, directive.position);

    if (!only_blanks_precede(source, line_start, directive.position)) {
      actx.report_diagnostic(
          diagnostic_id::sc1126,
          SourceLocation{directive.position, directive.length});
      continue;
    }

    let const word_below = read_word_below_directive(
        source, directive.position + directive.length);

    if (CLAUSE_KEYWORDS.contains(word_below)) {
      actx.report_diagnostic(
          diagnostic_id::sc1123,
          SourceLocation{directive.position, directive.length}, {word_below});
      continue;
    }

    if (word_holds_case_pattern(word_below) &&
        line_opens_case_branch(read_line_above_directive(source, line_start)))
    {
      actx.report_diagnostic(
          diagnostic_id::sc1124,
          SourceLocation{directive.position, directive.length});
    }
  }
}

fn check_heredoc_terminators(
    AnalysisContext &actx, StringView source,
    const ArrayList<heredoc_terminator_miss> &misses) throws -> void
{
  for (let const &miss : misses) {
    let const terminator =
        source.substring_of_length(miss.position, miss.length);

    switch (miss.kind) {
    case heredoc_miss_kind::IndentedTerminator:
      actx.report_diagnostic(diagnostic_id::sc1039,
                             SourceLocation{miss.position, miss.length},
                             {terminator});
      break;

    case heredoc_miss_kind::TabIndentedTerminator:
      actx.report_diagnostic(diagnostic_id::sc1040,
                             SourceLocation{miss.position, miss.length},
                             {terminator});
      break;

    case heredoc_miss_kind::TrailingBlankTerminator:
      actx.report_diagnostic(diagnostic_id::sc1118,
                             SourceLocation{miss.position, miss.length},
                             {terminator});
      break;
    }
  }
}

namespace {

struct unassigned_read
{
  StringView name;
  SourceLocation location;
};

/* The assigned name a read comes closest to, with the assignment that recorded
   it. */
struct resembling_assignment
{
  StringView name;
  SourceLocation location;
};

pure fn fold_name_byte(char byte) wontthrow -> char
{
  return byte >= 'a' && byte <= 'z' ? static_cast<char>(byte - ('a' - 'A'))
                                    : byte;
}

/* Whether two names differ only in letter case and in underscore placement,
   which is the shape a misspelled reference of an assigned name takes. */
pure fn names_resemble_each_other(StringView left, StringView right) wontthrow
    -> bool
{
  usize at_left = 0;
  usize at_right = 0;

  loop
  {
    while (at_left < left.length && left[at_left] == '_')
      at_left++;
    while (at_right < right.length && right[at_right] == '_')
      at_right++;

    if (at_left == left.length || at_right == right.length) break;
    if (fold_name_byte(left[at_left]) != fold_name_byte(right[at_right]))
      return false;

    at_left++;
    at_right++;
  }

  return at_left == left.length && at_right == right.length;
}

/* The same edit distance the command name suggestion spends, so a mistyped
   variable and a mistyped command are judged by one rule. */
pure fn names_are_near_misspellings(StringView left, StringView right) wontthrow
    -> bool
{
  let const budget = utils::suggestion_distance_budget(right.length);
  return utils::NameSuggestion::is_correction(
      utils::bounded_osa_distance(left, right, budget), right.length);
}

} /* namespace */

pure fn is_shell_maintained_variable(StringView name) wontthrow -> bool
{
  if (name.starts_with("KOSH_")) return true;

  return SHELL_MAINTAINED_VARIABLES.contains(name);
}

fn check_command_name_assignments(AnalysisContext &actx) throws -> void
{
  for (let const &assignment : actx.command_name_assignments) {
    if (actx.command_position_names.contains(assignment.name.view())) continue;

    actx.report_diagnostic(diagnostic_id::sc2209, assignment.location,
                           {assignment.name.view(), assignment.value.view()});
  }
}

fn check_unassigned_variable_reads(AnalysisContext &actx) throws -> void
{
  if (actx.reads_before_assignment.count() == 0) return;
  if (!actx.should_report(diagnostic_id::sc2154) &&
      !actx.should_report(diagnostic_id::sc2153))
  {
    return;
  }

  let reads = ArrayList<unassigned_read>{heap_allocator()};
  actx.reads_before_assignment.for_each(
      [&reads](StringView name, const SourceLocation &location)
          throws -> void { reads.push(unassigned_read{name, location}); });

  reads.sort([](const unassigned_read &left, const unassigned_read &right) {
    return left.location.position < right.location.position;
  });

  for (let const &read : reads) {
    resembling_assignment resembled{};
    resembling_assignment misspelled{};
    let const do_match = [&read, &resembled, &misspelled](
                             StringView assigned,
                             const SourceLocation &location) throws -> void {
      if (!resembled.name.is_empty()) return;
      if (assigned == read.name) return;

      if (names_resemble_each_other(assigned, read.name)) {
        resembled = resembling_assignment{assigned, location};
        return;
      }

      if (misspelled.name.is_empty() &&
          names_are_near_misspellings(assigned, read.name))
      {
        misspelled = resembling_assignment{assigned, location};
      }
    };

    actx.assigned_names_so_far.for_each(do_match);
    actx.global_assigned_names.for_each(do_match);
    actx.function_local_names.for_each(do_match);

    if (resembled.name.is_empty()) resembled = misspelled;

    if (resembled.name.is_empty()) {
      actx.report_diagnostic(diagnostic_id::sc2154, read.location, {read.name});
    } else {
      actx.report_diagnostic(diagnostic_id::sc2153, read.location,
                             {read.name, resembled.name}, resembled.location);
    }
  }
}

namespace {

/* Whether an earlier record already carries the name, so a redefinition is
   judged by the first body the file gives the name. */
pure fn definition_is_redefinition(
    const ArrayList<function_definition_record> &definitions,
    usize index) wontthrow -> bool
{
  for (usize earlier = 0; earlier < index; earlier++) {
    if (definitions[earlier].name == definitions[index].name) return true;
  }

  return false;
}

fn check_function_argument_use(AnalysisContext &actx, usize index) throws
    -> void
{
  let const &definition = actx.function_definitions[index];

  let has_call_with_arguments = false;
  let has_call_without_arguments = false;
  for (let const &call : actx.function_calls) {
    if (call.name != definition.name) continue;

    if (call.has_arguments) {
      has_call_with_arguments = true;
      break;
    }

    has_call_without_arguments = true;
  }

  /* A definition no call reaches may belong to a sourced library, where the
     caller lives outside this file. */
  if (has_call_with_arguments || !has_call_without_arguments) return;

  actx.report_diagnostic(diagnostic_id::sc2120, definition.location,
                         {definition.name});

  for (let const &call : actx.function_calls) {
    if (call.name != definition.name) continue;

    actx.report_diagnostic(diagnostic_id::sc2119, call.location,
                           {definition.name}, definition.location);
  }
}

fn check_call_before_definition(AnalysisContext &actx) throws -> void
{
  for (let const &call : actx.function_calls) {
    if (call.is_inside_function_body) continue;

    for (let const &definition : actx.function_definitions) {
      if (definition.name != call.name) continue;

      if (call.location.position < definition.location.position) {
        actx.report_diagnostic(diagnostic_id::sc2218, call.location,
                               {call.name}, definition.location);
      }

      break;
    }
  }
}

} /* namespace */

fn check_function_argument_dataflow(AnalysisContext &actx) throws -> void
{
  if (actx.function_definitions.is_empty()) return;

  if (actx.should_report(diagnostic_id::sc2119) ||
      actx.should_report(diagnostic_id::sc2120))
  {
    for (usize index = 0; index < actx.function_definitions.count(); index++) {
      if (!actx.function_definitions[index].does_read_positionals) continue;
      if (definition_is_redefinition(actx.function_definitions, index))
        continue;

      check_function_argument_use(actx, index);
    }
  }

  /* An interactive chunk runs against a live shell whose functions the file
     never defines, so the order the file states is not the order that runs. */
  if (actx.should_silence_unresolved_commands) return;
  if (!actx.should_report(diagnostic_id::sc2218)) return;

  check_call_before_definition(actx);
}

} /* namespace expressions */

} /* namespace koshka */
