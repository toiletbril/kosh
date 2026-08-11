#include "Diagnostics.hpp"

#include "ExpressionsInternal.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"
#include "Tokens.hpp"

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
    D(1014, "direct-command-in-test",
      "if runs a command directly, not inside test brackets",
      "Test brackets do not run the command written inside them",
      "Run the command directly as the if condition", None, Strict, Policy),
    D(1035, "test-bracket-spacing",
      "test brackets and operands require separating spaces",
      "Test brackets and operands require separating spaces",
      "Add spaces after the opening bracket and before the close", None, Strict,
      Policy),
    D(1037, "positional-parameter-braces",
      "a positional parameter above nine needs braces",
      "A positional parameter above nine needs braces",
      "Write ${10} to select positional parameter 10", None, Strict, Policy),
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
    D(2009, "grep-ps", "grepping ps output races the process table",
      "Grepping the ps output races the process table and matches the grep "
      "itself",
      "Use pgrep to match a process by name", None, Annoying, Policy),
    D(2010, "grep-ls", "grepping ls output mangles names",
      "Grepping the ls listing mangles a name with a space or a newline",
      "Match the names with a glob or with find instead", None, Lenient,
      Policy),
    D(2013, "for-command-output", "for over command output iterates words",
      "A for over the cat output iterates IFS-split words rather than lines",
      "Read the lines with 'while IFS= read -r line' instead", None, Strict,
      Policy),
    D(2015, "and-or-else", "A && B || C also runs C when B fails",
      "A && B || C also runs C when B fails",
      "Use an if statement when C is the else branch", None, Annoying, Policy),
    D(2016, "single-quoted-expansion", "single quotes prevent expansion",
      "Single quotes prevent the expansion written inside them",
      "Use double quotes if the value should expand", None, Lenient, Policy),
    D(2021, "tr-bracket-range", "brackets around tr ranges add literal bytes",
      "Brackets around a tr range add literal bracket bytes",
      "Use a quoted range without brackets", None, Annoying, Policy),
    D(2024, "sudo-glob", "sudo does not elevate shell glob expansion",
      "The shell expands this glob before sudo changes privileges",
      "Run the glob expansion inside a shell under sudo", None, Strict, Policy),
    D(2024, "sudo-redirection", "sudo does not elevate shell redirections",
      "The shell opens this redirection before sudo changes privileges",
      "Run a shell under sudo or pipe through sudo tee", None, Strict, Policy),
    D(2025, "prompt-display-guards", "PS1 control escapes need display guards",
      "PS1 control escapes need balanced display guards",
      "Wrap nonprinting prompt escapes in \\[ and \\]", None, Annoying, Policy),
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
    D(2035, "option-shaped-glob",
      "a bare glob can expand to an option-shaped filename",
      "A bare glob can expand to a filename that begins with '-'",
      "Prefix the glob with a directory or place -- before it", None, Annoying,
      Policy),
    D(2038, "find-xargs-names", "find piped to xargs breaks on special names",
      "An xargs splits the find output on whitespace and quotes",
      "Pair find -print0 with xargs -0 or use find -exec", None, Strict,
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
    D(2050, "constant-comparison", "a conditional compares two constant values",
      "The '{0}' comparison has literal operands on both sides",
      "Remove the constant condition or compare runtime data", None, Annoying,
      Policy),
    D(2051, "variable-brace-range", "brace ranges expand before variables",
      "Brace ranges are expanded before variables",
      "Use an arithmetic loop for a variable limit", None, Lenient, Policy),
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
    D(2071, "string-numeric-operator",
      "a string operator compares lexicographically",
      "The {0} operator compares strings lexicographically",
      "Use an arithmetic command or a numeric -lt or -gt operator", None,
      Lenient, Policy),
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
    D(2081, "test-glob-match", "test cannot glob-match strings",
      "[ and test compare strings byte for byte and never glob-match",
      "Use a case or the [[ ]] form for the pattern", None, Lenient, Policy),
    D(2086, "unquoted-expansion", "an unquoted variable can split and glob",
      "An unquoted variable can split into words and expand globs",
      "Quote the expansion to keep one argument", None, Strict, Policy),
    D(2086, "unquoted-test-expansion",
      "an unquoted test variable can split or vanish",
      "A test reads an unquoted variable",
      "Quote it to avoid an empty or split argument", None, Strict, Policy),
    D(2088, "quoted-tilde", "a quoted tilde remains literal",
      "A quoted tilde stays literal instead of expanding to the home directory",
      "Leave the tilde unquoted or use a quoted $HOME expansion", None, Lenient,
      Policy),
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
    D(2124, "scalar-at-assignment",
      "a scalar assignment from $@ loses boundaries",
      "A scalar assignment from $@ loses argument boundaries",
      "Assign one value or use an array", None, Lenient, Policy),
    D(2126, "grep-wc-count", "wc -l on grep output runs an extra process",
      "Counting grep output with wc -l runs an extra process",
      "Use grep -c to count the matching lines directly", None, Annoying,
      Policy),
    D(2129, "repeated-append", "several appends can share one redirection",
      "Several commands append to the same file separately",
      "Apply one append redirection to a grouped command",
      "this later append belongs under the same redirection", Annoying, Policy),
    D(2142, "alias-positional-arguments",
      "an alias body cannot receive positional arguments",
      "An alias body cannot receive positional arguments",
      "Use a function when the wrapper needs arguments", None, Annoying,
      Policy),
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
    D(2162, "read-without-r", "read without -r mangles backslashes",
      "A read without -r mangles a backslash in the input",
      "Add -r to read the line literally", None, Lenient, Policy),
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
    D(2174, "mkdir-parent-mode",
      "mkdir -pm applies mode only to the deepest directory",
      "A mkdir -pm applies the mode only to the deepest directory, the created "
      "parents keep the umask default",
      None, None, Lenient, Policy),
    D(2181, "indirect-exit-status-test",
      "test the command instead of testing $?",
      "Testing $? checks the exit status indirectly",
      "Test the command directly with if or && so an intervening command "
      "cannot clobber the status",
      None, Annoying, Policy),
    D(2183, "printf-missing-arguments",
      "printf has fewer arguments than its format consumes",
      "The printf format consumes more arguments than the command supplies",
      "Add the missing arguments or remove format directives", None, Lenient,
      Policy),
    D(2184, "unquoted-unset-index",
      "an unquoted unset array index can expand as a glob",
      "An unquoted unset array index can expand as a filename glob",
      "Quote the complete array element name", None, Strict, Policy),
    D(2196, "deprecated-egrep", "egrep is deprecated",
      "The egrep command is deprecated",
      "Use grep -E for the extended regular expression match", None, Annoying,
      Policy),
    D(2197, "deprecated-fgrep", "fgrep is deprecated",
      "The fgrep command is deprecated",
      "Use grep -F for the fixed string match", None, Annoying, Policy),
    D(2204, "parentheses-subshell",
      "parentheses start a subshell instead of a test",
      "Parentheses start a subshell rather than a file or string test",
      "Use [[ ... ]] or [ ... ] for the condition", None, Strict, Policy),
    D(2207, "array-from-command-output",
      "an array from command output splits and globs",
      "An array built from command output splits words and expands globs",
      "Use mapfile or readarray to preserve output records", None, Strict,
      Policy),
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
    D(2229, "read-variable-dollar",
      "read expects a variable name without a dollar sign",
      "A read operand is a variable name, not a variable value",
      "Drop the dollar sign from the variable name", None, Strict, Policy),
    D(2236, "negated-z", "a negated -z is -n", "A negated -z is just -n",
      "Test with -n instead", None, Annoying, Policy),
    D(2237, "negated-n", "a negated -n is -z", "A negated -n is just -z",
      "Test with -z instead", None, Annoying, Policy),
    D(2242, "invalid-status-code",
      "an exit or return code is outside 0 through 255",
      "The code '{0}' is not a number from 0 to 255, {1} either rejects it or "
      "wraps it modulo 256",
      None, None, Lenient, Policy),
    D(2244, "one-operand-test", "a one-operand test is a nonempty-string test",
      "A one-operand test is the nonempty-string test",
      "Write it with -n to read clearer", None, Annoying, Policy),
    D(2249, "case-without-default", "a case without a default can miss input",
      "This case has no default *) branch, a value no pattern matches is "
      "silently ignored",
      None, None, Annoying, Policy),
    D(2257, "redirection-mutation", "redirection expansion can lose mutation",
      "A redirection expansion can run in a child and lose its mutation",
      "Update the variable before forming the redirect path", None, Strict,
      Policy),
    D(2264, "recursive-wrapper", "a function wrapper calls itself recursively",
      "This function wrapper calls itself recursively",
      "Use command before the wrapped command name", None, Strict, Policy),
    D(2268, "x-prefix-test", "the x-prefix test workaround is obsolete",
      "The x-prefix test workaround is obsolete", "Quote the variable directly",
      None, Annoying, Policy),
    D(2281, "dollar-assignment-name",
      "an assignment name must not start with a dollar sign",
      "An assignment name must not start with a dollar sign",
      "Remove the dollar sign from the assignment name", None, Strict, Policy),
    D(2283, "assignment-equals-spacing",
      "an assignment cannot contain spaces around equals",
      "An assignment cannot contain spaces around equals",
      "Write NAME=value without spaces", None, Strict, Policy),
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
    D(3014, "posix-test-equals", "== is undefined in POSIX test",
      "== is undefined in POSIX test", "Use = for string equality", None,
      Strict, Policy),
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
    D(0, "byte-order-mark", "a byte-order mark precedes the script",
      "A UTF-8 byte-order mark precedes the script text",
      "Save the script as UTF-8 without a byte-order mark", None, Strict,
      Policy),
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
      "The variable '{0}' is read before it is assigned", None, None, Lenient,
      Policy),
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
        C("arch", Arch, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("basename", Basename, NEUTRAL_READER_GROUPS),
        C("builtin", Builtin, NO_COMMAND_GROUP),
        C("chmod", Chmod, COMMAND_GROUP_NON_STDIN_READER),
        C("chown", Chown, COMMAND_GROUP_NON_STDIN_READER),
        C("command", Command, NO_COMMAND_GROUP),
        C("cp", Cp, COMMAND_GROUP_NON_STDIN_READER),
        C("date", Date, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("declare", Declare, DECLARATION_GROUPS),
        C("dirname", Dirname, NEUTRAL_READER_GROUPS),
        C("echo", Echo, NEUTRAL_READER_GROUPS),
        C("egrep", Egrep, NO_COMMAND_GROUP),
        C("eval", Eval,
          COMMAND_GROUP_RUNTIME_DEFINER | COMMAND_GROUP_VARIABLE_PROBE),
        C("exit", Exit, NO_COMMAND_GROUP),
        C("export", Export, EXPORT_GROUPS),
        C("expr", Expr, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("false", False, NEUTRAL_READER_GROUPS),
        C("fgrep", Fgrep, NO_COMMAND_GROUP),
        C("find", Find, NO_COMMAND_GROUP),
        C("getopts", Getopts, COMMAND_GROUP_VARIABLE_TARGET),
        C("grep", Grep, NO_COMMAND_GROUP),
        C("hostname", Hostname, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("id", Id, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("kill", Kill, COMMAND_GROUP_NON_STDIN_READER),
        C("let", Let, COMMAND_GROUP_VARIABLE_PROBE),
        C("ln", Ln, COMMAND_GROUP_NON_STDIN_READER),
        C("local", Local, DECLARATION_GROUPS),
        C("mapfile", Mapfile, COMMAND_GROUP_VARIABLE_TARGET),
        C("mkdir", Mkdir, COMMAND_GROUP_NON_STDIN_READER),
        C("mv", Mv, COMMAND_GROUP_NON_STDIN_READER),
        C("printf", Printf, COMMAND_GROUP_NON_STDIN_READER),
        C("pwd", Pwd, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("read", Read, COMMAND_GROUP_VARIABLE_TARGET),
        C("readarray", Readarray, COMMAND_GROUP_VARIABLE_TARGET),
        C("readonly", Readonly, EXPORT_GROUPS),
        C("return", Return, NO_COMMAND_GROUP),
        C("rm", Rm, COMMAND_GROUP_NON_STDIN_READER),
        C("rmdir", Rmdir, COMMAND_GROUP_NON_STDIN_READER),
        C("seq", Seq, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("sleep", Sleep, COMMAND_GROUP_NON_STDIN_READER),
        C("source", Source, COMMAND_GROUP_RUNTIME_DEFINER),
        C("ssh", Ssh, NO_COMMAND_GROUP),
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
        C("which", Which, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("whoami", Whoami, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
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

/* The numeric comparison operators of test, for the SC2170 lint. */
constexpr PackedStringKey TEST_NUMERIC_OPERATOR_KEYS[] = {
    SSK("-eq"), SSK("-ne"), SSK("-lt"), SSK("-le"), SSK("-gt"), SSK("-ge"),
};
constexpr StaticStringSet TEST_NUMERIC_OPERATORS{TEST_NUMERIC_OPERATOR_KEYS};

cold fn is_test_numeric_operator_word(StringView op) wontthrow -> bool
{
  return TEST_NUMERIC_OPERATORS.contains(op);
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

cold fn printf_consumed_argument_count(StringView format) wontthrow -> usize
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

fn check_posix_parameter_expansion(AnalysisContext &actx,
                                   const WordSegment &segment, StringView text,
                                   SourceLocation fallback_location) throws
    -> void
{
  if (text.is_empty()) return;

  let const do_get_location = [&]() -> SourceLocation {
    return segment.get_source_location(fallback_location.filename)
        .value_or(fallback_location);
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

} /* namespace */

fn check_posix_word_portability(AnalysisContext &actx,
                                const WordSegment &segment,
                                SourceLocation fallback_location) throws -> void
{
  let const text = segment.text.view();
  let const do_get_location = [&]() -> SourceLocation {
    return segment.get_source_location(fallback_location.filename)
        .value_or(fallback_location);
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
    Maybe<SourceLocation> exec_location{};
    Maybe<SourceLocation> or_location{};
    for (usize i = 1; i < args.count(); i++) {
      let const literal = args[i]->raw_string();
      if (literal.view() == "-exec" || literal.view() == "-execdir") {
        has_exec = true;
        exec_location = args[i]->source_location();
      } else if (has_exec && (literal.view() == ";" || literal.view() == "+")) {
        has_exec_terminator = true;
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
    for (usize i = 1; i < args.count() && i <= 2; i++) {
      let const literal = args[i]->raw_string();
      if (literal.length() >= 5 && literal[0] == '[' &&
          literal[literal.length() - 1] == ']' &&
          literal.view().find_character('-').has_value())
      {
        actx.report_diagnostic(diagnostic_id::sc2021,
                               args[i]->source_location());
      }
    }
    break;

  case command_name_id::Echo:
    if (args.count() == 2 && args[1]->kind() == Token::Kind::Word) {
      let const &word = static_cast<const tokens::WordToken *>(args[1])->word();
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::CommandSubstitution)
      {
        actx.report_diagnostic(diagnostic_id::sc2005,
                               args[0]->source_location());
      }
    }
    break;

  default: break;
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

  if (command_literal.length > 1 && command_literal[0] == '$' &&
      lexer::is_variable_name_start(command_literal[1]) &&
      command_literal.find_character('=').has_value())
    actx.report_diagnostic(diagnostic_id::sc2281, location);

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

  if (args.count() >= 2 && args[1]->raw_view() == StringView{"="})
    actx.report_diagnostic(diagnostic_id::sc2283, args[1]->source_location());
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
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      if (should_skip_option_operand) {
        should_skip_option_operand = false;
        continue;
      }
      if (literal.view() == "-p" || literal.view() == "-t" ||
          literal.view() == "-n" || literal.view() == "-N" ||
          literal.view() == "-d" || literal.view() == "-u" ||
          literal.view() == "-i")
      {
        should_skip_option_operand = true;
        continue;
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

  case command_name_id::Export:
    if (!args_have_short_flag(args, 'n')) {
      for (usize i = 1; i < args.count(); i++) {
        let const raw = args[i]->raw_string();
        if (raw.view().starts_with(StringView{"CDPATH="}) ||
            raw.view() == "CDPATH")
          actx.report_diagnostic(diagnostic_id::exported_cdpath,
                                 args[i]->source_location());
      }
    }
    break;

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

      if (predicate.view() == "-exec" && i + 3 < args.count()) {
        let const shell_name = args[i + 1]->raw_string();
        let const shell_flag = args[i + 2]->raw_string();
        let const script = args[i + 3]->raw_string();
        if ((shell_name.view() == "sh" || shell_name.view() == "bash") &&
            shell_flag.view() == "-c" &&
            view_contains(script.view(), StringView{"{}"}))
          actx.report_diagnostic(diagnostic_id::sc2156,
                                 args[i + 3]->source_location());
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
    }
    break;

  case command_name_id::Printf: {
    usize format_index = 1;
    if (format_index < args.count() &&
        args[format_index]->raw_string().view() == "-v")
      format_index += 2;
    if (format_index < args.count() &&
        args[format_index]->kind() == Token::Kind::Word)
    {
      let const &format_word =
          static_cast<const tokens::WordToken *>(args[format_index])->word();
      if (word_is_fully_literal(format_word)) {
        let const format = format_word.to_literal_string();
        let const consumed = printf_consumed_argument_count(format.view());
        let const available = args.count() - format_index - 1;
        if (consumed > available)
          actx.report_diagnostic(diagnostic_id::sc2183,
                                 args[format_index]->source_location());
      }
    }
    break;
  }

  case command_name_id::Sudo:
    for (let const &redirection : input.redirections)
      if (redirection.target != nullptr)
        actx.report_diagnostic(diagnostic_id::sc2024_redirection,
                               redirection.target->source_location());
    for (usize i = 1; i < args.count(); i++)
      if (args[i]->kind() == Token::Kind::Word &&
          word_is_bare_glob(
              static_cast<const tokens::WordToken *>(args[i])->word()))
        actx.report_diagnostic(diagnostic_id::sc2024_glob,
                               args[i]->source_location());
    break;

  default: break;
  }
}

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
          actx.report_diagnostic(diagnostic_id::sc2086_test,
                                 args[i]->source_location());
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
  case command_name_id::Readarray:
    if (is_posix)
      actx.report_diagnostic(diagnostic_id::sc3030, location,
                             {input.command_literal});
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
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Assignment) continue;
      let const &value =
          static_cast<const tokens::Assignment *>(args[i])->value_word();
      let value_has_substitution = false;
      for (let const &segment : value.segments)
        if (segment.kind == WordSegment::Kind::CommandSubstitution) {
          value_has_substitution = true;
          break;
        }
      if (!value_has_substitution) continue;
      actx.report_diagnostic(diagnostic_id::sc2155, args[i]->source_location());
      break;
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
     SC2062, a pattern with a leading * that has nothing to repeat is SC2063.
     The pattern is the first word past the options. */
  case command_name_id::Grep:
  case command_name_id::Egrep:
  case command_name_id::Fgrep:
    for (usize i = 1; i < args.count(); i++) {
      if (args[i]->kind() != Token::Kind::Word) continue;
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const literal = word.to_literal_string();
      let const view = literal.view();
      if (view.length >= 1 && view[0] == '-') continue;
      if (word.segments.count() == 1 &&
          word.segments[0].kind == WordSegment::Kind::UnquotedText &&
          word.segments[0].has_glob_metacharacter())
      {
        actx.report_diagnostic(diagnostic_id::sc2062,
                               args[i]->source_location());
      } else if (!view.is_empty() && view[0] == '*') {
        actx.report_diagnostic(diagnostic_id::sc2063,
                               args[i]->source_location());
      }
      break;
    }
    break;

  /* mkdir -pm applies the mode only to the deepest directory, shellcheck
     SC2174. */
  case command_name_id::Mkdir:
    if (args_have_short_flag(args, 'p') && args_have_short_flag(args, 'm'))
      actx.report_diagnostic(diagnostic_id::sc2174, input.command_location());
    break;

  /* An exit or return code outside the literal 0-255 shape errors or wraps
     modulo 256, shellcheck SC2242. */
  case command_name_id::Exit:
  case command_name_id::Return:
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
      }
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

    let const is_file_output =
        redirection.kind == Redirection::Kind::TruncateOutput ||
        redirection.kind == Redirection::Kind::TruncateOutputOverride;
    if (is_file_output && redirection.fd == 1 && saw_stderr_to_stdout &&
        redirection.target != nullptr)
    {
      actx.report_diagnostic(diagnostic_id::sc2069,
                             redirection.target->source_location());
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

  /* A single-operand test with no operator is the nonempty-string test,
     shellcheck SC2244. A flag-shaped operand is left alone so [ -n ] is not
     told to use -n. */
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
  if (bracket_form_is_closed && operand_end == 2 &&
      args[1]->kind() == Token::Kind::Word)
  {
    let const operand = static_cast<const tokens::WordToken *>(args[1])
                            ->word()
                            .to_literal_string();
    if (!(operand.view().length >= 1 && operand.view()[0] == '-'))
      actx.report_diagnostic(diagnostic_id::sc2244, args[1]->source_location());
  }

  /* The operand-shape lints over the closed operand range. A -z or -n on a
     literal operand is SC2157, a numeric comparison against a non-numeric
     literal is SC2170, and a = or == against a glob literal is SC2081. */
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
      if (word_is_fully_literal(next))
        actx.report_diagnostic(diagnostic_id::sc2157,
                               args[i + 1]->source_location(), {view});
    }

    if (is_test_numeric_operator_word(view)) {
      for (usize side = i - 1; side <= i + 1; side += 2) {
        /* Index zero is the command word, never an operand. */
        if (side == 0 || side >= operand_end ||
            args[side]->kind() != Token::Kind::Word)
          continue;
        let const &operand =
            static_cast<const tokens::WordToken *>(args[side])->word();
        if (!word_is_fully_literal(operand)) continue;
        let const operand_literal = operand.to_literal_string();
        if (!view_is_integer_literal(operand_literal.view()))
          actx.report_diagnostic(diagnostic_id::sc2170,
                                 args[side]->source_location(),
                                 {view, operand_literal.view()});
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

/* A prefix assignment does not affect the expansion on the same command, so a
   reference to one of its names reads the old value. */
fn check_prefix_assignment_reads(AnalysisContext &actx,
                                 const command_lint_input &input) throws -> void
{
  if (input.local_vars.is_empty()) return;

  let const &args = input.args;

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

} /* namespace expressions */

} /* namespace koshka */
