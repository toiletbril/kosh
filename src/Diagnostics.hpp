#pragma once

#include "StringView.hpp"

namespace shit {

/* One shellcheck-style static check the analysis stage reports, paired with the
   shellcheck code it mirrors. */
struct shellcheck_check
{
  StringView code;
  StringView summary;
};

inline const shellcheck_check SHELLCHECK_CHECKS[] = {
    {"SC2162", "read without -r mangles a backslash in the input"            },
    {"SC2059", "printf format from a variable can inject format directives"  },
    {"SC2166", "test with -a or -o is obsolescent, join with && or ||"       },
    {"SC2236", "a negated -z is just -n"                                     },
    {"SC2237", "a negated -n is just -z"                                     },
    {"SC2046", "an unquoted command substitution can word-split, quote it"   },
    {"SC2002", "a useless cat, pass the file to the next command"            },
    {"SC2244", "a one-operand test reads clearer with -n"                    },
    {"SC2335", "a negated numeric comparison has a direct operator"          },
    {"SC2249", "a case with no default branch can miss an input"             },
    {"SC3014", "== is undefined in POSIX test, use ="                        },
    {"SC2115", "rm -r on \"$var/\" deletes / when the variable is empty"     },
    {"SC2114", "rm -r aimed at a system directory"                           },
    {"SC2062", "an unquoted grep pattern can glob against local files"       },
    {"SC2063", "a grep pattern that looks like a glob, grep reads regex"     },
    {"SC2069", "2>&1 before the file redirect sends stderr to the tty"       },
    {"SC2094", "reading and writing the same file in one command truncates"  },
    {"SC2116", "a useless echo inside a command substitution"                },
    {"SC2242", "an exit or return code is not a number from 0 to 255"        },
    {"SC2145", "$@ inside a longer word concatenates unpredictably"          },
    {"SC2068", "an unquoted $@ word-splits and globs each argument"          },
    {"SC2174", "mkdir -pm applies the mode to the deepest directory only"    },
    {"SC2157", "a literal test operand makes the test constant"              },
    {"SC2170", "a numeric test operator on a non-numeric literal"            },
    {"SC2081", "[ cannot glob-match, use case or [[ ]]"                      },
    {"SC2143", "grep in a test substitution buffers it all, use grep -q"     },
    {"SC2013", "for over command output iterates words, use while read -r"   },
    {"SC2044", "for over find output breaks on whitespace, use find -exec"   },
    {"SC2038", "find piped to xargs breaks on special names, use -print0"    },
    {"SC2216", "piping into a command that ignores stdin"                    },
    {"SC2217", "redirecting input into a command that ignores stdin"         },
    {"SC2064", "a double-quoted trap action expands when set, not when fired"},
    {"SC3037", "echo flags are not in POSIX echo, use printf"                },
    {"SC3044", "declare is not in POSIX, assign plainly under a sh shebang"  },
    {"SC3046", "source is the bash spelling, the POSIX dot command is ."     },
    {"SC2196", "egrep is deprecated, use grep -E"                            },
    {"SC2197", "fgrep is deprecated, use grep -F"                            },
    {"SC2003", "expr forks for arithmetic the shell does natively"           },
    {"SC2168", "local outside a function has no scope to bind"               },
    {"SC2005", "echo of a command substitution is redundant"                 },
    {"SC2009", "grepping ps output races the table, use pgrep"               },
    {"SC2010", "grepping ls output mangles names, use a glob or find"        },
    {"SC2126", "wc -l on grep output, use grep -c to count matches"          },
    {"SC2155", "declare and assign separately so the exit status is seen"    },
    {"SC2181", "test the command directly rather than $? indirectly"         },
    {"SC3043", "local is not in POSIX sh, the value stays global"            },
    {"SC3045", "printf -v is a bash extension absent from POSIX printf"      },
    {"SC3030", "mapfile and readarray are bash array builtins absent from sh"},
    {"SC2086", "an unquoted variable can split and glob, quote it"           },
    {"SC2045", "a for loop over ls output breaks filenames"                  },
    {"SC2207", "an array built from command output splits and globs"         },
    {"SC2061", "an unquoted find pattern expands before find sees it"        },
    {"SC2035", "a bare glob can expand to an option-shaped filename"         },
    {"SC2088", "a quoted tilde stays literal instead of naming the home"     },
    {"SC2066", "a quoted for-loop glob stays literal"                        },
    {"SC2184", "an unquoted unset array index can expand as a glob"          },
    {"SC2229", "read expects a variable name without a dollar sign"          },
    {"SC2048", "$* splits positional parameters, use quoted $@"              },
    {"SC2156", "find -exec shell text must receive the filename as an arg"   },
    {"SC2183", "printf has fewer arguments than its format consumes"         },
    {"SC2024", "sudo does not elevate shell redirections or glob expansion"  },
    {"SC2093", "commands after exec do not run when exec succeeds"           },
    {"SC2050", "a conditional compares two constant values"                  },
    {"SC2071", "a string operator performs a lexicographic comparison"       },
    {"SC2076", "a quoted regular expression is matched literally"            },
    {"SC2077", "a conditional operator needs surrounding spaces"             },
    {"SC2144", "a file test on a glob checks only one expanded path"         },
    {"SC2281", "an assignment name must not start with a dollar sign"        },
    {"SC2283", "an assignment cannot contain spaces around equals"           },
    {"SC1035", "test brackets and operands require separating spaces"        },
    {"SC1014", "if runs a command directly, not from inside test brackets"   },
    {"SC2204", "parentheses start a subshell rather than a file test"        },
    {"SC2215", "an option-shaped word in command position is not a command"  },
    {"SC2051", "brace ranges are expanded before variables"                  },
    {"SC2060", "tr ranges must be quoted so the shell does not glob them"    },
    {"SC2257", "redirection expansion can run in a child and lose mutation"  },
    {"SC2030", "a pipeline assignment is lost when its stage exits"          },
    {"SC2031", "a later read sees the value from before the pipeline"        },
    {"SC2016", "single quotes prevent the expansion written inside them"     },
};

/* One shit strictness diagnostic, the advisories and downgraded strict errors
   the shell's own default mood emits beyond the shellcheck mirrors. Each is
   named by the option or rule that drives it. */
struct strictness_warning
{
  StringView name;
  StringView summary;
};

inline const strictness_warning STRICTNESS_WARNINGS[] = {
    {"nounset",
     "a read of an unset variable errors in the strict default, -W reports "
     "it and expands empty"                                                 },
    {"failglob",
     "a glob that matches no file errors in the strict default, -W reports "
     "it and keeps the literal text"                                        },
    {"pipefail",
     "a pipeline reports the rightmost failing stage in the strict default" },
    {"no-local",
     "an assignment in a function without local leaks to the global scope"  },
    {"use-before-assign",
     "a top-level read of a variable assigned only later sees an empty "
     "value, the read errors in the strict default and -W reports it"       },
    {"arith-assign",
     "an arithmetic assignment uses let, including NAME=$((...)) and the "
     "mistaken array form NAME=((...))"                                     },
    {"typeset-spelling",
     "typeset is the ksh name of declare, the declare spelling is clearer"  },
    {"unquoted-test",
     "an unquoted variable in a test can split or vanish, quote it"         },
    {"malformed-glob",    "a pattern holds an unterminated '[' class"       },
    {"substitution-reap", "a process substitution child could not be reaped"},
    {"posix-bashism",
     "a bashism such as <<<, |&, or for ((...)) tripped the POSIX parse, the "
     "error names the owning dialect"                                       },
    {"byte-order-mark",
     "a UTF-8 byte-order mark before the script interferes with the shebang "
     "and first command"                                                    },
};

} // namespace shit
