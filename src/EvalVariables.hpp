/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements variables evaluation. It applies the corresponding
 * shell semantics through EvalContext while preserving state, source
 * locations, and allocation ownership.
 */

#pragma once

#include "Common.hpp"
#include "Maybe.hpp"
#include "StaticStringMap.hpp"
#include "StringView.hpp"

namespace koshka {

enum class shell_variable_fact : u8
{
  Plain = 0,
  Dynamic = 1 << 0,
  Array = 1 << 1,
  ReadOnly = 1 << 2,
  Exported = 1 << 3,
  BashOnly = 1 << 4,
  NotPosix = 1 << 5,
  Unmaintained = 1 << 6,
};

constexpr fn operator|(shell_variable_fact left,
                       shell_variable_fact right) wontthrow->shell_variable_fact
{
  return static_cast<shell_variable_fact>(static_cast<u8>(left) |
                                          static_cast<u8>(right));
}

constexpr fn has_shell_variable_fact(shell_variable_fact facts,
                                     shell_variable_fact fact) wontthrow -> bool
{
  return (static_cast<u8>(facts) & static_cast<u8>(fact)) != 0;
}

struct shell_variable_description
{
  const char *summary;
  shell_variable_fact facts;
};

inline constexpr char SHELL_ANSI_VARIABLE_PREFIX[] = "KOSH_ANSI_";

inline constexpr shell_variable_description SHELL_ANSI_VARIABLE{
    "The value is a terminal escape, and it is empty when color is disabled.",
    shell_variable_fact::Dynamic};

inline constexpr static_string_entry<shell_variable_description>
    SHELL_VARIABLE_ENTRIES[] = {
        {SSK("?"),
         {"The value is the exit status of the last command.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("$"),
         {"The value is the process id of the original shell, and it stays the "
          "same inside a subshell.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("!"),
         {"The value is the process id of the most recent background command.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("#"),
         {"The value is the number of positional parameters.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("-"),
         {"The value is the set of single-letter shell options that are on.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("*"),
         {"The positional parameters are joined by the first byte of IFS.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("@"),
         {"The positional parameters are expanded as separate words.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("0"),
         {"The value is the name the shell was invoked as.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("_"),
         {"The value is the last argument of the previous command.",
          shell_variable_fact::Dynamic}                                       },

        {SSK("IFS"),
         {"The value controls field splitting and the join performed by "
          "\"$*\". "
          "The default contains a space, a tab, and a newline.",
          shell_variable_fact::Plain}                                         },
        {SSK("LINENO"),
         {"The value is the line number of the command that is running.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("PATH"),
         {"The value is a colon-separated command search path. A restricted "
          "shell refuses to change it.",
          shell_variable_fact::Plain}                                         },
        {SSK("HOME"),
         {"The value is the home directory used by tilde expansion and by a "
          "bare cd.",
          shell_variable_fact::Plain}                                         },
        {SSK("PWD"),
         {"The value is the logical current directory.",
          shell_variable_fact::Plain}                                         },
        {SSK("OLDPWD"),
         {"The value is the logical previous directory, and cd - returns to "
          "it.",
          shell_variable_fact::Plain}                                         },
        {SSK("CDPATH"),
         {"The value supplies the search directories for a relative cd "
          "operand.",
          shell_variable_fact::Plain}                                         },
        {SSK("SHELL"),
         {"The value names the shell executable. A restricted shell refuses to "
          "change it.",
          shell_variable_fact::Plain}                                         },
        {SSK("SHLVL"),
         {"The value is the shell nesting depth, and a child shell increments "
          "it.",
          shell_variable_fact::Exported | shell_variable_fact::NotPosix}      },
        {SSK("ENV"),
         {"The value names the startup file an interactive shell reads. A "
          "restricted shell refuses to change it.",
          shell_variable_fact::Plain}                                         },
        {SSK("BASH_ENV"),
         {"The value names the startup file a non-interactive shell reads. A "
          "restricted shell refuses to change it.",
          shell_variable_fact::Plain}                                         },

        {SSK("PS0"),
         {"The value is the prompt printed after a command is read and before "
          "it runs.",
          shell_variable_fact::Plain}                                         },
        {SSK("PS1"),
         {"The value is the primary interactive prompt.",
          shell_variable_fact::Plain}                                         },
        {SSK("PS2"),
         {"The value is the continuation prompt, and the default is \"> \".",
          shell_variable_fact::Plain}                                         },
        {SSK("PS3"),
         {"The value is the select prompt, and the default is \"#? \".",
          shell_variable_fact::Plain}                                         },
        {SSK("PS4"),
         {"The value is the xtrace prefix, and its first byte is repeated once "
          "per enclosing subshell.",
          shell_variable_fact::Plain}                                         },
        {SSK("PROMPT_COMMAND"),
         {"The command runs before each interactive prompt, and its status "
          "does not replace the previous status.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("COLUMNS"),
         {"The value is the terminal width an interactive shell recorded at "
          "startup.",
          shell_variable_fact::Plain}                                         },
        {SSK("LINES"),
         {"The value is the terminal height an interactive shell recorded at "
          "startup.",
          shell_variable_fact::Plain}                                         },
        {SSK("TERM"),
         {"The value selects the terminal capabilities used on a POSIX system.",
          shell_variable_fact::Plain}                                         },
        {SSK("NO_COLOR"),
         {"A nonempty value disables prompt, diagnostic, and koshkit cat "
          "color.",
          shell_variable_fact::Plain}                                         },

        {SSK("REPLY"),
         {"The value is the line read by read with no name operand, and select "
          "stores the reply here.",
          shell_variable_fact::Plain}                                         },
        {SSK("OPTIND"),
         {"The value is the index of the next operand getopts reads.",
          shell_variable_fact::Plain}                                         },
        {SSK("OPTARG"),
         {"The value is the argument of the option getopts read last.",
          shell_variable_fact::Plain}                                         },
        {SSK("OPTERR"),
         {"A value of zero silences the getopts error message.",
          shell_variable_fact::Plain}                                         },
        {SSK("TIMEFORMAT"),
         {"The value is the format the time keyword prints.",
          shell_variable_fact::Plain}                                         },
        {SSK("PIPESTATUS"),
         {"The elements are the exit status of every stage of the last "
          "pipeline.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix}         },
        {SSK("BASH_XTRACEFD"),
         {"The value selects the xtrace descriptor, and standard error is used "
          "when it is unset or invalid.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("MANPATH"),
         {"Completion searches these manual paths for a subcommand and an "
          "option.",
          shell_variable_fact::Plain}                                         },
        {SSK("PAGER"),
         {"The value names the program that pages long output.",
          shell_variable_fact::Plain}                                         },
        {SSK("EDITOR"),
         {"The value names the editor fc uses when FCEDIT is unset.",
          shell_variable_fact::Plain}                                         },
        {SSK("VISUAL"),
         {"The value names the preferred full-screen editor.",
          shell_variable_fact::Plain}                                         },
        {SSK("FCEDIT"),
         {"The value names the editor fc uses, and it takes precedence over "
          "EDITOR.",
          shell_variable_fact::Plain}                                         },
        {SSK("HISTFILE"),
         {"The value names the command history file. A restricted shell "
          "refuses to change it.",
          shell_variable_fact::Plain}                                         },
        {SSK("HISTSIZE"),
         {"The value bounds the number of retained history events.",
          shell_variable_fact::Plain}                                         },
        {SSK("GLOBIGNORE"),
         {"The value is a colon-separated list of patterns a glob result "
          "excludes.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("POSIXLY_CORRECT"),
         {"A set value selects POSIX behavior.", shell_variable_fact::Plain}  },
        {SSK("TMPDIR"),
         {"The value names the directory a temporary file is created in.",
          shell_variable_fact::Plain}                                         },
        {SSK("TZ"),
         {"The value selects the time zone.", shell_variable_fact::Plain}     },
        {SSK("USER"),
         {"The value is the login name of the current user.",
          shell_variable_fact::Plain}                                         },
        {SSK("LOGNAME"),
         {"The value is the login name the system recorded.",
          shell_variable_fact::Plain}                                         },
        {SSK("DISPLAY"),
         {"The value names the X display.", shell_variable_fact::Plain}       },
        {SSK("MAIL"),
         {"The value names the mailbox that is checked for new mail.",
          shell_variable_fact::Plain}                                         },
        {SSK("MAILCHECK"),
         {"The value is the mail check interval in seconds.",
          shell_variable_fact::Plain}                                         },
        {SSK("MAILPATH"),
         {"The value is a colon-separated list of mailboxes.",
          shell_variable_fact::Plain}                                         },
        {SSK("LANG"),
         {"The value is the default locale.", shell_variable_fact::Plain}     },
        {SSK("LC_ALL"),
         {"The value overrides every other locale setting.",
          shell_variable_fact::Plain}                                         },
        {SSK("LC_COLLATE"),
         {"The value selects the collation order.",
          shell_variable_fact::Plain}                                         },
        {SSK("LC_CTYPE"),
         {"The value selects character classification.",
          shell_variable_fact::Plain}                                         },
        {SSK("LC_MESSAGES"),
         {"The value selects the message language.",
          shell_variable_fact::Plain}                                         },
        {SSK("LC_NUMERIC"),
         {"The value selects number formatting.", shell_variable_fact::Plain} },
        {SSK("LC_TIME"),
         {"The value selects time formatting.", shell_variable_fact::Plain}   },

        {SSK("KOSH"),
         {"The value is the path the shell was invoked from.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_VERSION"),
         {"The value is the shell version.", shell_variable_fact::Plain}      },
        {SSK("KOSH_COMMIT"),
         {"The value is the commit the shell was built from.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_BUILD_MODE"),
         {"The value is the build mode of the running executable.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_OS"),
         {"The value describes the build host distribution and kernel.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_FLAGS"),
         {"The value supplies default command-line options.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_HISTORY"),
         {"The value names the command history file, and the default is "
          "~/.kosh_history.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_CALC_HISTORY"),
         {"The value names the interactive calc history file.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_DIRECTORY_HISTORY"),
         {"The value names the z frecency store.", shell_variable_fact::Plain}},
        {SSK("KOSH_WELCOME"),
         {"A nonempty value is printed once at interactive startup, and an "
          "empty value prints nothing.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_FAREWELL"),
         {"A nonempty value is printed once at exit, and an empty value prints "
          "nothing.",
          shell_variable_fact::Plain}                                         },
        {SSK("KOSH_GIT_BRANCH"),
         {"The branch name is read from the repository that contains the "
          "working directory, and a detached head supplies a short commit "
          "hash.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("KOSH_GIT_AHEAD"),
         {"The value is the number of commits the branch is ahead of its "
          "upstream, and it is empty when the count is zero.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("KOSH_GIT_BEHIND"),
         {"The value is the number of commits the branch is behind its "
          "upstream, and it is empty when the count is zero.",
          shell_variable_fact::Dynamic}                                       },
        {SSK("KOSH_IDENTITY"),
         {"The value is the lowercase CRC-32 identity of the running "
          "executable, and an inherited value is discarded.",
          shell_variable_fact::Dynamic | shell_variable_fact::ReadOnly |
              shell_variable_fact::Exported}                                  },

        {SSK("RANDOM"),
         {"Each read supplies a new random number between 0 and 32767.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("SRANDOM"),
         {"Each read supplies a new 32-bit random number.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("SECONDS"),
         {"The value is the number of seconds since the shell started.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("EPOCHSECONDS"),
         {"The value is the wall clock time in seconds since the epoch.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("EPOCHREALTIME"),
         {"The value is the wall clock time in seconds with microseconds after "
          "the decimal point.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_MONOSECONDS"),
         {"The value is a monotonic millisecond count.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASHPID"),
         {"The value is the process id of the current evaluator, and it "
          "differs from $$ inside a subshell.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("PPID"),
         {"The value is the process id of the parent.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("UID"),
         {"The value is the real user id.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("EUID"),
         {"The value is the effective user id.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("GROUPS"),
         {"The value lists the groups the user belongs to.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("HOSTNAME"),
         {"The value is the host name.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("HOSTTYPE"),
         {"The value names the machine architecture.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("MACHTYPE"),
         {"The value describes the system type.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("OSTYPE"),
         {"The value names the operating system.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("SHELLOPTS"),
         {"The value is a colon-separated list of the set options that are on.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_SUBSHELL"),
         {"The value is the current subshell nesting depth.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_ARGV0"),
         {"The value is argument zero of the shell.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_EXECUTION_STRING"),
         {"The value is the command string the -c option supplied.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_COMMAND"),
         {"The value is the command that is running.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_SOURCE"),
         {"The value is the source file of the running function.",
          shell_variable_fact::Dynamic | shell_variable_fact::BashOnly}       },
        {SSK("BASH_LINENO"),
         {"The elements are the line number of each active call.",
          shell_variable_fact::Dynamic | shell_variable_fact::Array |
              shell_variable_fact::BashOnly}                                  },
        {SSK("FUNCNAME"),
         {"The elements are the name of each active function, and the "
          "innermost call is first.",
          shell_variable_fact::Dynamic | shell_variable_fact::Array |
              shell_variable_fact::BashOnly}                                  },

        {SSK("BASH"),
         {"The value names the shell executable, and it is seeded in the bash "
          "mood.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("BASH_VERSION"),
         {"The value is the advertised bash version, and it is seeded in the "
          "bash mood.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("BASH_VERSINFO"),
         {"The elements are the advertised bash version fields.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix}         },
        {SSK("BASH_REMATCH"),
         {"The elements are the groups of the last [[ =~ ]] match.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix}         },

        {SSK("COMP_WORDS"),
         {"The elements are the words on the line that is being completed.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix}         },
        {SSK("COMP_CWORD"),
         {"The value is the index of the word under the cursor.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("COMP_LINE"),
         {"The value is the complete line that is being completed.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("COMP_POINT"),
         {"The value is the cursor byte offset in the line.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("COMP_WORDBREAKS"),
         {"The value holds the bytes that split a word during completion.",
          shell_variable_fact::NotPosix}                                      },
        {SSK("COMPREPLY"),
         {"A completion function writes its candidates here.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix}         },
        {SSK("BASH_COMPLETION_VERSINFO"),
         {"The value advertises the bash-completion version to a completion "
          "script.",
          shell_variable_fact::NotPosix}                                      },

        {SSK("BASHOPTS"),
         {"The value is a colon-separated list of the shopt options that are "
          "on in bash.",
          shell_variable_fact::NotPosix | shell_variable_fact::Unmaintained}  },
        {SSK("BASH_ALIASES"),
         {"The name is an associative array of the defined aliases in bash.",
          shell_variable_fact::NotPosix | shell_variable_fact::Unmaintained}  },
        {SSK("BASH_ARGC"),
         {"The elements are the argument count of each active call in bash.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix |
              shell_variable_fact::Unmaintained}                              },
        {SSK("BASH_ARGV"),
         {"The elements are the arguments of each active call in bash.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix |
              shell_variable_fact::Unmaintained}                              },
        {SSK("DIRSTACK"),
         {"The elements are the directory stack entries in bash. The dirs "
          "builtin reports the stack this shell keeps.",
          shell_variable_fact::Array | shell_variable_fact::NotPosix |
              shell_variable_fact::Unmaintained}                              },
};
inline constexpr StaticStringMap SHELL_VARIABLES{SHELL_VARIABLE_ENTRIES};

inline fn describe_shell_variable(StringView name) throws
    -> Maybe<shell_variable_description>
{
  if (name.is_empty()) return None;

  if (name.starts_with(StringView{SHELL_ANSI_VARIABLE_PREFIX,
                                  countof(SHELL_ANSI_VARIABLE_PREFIX) - 1}))
  {
    return SHELL_ANSI_VARIABLE;
  }

  return SHELL_VARIABLES.find(name);
}

} /* namespace koshka */
