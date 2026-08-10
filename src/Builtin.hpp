#pragma once

#include "Cli.hpp"
#include "Common.hpp"
#include "Maybe.hpp"
#include "Platform.hpp"

namespace shit {

class ExecContext;
class EvalContext;
class Path;

class Builtin
{
public:
  enum class Kind : uint8_t
  {
    Echo,
    Cd,
    Exit,
    Pwd,
    Pushd,
    Popd,
    Dirs,
    Export,
    Break,
    Continue,
    Return,
    True,
    False,
    Test,
    Source,
    Eval,
    Set,
    Shift,
    Unset,
    Read,
    Printf,
    Umask,
    Getopts,
    Trap,
    Exec,
    Type,
    CommandBuiltin,
    BuiltinBuiltin,
    Readonly,
    Local,
    Declare,
    Mapfile,
    Shopt,
    Times,
    Let,
    Ulimit,
    Hash,
    Alias,
    Unalias,
    Jobs,
    Fg,
    Bg,
    Disown,
    Wait,
    Kill,
    Time,
    Bench,
    Assimilate,
    Newgrp,
    Z,
    Complete,
    Compgen,
    Shitbox,
    Compopt,
    History,
    Fc,
    Caller,
    Help,
    Logout,
    Suspend,
    Bind,
    Enable,
  };

  void set_fds(os::descriptor in, os::descriptor out) throws;
  void print_to_stdout(StringView s) const throws;

  pure virtual Kind kind() const wontthrow = 0;
  virtual i32 execute(ExecContext &ec, EvalContext &cxt) const throws = 0;

  virtual ~Builtin() = default;

protected:
  Builtin();
};

inline constexpr static_string_entry<Builtin::Kind> BUILTIN_ENTRIES[] = {
    {SSK("echo"),       Builtin::Kind::Echo          },
    {SSK("exit"),       Builtin::Kind::Exit          },
    {SSK("cd"),         Builtin::Kind::Cd            },
    {SSK("pwd"),        Builtin::Kind::Pwd           },
    {SSK("pushd"),      Builtin::Kind::Pushd         },
    {SSK("popd"),       Builtin::Kind::Popd          },
    {SSK("dirs"),       Builtin::Kind::Dirs          },
    {SSK("export"),     Builtin::Kind::Export        },
    {SSK("break"),      Builtin::Kind::Break         },
    {SSK("continue"),   Builtin::Kind::Continue      },
    {SSK("return"),     Builtin::Kind::Return        },
    {SSK(":"),          Builtin::Kind::True          },
    {SSK("true"),       Builtin::Kind::True          },
    {SSK("false"),      Builtin::Kind::False         },
    {SSK("test"),       Builtin::Kind::Test          },
    {SSK("["),          Builtin::Kind::Test          },
    {SSK("."),          Builtin::Kind::Source        },
    {SSK("source"),     Builtin::Kind::Source        },
    {SSK("eval"),       Builtin::Kind::Eval          },
    {SSK("set"),        Builtin::Kind::Set           },
    {SSK("shift"),      Builtin::Kind::Shift         },
    {SSK("unset"),      Builtin::Kind::Unset         },
    {SSK("read"),       Builtin::Kind::Read          },
    {SSK("printf"),     Builtin::Kind::Printf        },
    {SSK("umask"),      Builtin::Kind::Umask         },
    {SSK("getopts"),    Builtin::Kind::Getopts       },
    {SSK("trap"),       Builtin::Kind::Trap          },
    {SSK("exec"),       Builtin::Kind::Exec          },
    {SSK("type"),       Builtin::Kind::Type          },
    {SSK("command"),    Builtin::Kind::CommandBuiltin},
    {SSK("builtin"),    Builtin::Kind::BuiltinBuiltin},
    {SSK("readonly"),   Builtin::Kind::Readonly      },
    {SSK("local"),      Builtin::Kind::Local         },
    {SSK("declare"),    Builtin::Kind::Declare       },
    {SSK("typeset"),    Builtin::Kind::Declare       },
    {SSK("mapfile"),    Builtin::Kind::Mapfile       },
    {SSK("readarray"),  Builtin::Kind::Mapfile       },
    {SSK("shopt"),      Builtin::Kind::Shopt         },
    {SSK("times"),      Builtin::Kind::Times         },
    {SSK("let"),        Builtin::Kind::Let           },
    {SSK("ulimit"),     Builtin::Kind::Ulimit        },
    {SSK("hash"),       Builtin::Kind::Hash          },
    {SSK("alias"),      Builtin::Kind::Alias         },
    {SSK("unalias"),    Builtin::Kind::Unalias       },
    {SSK("jobs"),       Builtin::Kind::Jobs          },
    {SSK("fg"),         Builtin::Kind::Fg            },
    {SSK("bg"),         Builtin::Kind::Bg            },
    {SSK("disown"),     Builtin::Kind::Disown        },
    {SSK("wait"),       Builtin::Kind::Wait          },
    {SSK("kill"),       Builtin::Kind::Kill          },
    {SSK("time"),       Builtin::Kind::Time          },
    {SSK("bench"),      Builtin::Kind::Bench         },
    {SSK("assimilate"), Builtin::Kind::Assimilate    },
    {SSK("newgrp"),     Builtin::Kind::Newgrp        },
    {SSK("z"),          Builtin::Kind::Z             },
    {SSK("complete"),   Builtin::Kind::Complete      },
    {SSK("compgen"),    Builtin::Kind::Compgen       },
    {SSK("shitbox"),    Builtin::Kind::Shitbox       },
    {SSK("compopt"),    Builtin::Kind::Compopt       },
    {SSK("history"),    Builtin::Kind::History       },
    {SSK("fc"),         Builtin::Kind::Fc            },
    {SSK("caller"),     Builtin::Kind::Caller        },
    {SSK("help"),       Builtin::Kind::Help          },
    {SSK("logout"),     Builtin::Kind::Logout        },
    {SSK("suspend"),    Builtin::Kind::Suspend       },
    {SSK("bind"),       Builtin::Kind::Bind          },
    {SSK("enable"),     Builtin::Kind::Enable        },
};

inline constexpr StaticStringMap BUILTINS{BUILTIN_ENTRIES};

#define B_CASE(btin)                                                           \
  case Builtin::Kind::btin: {                                                  \
    btin builtin;                                                              \
    return builtin.execute(ec, cxt);                                           \
  }

#define BUILTIN_SWITCH_CASES()                                                 \
  B_CASE(Echo);                                                                \
  B_CASE(Cd);                                                                  \
  B_CASE(Exit);                                                                \
  B_CASE(Pwd);                                                                 \
  B_CASE(Pushd);                                                               \
  B_CASE(Popd);                                                                \
  B_CASE(Dirs);                                                                \
  B_CASE(Export);                                                              \
  B_CASE(Break);                                                               \
  B_CASE(Continue);                                                            \
  B_CASE(Return);                                                              \
  B_CASE(True);                                                                \
  B_CASE(False);                                                               \
  B_CASE(Test);                                                                \
  B_CASE(Source);                                                              \
  B_CASE(Eval);                                                                \
  B_CASE(Set);                                                                 \
  B_CASE(Shift);                                                               \
  B_CASE(Unset);                                                               \
  B_CASE(Read);                                                                \
  B_CASE(Printf);                                                              \
  B_CASE(Umask);                                                               \
  B_CASE(Getopts);                                                             \
  B_CASE(Trap);                                                                \
  B_CASE(Exec);                                                                \
  B_CASE(Type);                                                                \
  B_CASE(CommandBuiltin);                                                      \
  B_CASE(BuiltinBuiltin);                                                      \
  B_CASE(Readonly);                                                            \
  B_CASE(Local);                                                               \
  B_CASE(Declare);                                                             \
  B_CASE(Mapfile);                                                             \
  B_CASE(Shopt);                                                               \
  B_CASE(Times);                                                               \
  B_CASE(Let);                                                                 \
  B_CASE(Ulimit);                                                              \
  B_CASE(Hash);                                                                \
  B_CASE(Alias);                                                               \
  B_CASE(Unalias);                                                             \
  B_CASE(Jobs);                                                                \
  B_CASE(Fg);                                                                  \
  B_CASE(Bg);                                                                  \
  B_CASE(Disown);                                                              \
  B_CASE(Wait);                                                                \
  B_CASE(Kill);                                                                \
  B_CASE(Time);                                                                \
  B_CASE(Bench);                                                               \
  B_CASE(Assimilate);                                                          \
  B_CASE(Newgrp);                                                              \
  B_CASE(Z);                                                                   \
  B_CASE(Complete);                                                            \
  B_CASE(Compgen);                                                             \
  B_CASE(Shitbox);                                                             \
  B_CASE(Compopt);                                                             \
  B_CASE(History);                                                             \
  B_CASE(Fc);                                                                  \
  B_CASE(Caller);                                                              \
  B_CASE(Help);                                                                \
  B_CASE(Logout);                                                              \
  B_CASE(Suspend);                                                             \
  B_CASE(Bind);                                                                \
  B_CASE(Enable)

#define BUILTIN_STRUCT(b)                                                      \
  class b : public Builtin                                                     \
  {                                                                            \
  public:                                                                      \
    b();                                                                       \
                                                                               \
    pure Kind kind() const wontthrow override;                                 \
    i32 execute(ExecContext &ec, EvalContext &cxt) const throws override;      \
  };

BUILTIN_STRUCT(Echo);
BUILTIN_STRUCT(Cd);
BUILTIN_STRUCT(Pwd);
BUILTIN_STRUCT(Pushd);
BUILTIN_STRUCT(Popd);
BUILTIN_STRUCT(Dirs);
BUILTIN_STRUCT(Export);
BUILTIN_STRUCT(Break);
BUILTIN_STRUCT(Continue);
BUILTIN_STRUCT(Return);
BUILTIN_STRUCT(True);
BUILTIN_STRUCT(False);
BUILTIN_STRUCT(Test);
BUILTIN_STRUCT(Source);
BUILTIN_STRUCT(Eval);
BUILTIN_STRUCT(Set);
BUILTIN_STRUCT(Shift);
BUILTIN_STRUCT(Unset);
BUILTIN_STRUCT(Read);
BUILTIN_STRUCT(Printf);
BUILTIN_STRUCT(Umask);
BUILTIN_STRUCT(Getopts);
BUILTIN_STRUCT(Trap);
BUILTIN_STRUCT(Exec);
BUILTIN_STRUCT(Type);
BUILTIN_STRUCT(CommandBuiltin);
BUILTIN_STRUCT(BuiltinBuiltin);
BUILTIN_STRUCT(Readonly);
BUILTIN_STRUCT(Local);
BUILTIN_STRUCT(Declare);
BUILTIN_STRUCT(Mapfile);
BUILTIN_STRUCT(Shopt);
BUILTIN_STRUCT(Times);
BUILTIN_STRUCT(Let);
BUILTIN_STRUCT(Ulimit);
BUILTIN_STRUCT(Hash);
BUILTIN_STRUCT(Alias);
BUILTIN_STRUCT(Unalias);
BUILTIN_STRUCT(Jobs);
BUILTIN_STRUCT(Fg);
BUILTIN_STRUCT(Bg);
BUILTIN_STRUCT(Disown);
BUILTIN_STRUCT(Wait);
BUILTIN_STRUCT(Kill);
BUILTIN_STRUCT(Time);
BUILTIN_STRUCT(Bench);
BUILTIN_STRUCT(Assimilate);
BUILTIN_STRUCT(Newgrp);
BUILTIN_STRUCT(Complete);
BUILTIN_STRUCT(Compgen);
BUILTIN_STRUCT(Compopt);
BUILTIN_STRUCT(Z);
BUILTIN_STRUCT(Shitbox);
BUILTIN_STRUCT(History);
BUILTIN_STRUCT(Fc);
BUILTIN_STRUCT(Caller);
BUILTIN_STRUCT(Help);
BUILTIN_STRUCT(Logout);
BUILTIN_STRUCT(Suspend);
BUILTIN_STRUCT(Bind);
BUILTIN_STRUCT(Enable);

class Exit : public Builtin
{
public:
  Exit();

  pure Kind kind() const wontthrow override;
  i32 execute(ExecContext &ec, EvalContext &cxt) const throws override;
};

Maybe<Builtin::Kind> search_builtin(StringView builtin_name) throws;

/* True when the name is one of the POSIX special builtins, the set whose prefix
   assignments persist after the command and whose errors abort a
   non-interactive shell. The test is by name rather than by kind, since : is
   special while true is not. */
fn is_special_builtin_name(StringView name) wontthrow -> bool;

const ArrayList<String> &builtin_names() throws;

inline constexpr usize BUILTIN_KIND_COUNT =
    static_cast<usize>(Builtin::Kind::Enable) + 1;

/* The FLAG_LIST of a builtin, registered at static-init time by the
   REGISTER_BUILTIN_FLAGS line in its file. A kind with no registration reads
   back null. */
fn register_builtin_flag_list(Builtin::Kind kind,
                              const FlagList *flags) wontthrow -> void;
fn builtin_flag_list(Builtin::Kind kind) wontthrow -> const FlagList *;

#define REGISTER_BUILTIN_FLAGS(kind)                                           \
  static uchar t__builtin_flag_registrar =                                     \
      (shit::register_builtin_flag_list(shit::Builtin::Kind::kind,             \
                                        &FLAG_LIST),                           \
       0)

void show_builtin_help_impl(const ExecContext &ec, StringView description,
                            const SynopsisList &synopsis_lines,
                            const FlagList &flags,
                            StringView extra_sections = {}) throws;
fn builtin_error_context(StringView program) throws -> String;
fn builtin_error_message(StringView program, StringView message) throws
    -> String;

#define SHOW_BUILTIN_HELP_AND_RETURN(ec)                                       \
  do {                                                                         \
    show_builtin_help_impl(ec, HELP_DESCRIPTION, HELP_SYNOPSIS, FLAG_LIST);    \
    return 0;                                                                  \
  } while (false)

#define SHOW_BUILTIN_HELP_EXTRA_AND_RETURN(ec, extra)                          \
  do {                                                                         \
    show_builtin_help_impl(ec, HELP_DESCRIPTION, HELP_SYNOPSIS, FLAG_LIST,     \
                           (extra));                                           \
    return 0;                                                                  \
  } while (false)

#define PARSE_BUILTIN_ARGS(ec)                                                 \
  parse_flags_vec(FLAG_LIST, ec.args(), ec.source_location().position,         \
                  nullptr, &ec.arg_locations(), nullptr,                       \
                  builtin_error_context(ec.program()));                        \
  defer { reset_flags(FLAG_LIST); }

/* The same parse, but it also fills operand_locations with the source span of
   each surviving operand, so a builtin that iterates its parsed operands can
   caret the specific one. The caller declares the list and passes it by name.
 */
#define PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations)               \
  parse_flags_vec(FLAG_LIST, ec.args(), ec.source_location().position,         \
                  nullptr, &ec.arg_locations(), &(operand_locations),          \
                  builtin_error_context(ec.program()));                        \
  defer { reset_flags(FLAG_LIST); }

i32 execute_builtin(ExecContext &&ec, EvalContext &cxt) throws;

/* The state of a set -o option by name, or None when the name is not a known
   shell option, so shopt -o can bridge to the same options set -o drives.
   apply_shell_option sets one and reports whether the name was known. */
fn query_shell_option(const EvalContext &cxt, StringView name) throws
    -> Maybe<bool>;
fn apply_shell_option(EvalContext &cxt, StringView name, bool enable) throws
    -> bool;

fn shell_option_names(bool include_alias_spellings) throws
    -> const ArrayList<StringView> &;
fn shell_option_letters() throws -> const String &;
fn enabled_shell_option_names(const EvalContext &cxt) throws -> String;
fn enabled_shell_option_letters(const EvalContext &cxt) throws -> String;

fn shopt_option_name_list() throws -> const ArrayList<StringView> &;

fn shit_binary_flag_list() wontthrow -> const FlagList &;

/* Report a builtin error that must not abort the run, with the same located
   caret in the default and posix moods and the same soft unlocated line in the
   bash mood. A builtin that throws gets a fatal located error instead. */
fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message) throws -> void;

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message, StringView note) throws
    -> void;

/* The span-aware forms caret the specific argument whose SourceLocation is
   passed, rather than the whole command. A builtin that has identified the bad
   argument passes its span here. */
fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             SourceLocation location, StringView message) throws
    -> void;

fn report_soft_builtin_error(const ExecContext &ec, EvalContext &cxt,
                             SourceLocation location, StringView message,
                             StringView note) throws -> void;

fn report_usage_error(const ExecContext &ec, EvalContext &cxt,
                      StringView program_name) throws -> i32;

fn report_usage_error(EvalContext &cxt, SourceLocation location,
                      StringView program_name) throws -> i32;

/* Build a located error pointing at the argument at index, so a builtin can
   throw with a precise caret and let execute_builtin render it. The note
   variant produces ErrorWithLocationAndDetails. */
fn make_error_for_arg(const ExecContext &ec, usize index,
                      StringView message) throws -> ErrorWithLocation;

fn make_error_for_arg(const ExecContext &ec, usize index, StringView message,
                      StringView note) throws -> ErrorWithLocationAndDetails;

/* A name that opens with a letter or underscore and carries only letters,
   digits, and underscores, the shell's rule for an export or readonly target.
 */
pure fn name_is_valid_identifier(StringView name) wontthrow -> bool;

/* pushd, popd, and dirs share these. The cd runs through the cd builtin so the
   logical PWD, OLDPWD, and the -L rules stay in one place. The stack print
   shows the current directory first, then the saved stack from the top down,
   with the home directory abbreviated to ~ unless no_tilde is set. */
fn run_cd_to_directory(EvalContext &cxt, const ExecContext &ec,
                       StringView target) throws -> i32;
fn logical_working_directory(EvalContext &cxt) throws -> Path;
fn print_directory_stack(EvalContext &cxt, const ExecContext &ec,
                         bool one_per_line, bool numbered, bool no_tilde) throws
    -> void;

/* A +N or -N stack argument names an index into the ring of ring_count entries,
   the current directory at zero then the saved stack from the top. N counts
   from the top for +N and from the bottom for -N. False means the argument is
   not a rotation, and an out-of-range N throws. */
fn parse_directory_stack_rotation(StringView arg, usize ring_count,
                                  const ExecContext &ec,
                                  usize &index_out) throws -> bool;

/* The value a declare -x, declare -r, or declare -p line wraps in double
   quotes, with the characters special inside double quotes escaped, so the
   printed line reloads to the same value the way bash quotes it. */
fn quote_for_declare(StringView value) throws -> String;

/* The optional first integer argument of a builtin such as exit, return, break,
   continue, and shift, or default_value when no argument is given. A malformed
   argument propagates its parse error to the caller. */
fn parse_optional_integer_arg(const ExecContext &ec, i64 default_value) throws
    -> i64;

} // namespace shit
