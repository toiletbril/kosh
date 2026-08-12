#include "Diagnostics.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"

namespace koshka {

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

} /* namespace koshka */
