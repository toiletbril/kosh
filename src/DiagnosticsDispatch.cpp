/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file maps command names to compact command identifiers and diagnostic
 * behavior groups. Its static table lets checks dispatch by identifiers and
 * flags without repeating command-name comparisons.
 */

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
constexpr u32 VALUE_NAME_READER_GROUPS =
    COMMAND_GROUP_NAME_AS_VALUE | COMMAND_GROUP_NON_STDIN_READER;
constexpr u32 VALUE_NAME_NEUTRAL_GROUPS =
    COMMAND_GROUP_NAME_AS_VALUE | COMMAND_GROUP_ENVIRONMENT_NEUTRAL;

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
        C("awk", Awk, COMMAND_GROUP_NAME_AS_VALUE),
        C("basename", Basename, NEUTRAL_READER_GROUPS),
        C("break", Break, NO_COMMAND_GROUP),
        C("builtin", Builtin, NO_COMMAND_GROUP),
        C("cat", Cat, COMMAND_GROUP_NAME_AS_VALUE),
        C("cd", Cd, NO_COMMAND_GROUP),
        C("chmod", Chmod, VALUE_NAME_READER_GROUPS),
        C("chown", Chown, VALUE_NAME_READER_GROUPS),
        C("command", Command, NO_COMMAND_GROUP),
        C("continue", Continue, NO_COMMAND_GROUP),
        C("cp", Cp, VALUE_NAME_READER_GROUPS),
        C("curl", Curl, COMMAND_GROUP_NAME_AS_VALUE),
        C("date", Date, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("declare", Declare, DECLARATION_GROUPS),
        C("dirname", Dirname, NEUTRAL_READER_GROUPS),
        C("docker", Docker, COMMAND_GROUP_NAME_AS_VALUE),
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
        C("git", Git, COMMAND_GROUP_NAME_AS_VALUE),
        C("grep", Grep,
          COMMAND_GROUP_PATTERN_MATCHER | COMMAND_GROUP_NAME_AS_VALUE),
        C("gt", Unknown, COMMAND_GROUP_HTML_ENTITY_TAIL),
        C("hostname", Hostname, VALUE_NAME_NEUTRAL_GROUPS),
        C("id", Id, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("kill", Kill, COMMAND_GROUP_NON_STDIN_READER),
        C("let", Let, COMMAND_GROUP_VARIABLE_PROBE),
        C("ln", Ln, VALUE_NAME_READER_GROUPS),
        C("local", Local, DECLARATION_GROUPS),
        C("ls", Ls, VALUE_NAME_READER_GROUPS),
        C("lt", Unknown, COMMAND_GROUP_HTML_ENTITY_TAIL),
        C("mapfile", Mapfile, COMMAND_GROUP_VARIABLE_TARGET),
        C("mkdir", Mkdir, VALUE_NAME_READER_GROUPS),
        C("mv", Mv, VALUE_NAME_READER_GROUPS),
        C("printf", Printf, VALUE_NAME_READER_GROUPS),
        C("ps", Ps, COMMAND_GROUP_NON_STDIN_READER),
        C("pwd", Pwd, VALUE_NAME_NEUTRAL_GROUPS),
        C("read", Read, COMMAND_GROUP_VARIABLE_TARGET),
        C("readarray", Readarray, COMMAND_GROUP_VARIABLE_TARGET),
        C("readonly", Readonly, EXPORT_GROUPS),
        C("return", Return, NO_COMMAND_GROUP),
        C("rm", Rm, VALUE_NAME_READER_GROUPS),
        C("rmdir", Rmdir, VALUE_NAME_READER_GROUPS),
        C("sed", Sed, COMMAND_GROUP_NAME_AS_VALUE),
        C("seq", Seq, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("set", Set, NO_COMMAND_GROUP),
        C("sleep", Sleep, COMMAND_GROUP_NON_STDIN_READER),
        C("source", Source, COMMAND_GROUP_RUNTIME_DEFINER),
        C("ssh", Ssh, COMMAND_GROUP_NAME_AS_VALUE),
        C("su", Su, NO_COMMAND_GROUP),
        C("sudo", Sudo, COMMAND_GROUP_NAME_AS_VALUE),
        C("test", Test, BRACKET_GROUPS | COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("touch", Touch, VALUE_NAME_READER_GROUPS),
        C("tr", Tr, COMMAND_GROUP_NAME_AS_VALUE),
        C("trap", Trap, NO_COMMAND_GROUP),
        C("true", True, NEUTRAL_READER_GROUPS),
        C("tty", Tty, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("typeset", Typeset, DECLARATION_GROUPS),
        C("uname", Uname, VALUE_NAME_NEUTRAL_GROUPS),
        C("unlink", Unlink, COMMAND_GROUP_NON_STDIN_READER),
        C("unset", Unset, COMMAND_GROUP_VARIABLE_PROBE),
        C("wc", Wc, NO_COMMAND_GROUP),
        C("which", Which, COMMAND_GROUP_ENVIRONMENT_NEUTRAL),
        C("whoami", Whoami, VALUE_NAME_NEUTRAL_GROUPS),
        C("xargs", Xargs, COMMAND_GROUP_NAME_AS_VALUE),
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
