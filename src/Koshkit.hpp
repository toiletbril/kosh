#pragma once

#include "Builtin.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Maybe.hpp"
#include "PackedStringKey.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

class ExecContext;
class EvalContext;

namespace koshkit {

class Utility
{
public:
  enum class Kind : uint8_t
  {
    Ls,
    Ln,
    Rm,
    Mkdir,
    Rmdir,
    Cp,
    Mv,
    Cat,
    Tee,
    Touch,
    Basename,
    Dirname,
    Realpath,
    Du,
    Head,
    Tail,
    Wc,
    Seq,
    Tr,
    Grep,
    Sort,
    Uniq,
    Sleep,
    Timeout,
    Env,
    Yes,
    Pkill,
    Killall,
    Ps,
    Make,
    Find,
    Which,
    WhoAmI,
    Unlink,
    Nproc,
    Flock,
    Calc,
  };

  pure virtual Kind kind() const wontthrow = 0;
  virtual i32
  execute(const ExecContext &ec, EvalContext &cxt,
          const ArrayList<String> &args,
          const ArrayList<SourceLocation> &arg_locations) const throws = 0;

  virtual ~Utility() = default;

protected:
  Utility();
};

inline constexpr static_string_entry<Utility::Kind> KOSHKIT_ENTRIES[] = {
    {SSK("ls"),       Utility::Kind::Ls      },
    {SSK("ln"),       Utility::Kind::Ln      },
    {SSK("rm"),       Utility::Kind::Rm      },
    {SSK("mkdir"),    Utility::Kind::Mkdir   },
    {SSK("rmdir"),    Utility::Kind::Rmdir   },
    {SSK("cp"),       Utility::Kind::Cp      },
    {SSK("mv"),       Utility::Kind::Mv      },
    {SSK("cat"),      Utility::Kind::Cat     },
    {SSK("tee"),      Utility::Kind::Tee     },
    {SSK("touch"),    Utility::Kind::Touch   },
    {SSK("basename"), Utility::Kind::Basename},
    {SSK("dirname"),  Utility::Kind::Dirname },
    {SSK("realpath"), Utility::Kind::Realpath},
    {SSK("du"),       Utility::Kind::Du      },
    {SSK("head"),     Utility::Kind::Head    },
    {SSK("tail"),     Utility::Kind::Tail    },
    {SSK("wc"),       Utility::Kind::Wc      },
    {SSK("seq"),      Utility::Kind::Seq     },
    {SSK("tr"),       Utility::Kind::Tr      },
    {SSK("grep"),     Utility::Kind::Grep    },
    {SSK("sort"),     Utility::Kind::Sort    },
    {SSK("uniq"),     Utility::Kind::Uniq    },
    {SSK("sleep"),    Utility::Kind::Sleep   },
    {SSK("timeout"),  Utility::Kind::Timeout },
    {SSK("env"),      Utility::Kind::Env     },
    {SSK("yes"),      Utility::Kind::Yes     },
    {SSK("pkill"),    Utility::Kind::Pkill   },
    {SSK("killall"),  Utility::Kind::Killall },
    {SSK("ps"),       Utility::Kind::Ps      },
    {SSK("make"),     Utility::Kind::Make    },
    {SSK("find"),     Utility::Kind::Find    },
    {SSK("which"),    Utility::Kind::Which   },
    {SSK("whoami"),   Utility::Kind::WhoAmI  },
    {SSK("unlink"),   Utility::Kind::Unlink  },
    {SSK("nproc"),    Utility::Kind::Nproc   },
    {SSK("flock"),    Utility::Kind::Flock   },
    {SSK("calc"),     Utility::Kind::Calc    },
};

inline constexpr StaticStringMap KOSHKIT_UTILS{KOSHKIT_ENTRIES};

inline constexpr usize KOSHKIT_UTIL_COUNT =
    static_cast<usize>(Utility::Kind::Calc) + 1;

/* A utility with no registration reads back null. */
fn register_koshkit_util_flags(Utility::Kind chosen,
                               const FlagList *flags) wontthrow -> void;
fn koshkit_util_flag_list(Utility::Kind chosen) wontthrow -> const FlagList *;

#define REGISTER_KOSHKIT_UTIL_FLAGS(util)                                      \
  static uchar t__koshkit_flag_registrar =                                     \
      (koshka::koshkit::register_koshkit_util_flags(                           \
           koshka::koshkit::Utility::Kind::util, &FLAG_LIST),                  \
       0)

fn find_util(StringView name) throws -> Maybe<Utility::Kind>;

fn util_names() throws -> const ArrayList<String> &;

fn collect_makefile_targets(EvalContext &cxt, const Path &makefile) throws
    -> ArrayList<String>;

/* The koshkit builtin passes 1 for `koshkit ls` and 0 for a bare-name
   invocation. */
fn dispatch(const ExecContext &ec, EvalContext &cxt, usize name_index,
            Maybe<Utility::Kind> chosen = {}) throws -> i32;

fn run_as_multicall(StringView util_name, ArrayList<String> operands,
                    EvalContext &cxt) throws -> i32;

fn run_util(Utility::Kind chosen, const ExecContext &ec, EvalContext &cxt,
            const ArrayList<String> &args,
            const ArrayList<SourceLocation> &arg_locations) throws -> i32;

fn preflight_timeout_stage(const ExecContext &ec, EvalContext &cxt,
                           usize name_index, SourceLocation &error_location,
                           String &error_message) throws -> Maybe<i32>;

fn parse_util_operands(const FlagList &flags, const ArrayList<String> &args,
                       const ArrayList<SourceLocation> *arg_locations = nullptr,
                       ArrayList<SourceLocation> *operand_locations = nullptr,
                       bool should_accept_negative_number_operand =
                           false) throws -> ArrayList<String>;

fn print_util_help(const ExecContext &ec, StringView name, StringView synopsis,
                   StringView description, const FlagList &flags) throws
    -> void;

/* Reads FLAG_HELP, HELP_SYNOPSIS, HELP_DESCRIPTION, and FLAG_LIST from the
   caller's scope. */
#define KOSHKIT_SHOW_HELP_AND_RETURN(ec, args)                                 \
  do {                                                                         \
    if (FLAG_HELP.is_enabled()) {                                              \
      koshka::koshkit::print_util_help((ec), (args)[0].view(),                 \
                                       HELP_SYNOPSIS[0], HELP_DESCRIPTION,     \
                                       FLAG_LIST);                             \
      return 0;                                                                \
    }                                                                          \
  } while (false)

#define U_CASE(util)                                                           \
  case Utility::Kind::util: {                                                  \
    util utility;                                                              \
    return utility.execute(ec, cxt, args, arg_locations);                      \
  }

#define UTILITY_SWITCH_CASES()                                                 \
  U_CASE(Ls);                                                                  \
  U_CASE(Ln);                                                                  \
  U_CASE(Rm);                                                                  \
  U_CASE(Mkdir);                                                               \
  U_CASE(Rmdir);                                                               \
  U_CASE(Cp);                                                                  \
  U_CASE(Mv);                                                                  \
  U_CASE(Cat);                                                                 \
  U_CASE(Tee);                                                                 \
  U_CASE(Touch);                                                               \
  U_CASE(Basename);                                                            \
  U_CASE(Dirname);                                                             \
  U_CASE(Realpath);                                                            \
  U_CASE(Du);                                                                  \
  U_CASE(Head);                                                                \
  U_CASE(Tail);                                                                \
  U_CASE(Wc);                                                                  \
  U_CASE(Seq);                                                                 \
  U_CASE(Tr);                                                                  \
  U_CASE(Grep);                                                                \
  U_CASE(Sort);                                                                \
  U_CASE(Uniq);                                                                \
  U_CASE(Sleep);                                                               \
  U_CASE(Timeout);                                                             \
  U_CASE(Env);                                                                 \
  U_CASE(Yes);                                                                 \
  U_CASE(Pkill);                                                               \
  U_CASE(Killall);                                                             \
  U_CASE(Ps);                                                                  \
  U_CASE(Make);                                                                \
  U_CASE(Find);                                                                \
  U_CASE(Which);                                                               \
  U_CASE(WhoAmI);                                                              \
  U_CASE(Unlink);                                                              \
  U_CASE(Nproc);                                                               \
  U_CASE(Flock);                                                               \
  U_CASE(Calc)

#define UTILITY_STRUCT(u)                                                      \
  class u : public Utility                                                     \
  {                                                                            \
  public:                                                                      \
    u();                                                                       \
                                                                               \
    pure Kind kind() const wontthrow override;                                 \
    i32 execute(                                                               \
        const ExecContext &ec, EvalContext &cxt,                               \
        const ArrayList<String> &args,                                         \
        const ArrayList<SourceLocation> &arg_locations) const throws override; \
  };

UTILITY_STRUCT(Ls);
UTILITY_STRUCT(Ln);
UTILITY_STRUCT(Rm);
UTILITY_STRUCT(Mkdir);
UTILITY_STRUCT(Rmdir);
UTILITY_STRUCT(Cp);
UTILITY_STRUCT(Mv);
UTILITY_STRUCT(Cat);
UTILITY_STRUCT(Tee);
UTILITY_STRUCT(Touch);
UTILITY_STRUCT(Basename);
UTILITY_STRUCT(Dirname);
UTILITY_STRUCT(Realpath);
UTILITY_STRUCT(Du);
UTILITY_STRUCT(Head);
UTILITY_STRUCT(Tail);
UTILITY_STRUCT(Wc);
UTILITY_STRUCT(Seq);
UTILITY_STRUCT(Tr);
UTILITY_STRUCT(Grep);
UTILITY_STRUCT(Sort);
UTILITY_STRUCT(Uniq);
UTILITY_STRUCT(Sleep);
UTILITY_STRUCT(Timeout);
UTILITY_STRUCT(Env);
UTILITY_STRUCT(Yes);
UTILITY_STRUCT(Pkill);
UTILITY_STRUCT(Killall);
UTILITY_STRUCT(Ps);
UTILITY_STRUCT(Make);
UTILITY_STRUCT(Find);
UTILITY_STRUCT(Which);
UTILITY_STRUCT(WhoAmI);
UTILITY_STRUCT(Unlink);
UTILITY_STRUCT(Nproc);
UTILITY_STRUCT(Flock);
UTILITY_STRUCT(Calc);

fn read_fd_to_string(os::descriptor fd) throws -> Maybe<String>;

/* Returns false on the first failure with the reason in
   os::last_system_error_message. */
enum class removal_mode : u8
{
  SinglePath,
  Recursive,
};

fn remove_path(StringView path, removal_mode mode) throws -> bool;
fn read_named_or_stdin(const ExecContext &ec, StringView path) throws
    -> Maybe<String>;

struct input_descriptor
{
  os::descriptor descriptor;
  bool should_close;
};

fn open_named_or_stdin(const ExecContext &ec, StringView path) wontthrow
    -> Maybe<input_descriptor>;

fn split_keep_newlines(StringView text) throws -> ArrayList<StringView>;

/* The operand list becomes a source list, a single "-" stdin source when no
   operand is given, otherwise each operand as a view. */
fn source_list_from_operands(const ArrayList<String> &operands,
                             Allocator allocator,
                             usize first_operand_index = 0) throws
    -> ArrayList<StringView>;

fn sort_string_list(ArrayList<String> &items) wontthrow -> void;
fn sort_stringview_list(ArrayList<StringView> &items) wontthrow -> void;

fn format_human_size(u64 bytes, Allocator allocator) throws -> String;

fn parse_koshkit_duration_seconds(StringView text, StringView utility_name,
                                  Allocator allocator) throws -> f64;

fn resolve_koshkit_signal(StringView spelled, Allocator allocator) throws
    -> i32;

fn format_signal_list() throws -> String;

/* Report a utility error that must not abort the run, with a located caret in
   the default and posix moods and a soft line in the bash mood. A fatal error
   throws an Error instead. */
fn report_soft_koshkit_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message) throws -> void;

fn report_soft_koshkit_error(const ExecContext &ec, EvalContext &cxt,
                             StringView message, StringView note) throws
    -> void;

} /* namespace koshkit */

} /* namespace koshka */
