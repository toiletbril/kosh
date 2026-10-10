/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell process startup. It parses invocation options,
 * selects startup files and scripts, and owns noninteractive execution and
 * the interactive loop.
 */

#include "CLI.hpp"
#include "CLIColors.hpp"
#include "Completion.hpp"
#include "Diagnostics.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "EvalVariablesInternal.hpp"
#include "Expressions.hpp"
#include "Formatter.hpp"
#include "Koshconf.hpp"
#include "Koshkit.hpp"
#include "LanguageServer.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/PackedStringKey.hpp"
#include "base/Path.hpp"
#include "base/StaticStringMap.hpp"
#include "base/Trace.hpp"

FLAG_LIST_DECL();

/* clang-format off */
HELP_SYNOPSIS_DECL("[-OPTIONS] [--] <file> [argument ...]",
                   "[-OPTIONS] -c <script1> [-c <script2> ...] [argument ...]",
                   "[-OPTIONS] (--lint [--format] | --format) [--apply] [file ...]",
                   "[-OPTIONS] --as-language-server");
/* clang-format on */

FLAG(VERSION, Bool, '\0', "version", "Display program version and notices.");
FLAG(SHORT_VERSION, Bool, 'V', "short-version",
     "Display version in a short form.");
FLAG(HELP, Bool, '\0', "help", "Display help message.");

FLAG(INTERACTIVE, Bool, 'i', "interactive", Posix,
     "Specify that the shell is interactive.");
FLAG(STDIN, Bool, 's', "stdin", Posix, "Execute command from stdin and exit.");
FLAG(COMMAND, ManyStrings, 'c', "command", Posix,
     "Execute specified command and exit. Can be used multiple times.");
FLAG(ERROR_EXIT, Bool, 'e', "error-exit", Posix, "Die on first error.");
FLAG(DISABLE_EXPANSION, Bool, 'f', "no-glob", Posix, "Disable path expansion.");
FLAG(ONE_COMMAND, Bool, 't', "one-command", Posix,
     "Exit after executing one command.");
FLAG(VERBOSE, Bool, 'v', "verbose", Posix,
     "Write input to standard error as it is read.");
FLAG(EXPAND_VERBOSE, Bool, 'x', "xtrace", Posix,
     "Write expanded input to standard error as it is read.");
FLAG(EXPORT_ALL, Bool, 'a', "export-all", Posix,
     "Mark every assigned variable for the environment.");
FLAG(NO_CLOBBER, Bool, 'C', "no-clobber", Posix,
     "Refuse to overwrite an existing file through '>'.");
FLAG(NO_EXEC, Bool, 'n', "no-exec", Posix,
     "Parse and analyze the script but do not run it.");
FLAG(NOUNSET, Bool, 'u', "no-unset", Posix,
     "Treat an unset variable as an error.");
FLAG(LOGIN, Bool, 'l', "login", Posix,
     "Act as a login shell and source the profiles.");
FLAG(IGNORED1, Bool, 'h', "\0", Posix, "Ignored, left for compatibility.");
FLAG(IGNORED2, Bool, 'm', "\0", Posix, "Ignored, left for compatibility.");
FLAG(SET_OPTION, ManyStrings, 'o', "\0", Posix,
     "Turn on the set option NAME, or turn it off as +o NAME. A letter after "
     "a plus, such as +e, turns its option off.");

FLAG(RCFILE, String, '\0', "rcfile", Bash,
     "Source FILE as the interactive rc in place of the mood default.");
FLAG(INIT_FILE, String, '\0', "init-file", Bash,
     "Alias for --rcfile, with the last occurrence taking precedence.");
FLAG(NORC, Bool, '\0', "norc", Bash,
     "Do not source the interactive bash rc or a custom rc file.");
FLAG(RESTRICTED, Bool, 'r', "restricted", Bash,
     "Start a restricted shell after the startup files finish.");
FLAG(PRIVILEGED, Bool, 'p', "privileged", Bash,
     "Run privileged, suppressing BASH_ENV. Unequal ids skip startup files.");
FLAG(SHOPT_OPTION, ManyStrings, 'O', "\0", Bash,
     "Turn on the shopt option NAME, or turn it off as +O NAME.");
FLAG(CLEAN, Bool, 'Q', "no-init-files", Kosh,
     "Start clean, reading no startup file and setting a minimal PATH.");
FLAG(NO_CONFIG, Bool, '\0', "no-config", Kosh,
     "Read no kosh.conf file and ignore KOSHCONF, keeping PATH and the shell "
     "startup files.");
FLAG(POSIX_COMPAT, Bool, '\0', "posix", Bash,
     "Run in bash POSIX mode, equivalent to --mood bash-posix.");

FLAG(MOOD, String, 'M', "mood", Compat,
     "Select the runtime mood, 'kosh' is strict with the analysis stage on, "
     "'bash' runs the extensions with it off, 'sh' behaves like dash, and "
     "'bash-posix' is bash with the posix identity reached by --posix.");
FLAG(INIT_MOODS, ManyStrings, 'L', "init-moods", Compat,
     "Source the startup files for each listed mood, in order, comma separated "
     "or by repeating the flag. Defaults to init_moods in an "
     "interactive or login shell, then to --mood.");
FLAG(MIMICRY, Bool, 'I', "enable-mimicry", Compat,
     "Mimic the shell specified by a script's shebang, running a known shell "
     "shebang "
     "in-process in the matching mode.");
FLAG(DUMB, Bool, '\0', "dumb", Compat,
     "Make the shell extremely dumb. Equivalent to --mood sh --no-completion "
     "--no-diagnostics --tab-selector plain.");

FLAG(LINT, Bool, '\0', "lint", Auxiliary,
     "Analyze shell inputs without running them and enable every diagnostic "
     "tier at its normal severity.");
FLAG(FORMAT, Bool, '\0', "format", Auxiliary,
     "Format shell input without running it. Read one file or standard input.");
FLAG(APPLY, Bool, '\0', "apply", Auxiliary,
     "Apply lint fixes or formatted output to named files.");
FLAG(LANGUAGE_SERVER, Bool, '\0', "as-language-server", Auxiliary,
     "Run the shell language server over standard input and standard output.");
FLAG(WARNINGS, RepeatedBool, 'W', "", Kosh,
     "In the default mood, demote annoying, lenient, then strict diagnostics "
     "as W is repeated. In other moods, enable those tiers in reverse order.");
FLAG(LIST_CHECKS, Bool, '\0', "list-diagnostics", Kosh,
     "List the shellcheck-style checks the analysis stage reports, then exit.");
FLAG(SUPPRESS_DIAGNOSTICS, Bool, '\0', "no-diagnostics", Kosh,
     "Skip the analysis stage. No warnings or pre-run diagnostics are "
     "reported.");
FLAG(SUPPRESS_ANNOYING_DIAGNOSTICS, Bool, '\0', "no-annoying-diagnostics", Kosh,
     "Suppress the annoying diagnostic tier while retaining strict and "
     "lenient analysis.");
FLAG(SUPPRESS_INIT_DIAGNOSTICS, Bool, '\0', "no-init-diagnostics", Kosh,
     "Suppress diagnostics only while the startup files source, then restore "
     "them for the prompt.");
FLAG(NO_TRACES, Bool, '\0', "no-traces", Kosh,
     "Suppress source backtraces for errors and warnings.");
FLAG(NO_COMPLETION, Bool, 'T', "no-completion", Kosh,
     "Disable interactive tab completion, ghost-text, and syntax coloring.");
FLAG(NO_SYNTAX_HIGHLIGHTING, Bool, '\0', "no-syntax-highlighting", Kosh,
     "Disable the syntax coloring and the ghost suggestion, leaving tab "
     "completion working.");
FLAG(TAB_SELECTOR, String, '\0', "tab-selector", Kosh,
     "Select how several completion candidates are presented, 'interactive' "
     "draws the shell's own menu, 'external' launches the configured selector "
     "program, and 'plain' lists the candidates. --dumb selects 'plain'.");
FLAG(ENABLE_KOSHKIT, Bool, '\0', "enable-koshkit", Kosh,
     "Resolve the bundled koshkit utility names such as ls and mkdir directly "
     "as commands, the same as koshconf set "
     "interpreter.resolve_koshkit_applets_as_commands on.");
FLAG(EXTENDED_ARITHMETIC, Bool, '\0', "enable-extended-arithmetic", Kosh,
     "Use arbitrary-precision integers and finite decimal values, the same as "
     "koshconf set interpreter.arithmetic_uses_big_numbers on.");

FLAG(AST, Bool, 'A', "show-ast", Debug,
     "Print syntax trees before execution and during formatting or linting.");
FLAG(OPTIMIZER_DIAGNOSTICS, Bool, '\0', "show-optimizer-diagnostics", Debug,
     "Trace the optimizer prepass and report every folded and eliminated node "
     "as an analysis diagnostic.");
FLAG(EXIT_CODE, Bool, '\0', "show-exit-code", Debug,
     "Show diagnostics for every non-zero exit code.");
FLAG(ALL_EXIT_CODES, Bool, 'N', "show-all-exit-codes", Debug,
     "Show diagnostics for every exit code, including zero.");
FLAG(ESCAPE_MAP, Bool, 'R', "show-lexed-words", Debug,
     "Print escape bitmap after each parsed command.");
FLAG(
    STATS, Bool, '\0', "show-stats", Debug,
    "Print run statistics after each command, commands, expansions, nodes, and "
    "arena bytes.");
FLAG(MEMORY, Bool, '\0', "show-memory", Debug,
     "Print a memory report at exit, the arena bytes and the heap in use.");
#if !defined NDEBUG
FLAG(LOG, String, 'X', "debug-logging", Debug,
     "Enable internal logging at the given level, one of 'info', 'debug', or "
     "'all'. An unknown spelling is an error.");
FLAG(
    DEBUG_OUTPUT_FILE, String, '\0', "debug-logging-file", Debug,
    "Append the debug log to the named file, created when missing. The default "
    "is stderr.");
FLAG(DEBUG_COMPLETE_AT, String, '\0', "debug-complete-at", Debug,
     "Print the completion candidates for the given line, then exit. The "
     "completion test driver.");
FLAG(DEBUG_HIGHLIGHT_AT, String, '\0', "debug-highlight-at", Debug,
     "Print the highlight spans for the given line, then exit. The highlighter "
     "test driver.");
FLAG(DEBUG_GHOST_AT, String, '\0', "debug-ghost-at", Debug,
     "Print the ghost completion result and operation counts, then exit.");
FLAG(DEBUG_BRACKETS_AT, String, '\0', "debug-brackets-at", Debug,
     "Print the bracket pair matched at each caret offset of the given line, "
     "then exit. The bracket matching test driver.");
FLAG(DEBUG_HINT_AT, String, '\0', "debug-hint-at", Debug,
     "Print the inline hint row for the given line with the caret at its end, "
     "then exit. The hint row test driver.");
#endif

#include "MainOperations.hpp"

namespace koshka {

struct invocation_identity
{
  String program_path;
  String executable_path;
  mimic_mood invocation_mood;
  mimic_mood session_mood;
  bool is_login_shell;
  bool is_restricted_shell;
  bool was_mood_named_on_command_line;
};

static fn make_invocation_identity(String program_path) throws
    -> invocation_identity
{
  let const is_login_name =
      !program_path.view().is_empty() && program_path.view()[0] == '-';
  let normalized_program_basename =
      String{Path::invocation_filename(program_path.view(), is_login_name)};
  let const program_name_info =
      os::normalize_program_name(normalized_program_basename);
  StringView program_basename = normalized_program_basename.substring_of_length(
      0, program_name_info.stem_length);

  let executable_path = program_path.clone();
  if (is_login_name && program_path.view().length > 1) {
    executable_path = String{program_path.view().substring(1)};
  }
  if (!executable_path.is_empty() && executable_path.view() != "<unknown>" &&
      !os::has_directory_separator(executable_path.view()))
  {
    let inherited_path = os::get_environment_variable("PATH");
    let const found_paths = inherited_path.has_value()
                                ? ProgramResolver{steal(inherited_path)}.search(
                                      executable_path.view())
                                : ArrayList<Path>{heap_allocator()};
    if (found_paths.count() > 0) {
      executable_path = String{found_paths[0].text()};
    } else if (let running_path = os::current_executable_path();
               running_path.has_value())
    {
      executable_path = steal(*running_path);
    }
  }
  if (!executable_path.is_empty() &&
      !Path{executable_path.view()}.is_absolute())
  {
    executable_path = String{
        Path{executable_path.view()}.to_absolute_without_normalizing().view()};
  }

  const mimic_mood invocation_mood =
      (program_basename == "sh" || program_basename == "dash")
          ? mimic_mood::Posix
      : program_basename == "bash" || program_basename == "rbash"
          ? mimic_mood::Bash
          : mimic_mood::Default;
  let const is_restricted_shell =
      FLAG_RESTRICTED.is_enabled() || program_basename == "rbash";
  LOG(Info, "invocation basename is '%.*s'",
      static_cast<int>(program_basename.length), program_basename.data);
  let session_mood = resolve_session_mood(invocation_mood);
  LOG(Info, "selecting the %s mood",
      session_mood == mimic_mood::Posix       ? "posix"
      : session_mood == mimic_mood::Bash      ? "bash"
      : session_mood == mimic_mood::BashPosix ? "bash-posix"
                                              : "default");

  let const is_login_shell = FLAG_LOGIN.is_enabled() || is_login_name;
  LOG(Info, "the shell %s a login shell", is_login_shell ? "is" : "is not");

  let const was_mood_named_on_command_line =
      FLAG_MOOD.is_set() || FLAG_DUMB.is_enabled() ||
      FLAG_POSIX_COMPAT.is_enabled() || invocation_mood != mimic_mood::Default;

  return invocation_identity{steal(program_path),
                             steal(executable_path),
                             invocation_mood,
                             session_mood,
                             is_login_shell,
                             is_restricted_shell,
                             was_mood_named_on_command_line};
}

struct command_line
{
  int argc;
  char **argv;
  ArrayList<String> flag_tokens{heap_allocator()};
  ArrayList<const char *> spliced_argv{heap_allocator()};
  ArrayList<String> operands{heap_allocator()};
  bool is_login_invocation = false;
  bool is_rescue_mode = false;

  fn get_parse_argc() const wontthrow -> int
  {
    return spliced_argv.is_empty() ? argc
                                   : static_cast<int>(spliced_argv.count());
  }

  fn get_parse_argv() const wontthrow -> const char *const *
  {
    return spliced_argv.is_empty() ? argv : spliced_argv.begin();
  }
};

static fn splice_environment_flags(command_line &line) throws -> void
{
  if (Maybe<String> kosh_flags = os::get_environment_variable("KOSH_FLAGS");
      kosh_flags.has_value() && !kosh_flags->is_empty())
  {
    static constexpr PackedStringKey IGNORED_KOSH_FLAG_KEYS[] = {
        SSK("--apply"), SSK("--format"), SSK("--as-language-server")};
    static constexpr StaticStringSet IGNORED_KOSH_FLAGS{IGNORED_KOSH_FLAG_KEYS};
    let const view = kosh_flags->view();
    bool should_skip_next_command_word = false;

    view.for_each_ascii_whitespace_word([&](StringView token) throws {
      if (should_skip_next_command_word) {
        should_skip_next_command_word = false;
      } else if (token == "-c") {
        should_skip_next_command_word = true;
      } else if (!IGNORED_KOSH_FLAGS.contains(token)) {
        line.flag_tokens.push(String{token});
      }
    });
  }

  if (!line.flag_tokens.is_empty() && line.argc > 0) {
    line.spliced_argv.reserve(static_cast<usize>(line.argc) +
                              line.flag_tokens.count());
    line.spliced_argv.push(line.argv[0]);
    for (let const &token : line.flag_tokens)
      line.spliced_argv.push(token.c_str());
    for (int i = 1; i < line.argc; i++)
      line.spliced_argv.push(line.argv[i]);
  }
}

static constexpr flag_parse_options INVOCATION_PARSE_OPTIONS{
    .plus_letters = "aCefhmnptuvxoO"};

static fn parse_invocation_flags(int argc, const char *const *argv) throws
    -> ArrayList<String>
{
  return parse_flags(FLAG_LIST, argc, argv, 0, &FLAG_COMMAND, nullptr, nullptr,
                     StringView{}, INVOCATION_PARSE_OPTIONS);
}

static fn enter_rescue_mode(command_line &line) throws -> void
{
  show_message("Entering rescue.");
  line.is_rescue_mode = true;
  reset_flags(FLAG_LIST);
  try {
    line.operands = parse_invocation_flags(line.argc, line.argv);
  } catch (...) {
    reset_flags(FLAG_LIST);
    line.operands = ArrayList<String>{heap_allocator()};
    if (line.argc > 0) line.operands.push(String{line.argv[0]});
  }
}

static fn parse_command_line(command_line &line) throws -> Maybe<int>
{
  splice_environment_flags(line);
  line.is_login_invocation = line.argc > 0 && line.argv[0][0] == '-';

  let const parse_argc = line.get_parse_argc();
  let const parse_argv = line.get_parse_argv();
  try {
    line.operands = parse_invocation_flags(parse_argc, parse_argv);
  } catch (const ErrorWithLocation &e) {
    let const source = join_command_line(parse_argc, parse_argv);
    let highlight_context =
        EvalContext{startup_options{}, String{parse_argv[0]}};
    show_message(e.to_string(source, &highlight_context));
    if (!line.is_login_invocation) {
      return 2;
    }
    enter_rescue_mode(line);
  } catch (const Error &e) {
    show_message(e.to_string());
    if (!line.is_login_invocation) {
      return 2;
    }
    enter_rescue_mode(line);
  }

  return None;
}

static fn is_debug_driver_run() wontthrow -> bool
{
#if !defined NDEBUG
  return FLAG_DEBUG_COMPLETE_AT.is_set() || FLAG_DEBUG_HIGHLIGHT_AT.is_set() ||
         FLAG_DEBUG_GHOST_AT.is_set() || FLAG_DEBUG_BRACKETS_AT.is_set() ||
         FLAG_DEBUG_HINT_AT.is_set();
#else
  return false;
#endif
}

struct input_plan
{
  bool should_read_stdin = false;
  bool should_execute_commands = false;
  bool should_read_files = false;
  bool should_be_interactive = false;
};

static fn show_unknown_flag_value(StringView flag_prefix, StringView value,
                                  StringView message) throws -> void
{
  String source = flag_prefix;
  let const value_position = source.count();
  source += value;
  show_message(ErrorWithLocation{
      SourceLocation{value_position, value.length},
      message
  }
                   .to_string(source.view()));
}

static fn parse_init_moods(ArrayList<mimic_mood> &moods) throws -> Maybe<int>
{
  for (usize i = 0; i < FLAG_INIT_MOODS.count(); i++) {
    let const unknown_name = parse_mood_list(FLAG_INIT_MOODS.get(i), moods);
    if (unknown_name.has_value()) {
      show_unknown_flag_value(
          "--init-moods ", *unknown_name,
          "Unknown --init-moods value, expected one of 'kosh', 'bash', or "
          "'sh'");
      return 2;
    }
  }

  return None;
}

struct invocation_option
{
  const option_descriptor *option;
  option_origin origin;
  bool is_on;
};

static fn find_letter_flag(char letter) wontthrow -> FlagBool *
{
  switch (letter) {
  case 'a': return &FLAG_EXPORT_ALL;
  case 'C': return &FLAG_NO_CLOBBER;
  case 'e': return &FLAG_ERROR_EXIT;
  case 'f': return &FLAG_DISABLE_EXPANSION;
  case 'n': return &FLAG_NO_EXEC;
  case 'p': return &FLAG_PRIVILEGED;
  case 't': return &FLAG_ONE_COMMAND;
  case 'u': return &FLAG_NOUNSET;
  case 'v': return &FLAG_VERBOSE;
  case 'x': return &FLAG_EXPAND_VERBOSE;
  default: return nullptr;
  }
}

static fn
resolve_invocation_options(ArrayList<invocation_option> &options) throws
    -> Maybe<int>
{
  for (usize i = 0; i < FLAG_SET_OPTION.count(); i++) {
    let const name = FLAG_SET_OPTION.get(i);
    let const is_on = !FLAG_SET_OPTION.was_given_after_plus(i);
    let const *option = find_option_by_set_name(name);
    if (option == nullptr) {
      show_unknown_flag_value(is_on ? "-o " : "+o ", name,
                              "Unknown -o option name");
      return 2;
    }

    let const position = FLAG_SET_OPTION.get_position(i);
    if (let *letter_flag = find_letter_flag(option->letter);
        letter_flag != nullptr)
    {
      if (position > letter_flag->position()) {
        if (is_on)
          letter_flag->enable();
        else
          letter_flag->disable();
        letter_flag->set_position(static_cast<u32>(position));
      }
      continue;
    }
    if (option->storage == option_storage::Posix) {
      if (is_on) FLAG_POSIX_COMPAT.enable();
      continue;
    }

    options.push(invocation_option{option, option_origin::Set, is_on});
  }

  for (usize i = 0; i < FLAG_SHOPT_OPTION.count(); i++) {
    let const name = FLAG_SHOPT_OPTION.get(i);
    let const is_on = !FLAG_SHOPT_OPTION.was_given_after_plus(i);
    let const *option = find_option_by_shopt_name(name);
    if (option == nullptr) {
      show_unknown_flag_value(is_on ? "-O " : "+O ", name,
                              "Unknown -O option name");
      return 2;
    }
    if (option->is_read_only) continue;

    options.push(invocation_option{option, option_origin::Shopt, is_on});
  }

  return None;
}

static fn validate_invocation(const ArrayList<String> &operands,
                              ArrayList<mimic_mood> &init_moods) throws
    -> Maybe<int>
{
  if (FLAG_MOOD.is_set() && !parse_mood_name(FLAG_MOOD.value())) {
    show_unknown_flag_value("--mood ", FLAG_MOOD.value(),
                            "Unknown --mood value, expected one of 'kosh', "
                            "'bash', 'sh', or 'bash-posix'");
    return 2;
  }

  if (FLAG_TAB_SELECTOR.is_set() &&
      !parse_tab_selector_name(FLAG_TAB_SELECTOR.value()))
  {
    show_unknown_flag_value(
        "--tab-selector ", FLAG_TAB_SELECTOR.value(),
        "Unknown --tab-selector value, expected one of 'interactive', "
        "'external', or 'plain'");
    return 2;
  }

  if (FLAG_LANGUAGE_SERVER.is_enabled() &&
      (FLAG_STDIN.is_enabled() || FLAG_INTERACTIVE.is_enabled() ||
       FLAG_LINT.is_enabled() || FLAG_FORMAT.is_enabled() ||
       FLAG_APPLY.is_enabled() || !FLAG_COMMAND.is_empty() ||
       !operands.is_empty()))
  {
    show_message("The '--as-language-server' option does not accept '-s', "
                 "'-i', '--lint', '--format', '--apply', '-c', or file "
                 "operands.");
    return 2;
  }
  if (FLAG_APPLY.is_enabled() && !FLAG_LINT.is_enabled() &&
      !FLAG_FORMAT.is_enabled())
  {
    show_message("The '--apply' option requires '--lint' or '--format'.");
    return 2;
  }
  if (FLAG_APPLY.is_enabled() &&
      (FLAG_STDIN.is_enabled() || FLAG_INTERACTIVE.is_enabled() ||
       !FLAG_COMMAND.is_empty()))
  {
    show_message("The '--apply' option does not accept '-s', '-i', or '-c'.");
    return 2;
  }
  if (FLAG_APPLY.is_enabled()) {
    if (operands.is_empty()) {
      show_message("The '--apply' option requires named files.");
      return 2;
    }
    for (let const &operand : operands) {
      if (operand != "-") continue;
      show_message("The '--apply' option does not accept '-'.");
      return 2;
    }
  }
  if (FLAG_FORMAT.is_enabled() && !FLAG_APPLY.is_enabled() &&
      operands.count() > 1)
  {
    show_message("The '--format' option accepts one file without '--apply'.");
    return 2;
  }

  return parse_init_moods(init_moods);
}

static fn select_input_source(const ArrayList<String> &operands) throws
    -> input_plan
{
  let plan = input_plan{};
  if (FLAG_LANGUAGE_SERVER.is_enabled() || FLAG_FORMAT.is_enabled()) {
    return plan;
  }

  if (FLAG_STDIN.is_enabled()) {
    if (!FLAG_COMMAND.is_empty()) {
      show_message("Incompatible options or arguments were specified along "
                   "with '-s' option. "
                   "Falling back to '-s'.");
    }
    if (FLAG_LINT.is_enabled() && !operands.is_empty()) {
      show_message("The '-s' option was given along with file operands, "
                   "so '--lint' takes standard input and analyzes no "
                   "named file.");
    }
    plan.should_read_stdin = true;
  } else if (FLAG_LINT.is_enabled() &&
             (!FLAG_COMMAND.is_empty() || !operands.is_empty()))
  {
    plan.should_execute_commands = !FLAG_COMMAND.is_empty();
    plan.should_read_files = !operands.is_empty();
  } else if (FLAG_LINT.is_enabled()) {
    plan.should_read_stdin = true;
  } else if (!FLAG_COMMAND.is_empty()) {
    if (FLAG_INTERACTIVE.is_enabled()) {
      show_message("Incompatible options or arguments were specified along "
                   "with '-c' options. "
                   "Falling back to '-c'.");
    }
    plan.should_execute_commands = true;
  } else if (!operands.is_empty()) {
    if (FLAG_INTERACTIVE.is_enabled()) {
      show_message("Both file argument and '-i' option were given. "
                   "Falling back to reading files.");
    }
    plan.should_read_files = true;
  } else if (FLAG_INTERACTIVE.is_enabled() || os::is_stdin_a_tty()) {
    plan.should_be_interactive = true;
  } else {
    plan.should_read_stdin = true;
  }

  return plan;
}

static fn resolve_input_plan(const ArrayList<String> &operands) throws
    -> input_plan
{
  if (FLAG_STDIN.is_enabled() && FLAG_INTERACTIVE.is_enabled()) {
    let const should_use_interactive =
        !FLAG_LINT.is_enabled() && os::is_stdin_a_tty();

    let s = String{heap_allocator()};
    s += "Both '-s' and '-i' options were specified. Falling back to ";
    if (should_use_interactive)
      s += "'-i'";
    else if (FLAG_LINT.is_enabled())
      s += "'-s'.";
    else
      s += "'-s' because stdin is not a tty.";
    show_message(s);

    if (should_use_interactive)
      FLAG_STDIN.toggle();
    else
      FLAG_INTERACTIVE.toggle();
  }

  let plan = select_input_source(operands);
  if (is_debug_driver_run()) {
    plan.should_be_interactive = false;
    plan.should_read_files = false;
    if (!plan.should_execute_commands) plan.should_read_stdin = true;
  }
  LOG(Info, "the input source is %s",
      plan.should_read_stdin         ? "standard input"
      : plan.should_execute_commands ? "the -c command strings"
      : plan.should_read_files       ? "the named script file"
                                     : "the interactive prompt");

  return plan;
}

static fn prefetch_script_shebang(invocation_identity &identity,
                                  const input_plan &input,
                                  const ArrayList<String> &operands) throws
    -> Maybe<String>
{
  Maybe<String> contents = None;
  if (!input.should_read_files || FLAG_LINT.is_enabled() ||
      operands.is_empty() || operands[0] == "-" ||
      identity.was_mood_named_on_command_line)
  {
    return contents;
  }

  contents = Path{operands[0].view()}.read_entire_file();
  if (contents.has_value()) {
    let const shebang_mood = detect_mimic_shell_from_source(contents->view());
    LOG(Info, "the script operand '%s' %s a shell to mimic",
        operands[0].c_str(),
        shebang_mood.has_value() ? "selects" : "does not select");
    identity.session_mood = shebang_mood.value_or(identity.session_mood);
  }

  return contents;
}

static fn make_startup_options(const input_plan &input) wontthrow
    -> startup_options
{
  startup_options options{};
  options.should_disable_path_expansion = FLAG_DISABLE_EXPANSION.is_enabled();
  options.should_echo = FLAG_VERBOSE.is_enabled();
  options.should_echo_expanded = FLAG_EXPAND_VERBOSE.is_enabled();
  options.is_interactive = input.should_be_interactive;
  options.should_error_exit = FLAG_ERROR_EXIT.is_enabled();

  return options;
}

struct inherited_shell
{
  decltype(os::take_subshell_bootstrap()) bootstrap;
  root_evaluation_mode evaluation_mode;
  String source_origin{heap_allocator()};
  ArrayList<String> source_windows{heap_allocator()};
  Maybe<os::inherited_subshell_state> state = None;
  bool has_invalid_state = false;
  bool should_suppress_root_source_trace = false;
  bool should_use_command_string_status = false;

  fn take_evaluation_mode() wontthrow -> root_evaluation_mode
  {
    let const mode = evaluation_mode;
    evaluation_mode = root_evaluation_mode::Normal;

    return mode;
  }
};

static fn take_inherited_shell() throws -> inherited_shell
{
  os::unset_environment_variable("KOSH_IDENTITY");
  let bootstrap = os::take_subshell_bootstrap();
  let const evaluation_mode = bootstrap.evaluation_mode;
  let const should_use_command_string_status =
      bootstrap.should_use_command_string_status;
  let inherited = inherited_shell{steal(bootstrap), evaluation_mode};
  inherited.should_use_command_string_status = should_use_command_string_status;
  inherited.source_origin = steal(inherited.bootstrap.source_origin);
  if (!os::can_fork_evaluator() && !inherited.bootstrap.payload.is_empty()) {
    inherited.state = os::inherited_subshell_state::take_from_environment();
    if (!inherited.state.has_value()) {
      show_message("Invalid inherited shell state");
      inherited.has_invalid_state = true;

      return inherited;
    }

    os::set_shell_process_id(inherited.state->shell_process_id);
    os::set_shell_parent_process_id(inherited.state->shell_parent_process_id);
  }
  inherited.should_suppress_root_source_trace =
      os::get_environment_variable(internal::SUPPRESS_ROOT_TRACE).has_value();
  os::unset_environment_variable(internal::SUPPRESS_ROOT_TRACE);

  return inherited;
}

static fn apply_inherited_shell(inherited_shell &inherited,
                                EvalContext &context) throws -> Maybe<int>
{
  if (!inherited.bootstrap.payload.is_empty()) {
    try {
      context.apply_subshell_bootstrap(steal(inherited.bootstrap));
    } catch (const Error &error) {
      show_message(error.to_string());
      return 1;
    } catch (const std::bad_alloc &) {
      show_message("Could not allocate inherited shell state");
      return 1;
    }
    context.runtime_state().set_show_ast(false);
    context.runtime_state().set_show_lexed_words(false);
  }
  if (inherited.state.has_value()) {
    context.execution_store().set_last_exit_status(
        inherited.state->previous_exit_status);
    context.set_subshell_depth(inherited.state->subshell_depth);
  }

  return None;
}

static pure fn
is_kept_in_restricted_shell(const option_descriptor &option) wontthrow -> bool
{
  if (option.category == option_class::Semantic) return false;

  switch (option.storage) {
  case option_storage::Mood:
  case option_storage::Variable: return false;
  default: return true;
  }
}

static fn keep_restricted_settings(koshconf_reading &reading) throws -> void
{
  let kept = ArrayList<koshconf_setting>{heap_allocator()};
  for (let &setting : reading.settings) {
    if (is_kept_in_restricted_shell(*setting.option)) {
      kept.push(steal(setting));
      continue;
    }
    reading.warnings.push(Warning{StringView{"A restricted shell ignores '"} +
                                  setting.option->koshconf_name +
                                  "' from the configuration files"}
                              .to_string());
  }
  reading.settings = steal(kept);
}

static fn find_system_koshconf_path() throws -> Maybe<Path>
{
#if defined KOSH_DEBUG_SYSTEM_KOSHCONF_OVERRIDE
  if (let const debug_path =
          os::get_environment_variable("KOSH_DEBUG_SYSTEM_KOSHCONF");
      debug_path.has_value() && !debug_path->is_empty())
  {
    return Path{debug_path->view()};
  }
#endif

  return os::get_system_koshconf_path();
}

static fn read_startup_configuration(const command_line &line,
                                     const inherited_shell &inherited,
                                     const invocation_identity &identity,
                                     bool has_elevated_identity) throws
    -> koshconf_reading
{
  let reading = koshconf_reading{};
  let encoded = os::get_environment_variable(KOSHCONF_VARIABLE_NAME);
  if (encoded.has_value())
    os::unset_environment_variable(KOSHCONF_VARIABLE_NAME);
  if (encoded.has_value() && identity.is_restricted_shell) {
    show_message(Warning{"A restricted shell ignores the KOSHCONF environment "
                         "variable"}
                     .to_string()
                     .view());
    encoded = None;
  }
  if (encoded.has_value() && FLAG_PRIVILEGED.is_enabled()) {
    LOG(Info, "privileged mode ignores the KOSHCONF environment variable");
    encoded = None;
  }

  let const should_skip =
      has_elevated_identity || line.is_rescue_mode || FLAG_CLEAN.is_enabled() ||
      FLAG_NO_CONFIG.is_enabled() || FLAG_LINT.is_enabled() ||
      FLAG_FORMAT.is_enabled() || FLAG_LANGUAGE_SERVER.is_enabled() ||
      is_debug_driver_run();
  if (should_skip) {
    LOG(Info, "skipping the configuration files");
    return reading;
  }
  if (!inherited.bootstrap.payload.is_empty()) {
    LOG(Info, "a fresh evaluator applies only the inherited KOSHCONF");
    if (encoded.has_value())
      unused(read_koshconf_blob(encoded->view(), reading));
    return reading;
  }

  if (let const system_path = find_system_koshconf_path();
      system_path.has_value())
  {
    unused(read_system_koshconf_file(*system_path, reading));
  }
  if (let const user_path = get_user_koshconf_path(); user_path.has_value())
    unused(read_koshconf_file(*user_path, reading));
  let const blob_problem = encoded.has_value()
                               ? read_koshconf_blob(encoded->view(), reading)
                               : Maybe<StringView>{};
  if (blob_problem.has_value()) {
    reading.warnings.push(Warning{
        "The KOSHCONF environment variable is ignored because " + *blob_problem}
                              .to_string());
  }
  if (identity.is_restricted_shell) keep_restricted_settings(reading);
  for (let const &warning : reading.warnings)
    show_message(warning.view());
  reading.warnings.clear();

  return reading;
}

static fn apply_configured_mood(invocation_identity &identity,
                                const koshconf_reading &reading) throws -> void
{
  if (identity.was_mood_named_on_command_line) return;

  for (let const &setting : reading.settings) {
    if (setting.option->storage != option_storage::Mood) continue;
    let const mood = parse_option_number(*setting.option, setting.value.view());
    if (mood.has_value())
      identity.session_mood = static_cast<mimic_mood>(*mood);
  }
}

static fn apply_configured_init_moods(ArrayList<mimic_mood> &init_moods,
                                      const koshconf_reading &reading,
                                      bool should_source_session_files) throws
    -> void
{
  if (FLAG_INIT_MOODS.count() != 0 || !should_source_session_files) return;

  for (let const &setting : reading.settings) {
    if (setting.option->storage != option_storage::InitMoods) continue;

    init_moods.clear();
    unused(parse_mood_list(setting.value.view(), init_moods));
  }
}

static fn describe_init_moods(const ArrayList<mimic_mood> &init_moods) throws
    -> String
{
  let text = String{heap_allocator()};
  for (let const mood : init_moods) {
    if (!text.is_empty()) text += ',';
    text += mood_name(mood);
  }

  return text;
}

static fn is_pinned_by_invocation(
    const option_descriptor &option,
    const ArrayList<invocation_option> &invocation_options) wontthrow -> bool
{
  for (let const &named : invocation_options) {
    let const is_same_editor_mode =
        named.option->storage == option_storage::EditorMode &&
        option.storage == option_storage::EditorMode;
    if (named.option == &option || is_same_editor_mode) return true;
  }

  let const is_analysis_inherited = os::has_environment_variable(
      inheritable_analysis_state::ENVIRONMENT_NAME);
  switch (option.storage) {
  case option_storage::Mood: return true;
  case option_storage::TabSelector:
    return FLAG_TAB_SELECTOR.is_set() || FLAG_DUMB.is_enabled();
  case option_storage::WarningLevel:
    return FLAG_WARNINGS.count() != 0 || is_analysis_inherited;
  case option_storage::AnnoyingDiagnostics:
    return FLAG_SUPPRESS_ANNOYING_DIAGNOSTICS.is_enabled() ||
           is_analysis_inherited;
  case option_storage::Analysis:
    return FLAG_SUPPRESS_DIAGNOSTICS.is_enabled() || is_analysis_inherited;
  case option_storage::SourceTraces: return FLAG_NO_TRACES.is_enabled();
  case option_storage::InitMoods: return FLAG_INIT_MOODS.count() != 0;
  case option_storage::Variable:
    return os::has_environment_variable(option.variable_name);
  case option_storage::ShellOption: break;
  default: return false;
  }

  switch (option.shell_option) {
  case shell_option_id::Errexit:
  case shell_option_id::Noglob:
  case shell_option_id::Verbose:
  case shell_option_id::Xtrace:
  case shell_option_id::Allexport:
  case shell_option_id::Noclobber:
  case shell_option_id::Nounset:
    return find_letter_flag(option.letter)->position() != 0;
  case shell_option_id::ExtendedArithmetic:
    return FLAG_EXTENDED_ARITHMETIC.is_enabled();
  case shell_option_id::Koshkit: return FLAG_ENABLE_KOSHKIT.is_enabled();
  case shell_option_id::Mimicry:
    return FLAG_MIMICRY.is_enabled() || is_analysis_inherited;
  case shell_option_id::ShowAst: return FLAG_AST.is_enabled();
  case shell_option_id::ShowLexedWords: return FLAG_ESCAPE_MAP.is_enabled();
  case shell_option_id::ShowExitCode: return FLAG_EXIT_CODE.is_enabled();
  case shell_option_id::ShowAllExitCodes:
    return FLAG_ALL_EXIT_CODES.is_enabled();
  case shell_option_id::ShowStats: return FLAG_STATS.is_enabled();
  case shell_option_id::ShowMemory: return FLAG_MEMORY.is_enabled();
  case shell_option_id::TabCompletion: return FLAG_NO_COMPLETION.is_enabled();
  case shell_option_id::SyntaxHighlighting:
    return FLAG_NO_SYNTAX_HIGHLIGHTING.is_enabled();
  default: return false;
  }
}

static fn apply_startup_configuration(
    EvalContext &context, koshconf_reading &reading,
    const ArrayList<invocation_option> &invocation_options) throws -> Maybe<int>
{
  let const is_kosh_mood =
      context.runtime_state().get_mood() == mimic_mood::Default;
  let unpinned = ArrayList<koshconf_setting>{heap_allocator()};
  for (let &setting : reading.settings) {
    if (is_pinned_by_invocation(*setting.option, invocation_options)) continue;

    let const &option = *setting.option;
    let const value = option.type == option_type::String
                          ? Maybe<u32>{}
                          : parse_option_number(option, setting.value.view());
    let const is_held_by_kosh_mood =
        is_kosh_mood && option.is_fixed_in_kosh_mood && value.has_value() &&
        *value != option.strict_value;
    if (is_held_by_kosh_mood) {
      reading.warnings.push(
          !setting.kosh_mood_warning.is_empty()
              ? steal(setting.kosh_mood_warning)
              : Warning{describe_kosh_mood_hold(option).view()}.to_string());
      continue;
    }

    unpinned.push(steal(setting));
  }
  reading.settings.clear();

  apply_koshconf_settings(context, unpinned, option_origin::Startup,
                          reading.warnings);
  for (let const &warning : reading.warnings)
    show_message(warning.view());
  reading.warnings.clear();

  for (let const &named : invocation_options) {
    try {
      write_option_number(context, *named.option, named.is_on ? 1 : 0,
                          named.origin);
    } catch (const Error &error) {
      show_message(error.to_string());
      return 2;
    }
  }

  return None;
}

struct session_config
{
  mimic_mood mood;
  tab_selector_mode tab_selector;
  inheritable_analysis_state analysis;
  bool is_interactive;
  bool is_login_shell;
  bool is_restricted_shell;
  bool has_custom_rcfile;
  bool has_execution_string;
  bool is_stats_enabled;
  bool should_show_ast;
  bool should_show_lexed_words;
  bool should_show_exit_code;
  bool should_show_all_exit_codes;
  bool is_memory_stats_enabled;
  bool has_source_traces;
  bool is_tab_completion_enabled;
  bool is_syntax_highlighting_enabled;
  bool is_privileged;
  bool is_one_command;
  bool is_extended_arithmetic_enabled;
  bool is_extended_arithmetic_explicit;
  bool is_nounset_enabled;
  bool is_no_clobber;
  bool is_export_all;
  bool is_no_exec;
  bool is_koshkit_enabled;
};

static fn read_analysis_state(const invocation_identity &identity) throws
    -> inheritable_analysis_state
{
  let analysis = inheritable_analysis_state::from_environment();
  analysis.is_mimicry_enabled |= FLAG_MIMICRY.is_enabled();
  analysis.reporting.is_diagnostics_disabled |=
      FLAG_SUPPRESS_DIAGNOSTICS.is_enabled();
  analysis.reporting.is_annoying_disabled |=
      FLAG_SUPPRESS_ANNOYING_DIAGNOSTICS.is_enabled();
  if (let const warnings_specified_count = FLAG_WARNINGS.count();
      warnings_specified_count != 0)
  {
    analysis.reporting.warning_level = static_cast<u8>(
        warnings_specified_count > 3 ? 3 : warnings_specified_count);
  }

  if (FLAG_LINT.is_enabled()) {
    analysis.reporting.is_diagnostics_disabled = false;
    analysis.reporting.is_annoying_disabled = false;
    analysis.reporting.warning_level =
        warning_level_for_mood(identity.session_mood);
  }

  return analysis;
}

static fn read_session_config(const invocation_identity &identity,
                              const input_plan &input) throws -> session_config
{
  return session_config{identity.session_mood,
                        resolve_session_tab_selector(),
                        read_analysis_state(identity),
                        input.should_be_interactive,
                        identity.is_login_shell,
                        identity.is_restricted_shell,
                        selected_rcfile().has_value(),
                        input.should_execute_commands,
                        FLAG_STATS.is_enabled(),
                        FLAG_AST.is_enabled(),
                        FLAG_ESCAPE_MAP.is_enabled(),
                        FLAG_EXIT_CODE.is_enabled(),
                        FLAG_ALL_EXIT_CODES.is_enabled(),
                        FLAG_MEMORY.is_enabled(),
                        !FLAG_NO_TRACES.is_enabled(),
                        !FLAG_NO_COMPLETION.is_enabled(),
                        !FLAG_NO_SYNTAX_HIGHLIGHTING.is_enabled(),
                        FLAG_PRIVILEGED.is_enabled(),
                        FLAG_ONE_COMMAND.is_enabled(),
                        identity.session_mood == mimic_mood::Default ||
                            FLAG_EXTENDED_ARITHMETIC.is_enabled(),
                        FLAG_EXTENDED_ARITHMETIC.is_enabled(),
                        FLAG_NOUNSET.is_enabled(),
                        FLAG_NO_CLOBBER.is_enabled(),
                        FLAG_EXPORT_ALL.is_enabled(),
                        FLAG_NO_EXEC.is_enabled() || FLAG_LINT.is_enabled(),
                        FLAG_ENABLE_KOSHKIT.is_enabled()};
}

static fn apply_session_config(EvalContext &context,
                               const session_config &config) throws -> void
{
  let &state = context.runtime_state();
  state.set_stats_enabled(config.is_stats_enabled);
  state.set_show_ast(config.should_show_ast);
  state.set_show_lexed_words(config.should_show_lexed_words);
  state.set_show_exit_code(config.should_show_exit_code);
  state.set_show_all_exit_codes(config.should_show_all_exit_codes);
  state.set_memory_stats_enabled(config.is_memory_stats_enabled);
  context.diagnostics_store().set_source_traces_enabled(
      config.has_source_traces);
  state.set_option(shell_option_id::TabCompletion,
                   config.is_tab_completion_enabled);
  state.set_option(shell_option_id::SyntaxHighlighting,
                   config.is_syntax_highlighting_enabled);
  state.set_option(shell_option_id::Privileged, config.is_privileged);
  state.set_option(shell_option_id::Onecmd, config.is_one_command);
  if (config.has_execution_string) {
    context.execution_store().set_execution_string(
        String{heap_allocator(), FLAG_COMMAND.get(0)});
  }
  context.startup_store().set_login_shell(config.is_login_shell);
  context.startup_store().set_custom_rcfile(config.has_custom_rcfile);
  if (config.is_restricted_shell) {
    context.startup_store().request_restricted_shell();
  }
  state.set_mood(config.mood);
  state.set_tab_selector(config.tab_selector);
  state.set_extended_arithmetic(config.is_extended_arithmetic_enabled);
  if (config.is_extended_arithmetic_explicit) {
    state.set_extended_arithmetic_set_explicitly(true);
  }
  state.set_error_unset(config.is_nounset_enabled);
  if (config.is_nounset_enabled) {
    state.set_error_unset_set_explicitly(true);
  }
  state.set_inheritable_analysis_state(config.analysis);
  state.set_pipefail(false);
  state.set_no_clobber(config.is_no_clobber);
  state.set_export_all(config.is_export_all);
  state.set_no_exec(config.is_no_exec);
  state.set_koshkit(config.is_koshkit_enabled);
  state.set_failglob(false);
  state.set_option(shell_option_id::Monitor, config.is_interactive);
}

static fn seed_shell_level() throws -> void
{
  i64 shell_level = 0;
  if (Maybe<String> inherited = os::get_environment_variable("SHLVL");
      inherited.has_value())
  {
    if (ErrorOr<i64> parsed_level = inherited->view().to<i64>();
        !parsed_level.is_error() && parsed_level.value() > 0)
    {
      shell_level = parsed_level.value();
    }
  }
  constexpr i64 MAX_SHLVL = 999;
  if (shell_level > MAX_SHLVL) shell_level = 0;
  os::set_environment_variable("SHLVL",
                               String::from(shell_level + 1, heap_allocator()));
}

static fn seed_session_variables(EvalContext &context,
                                 invocation_identity &identity,
                                 const ArrayList<mimic_mood> &init_moods,
                                 const inherited_shell &inherited,
                                 bool is_interactive) throws -> void
{
  context.execution_store().set_shell_executable_path(
      steal(identity.executable_path));
  let const shell_executable_path =
      context.execution_store().get_shell_executable_path();
  context.mark_exported("KOSH_IDENTITY");
  context.variable_store().attributes().mark_readonly("KOSH_IDENTITY");
  if (!os::has_environment_variable("SHELL"))
    context.set_shell_variable("SHELL", shell_executable_path);
  context.set_shell_variable("PWD", Path::current_directory().text());
  context.set_shell_variable("KOSH", shell_executable_path);
  context.set_shell_variable("KOSH_VERSION", KOSH_VERSION_STRING);
  context.set_shell_variable("KOSH_COMMIT", KOSH_COMMIT_HASH);
  context.set_shell_variable("KOSH_BUILD_MODE", KOSH_BUILD_MODE);
  context.set_shell_variable("KOSH_OS", KOSH_OS_INFO);
  for (let const name : {StringView{"KOSH"}, StringView{"KOSH_VERSION"},
                         StringView{"KOSH_COMMIT"},
                         StringView{"KOSH_BUILD_MODE"}, StringView{"KOSH_OS"}})
  {
    if (os::has_environment_variable(name))
      context.unexport_shell_variable(name);
  }
  if (!context.variable_store()
           .shell_variables()
           .find("KOSH_HISTORY_FILE")
           .has_value())
  {
    if (let const history_path = toiletline::get_history_path();
        history_path.has_value())
    {
      context.set_shell_variable("KOSH_HISTORY_FILE", history_path->text());
    }
  }
  if (!context.get_variable_value("KOSH_HISTORY_SIZE").has_value())
    context.set_shell_variable("KOSH_HISTORY_SIZE", "4096");
  toiletline::set_history_persistent(is_interactive);

  let identity_mode = shell_identity_mode::Native;
  if (identity.session_mood == mimic_mood::Bash ||
      identity.session_mood == mimic_mood::BashPosix)
  {
    identity_mode = shell_identity_mode::Bash;
  }
  for (let listed : init_moods)
    if (listed == mimic_mood::Bash || listed == mimic_mood::BashPosix)
      identity_mode = shell_identity_mode::Bash;
  context.seed_shell_identity_variables(identity_mode);

  if (!inherited.state.has_value()) seed_shell_level();
  context.mark_exported("SHLVL");

  if (is_interactive && !os::has_environment_variable("PS1")) {
    context.set_shell_variable("PS1",
                               toiletline::get_default_prompt_template());
  }
  if (!os::has_environment_variable("PS2"))
    context.set_shell_variable("PS2", "> ");
  if (!os::has_environment_variable("PS4"))
    context.set_shell_variable("PS4", "+ ");

  context.set_shell_variable("OPTIND", "1");

  if (is_interactive) {
    if (let const dimensions = os::get_terminal_dimensions()) {
      context.set_shell_variable(
          "COLUMNS", String::from(dimensions->columns, heap_allocator()));
      context.set_shell_variable(
          "LINES", String::from(dimensions->rows, heap_allocator()));
    }
  }
}

struct init_diagnostics_scope
{
  EvalContext &context;
  bool should_restore;
  bool was_diagnostics_disabled;
  u64 saved_mutation_revision;

  init_diagnostics_scope(EvalContext &target, bool should_suppress)
      : context{target}, should_restore{should_suppress},
        was_diagnostics_disabled{
            target.runtime_state().is_diagnostics_disabled()}
  {
    if (should_suppress) target.runtime_state().set_diagnostics_disabled(true);
    saved_mutation_revision =
        target.runtime_control_store().diagnostics_mutation_revision();
  }

  init_diagnostics_scope(const init_diagnostics_scope &) = delete;
  fn operator=(const init_diagnostics_scope &) = delete;

  ~init_diagnostics_scope()
  {
    if (should_restore &&
        context.runtime_control_store().diagnostics_mutation_revision() ==
            saved_mutation_revision)
    {
      context.runtime_state().set_diagnostics_disabled(
          was_diagnostics_disabled);
    }
  }
};

static fn run_startup(EvalContext &context, ArrayList<mimic_mood> &init_moods,
                      const invocation_identity &identity,
                      const command_line &line,
                      const inherited_shell &inherited,
                      bool has_elevated_identity, bool is_interactive) throws
    -> void
{
  let const is_fresh_evaluator = !inherited.bootstrap.payload.is_empty();
  if (has_elevated_identity || line.is_rescue_mode || FLAG_CLEAN.is_enabled() ||
      FLAG_LINT.is_enabled() || FLAG_FORMAT.is_enabled() || is_fresh_evaluator)
  {
    LOG(Info, "skipping every startup config file in %s mode",
        line.is_rescue_mode       ? "rescue"
        : is_fresh_evaluator      ? "fresh evaluator"
        : FLAG_CLEAN.is_enabled() ? "clean"
        : FLAG_LINT.is_enabled()  ? "lint"
                                  : "privileged");
    return;
  }

  let const diagnostics_scope = init_diagnostics_scope{
      context, FLAG_SUPPRESS_INIT_DIAGNOSTICS.is_enabled()};
  if (!init_moods.is_empty() || identity.is_login_shell || is_interactive ||
      identity.session_mood == mimic_mood::Bash)
  {
    if (init_moods.is_empty()) init_moods.push(identity.session_mood);
    source_init_moods(context, init_moods, identity.is_login_shell,
                      is_interactive);
  }
}

static fn finish_startup(EvalContext &context,
                         const invocation_identity &identity,
                         inherited_shell &inherited, bool is_interactive) throws
    -> Maybe<int>
{
  if (is_interactive && !context.get_variable_value("PS1").has_value()) {
    context.set_shell_variable("PS1",
                               toiletline::get_default_prompt_template());
  }

  context.set_startup_finished();

  context.select_mood(context.runtime_control_store().was_mood_set_explicitly()
                          ? context.runtime_state().get_mood()
                          : identity.session_mood);
  if (FLAG_LINT.is_enabled()) {
    context.runtime_state().set_warning_level(
        warning_level_for_mood(context.runtime_state().get_mood()));
  }

  if (os::has_environment_variable(
          inheritable_analysis_state::ENVIRONMENT_NAME))
    context.sync_analysis_environment();

  if (Maybe<int> inherit_status = apply_inherited_shell(inherited, context);
      inherit_status.has_value())
  {
    return inherit_status;
  }

  context.clear_retained_sources();

  return None;
}

struct script_operands
{
  String shell_name;
  ArrayList<String> positional_params;
};

static fn take_script_operands(String program_name, ArrayList<String> &operands,
                               const input_plan &input) throws
    -> script_operands
{
  let shell_name = steal(program_name);
  let positional_params = ArrayList<String>{heap_allocator()};
  let const should_retain_operands =
      input.should_read_files || FLAG_FORMAT.is_enabled();

  usize first_param_index = 0;
  if (FLAG_LINT.is_enabled() && input.should_read_files) {
    first_param_index = operands.count();
  } else if ((input.should_read_files || input.should_execute_commands) &&
             !operands.is_empty())
  {
    shell_name =
        should_retain_operands ? operands[0].clone() : steal(operands[0]);
    first_param_index = 1;
  }

  positional_params.reserve(operands.count() - first_param_index);
  for (usize i = first_param_index; i < operands.count(); i++) {
    if (should_retain_operands)
      positional_params.push(operands[i].clone());
    else
      positional_params.push(steal(operands[i]));
  }

  return script_operands{steal(shell_name), steal(positional_params)};
}

struct script_chunk
{
  String contents{heap_allocator()};
  Maybe<StringView> filename = None;
  Maybe<StringView> command_string_name = None;
  Maybe<SourceLocation> root_frame_call_site = None;
  Maybe<usize> history_event_number = None;
  bool should_analyze = true;
  bool is_fresh_evaluator_command = false;
  bool should_use_command_string_status = true;
};

static fn find_command_call_site(const command_line &line,
                                 usize consumed_command_index) wontthrow
    -> Maybe<SourceLocation>
{
  let const parse_argc = line.get_parse_argc();
  let const parse_argv = line.get_parse_argv();
  usize seen_command_count = 0;
  usize flag_offset = 0;
  for (int a = 0; a < parse_argc; a++) {
    let const token_length = std::strlen(parse_argv[a]);
    const StringView token{parse_argv[a], token_length};
    let const quoted_length = shell_quoted_arg_length(token);
    if (token == "-c" || token == "--command") {
      seen_command_count++;
      if (seen_command_count == consumed_command_index && a + 1 < parse_argc) {
        const usize argument_length = shell_quoted_arg_length(
            StringView{parse_argv[a + 1], std::strlen(parse_argv[a + 1])});
        return SourceLocation{flag_offset, quoted_length + 1 + argument_length};
      }
    }
    flag_offset += quoted_length + 1;
  }

  return None;
}

struct script_cursor
{
  const command_line &line;
  const ArrayList<String> &operands;
  const input_plan &input;
  const invocation_identity &identity;
  const String &cli_invocation;
  Maybe<String> &prefetched_script_contents;
  bool should_quit;
  usize next_file_index = 0;

  fn read_whole_standard_input(script_chunk &chunk) const throws -> void
  {
    if (is_debug_driver_run()) return;

    LOG(Info, "reading the whole standard input");
    chunk.contents = utils::read_entire_standard_input();
  }

  fn read_standard_input(script_chunk &chunk) throws -> void
  {
    read_whole_standard_input(chunk);
    should_quit = true;
  }

  fn read_next_command(EvalContext &context, script_chunk &chunk,
                       bool is_fresh_evaluator) throws -> void
  {
    chunk.contents = FLAG_COMMAND.take_next();
    chunk.command_string_name = COMMAND_STRING_SOURCE_NAME;
    if (!is_fresh_evaluator) {
      context.execution_store().set_execution_string(
          String{heap_allocator(), chunk.contents.view()});
    }
    LOG(Info, "taking the next -c command string, %zu bytes",
        chunk.contents.count());
    chunk.root_frame_call_site =
        find_command_call_site(line, FLAG_COMMAND.value_position());
    if (FLAG_COMMAND.at_end() &&
        (!FLAG_LINT.is_enabled() || !input.should_read_files))
    {
      should_quit = true;
    }
  }

  fn read_next_file(EvalContext &context, script_chunk &chunk) throws -> void
  {
    ASSERT(next_file_index < operands.count());
    const String &file_name = operands[next_file_index++];

    if (file_name == "-")
      read_whole_standard_input(chunk);
    else
      read_script_file(context, chunk, file_name);

    should_quit =
        !FLAG_LINT.is_enabled() || next_file_index == operands.count();
  }

  fn read_script_file(EvalContext &context, script_chunk &chunk,
                      const String &file_name) throws -> void
  {
    let const operand_offset = quoted_argv_offset_until(
        line.get_parse_argc(), line.get_parse_argv(), file_name.view());
    const SourceLocation operand_location{
        operand_offset, shell_quoted_arg_length(file_name.view())};
    const Path script_path{file_name.view()};

    if (script_path.is_directory()) {
      let const verb = FLAG_LINT.is_enabled() ? StringView{"analyze"}
                                              : StringView{"execute"};
      show_message(ErrorWithLocation{
          operand_location, "Unable to " + verb + " `" + file_name.view() +
                                "` because the file is a directory"}
                       .to_string(cli_invocation.view(), &context));
      if (!FLAG_LINT.is_enabled()) {
        utils::quit(126, utils::farewell_policy::Goodbye);
      }
      chunk.should_analyze = false;
      return;
    }

    LOG(Info, "reading the script file '%s'", file_name.c_str());
    Maybe<String> contents =
        next_file_index == 1 && prefetched_script_contents.has_value()
            ? steal(prefetched_script_contents)
            : script_path.read_entire_file();
    if (!contents) {
      let const looks_like_command =
          !FLAG_LINT.is_enabled() &&
          !file_name.view().find_character('/').has_value();
      let hint = String{heap_allocator()};
      if (looks_like_command) hint = "Pass -c to run this as a command string";
      let const message = "Could not open '" + file_name.view() +
                          "': " + os::last_system_error_message();
      if (hint.is_empty()) {
        show_message(ErrorWithLocation{operand_location, message}.to_string(
            cli_invocation.view(), &context));
      } else {
        show_message(
            ErrorWithLocationAndDetails{operand_location, message, hint.view()}
                .to_string(cli_invocation.view(), &context));
      }
      if (!FLAG_LINT.is_enabled()) {
        utils::quit(127, utils::farewell_policy::Goodbye);
      }
      chunk.should_analyze = false;
      return;
    }

    chunk.contents = steal(*contents);
    chunk.filename = file_name.view();
    context.source_store().set_script_run(true);
    chunk.root_frame_call_site = operand_location;
    mimic_script_shell(context, chunk);
  }

  fn mimic_script_shell(EvalContext &context,
                        const script_chunk &chunk) const throws -> void
  {
    if (!context.runtime_state().is_mimicry_enabled() ||
        identity.was_mood_named_on_command_line ||
        context.runtime_control_store().was_mood_set_explicitly())
    {
      return;
    }

    let const detected_mood =
        detect_mimic_shell_from_source(chunk.contents.view());
    LOG(Info, "the script operand '%s' %s a shell to mimic",
        String{chunk.filename.value_or(StringView{})}.c_str(),
        detected_mood.has_value() ? "selects" : "does not select");
    context.select_mood(detected_mood.value_or(identity.session_mood));

    if (FLAG_LINT.is_enabled()) {
      context.runtime_state().set_warning_level(
          warning_level_for_mood(context.runtime_state().get_mood()));
    }
  }
};

static fn enter_line_editor(EvalContext &context) throws -> void
{
  toiletline::set_extended_keys(
      context.runtime_state().option_is_enabled(shell_option_id::ExtendedKeys));
  toiletline::enter_raw_mode();
}

static fn start_line_editor(EvalContext &context,
                            const invocation_identity &identity) throws -> void
{
  if (toiletline::is_active()) return;

  LOG(Info, "initializing the line editor");
  toiletline::initialize();
  os::install_fatal_exit_hook(toiletline::restore_terminal_for_exit);
  toiletline::set_history_enabled(false);
  toiletline::enable_job_notifications(context);
  toiletline::set_colors_enabled(colors::stdout_wants_color());
  if (let const welcome = context.get_variable_value("KOSH_WELCOME");
      welcome.has_value())
  {
    if (!welcome->is_empty()) show_message(welcome->view());
  } else {
    show_message(identity.session_mood == mimic_mood::Posix ? "POSIX me harder!"
                 : (identity.session_mood == mimic_mood::Bash ||
                    identity.session_mood == mimic_mood::BashPosix)
                     ? "Bash me harder!"
                     : "Welcome :3");
  }

  toiletline::exit_raw_mode();
}

static fn emit_prompt_line_break() throws -> void
{
  let const dimensions = os::get_terminal_dimensions();
  if (!dimensions.has_value() || dimensions->columns == 0) return;

  String eol_marker{heap_allocator()};
  eol_marker.reserve(dimensions->columns + 12);
  if (colors::stdout_wants_color()) {
    eol_marker += colors::ansi::INVERSE;
    eol_marker += "\\n";
    eol_marker += colors::ansi::RESET;
  } else {
    eol_marker += "\\n";
  }
  constexpr u32 EOL_GLYPH_COLUMN_COUNT = 2;
  for (u32 column = EOL_GLYPH_COLUMN_COUNT; column < dimensions->columns;
       column++)
    eol_marker.push(' ');
  eol_marker.push('\r');
  print(eol_marker);
  flush();
}

static fn configure_line_editor(EvalContext &context) throws -> void
{
  let const is_tab_completion_enabled =
      context.runtime_state().option_is_enabled(shell_option_id::TabCompletion);
  if (is_tab_completion_enabled && !toiletline::is_completion_enabled()) {
    toiletline::enable_completion(context);
  } else if (!is_tab_completion_enabled && toiletline::is_completion_enabled())
  {
    toiletline::disable_completion();
  }

  let const should_highlight =
      is_tab_completion_enabled && context.runtime_state().option_is_enabled(
                                       shell_option_id::SyntaxHighlighting);
  toiletline::set_highlight_enabled(should_highlight);
  toiletline::set_ghost_enabled(should_highlight);
  toiletline::set_edit_mode(
      context.runtime_state().option_is_enabled(shell_option_id::Vi)
          ? toiletline::edit_mode::Vi
          : toiletline::edit_mode::Emacs);
  toiletline::set_tab_selector(context.runtime_state().get_tab_selector());
  let const &state = context.runtime_state();
  let const should_space_after_completion =
      state.option_is_enabled(shell_option_id::SpaceAfterCompletion);
  let const should_space_after_directory =
      state.option_is_enabled(shell_option_id::SpaceAfterDirectoryCompletion);
  toiletline::set_space_after_completion(!should_space_after_completion ? 0
                                         : should_space_after_directory ? 1
                                                                        : 2);
  toiletline::set_history_prefix_search(
      context.runtime_state().option_is_enabled(
          shell_option_id::HistoryPrefixSearch));
  toiletline::set_hint_row(context.runtime_state().option_is_enabled(
                               shell_option_id::InteractiveHints),
                           context.runtime_state().option_is_enabled(
                               shell_option_id::InteractiveDiagnostics));
  toiletline::set_auto_pair(
      context.runtime_state().option_is_enabled(shell_option_id::AutoPair));
  toiletline::set_history_limit(
      context.variable_store().history_limit("KOSH_HISTORY_SIZE", 4096));
}

struct interactive_session
{
  bool did_seed_path_map = false;
  bool did_prompt = false;
  usize ignored_eof_count = 0;
  history_expansion_state expansion_state{};

  fn read_line(EvalContext &context, BumpArena &ast_arena,
               const invocation_identity &identity, const command_line &line,
               i32 exit_code, script_chunk &chunk) throws -> void
  {
    start_line_editor(context, identity);

    context.notify_done_jobs();

    toiletline::set_idle_title();

    run_prompt_command(context, ast_arena);

    prepare_completion(context, line);
    if (did_prompt) toiletline::emit_command_end_mark(context, exit_code);
    did_prompt = true;
    emit_prompt_line_break();
    toiletline::emit_prompt_start_marks(context);

    String prompt = toiletline::build_prompt(context);
    toiletline::append_prompt_end_mark(context, prompt);
    let const right_prompt = toiletline::build_right_prompt(context);
    let transient_prompt = String{heap_allocator()};
    if (context.runtime_state().option_is_enabled(
            shell_option_id::TransientPrompt))
    {
      transient_prompt = toiletline::build_transient_prompt(context);
      toiletline::append_prompt_end_mark(context, transient_prompt);
    }
    enter_line_editor(context);
    configure_line_editor(context);
    read_accepted_line(context, line, prompt, right_prompt, transient_prompt,
                       exit_code, chunk);

    LOG(Info, "accepted an interactive line of %zu bytes",
        chunk.contents.count());
    toiletline::exit_raw_mode();
  }

  fn prepare_completion(EvalContext &context, const command_line &line) throws
      -> void
  {
    let const &state = context.runtime_state();
    let const is_tab_completion_enabled =
        state.option_is_enabled(shell_option_id::TabCompletion);
    if (did_seed_path_map) {
      context.program_resolver().revalidate_for_prompt();
    } else if (!line.is_rescue_mode && is_tab_completion_enabled &&
               state.option_is_enabled(shell_option_id::SyntaxHighlighting))
    {
      context.program_resolver().initialize_path_map();
      did_seed_path_map = true;
    }

    if (!line.is_rescue_mode && is_tab_completion_enabled) {
      try {
        utils::warm_directory_index(Path::current_directory());
      } catch (const Error &) {}
    }
  }

  fn read_accepted_line(EvalContext &context, const command_line &line,
                        const String &prompt, const String &right_prompt,
                        const String &transient_prompt, i32 exit_code,
                        script_chunk &chunk) throws -> void
  {
    loop
    {
      let[code, input, accepted_history_event_number] =
          toiletline::get_input(prompt, right_prompt, transient_prompt);

      switch (code) {
      case TL_PRESSED_TAB: toiletline::set_input(input); continue;
      case TL_PRESSED_EOF:
        if (input.is_empty()) {
          i64 ignored_eof_limit_count = 0;
          if (context.runtime_state().option_is_enabled(
                  shell_option_id::Ignoreeof))
          {
            ignored_eof_limit_count = 10;
            if (let const value = context.get_variable_value("IGNOREEOF");
                value.has_value())
            {
              let const parsed = utils::parse_decimal_i64(value->view());
              if (!parsed.is_error() && parsed.value() >= 0)
                ignored_eof_limit_count = parsed.value();
            }
          }
          if (ignored_eof_count < static_cast<usize>(ignored_eof_limit_count)) {
            ignored_eof_count++;
            toiletline::emit_newlines(input);
            show_message("Use \"exit\" to leave the shell.");
            continue;
          }
          print("^D");
          flush();
          toiletline::emit_newlines(input);
          utils::quit(exit_code, utils::farewell_policy::Goodbye);
        } else {
          toiletline::set_input(input);
          continue;
        }
        break;
      case TL_PRESSED_QUIT:
        toiletline::emit_newlines(input);
        utils::quit(exit_code, utils::farewell_policy::Goodbye);
        break;
      case TL_PRESSED_INTERRUPT:
        print("^C");
        flush();
        break;
      default:;
      }

      if (code != TL_PRESSED_EOF) ignored_eof_count = 0;

      toiletline::emit_newlines(input);

      if (code == TL_PRESSED_ENTER && !input.is_empty()) {
        chunk.contents = steal(input);
        chunk.history_event_number = accepted_history_event_number;
        break;
      }

      prepare_completion(context, line);
    }
  }

  fn prepare_history(EvalContext &context, script_chunk &chunk) throws -> bool
  {
    let const is_interactive = context.execution_store().shell_is_interactive();
    bool should_execute = true;
    if (is_interactive &&
        context.runtime_state().option_is_enabled(
            shell_option_id::Histexpand) &&
        !chunk.contents.is_empty())
    {
      try {
        let expanded = expand_interactive_history(chunk.contents.view(),
                                                  chunk.history_event_number,
                                                  expansion_state, context);
        if (expanded.has_value()) {
          show_message(expanded->command.view());
          chunk.contents = steal(expanded->command);
          should_execute = expanded->should_execute;
        }
      } catch (const Error &error) {
        show_message(error.message().view());
        return false;
      }
    }

    if (is_interactive &&
        context.runtime_state().option_is_enabled(shell_option_id::History) &&
        !chunk.contents.is_empty())
    {
      chunk.history_event_number =
          toiletline::append_history_event(chunk.contents.view());
    }

    return should_execute;
  }
};

struct lint_run
{
  bool did_input_fail = false;
  analysis_diagnostic_totals totals{};

  fn analyze(const script_chunk &chunk, EvalContext &context,
             BumpArena &ast_arena) throws -> i32
  {
    return run_lint_document_contents(chunk.contents, context, ast_arena,
                                      chunk.filename, &totals, nullptr, nullptr,
                                      true, chunk.command_string_name);
  }

  fn record(i32 exit_code, bool should_quit) wontthrow -> i32
  {
    did_input_fail = did_input_fail || exit_code != EXIT_SUCCESS;

    return should_quit && did_input_fail ? EXIT_FAILURE : exit_code;
  }

  fn print_summary(EvalContext &context) throws -> void
  {
    if (context.runtime_state().memory_stats_enabled()) {
      utils::print_memory_report();
      context.runtime_state().set_memory_stats_enabled(false);
    }
    print_analysis_diagnostic_summary(totals);
  }
};

static fn run_chunk(script_chunk &chunk, EvalContext &context,
                    BumpArena &ast_arena, lint_run &lint,
                    root_evaluation_mode evaluation_mode) throws -> i32
{
  if (!chunk.should_analyze) return EXIT_FAILURE;

  chunk.contents.normalize_crlf_line_endings();
  if (FLAG_LINT.is_enabled()) return lint.analyze(chunk, context, ast_arena);

  let run_options = script_run_options{};
  run_options.should_analyze = !chunk.is_fresh_evaluator_command;
  run_options.should_use_command_string_status =
      chunk.should_use_command_string_status;
  run_options.is_whole_line =
      chunk.command_string_name.has_value() || chunk.is_fresh_evaluator_command;
  if (chunk.command_string_name.has_value()) {
    run_options.should_require_shebang = false;

    return run_script_contents(
        chunk.contents, context, ast_arena, chunk.command_string_name, nullptr,
        nullptr, chunk.history_event_number, {}, run_options, evaluation_mode);
  }

  return run_script_contents(chunk.contents, context, ast_arena, chunk.filename,
                             nullptr, nullptr, chunk.history_event_number, {},
                             run_options, evaluation_mode);
}

static fn allow_terminal_exec_on_final_chunk(EvalContext &context,
                                             bool should_quit) wontthrow -> void
{
  let const should_print_post_run_trailer =
      context.runtime_state().show_exit_code() ||
      context.runtime_state().stats_enabled();
  context.execution_store().terminal_exec_allowed() =
      should_quit && !context.execution_store().shell_is_interactive() &&
      !context.has_exit_trap() && !should_print_post_run_trailer;
}

static fn should_exit_after_chunk(EvalContext &context,
                                  const script_cursor &cursor,
                                  i32 exit_code) wontthrow -> bool
{
  return cursor.should_quit ||
         context.runtime_state().option_is_enabled(shell_option_id::Onecmd) ||
         os::is_child_process() ||
         (!FLAG_LINT.is_enabled() && FLAG_ERROR_EXIT.is_enabled() &&
          exit_code != 0);
}

wontreturn static fn exit_after_final_chunk(EvalContext &context, i32 exit_code,
                                            lint_run &lint) throws -> void
{
#if !defined NDEBUG
  if (FLAG_DEBUG_COMPLETE_AT.is_set() && !os::is_child_process()) {
    exit_code =
        run_debug_completion_driver(FLAG_DEBUG_COMPLETE_AT.value(), context);
  }
  if (FLAG_DEBUG_HIGHLIGHT_AT.is_set() && !os::is_child_process()) {
    exit_code =
        run_debug_highlight_driver(FLAG_DEBUG_HIGHLIGHT_AT.value(), context);
  }
  if (FLAG_DEBUG_GHOST_AT.is_set() && !os::is_child_process()) {
    exit_code = run_debug_ghost_driver(FLAG_DEBUG_GHOST_AT.value(), context);
  }
  if (FLAG_DEBUG_BRACKETS_AT.is_set() && !os::is_child_process()) {
    exit_code =
        run_debug_bracket_driver(FLAG_DEBUG_BRACKETS_AT.value(), context);
  }
  if (FLAG_DEBUG_HINT_AT.is_set() && !os::is_child_process()) {
    exit_code = run_debug_hint_driver(FLAG_DEBUG_HINT_AT.value(), context);
  }
#endif
  LOG(Info, "exiting after the final chunk with code %d", exit_code);
  if (!os::is_child_process()) context.run_exit_trap(exit_code);
  if (FLAG_LINT.is_enabled()) lint.print_summary(context);
  utils::quit(exit_code, FLAG_ERROR_EXIT.is_enabled()
                             ? utils::farewell_policy::Goodbye
                             : utils::farewell_policy::Silent);
}

} /* namespace koshka */

fn kosh_main(int argc, char **argv) -> int
{
  koshka::os::initialize_platform_runtime();
  koshka::os::register_platform_flags(FLAG_LIST);

  if (argc > 0) {
    koshka::StringView invocation =
        koshka::Path::invocation_filename(koshka::StringView{argv[0]}, true);
    let invocation_name = koshka::String{invocation};
    let const invocation_info =
        koshka::os::normalize_program_name(invocation_name);
    invocation =
        invocation_name.substring_of_length(0, invocation_info.stem_length);
    let const is_koshkit_invocation = invocation == "koshkit";
    int first_operand_index = 1;
    if (is_koshkit_invocation && argc > 1) {
      invocation = koshka::StringView{argv[1]};
      first_operand_index = 2;
    }

    let const chosen_utility = koshka::koshkit::find_util(invocation);
    if (is_koshkit_invocation && !chosen_utility.has_value()) {
      invocation = koshka::StringView{"koshkit"};
      first_operand_index = 1;
    }

    if (chosen_utility.has_value() || is_koshkit_invocation) {
      if (koshka::os::is_running_setuid() &&
          !koshka::os::drop_elevated_identity())
      {
        koshka::show_message("Unable to drop elevated ids: " +
                             koshka::os::last_system_error_message());
        return 1;
      }
      LOG(Info, "acting as the koshkit utility '%.*s' from argv[0]",
          static_cast<int>(invocation.length), invocation.data);
      koshka::os::set_default_signal_handlers(
          koshka::os::signal_profile::NonInteractive);
      let ast_arena = koshka::BumpArena{};
      let function_arena = koshka::BumpArena{};

      let context = koshka::EvalContext{koshka::startup_options{},
                                        koshka::String{invocation}};
      context.arena_store().set_parse_arena(&ast_arena);
      context.arena_store().set_function_arena(&function_arena);

      koshka::ArrayList<koshka::String> operands{koshka::heap_allocator()};
      operands.reserve(static_cast<usize>(argc - first_operand_index));
      for (int i = first_operand_index; i < argc; i++)
        operands.push(koshka::String{koshka::StringView{argv[i]}});

      return static_cast<int>(koshka::koshkit::run_as_multicall(
          invocation, chosen_utility, steal(operands), context));
    }
  }

  let line = koshka::command_line{argc, argv};
  if (koshka::Maybe<int> usage_status = koshka::parse_command_line(line);
      usage_status.has_value())
  {
    return *usage_status;
  }
  let invocation_options =
      koshka::ArrayList<koshka::invocation_option>{koshka::heap_allocator()};
  if (koshka::Maybe<int> usage_status =
          koshka::resolve_invocation_options(invocation_options);
      usage_status.has_value())
  {
    return *usage_status;
  }
  let &file_names = line.operands;
  let const parse_argc = line.get_parse_argc();
  let const parse_argv = line.get_parse_argv();

  let const has_elevated_identity = koshka::os::is_running_setuid();
  if (has_elevated_identity && !FLAG_PRIVILEGED.is_enabled() &&
      !koshka::os::drop_elevated_identity())
  {
    koshka::show_message("Unable to drop elevated ids: " +
                         koshka::os::last_system_error_message());
    return 1;
  }

  if (FLAG_DUMB.is_enabled()) {
    if (!FLAG_NO_COMPLETION.is_enabled()) FLAG_NO_COMPLETION.toggle();
    if (!FLAG_SUPPRESS_DIAGNOSTICS.is_enabled())
      FLAG_SUPPRESS_DIAGNOSTICS.toggle();
    koshka::os::set_environment_variable("NO_COLOR", "1");
  }

  if (FLAG_CLEAN.is_enabled()) {
    koshka::os::set_environment_variable("PATH", "/usr/bin:/bin");
  }

#if !defined NDEBUG
  if (FLAG_LOG.is_set()) {
    struct log_level_name
    {
      const char *name;
      koshka::verbosity level;
    };
    static const log_level_name LOG_LEVEL_NAMES[] = {
        {"info",  koshka::verbosity::Info },
        {"debug", koshka::verbosity::Debug},
        {"all",   koshka::verbosity::All  },
    };
    let is_known_level = false;
    for (let const &entry : LOG_LEVEL_NAMES)
      if (FLAG_LOG.value() == entry.name) {
        koshka::LOGGER_VERBOSITY = entry.level;
        is_known_level = true;
        break;
      }
    if (!is_known_level) {
      koshka::show_message(
          koshka::ErrorWithDetails{"Unknown debug logging level '" +
                                       koshka::String{FLAG_LOG.value()} + "'",
                                   "Pass `info`, `debug`, or `all` to `-X`"}
              .to_string());
      return 2;
    }
  }

  if (FLAG_DEBUG_OUTPUT_FILE.is_set() &&
      !FLAG_DEBUG_OUTPUT_FILE.value().is_empty())
  {
    let const log_file_name = koshka::String{FLAG_DEBUG_OUTPUT_FILE.value()};
    if (std::FILE *log_file = std::fopen(log_file_name.c_str(), "a");
        log_file != nullptr)
    {
      koshka::LOGGER_OUTPUT = log_file;
    }
  }
#endif

  let program_path = koshka::String{koshka::heap_allocator()};

  if (file_names.count() > 0) {
    program_path = steal(file_names[0]);
    file_names.remove(0);
  } else {
    program_path = "<unknown>";
  }

  try {
    if (koshka::Maybe<int> code =
            koshka::print_help_or_version_status(program_path))
      return *code;
  } catch (const koshka::Error &error) {
    koshka::show_message(error.to_string());
    return 1;
  }

  let identity = koshka::make_invocation_identity(steal(program_path));

  let init_moods =
      koshka::ArrayList<koshka::mimic_mood>{koshka::heap_allocator()};
  if (koshka::Maybe<int> usage_status =
          koshka::validate_invocation(file_names, init_moods);
      usage_status.has_value())
  {
    return *usage_status;
  }
  let const is_language_server = FLAG_LANGUAGE_SERVER.is_enabled();

  LOG(Info, "privileged mode is %s",
      FLAG_PRIVILEGED.is_enabled() || has_elevated_identity ? "on" : "off");

  let inherited = koshka::take_inherited_shell();
  if (inherited.has_invalid_state) return 1;

  let configuration = koshka::read_startup_configuration(
      line, inherited, identity, has_elevated_identity);
  koshka::apply_configured_mood(identity, configuration);

  let const input = koshka::resolve_input_plan(file_names);
  koshka::apply_configured_init_moods(init_moods, configuration,
                                      input.should_be_interactive ||
                                          identity.is_login_shell);
  let prefetched_script_contents =
      koshka::prefetch_script_shebang(identity, input, file_names);
  let operands = koshka::take_script_operands(steal(identity.program_path),
                                              file_names, input);

  let context = koshka::EvalContext{koshka::make_startup_options(input),
                                    steal(operands.shell_name),
                                    steal(operands.positional_params)};

  koshka::utils::set_quit_context(&context);

  let cli_invocation = koshka::String{koshka::heap_allocator()};
  if (input.should_execute_commands || input.should_read_files)
    cli_invocation = koshka::join_command_line(parse_argc, parse_argv);

  koshka::apply_session_config(context,
                               koshka::read_session_config(identity, input));
  if (FLAG_INIT_MOODS.count() != 0) {
    context.startup_store().set_init_moods(
        koshka::describe_init_moods(init_moods).view());
  }
  koshka::seed_session_variables(context, identity, init_moods, inherited,
                                 input.should_be_interactive);
  if (koshka::Maybe<int> option_status = koshka::apply_startup_configuration(
          context, configuration, invocation_options);
      option_status.has_value())
  {
    return *option_status;
  }

  koshka::os::set_default_signal_handlers(
      input.should_be_interactive ? koshka::os::signal_profile::Interactive
                                  : koshka::os::signal_profile::NonInteractive);
  LOG(Info, "installed the default signal handlers");

  let ast_arena = koshka::BumpArena{};

  let function_arena = koshka::BumpArena{};
  context.arena_store().set_parse_arena(&ast_arena);
  context.arena_store().set_function_arena(&function_arena);

  if (is_language_server)
    return koshka::language_server::run(context, ast_arena);
  if (FLAG_LINT.is_enabled() && FLAG_APPLY.is_enabled())
    return koshka::run_lint_apply_operation(
        file_names, FLAG_FORMAT.is_enabled(), context, ast_arena);
  if (FLAG_FORMAT.is_enabled()) {
    try {
      return koshka::run_format_operation(
          file_names, FLAG_APPLY.is_enabled(), FLAG_LINT.is_enabled(),
          ast_arena, context, identity.session_mood);
    } catch (const koshka::Error &error) {
      koshka::show_message(error.to_string());
      return 1;
    }
  }

  koshka::run_startup(context, init_moods, identity, line, inherited,
                      has_elevated_identity, input.should_be_interactive);
  if (koshka::Maybe<int> startup_status = koshka::finish_startup(
          context, identity, inherited, input.should_be_interactive);
      startup_status.has_value())
  {
    return *startup_status;
  }

  let cursor = koshka::script_cursor{line,
                                     file_names,
                                     input,
                                     identity,
                                     cli_invocation,
                                     prefetched_script_contents,
                                     FLAG_ONE_COMMAND.is_enabled() &&
                                         !FLAG_LINT.is_enabled()};
  let session = koshka::interactive_session{};
  let lint = koshka::lint_run{};
  i32 exit_code = EXIT_SUCCESS;

  loop
  {
    ASSERT(!koshka::os::can_fork_evaluator() ||
           !koshka::os::is_child_process());

    let chunk = koshka::script_chunk{};
    bool did_register_origin = false;
    defer
    {
      if (did_register_origin) context.unregister_embedded_source();
    };

    try {
      if (input.should_read_stdin) {
        cursor.read_standard_input(chunk);
      } else if (input.should_execute_commands && !FLAG_COMMAND.at_end()) {
        cursor.read_next_command(context, chunk, inherited.state.has_value());
        chunk.is_fresh_evaluator_command = inherited.state.has_value();
        chunk.should_use_command_string_status =
            !inherited.state.has_value() ||
            inherited.should_use_command_string_status;
        did_register_origin = context.register_inherited_source_origin(
            inherited.source_origin.view(), chunk.contents,
            inherited.source_windows, chunk.command_string_name);
        inherited.source_origin.clear();
      } else if (input.should_read_files) {
        cursor.read_next_file(context, chunk);
      } else if (input.should_be_interactive) {
        session.read_line(context, ast_arena, identity, line, exit_code, chunk);
      } else {
        unreachable("the input loop has no configured input source");
      }
    } catch (const koshka::Error &e) {
      koshka::show_message(e.to_string());
      koshka::utils::quit(EXIT_FAILURE);
    } catch (const std::exception &e) {
      koshka::show_message(
          "Uncaught exception while getting the input. Exiting.");
      koshka::show_message("Context: '" + koshka::String{e.what()} + "'.");
      koshka::utils::quit(EXIT_FAILURE);
    } catch (...) {
      koshka::show_message(
          "Unexpected system explosion while getting the input. Exiting.");
      koshka::show_message("Last system message: " +
                           koshka::os::last_system_error_message());
      koshka::utils::quit(EXIT_FAILURE);
    }

    if (!session.prepare_history(context, chunk)) continue;

    koshka::os::INTERRUPT_REQUESTED = 0;

    koshka::allow_terminal_exec_on_final_chunk(context, cursor.should_quit);

    if (context.execution_store().shell_is_interactive() &&
        !chunk.contents.is_empty())
    {
      koshka::String ps0 = toiletline::render_ps0(context);
      if (!ps0.is_empty()) {
        koshka::print(ps0);
        koshka::flush();
      }
      toiletline::emit_command_start_marks(context, chunk.contents.view());
    }

    let const has_multiple_root_sources =
        input.should_execute_commands
            ? FLAG_COMMAND.count() > 1
            : FLAG_LINT.is_enabled() && file_names.count() > 1;
    let const should_push_root_frame =
        chunk.root_frame_call_site.has_value() && has_multiple_root_sources &&
        !inherited.should_suppress_root_source_trace;
    if (should_push_root_frame) {
      context.push_root_source_frame(&cli_invocation,
                                     *chunk.root_frame_call_site,
                                     koshka::source_frame_kind::CliRoot);
    }
    defer
    {
      if (should_push_root_frame) context.pop_root_source_frame();
    };

    exit_code = koshka::run_chunk(chunk, context, ast_arena, lint,
                                  inherited.take_evaluation_mode());
    if (FLAG_LINT.is_enabled())
      exit_code = lint.record(exit_code, cursor.should_quit);

    if (koshka::should_exit_after_chunk(context, cursor, exit_code)) {
      koshka::exit_after_final_chunk(context, exit_code, lint);
    }
  }

  unreachable("the main command loop terminated without exiting");
}
