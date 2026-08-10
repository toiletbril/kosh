#pragma once

#include "Containers.hpp"
#include "StringView.hpp"

namespace shit {

enum class diagnostic_tier : u8
{
  Strict,
  Lenient,
  Annoying,
};

struct shellcheck_directive_span
{
  usize position;
  usize length;
};

struct shellcheck_suppression
{
  usize start_position;
  usize end_position;
  ArrayList<shellcheck_directive_span> directives;
};

/* One shellcheck-style static check the analysis stage reports, paired with the
   shellcheck code it mirrors. */
struct shellcheck_check
{
  StringView code;
  StringView summary;
  diagnostic_tier tier{diagnostic_tier::Strict};
};

/* Sorted. */
inline const shellcheck_check SHELLCHECK_CHECKS[] = {
    {"SC1014", "if runs a command directly, not from inside test brackets"},
    {"SC1035", "test brackets and operands require separating spaces"},
    {"SC1037", "a positional parameter above nine needs braces"},
    {"SC2002", "a useless cat, pass the file to the next command",
     diagnostic_tier::Annoying},
    {"SC2003", "expr forks for arithmetic the shell does natively"},
    {"SC2004", "arithmetic variables do not need a dollar sign",
     diagnostic_tier::Annoying},
    {"SC2005", "echo of a command substitution is redundant"},
    {"SC2006", "backticks are harder to nest than command substitutions",
     diagnostic_tier::Annoying},
    {"SC2007", "$[...] is the obsolete arithmetic expansion spelling",
     diagnostic_tier::Annoying},
    {"SC2009", "grepping ps output races the table, use pgrep",
     diagnostic_tier::Lenient},
    {"SC2010", "grepping ls output mangles names, use a glob or find",
     diagnostic_tier::Lenient},
    {"SC2013", "for over command output iterates words, use while read -r"},
    {"SC2015", "A && B || C also runs C when B fails",
     diagnostic_tier::Annoying},
    {"SC2016", "single quotes prevent the expansion written inside them",
     diagnostic_tier::Lenient},
    {"SC2021", "brackets around tr ranges add literal bracket bytes",
     diagnostic_tier::Annoying},
    {"SC2024", "sudo does not elevate shell redirections or glob expansion"},
    {"SC2025", "PS1 control escapes need balanced display guards",
     diagnostic_tier::Annoying},
    {"SC2030", "a pipeline assignment is lost when its stage exits"},
    {"SC2031", "a later read sees the value from before the pipeline"},
    {"SC2035", "a bare glob can expand to an option-shaped filename",
     diagnostic_tier::Annoying},
    {"SC2038", "find piped to xargs breaks on special names, use -print0"},
    {"SC2044", "for over find output breaks on whitespace, use find -exec"},
    {"SC2045", "a for loop over ls output breaks filenames"},
    {"SC2046", "an unquoted command substitution can word-split, quote it"},
    {"SC2048", "$* splits positional parameters, use quoted $@"},
    {"SC2050", "a conditional compares two constant values",
     diagnostic_tier::Annoying},
    {"SC2051", "brace ranges are expanded before variables",
     diagnostic_tier::Lenient},
    {"SC2059", "printf format from a variable can inject format directives"},
    {"SC2060", "tr ranges must be quoted so the shell does not glob them"},
    {"SC2061", "an unquoted find pattern expands before find sees it"},
    {"SC2062", "an unquoted grep pattern can glob against local files"},
    {"SC2063", "a grep pattern that looks like a glob, grep reads regex",
     diagnostic_tier::Lenient},
    {"SC2064", "a double-quoted trap action expands when set, not when fired",
     diagnostic_tier::Lenient},
    {"SC2066", "a quoted for-loop glob stays literal",
     diagnostic_tier::Lenient},
    {"SC2067", "find -exec needs a terminating semicolon or plus"},
    {"SC2068", "an unquoted $@ word-splits and globs each argument"},
    {"SC2069", "2>&1 before the file redirect sends stderr to the tty"},
    {"SC2071", "a string operator performs a lexicographic comparison",
     diagnostic_tier::Lenient},
    {"SC2074", "the test builtin does not support the regex match operator"},
    {"SC2076", "a quoted regular expression is matched literally",
     diagnostic_tier::Lenient},
    {"SC2077", "a conditional operator needs surrounding spaces"},
    {"SC2081", "[ cannot glob-match, use case or [[ ]]",
     diagnostic_tier::Lenient},
    {"SC2086", "an unquoted variable can split and glob, quote it"},
    {"SC2088", "a quoted tilde stays literal instead of naming the home",
     diagnostic_tier::Lenient},
    {"SC2091", "a command substitution in command position executes its output",
     diagnostic_tier::Lenient},
    {"SC2093", "commands after exec do not run when exec succeeds",
     diagnostic_tier::Lenient},
    {"SC2094", "reading and writing the same file in one command truncates"},
    {"SC2095", "a command in a while-read loop can consume the loop input",
     diagnostic_tier::Annoying},
    {"SC2114", "rm -r aimed at a system directory"},
    {"SC2115", "rm -r on \"$var/\" deletes / when the variable is empty"},
    {"SC2116", "a useless echo inside a command substitution",
     diagnostic_tier::Annoying},
    {"SC2124", "a scalar assignment from $@ loses argument boundaries",
     diagnostic_tier::Lenient},
    {"SC2126", "wc -l on grep output, use grep -c to count matches",
     diagnostic_tier::Annoying},
    {"SC2129", "several appends to one file can share a redirection",
     diagnostic_tier::Annoying},
    {"SC2142", "an alias body cannot receive positional arguments",
     diagnostic_tier::Annoying},
    {"SC2144", "a file test on a glob checks only one expanded path"},
    {"SC2145", "$@ inside a longer word concatenates unpredictably"},
    {"SC2146", "find actions after -o need grouping to preserve precedence",
     diagnostic_tier::Lenient},
    {"SC2147", "a tilde inside PATH remains literal", diagnostic_tier::Lenient},
    {"SC2155", "declare and assign separately so the exit status is seen",
     diagnostic_tier::Lenient},
    {"SC2156", "find -exec shell text must receive the filename as an arg"},
    {"SC2157", "a literal test operand makes the test constant",
     diagnostic_tier::Annoying},
    {"SC2162", "read without -r mangles a backslash in the input",
     diagnostic_tier::Lenient},
    {"SC2164",
     "an unchecked cd can leave later commands in the wrong directory", diagnostic_tier::Annoying},
    {"SC2165", "a nested loop reuses the outer loop variable",
     diagnostic_tier::Annoying},
    {"SC2166", "test with -a or -o is obsolescent, join with && or ||",
     diagnostic_tier::Annoying},
    {"SC2167", "a nested loop overwrites the outer loop variable",
     diagnostic_tier::Annoying},
    {"SC2168", "local outside a function has no scope to bind"},
    {"SC2170", "a numeric test operator on a non-numeric literal"},
    {"SC2174", "mkdir -pm applies the mode to the deepest directory only",
     diagnostic_tier::Lenient},
    {"SC2181", "test the command directly rather than $? indirectly",
     diagnostic_tier::Annoying},
    {"SC2183", "printf has fewer arguments than its format consumes",
     diagnostic_tier::Lenient},
    {"SC2184", "an unquoted unset array index can expand as a glob"},
    {"SC2196", "egrep is deprecated, use grep -E", diagnostic_tier::Annoying},
    {"SC2197", "fgrep is deprecated, use grep -F", diagnostic_tier::Annoying},
    {"SC2204", "parentheses start a subshell rather than a file test"},
    {"SC2207", "an array built from command output splits and globs"},
    {"SC2215", "an option-shaped word in command position is not a command"},
    {"SC2216", "piping into a command that ignores stdin",
     diagnostic_tier::Lenient},
    {"SC2217", "redirecting input into a command that ignores stdin",
     diagnostic_tier::Annoying},
    {"SC2221", "an earlier case pattern makes this pattern unreachable",
     diagnostic_tier::Annoying},
    {"SC2222", "an earlier case pattern shadows this pattern",
     diagnostic_tier::Annoying},
    {"SC2229", "read expects a variable name without a dollar sign"},
    {"SC2236", "a negated -z is just -n", diagnostic_tier::Annoying},
    {"SC2237", "a negated -n is just -z", diagnostic_tier::Annoying},
    {"SC2242", "an exit or return code is not a number from 0 to 255",
     diagnostic_tier::Lenient},
    {"SC2244", "a one-operand test reads clearer with -n",
     diagnostic_tier::Annoying},
    {"SC2249", "a case with no default branch can miss an input",
     diagnostic_tier::Annoying},
    {"SC2257", "redirection expansion can run in a child and lose mutation"},
    {"SC2264", "a function wrapper calls itself recursively"},
    {"SC2268", "the x-prefix test workaround is obsolete",
     diagnostic_tier::Annoying},
    {"SC2281", "an assignment name must not start with a dollar sign"},
    {"SC2283", "an assignment cannot contain spaces around equals"},
    {"SC2335", "a negated numeric comparison has a direct operator",
     diagnostic_tier::Annoying},
    {"SC3014", "== is undefined in POSIX test, use ="},
    {"SC3030", "mapfile and readarray are bash array builtins absent from sh"},
    {"SC3037", "echo flags are not in POSIX echo, use printf"},
    {"SC3043", "local is not in POSIX sh, the value stays global"},
    {"SC3044", "declare is not in POSIX, assign plainly under a sh shebang"},
    {"SC3045", "printf -v is a bash extension absent from POSIX printf"},
    {"SC3046", "source is the bash spelling, the POSIX dot command is ."},
};

/* One shit strictness diagnostic, the advisories and downgraded strict errors
   the shell's own default mood emits beyond the shellcheck mirrors. Each is
   named by the option or rule that drives it. */
struct strictness_warning
{
  StringView name;
  StringView summary;
};

/* Sorted. */
inline const strictness_warning STRICTNESS_WARNINGS[] = {
    {"arith-assign",
     "the mistaken array form NAME=((...)) is used for arithmetic "
     "assignment"                                                           },
    {"byte-order-mark",
     "a UTF-8 byte-order mark before the script interferes with the shebang "
     "and first command"                                                    },
    {"failglob",
     "a glob that matches no file errors in the strict default, -W reports "
     "it and keeps the literal text"                                        },
    {"malformed-glob",    "a pattern holds an unterminated '[' class"       },
    {"no-local",
     "an assignment in a function without local leaks to the global scope"  },
    {"nounset",
     "a read of an unset variable errors in the strict default, -W reports "
     "it and expands empty"                                                 },
    {"pipefail",
     "a pipeline reports the rightmost failing stage in the strict default" },
    {"posix-bashism",
     "a bashism such as <<<, |&, or for ((...)) tripped the POSIX parse, the "
     "error names the owning dialect"                                       },
    {"substitution-reap", "a process substitution child could not be reaped"},
    {"typeset-spelling",
     "typeset is the ksh name of declare, the declare spelling is clearer"  },
    {"unquoted-test",
     "an unquoted variable in a test can split or vanish, quote it"         },
    {"use-before-assign",
     "a top-level read of a variable assigned only later sees an empty "
     "value, the read errors in the strict default and -W reports it"       },
};

} // namespace shit
