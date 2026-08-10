#include "Diagnostics.hpp"

namespace shit {

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
      Annoying, Policy),
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
    D(3014, "posix-test-equals", "== is undefined in POSIX test",
      "== is undefined in POSIX test", "Use = for string equality", None,
      Strict, Policy),
    D(3030, "posix-array-reader",
      "mapfile and readarray are absent from POSIX sh",
      "{0} is a bash array builtin absent from POSIX sh",
      "read the input with a while read loop or switch the shebang to bash",
      None, Strict, Policy),
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

static pure fn diagnostic_selector_matches(
    StringView selector, const diagnostic_definition &definition) wontthrow
    -> bool
{
  if (selector == StringView{"all"}) return true;
  if (selector == StringView{definition.slug}) return true;

  let const code = parse_diagnostic_code(selector);
  if (code.has_value()) {
    return definition.shellcheck_code != 0 &&
           *code == definition.shellcheck_code;
  }

  let const separator = selector.find_character('-');
  if (!separator.has_value()) return false;

  let const range_start =
      parse_diagnostic_code(selector.substring_of_length(0, *separator));
  let const range_end =
      parse_diagnostic_code(selector.substring(*separator + 1));
  if (!range_start.has_value() || !range_end.has_value()) return false;

  return definition.shellcheck_code != 0 &&
         definition.shellcheck_code >= *range_start &&
         definition.shellcheck_code < *range_end;
}

static pure fn diagnostic_value_disables(
    StringView value, const diagnostic_definition &definition) wontthrow -> bool
{
  usize component_start = 0;
  while (component_start <= value.length) {
    usize component_end = component_start;
    while (component_end < value.length && value[component_end] != ',') {
      component_end++;
    }
    let const component = value.substring_of_length(
        component_start, component_end - component_start);
    if (diagnostic_selector_matches(component, definition)) return true;
    if (component_end == value.length) break;
    component_start = component_end + 1;
  }

  return false;
}

pure fn diagnostic_directive_disables(StringView comment,
                                      diagnostic_id id) wontthrow -> bool
{
  let const &definition = get_diagnostic_definition(id);
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
    return false;
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
      if (position == comment.length) return false;
    } else {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
    }
    let const value =
        comment.substring_of_length(value_start, position - value_start);
    if (diagnostic_value_disables(value, definition)) return true;
    if (quote != '\0' && position < comment.length) position++;
  }

  return false;
}

} /* namespace shit */
