#include "Arena.hpp"
#include "Cli.hpp"
#include "Colors.hpp"
#include "Common.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Diagnostics.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Formatter.hpp"
#include "Koshkit.hpp"
#include "LanguageServer.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

FLAG_LIST_DECL();

/* clang-format off */
HELP_SYNOPSIS_DECL("[-OPTIONS] [--] <file> [argument ...]",
                   "[-OPTIONS] -c <script1> [-c <script2> ...] [argument ...]",
                   "[-OPTIONS] (--lint [--format] | --format) [--apply] [file ...]",
                   "[-OPTIONS] --language-server");
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
FLAG(CLEAN, Bool, '\0', "clean", Kosh,
     "Start clean, reading no startup file and setting a minimal PATH.");
FLAG(POSIX_COMPAT, Bool, '\0', "posix", Bash,
     "Run in bash POSIX mode, equivalent to --mood bash-posix.");

FLAG(MOOD, String, 'M', "mood", Compat,
     "Select the runtime mood, 'kosh' is strict with the analysis stage on, "
     "'bash' runs the extensions with it off, 'sh' behaves like dash, and "
     "'bash-posix' is bash with the posix identity reached by --posix.");
FLAG(INIT_MOODS, ManyStrings, 'L', "init-moods", Compat,
     "Source the startup files for each listed mood, in order, comma separated "
     "or by repeating the flag. Defaults to --mood.");
FLAG(MIMICRY, Bool, 'I', "mimicry", Compat,
     "Mimic the shell a script's shebang names, running a known shell shebang "
     "in-process in the matching mode.");
FLAG(DUMB, Bool, '\0', "dumb", Compat,
     "Make the shell extremely dumb. Equivalent to --mood sh -T "
     "--no-diagnostics.");

FLAG(LINT, Bool, '\0', "lint", Auxiliary,
     "Analyze shell inputs without running them and enable every diagnostic "
     "tier at its normal severity.");
FLAG(FORMAT, Bool, '\0', "format", Auxiliary,
     "Format shell input without running it. Read one file or standard input.");
FLAG(APPLY, Bool, '\0', "apply", Auxiliary,
     "Apply lint fixes or formatted output to named files.");
FLAG(LANGUAGE_SERVER, Bool, '\0', "language-server", Auxiliary,
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
     "Disable interactive tab completion and ghost-text.");
FLAG(NO_SYNTAX_HIGHLIGHTING, Bool, '\0', "no-syntax-highlighting", Kosh,
     "Disable the syntax coloring and the ghost suggestion, leaving tab "
     "completion working.");
FLAG(ENABLE_KOSHKIT, Bool, '\0', "enable-koshkit", Kosh,
     "Resolve the bundled koshkit utility names such as ls and mkdir directly "
     "as commands, the same as set -o koshkit.");

FLAG(AST, Bool, 'A', "show-ast", Debug,
     "Print AST before executing each command.");
FLAG(OPTIMIZER_DIAGNOSTICS, Bool, '\0', "optimizer-diagnostics", Debug,
     "Trace the optimizer prepass and report every folded and eliminated node "
     "as an analysis diagnostic.");
FLAG(EXIT_CODE, Bool, 'E', "show-exit-code", Debug,
     "Print exit code after each executed command.");
FLAG(ESCAPE_MAP, Bool, 'R', "show-lexed-words", Debug,
     "Print escape bitmap after each parsed command.");
FLAG(
    STATS, Bool, '\0', "show-stats", Debug,
    "Print run statistics after each command, commands, expansions, nodes, and "
    "arena bytes.");
FLAG(MEMORY, Bool, '\0', "show-memory", Debug,
     "Print a memory report at exit, the arena bytes and the heap in use.");
/* A release binary rejects these flags as unknown, since its LOG calls compile
   out. */
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
#endif

#include "MainOperations.hpp"

fn main(int argc, char **argv) -> int
{
  koshka::os::initialize_platform_runtime();
  koshka::os::register_platform_flags(FLAG_LIST);

  /* A symlink or rename to a koshkit utility name runs that utility directly,
     before any flag parsing, so `ls -l` reaches ls and its own flag parser. */
  if (argc > 0) {
    koshka::StringView invocation = koshka::StringView{argv[0]};
    usize basename_start = 0;
    for (usize i = 0; i < invocation.length; i++)
      if (koshka::os::is_directory_separator(invocation[i]))
        basename_start = i + 1;
    invocation = invocation.substring(basename_start);
    if (!invocation.is_empty() && invocation[0] == '-') {
      invocation = invocation.substring(1);
    }
    let invocation_name = koshka::String{invocation};
    let const invocation_info =
        koshka::os::normalize_program_name(invocation_name);
    invocation =
        invocation_name.substring_of_length(0, invocation_info.stem_length);

    if (koshka::koshkit::find_util(invocation).has_value()) {
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
      koshka::AST_ARENA = &ast_arena;
      let function_arena = koshka::BumpArena{};
      koshka::FUNCTION_ARENA = &function_arena;

      let context = koshka::EvalContext{
          false, false, false, false, false, koshka::String{invocation}};

      koshka::ArrayList<koshka::String> operands{koshka::heap_allocator()};
      operands.reserve(static_cast<usize>(argc - 1));
      for (int i = 1; i < argc; i++)
        operands.push(koshka::String{koshka::StringView{argv[i]}});

      return static_cast<int>(koshka::koshkit::run_as_multicall(
          invocation, steal(operands), context));
    }
  }

  bool is_login_shell = false;
  let file_names = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};

  /* KOSH_FLAGS supplies options through the environment. The whitespace-split
     tokens are spliced in right after the program name, so a command-line flag
     still has the final say. The token strings and the spliced pointer array
     outlive the parse below. */
  koshka::ArrayList<koshka::String> kosh_flags_tokens{koshka::heap_allocator()};
  koshka::ArrayList<const char *> spliced_argv{koshka::heap_allocator()};
  if (koshka::Maybe<koshka::String> kosh_flags =
          koshka::os::get_environment_variable("KOSH_FLAGS");
      kosh_flags.has_value() && !kosh_flags->is_empty())
  {
    static constexpr koshka::PackedStringKey IGNORED_KOSH_FLAG_KEYS[] = {
        SSK("--apply"), SSK("--format"), SSK("--language-server")};
    static constexpr koshka::StaticStringSet IGNORED_KOSH_FLAGS{
        IGNORED_KOSH_FLAG_KEYS};
    let const view = kosh_flags->view();
    /* A -c in KOSH_FLAGS is dropped with the command word after it, since the
       variable must not splice a command into every invocation. */
    bool should_skip_next_command_word = false;

    view.for_each_ascii_whitespace_word([&](koshka::StringView token) throws {
      if (should_skip_next_command_word) {
        should_skip_next_command_word = false;
      } else if (token == "-c") {
        should_skip_next_command_word = true;
      } else if (!IGNORED_KOSH_FLAGS.contains(token)) {
        kosh_flags_tokens.push(koshka::String{token});
      }
    });
  }

  if (!kosh_flags_tokens.is_empty() && argc > 0) {
    spliced_argv.push(argv[0]);
    for (let const &token : kosh_flags_tokens)
      spliced_argv.push(token.c_str());
    for (int i = 1; i < argc; i++)
      spliced_argv.push(argv[i]);
  }

  const char *const *parse_argv =
      spliced_argv.is_empty() ? argv : spliced_argv.begin();
  let const parse_argc =
      spliced_argv.is_empty() ? argc : static_cast<int>(spliced_argv.count());

  /* A login shell that launches with a broken flag config drops to a rescue
     prompt rather than exiting and locking the user out. The lockout-risk case
     is marked by a dash-prefixed argv[0], a bare - or -bash, so rescue is
     offered only there and any other invocation keeps the usage exit. */
  let const invocation_path =
      argc > 0 ? koshka::Path{koshka::StringView{argv[0]}} : koshka::Path{};
  let const invocation_name = invocation_path.filename();
  const bool is_login_invocation =
      !invocation_name.is_empty() && invocation_name[0] == '-';

  bool is_rescue_mode = false;
  let const do_enter_rescue = [&]() {
    koshka::show_message("Entering rescue.");
    is_rescue_mode = true;
    koshka::reset_flags(FLAG_LIST);
    try {
      file_names = koshka::parse_flags(FLAG_LIST, argc, argv, 0, &FLAG_COMMAND);
    } catch (...) {
      /* The real argv carried the bad flag too, so even the clean reparse
         fails. The program name is kept as the sole operand so $0 and SHELL
         stay the real name. */
      koshka::reset_flags(FLAG_LIST);
      file_names = koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
      if (argc > 0) file_names.push(koshka::String{argv[0]});
    }
  };

  try {
    file_names = koshka::parse_flags(FLAG_LIST, parse_argc, parse_argv, 0,
                                     &FLAG_COMMAND);
  } catch (const koshka::ErrorWithLocation &e) {
    koshka::show_message(
        e.to_string(koshka::join_command_line(parse_argc, parse_argv)));
    if (!is_login_invocation) {
      return 2;
    }
    do_enter_rescue();
  } catch (const koshka::Error &e) {
    koshka::show_message(e.to_string());
    if (!is_login_invocation) {
      return 2;
    }
    do_enter_rescue();
  }

  let const has_elevated_identity = koshka::os::is_running_setuid();
  if (has_elevated_identity && !FLAG_PRIVILEGED.is_enabled() &&
      !koshka::os::drop_elevated_identity())
  {
    koshka::show_message("Unable to drop elevated ids: " +
                         koshka::os::last_system_error_message());
    return 1;
  }

  /* --dumb enables -T and --no-diagnostics and turns color off. The sh mood is
     selected by resolve_session_mood. */
  if (FLAG_DUMB.is_enabled()) {
    if (!FLAG_NO_COMPLETION.is_enabled()) FLAG_NO_COMPLETION.toggle();
    if (!FLAG_SUPPRESS_DIAGNOSTICS.is_enabled())
      FLAG_SUPPRESS_DIAGNOSTICS.toggle();
    koshka::os::set_environment_variable("NO_COLOR", "1");
  }

  /* --clean resets PATH to a minimal default before the context seeds its
     variables from the environment. */
  if (FLAG_CLEAN.is_enabled()) {
    koshka::os::set_environment_variable("PATH", "/usr/bin:/bin");
  }

  /* Raise the runtime log level before any helper runs, so the trace covers
     startup. */
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

  /* The sink opens in append mode. A file that cannot open leaves it on
     stderr. */
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

  /* A basename of sh or dash selects POSIX mode and a basename of bash selects
     bash mode, so a symlink named after a system shell behaves like it. */
  usize program_basename_start = 0;
  for (usize i = 0; i < program_path.length(); i++)
    if (koshka::os::is_directory_separator(program_path[i]))
      program_basename_start = i + 1;
  let normalized_program_basename =
      koshka::String{program_path.substring(program_basename_start)};
  let const program_name_info =
      koshka::os::normalize_program_name(normalized_program_basename);
  koshka::StringView program_basename =
      normalized_program_basename.substring_of_length(
          0, program_name_info.stem_length);
  /* A login shell receives argv[0] prefixed with a dash, such as -bash, and
     exec -l prepends the dash to the whole path, such as -/usr/bin/bash. The
     mark is the first byte of argv[0], not of the basename, so a path whose
     directory component contains a dash is not mistaken for a login shell. */
  const bool is_login_name =
      !program_path.view().is_empty() && program_path.view()[0] == '-';

  /* SHELL and BASH must name a runnable file a child can exec, so the login
     dash is dropped here for the executable identity while $0 keeps the dashed
     spelling below. A bare dash keeps its spelling since it names nothing to
     run. */
  let executable_path = program_path.clone();
  if (is_login_name && program_path.view().length > 1)
    executable_path = koshka::String{program_path.view().substring(1)};

  if (is_login_name && !program_basename.is_empty() &&
      program_basename[0] == '-')
  {
    program_basename = program_basename.substring(1);
  }

  const koshka::mimic_mood invocation_mood =
      (program_basename == "sh" || program_basename == "dash")
          ? koshka::mimic_mood::Posix
      : program_basename == "bash" || program_basename == "rbash"
          ? koshka::mimic_mood::Bash
          : koshka::mimic_mood::Default;
  let const is_restricted_shell =
      FLAG_RESTRICTED.is_enabled() || program_basename == "rbash";
  LOG(Info, "invocation basename is '%.*s'",
      static_cast<int>(program_basename.length), program_basename.data);
  let const session_mood = koshka::resolve_session_mood(invocation_mood);
  LOG(Info, "selecting the %s mood",
      session_mood == koshka::mimic_mood::Posix       ? "posix"
      : session_mood == koshka::mimic_mood::Bash      ? "bash"
      : session_mood == koshka::mimic_mood::BashPosix ? "bash-posix"
                                                      : "default");

  if (koshka::Maybe<int> code =
          koshka::print_help_or_version_status(program_path))
    return *code;

  /* A dash-prefixed invocation name, -bash or a bare -, is the login spawn
     convention, the same mark -l sets. */
  if (FLAG_LOGIN.is_enabled() || is_login_name) {
    is_login_shell = true;
  }
  LOG(Info, "the shell %s a login shell", is_login_shell ? "is" : "is not");

  if (FLAG_MOOD.is_set() && !koshka::parse_mood_name(FLAG_MOOD.value())) {
    koshka::String source = "--mood ";
    let const value_position = source.count();
    source += FLAG_MOOD.value();
    koshka::show_message(koshka::ErrorWithLocation{
        koshka::SourceLocation{value_position, FLAG_MOOD.value().length},
        "Unknown --mood value, expected one of 'kosh', 'bash', 'sh', or "
        "'bash-posix'"
    }
                             .to_string(source.view()));
    return 2;
  }

  let const is_language_server = FLAG_LANGUAGE_SERVER.is_enabled();
  if (is_language_server &&
      (FLAG_STDIN.is_enabled() || FLAG_INTERACTIVE.is_enabled() ||
       FLAG_LINT.is_enabled() || FLAG_FORMAT.is_enabled() ||
       FLAG_APPLY.is_enabled() || !FLAG_COMMAND.is_empty() ||
       !file_names.is_empty()))
  {
    koshka::show_message(
        "The '--language-server' option does not accept '-s', '-i', "
        "'--lint', '--format', '--apply', '-c', or file operands.");
    return 2;
  }
  if (FLAG_APPLY.is_enabled() && !FLAG_LINT.is_enabled() &&
      !FLAG_FORMAT.is_enabled())
  {
    koshka::show_message(
        "The '--apply' option requires '--lint' or '--format'.");
    return 2;
  }
  if (FLAG_APPLY.is_enabled() &&
      (FLAG_STDIN.is_enabled() || FLAG_INTERACTIVE.is_enabled() ||
       !FLAG_COMMAND.is_empty()))
  {
    koshka::show_message(
        "The '--apply' option does not accept '-s', '-i', or '-c'.");
    return 2;
  }
  if (FLAG_APPLY.is_enabled()) {
    if (file_names.is_empty()) {
      koshka::show_message("The '--apply' option requires named files.");
      return 2;
    }
    for (let const &file_name : file_names) {
      if (file_name != "-") continue;
      koshka::show_message("The '--apply' option does not accept '-'.");
      return 2;
    }
  }
  if (FLAG_FORMAT.is_enabled() && !FLAG_APPLY.is_enabled() &&
      file_names.count() > 1)
  {
    koshka::show_message(
        "The '--format' option accepts one file without '--apply'.");
    return 2;
  }

  let init_moods =
      koshka::ArrayList<koshka::mimic_mood>{koshka::heap_allocator()};
  for (usize i = 0; i < FLAG_INIT_MOODS.count(); i++) {
    koshka::StringView entry = FLAG_INIT_MOODS.get(i);
    /* A single --init-moods value may itself be comma-separated. */
    usize name_start = 0;
    for (usize j = 0; j <= entry.length; j++) {
      if (j != entry.length && entry[j] != ',') {
        continue;
      }
      koshka::StringView name =
          entry.substring_of_length(name_start, j - name_start);
      name_start = j + 1;
      if (name.is_empty()) continue;
      koshka::Maybe<koshka::mimic_mood> parsed_mood =
          koshka::parse_mood_name(name);
      if (!parsed_mood.has_value()) {
        koshka::String source = "--init-moods ";
        let const value_position = source.count();
        source += name;
        koshka::show_message(koshka::ErrorWithLocation{
            koshka::SourceLocation{value_position, name.length},
            "Unknown --init-moods value, expected one of 'kosh', 'bash', "
            "or 'sh'"
        }
                                 .to_string(source.view()));
        return 2;
      }
      init_moods.push(*parsed_mood);
    }
  }

  if (init_moods.is_empty()) init_moods.push(session_mood);

  /* A shell with unequal ids skips config controlled by the real user. */
  let const is_privileged =
      FLAG_PRIVILEGED.is_enabled() || has_elevated_identity;
  LOG(Info, "privileged mode is %s", is_privileged ? "on" : "off");
  unused(is_privileged);

  if (FLAG_STDIN.is_enabled() && FLAG_INTERACTIVE.is_enabled()) {
    let const should_use_interactive =
        !FLAG_LINT.is_enabled() && koshka::os::is_stdin_a_tty();

    let s = koshka::String{koshka::heap_allocator()};
    s += "Both '-s' and '-i' options were specified. Falling back to ";
    if (should_use_interactive)
      s += "'-i'";
    else if (FLAG_LINT.is_enabled())
      s += "'-s'.";
    else
      s += "'-s' because stdin is not a tty.";
    koshka::show_message(s);

    if (should_use_interactive)
      FLAG_STDIN.toggle();
    else
      FLAG_INTERACTIVE.toggle();
  }

  bool should_read_stdin = false, should_execute_commands = false,
       should_read_files = false, should_be_interactive = false;

  /* The input source is chosen by flag precedence, -s first, then -c, then a
     file operand, then -i or no arguments. */
  if (is_language_server || FLAG_FORMAT.is_enabled()) {
    should_read_stdin = false;
  } else if (FLAG_STDIN.is_enabled()) {
    if (!FLAG_COMMAND.is_empty() || FLAG_INTERACTIVE.is_enabled()) {
      koshka::show_message(
          "Incompatible options or arguments were specified along "
          "with '-s' option. "
          "Falling back to '-s'.");
    }
    if (FLAG_LINT.is_enabled() && !file_names.is_empty()) {
      koshka::show_message(
          "The '-s' option was given along with file operands, "
          "so '--lint' reads standard input and analyzes no "
          "named file.");
    }
    should_read_stdin = true;
  } else if (FLAG_LINT.is_enabled() &&
             (!FLAG_COMMAND.is_empty() || !file_names.is_empty()))
  {
    should_execute_commands = !FLAG_COMMAND.is_empty();
    should_read_files = !file_names.is_empty();
  } else if (FLAG_LINT.is_enabled()) {
    should_read_stdin = true;
  } else if (!FLAG_COMMAND.is_empty()) {
    if (FLAG_INTERACTIVE.is_enabled()) {
      koshka::show_message(
          "Incompatible options or arguments were specified along "
          "with '-c' options. "
          "Falling back to '-c'.");
    }
    should_execute_commands = true;
  } else if (!file_names.is_empty()) {
    if (FLAG_INTERACTIVE.is_enabled()) {
      koshka::show_message("Both file argument and '-i' option were given. "
                           "Falling back to reading files.");
    }
    should_read_files = true;
  } else if (FLAG_INTERACTIVE.is_enabled() || koshka::os::is_stdin_a_tty()) {
    should_be_interactive = true;
  } else {
    should_read_stdin = true;
  }
#if !defined NDEBUG
  if (FLAG_DEBUG_COMPLETE_AT.is_set() || FLAG_DEBUG_HIGHLIGHT_AT.is_set() ||
      FLAG_DEBUG_GHOST_AT.is_set())
  {
    should_be_interactive = false;
    should_read_files = false;
    if (!should_execute_commands) should_read_stdin = true;
  }
#endif
  LOG(Info, "the input source is %s",
      should_read_stdin         ? "standard input"
      : should_execute_commands ? "the -c command strings"
      : should_read_files       ? "the named script file"
                                : "the interactive prompt");

  /* A script file or a -c run takes its first operand as $0 and the rest as the
     arguments, while an interactive or -s shell keeps the shell name as $0 and
     takes every operand as a positional parameter. */
  let shell_name = program_path.clone();
  let positional_params =
      koshka::ArrayList<koshka::String>{koshka::heap_allocator()};

  usize first_param_index = 0;
  if (FLAG_LINT.is_enabled() && should_read_files) {
    first_param_index = file_names.count();
  } else if ((should_read_files || should_execute_commands) &&
             !file_names.is_empty())
  {
    shell_name = file_names[0].clone();
    first_param_index = 1;
  }

  positional_params.reserve(file_names.count() - first_param_index);
  for (usize i = first_param_index; i < file_names.count(); i++)
    positional_params.push(koshka::String{
        koshka::heap_allocator(),
        koshka::StringView{file_names[i].data(), file_names[i].count()}
    });

  koshka::os::unset_environment_variable("KOSH_IDENTITY");
  let const should_suppress_root_source_trace =
      koshka::os::get_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE")
          .has_value();
  koshka::os::unset_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE");

  let context = koshka::EvalContext{FLAG_DISABLE_EXPANSION.is_enabled(),
                                    FLAG_VERBOSE.is_enabled(),
                                    FLAG_EXPAND_VERBOSE.is_enabled(),
                                    should_be_interactive,
                                    FLAG_ERROR_EXIT.is_enabled(),
                                    shell_name.clone(),
                                    steal(positional_params)};

  koshka::utils::set_quit_context(&context);

  context.set_cli_invocation(koshka::join_command_line(parse_argc, parse_argv));

  context.set_stats_enabled(FLAG_STATS.is_enabled());
  context.set_show_ast(FLAG_AST.is_enabled());
  context.set_show_lexed_words(FLAG_ESCAPE_MAP.is_enabled());
  context.set_show_exit_code(FLAG_EXIT_CODE.is_enabled());
  context.set_memory_stats_enabled(FLAG_MEMORY.is_enabled());
  context.set_diagnostics_disabled(FLAG_SUPPRESS_DIAGNOSTICS.is_enabled() &&
                                   !FLAG_LINT.is_enabled());
  context.set_annoying_diagnostics_enabled(
      FLAG_LINT.is_enabled() ||
      !FLAG_SUPPRESS_ANNOYING_DIAGNOSTICS.is_enabled());
  context.set_source_traces_enabled(!FLAG_NO_TRACES.is_enabled());
  context.set_shell_option_state(koshka::shell_option_id::Privileged,
                                 FLAG_PRIVILEGED.is_enabled());
  context.set_login_shell(is_login_shell);
  context.set_custom_rcfile(koshka::selected_rcfile().has_value());
  if (is_restricted_shell) context.request_restricted_shell();
  /* The startup files source with strictness off, since they read unset
     variables such as $BASH_VERSION on the /etc/profile path. The session
     strictness is applied at the seam below once the config has loaded. */
  context.set_mood(session_mood);
  /* The CLI -u is the user's own ask, so the -W downgrade leaves it fatal and
     the mood seam keeps it on. */
  context.set_error_unset(FLAG_NOUNSET.is_enabled());
  if (FLAG_NOUNSET.is_enabled()) context.set_error_unset_explicit(true);
  let const warnings_specified_count = FLAG_WARNINGS.count();
  let const specified_warning_level = static_cast<u8>(
      warnings_specified_count > 3 ? 3 : warnings_specified_count);
  let warning_level = specified_warning_level;
  if (FLAG_LINT.is_enabled())
    warning_level = session_mood == koshka::mimic_mood::Default ? 0 : 3;
  context.set_warning_level(warning_level);
  context.set_pipefail(false);
  context.set_no_clobber(FLAG_NO_CLOBBER.is_enabled());
  context.set_export_all(FLAG_EXPORT_ALL.is_enabled());
  context.set_no_exec(FLAG_NO_EXEC.is_enabled() || FLAG_LINT.is_enabled());
  context.set_koshkit(FLAG_ENABLE_KOSHKIT.is_enabled());
  context.set_failglob(false);
  /* Mimicry is mirrored onto the context, since the execution path in Utils
     reads it there rather than the static flag. */
  context.set_mimicry(FLAG_MIMICRY.is_enabled());
  context.set_monitor(should_be_interactive);

  /* BASH names the path used to invoke this shell, the symlink spelling such as
     /usr/local/bin/bash when kosh is symlinked to bash. */
  context.set_shell_executable_path(executable_path);
  context.mark_exported("KOSH_IDENTITY");
  context.mark_readonly("KOSH_IDENTITY");
  /* SHELL is owned by login, getty, or the display manager, so an inherited
     value is left untouched. Only a shell that received no SHELL seeds its own
     invocation path. */
  if (!koshka::os::get_environment_variable("SHELL").has_value())
    context.set_shell_variable("SHELL", executable_path);
  context.set_shell_variable("PWD", koshka::Path::current_directory().text());
  context.set_shell_variable("KOSH", executable_path);
  context.set_shell_variable("KOSH_VERSION", KOSH_VERSION_STRING);
  context.set_shell_variable("KOSH_COMMIT", KOSH_COMMIT_HASH);
  context.set_shell_variable("KOSH_BUILD_MODE", KOSH_BUILD_MODE);
  context.set_shell_variable("KOSH_OS", KOSH_OS_INFO);

  /* A bash session, a bash-posix session, or a bash flavor in the init list
     advertises BASH_VERSION so a bash rc detects it. */
  bool should_seed_bash_identity =
      session_mood == koshka::mimic_mood::Bash ||
      session_mood == koshka::mimic_mood::BashPosix;
  for (let listed : init_moods)
    if (listed == koshka::mimic_mood::Bash ||
        listed == koshka::mimic_mood::BashPosix)
      should_seed_bash_identity = true;
  context.seed_shell_identity_variables(should_seed_bash_identity);

  /* SHLVL counts shell nesting, incremented and exported so a child shell
     continues the count. */
  i64 shell_level = 0;
  if (koshka::Maybe<koshka::String> inherited =
          koshka::os::get_environment_variable("SHLVL");
      inherited.has_value())
  {
    if (koshka::ErrorOr<i64> parsed_level = inherited->view().to<i64>();
        !parsed_level.is_error() && parsed_level.value() > 0)
      shell_level = parsed_level.value();
  }
  /* An inherited level past the cap is reset so the increment cannot overflow,
     the way bash bounds SHLVL. */
  constexpr i64 MAX_SHLVL = 999;
  if (shell_level > MAX_SHLVL) shell_level = 0;
  koshka::os::set_environment_variable(
      "SHLVL", koshka::String::from(shell_level + 1, koshka::heap_allocator()));
  /* The exported set must know SHLVL even on a first shell that did not inherit
     one. */
  context.mark_exported("SHLVL");

  /* PS1 is seeded only for an interactive shell, since bash leaves it unset in
     a non-interactive run and a config that gates on -z "$PS1", such as
     bash_completion.sh, returns early before sourcing its body. PS2 is the
     continuation prompt and PS4 prefixes the xtrace lines, and both carry
     their defaults in every run. PS3 is left unset, since the select loop
     falls back to its own default. */
  if (should_be_interactive) {
    if (!koshka::os::get_environment_variable("PS1").has_value())
      context.set_shell_variable("PS1", toiletline::default_prompt_template());
  }

  if (!koshka::os::get_environment_variable("PS2").has_value())
    context.set_shell_variable("PS2", "> ");
  if (!koshka::os::get_environment_variable("PS4").has_value())
    context.set_shell_variable("PS4", "+ ");

  /* COLUMNS and LINES carry the terminal size so a config that divides by
     COLUMNS, such as ble.sh, sees a non-zero width. They are seeded once and
     not tracked across a later resize. */
  if (should_be_interactive) {
    u32 columns = 0, rows = 0;
    if (koshka::os::terminal_size(columns, rows)) {
      context.set_shell_variable(
          "COLUMNS", koshka::String::from(columns, koshka::heap_allocator()));
      context.set_shell_variable(
          "LINES", koshka::String::from(rows, koshka::heap_allocator()));
    }
  }

  bool should_quit = FLAG_ONE_COMMAND.is_enabled() && !FLAG_LINT.is_enabled();
  i32 exit_code = EXIT_SUCCESS;
  koshka::Maybe<usize> history_event_number = koshka::None;

  /* The path map is reset rather than seeded here, since the eager scan pays
     off only in interactive mode. */
  koshka::os::set_default_signal_handlers(
      should_be_interactive ? koshka::os::signal_profile::Interactive
                            : koshka::os::signal_profile::NonInteractive);
  LOG(Info, "installed the default signal handlers");

  /* The parse arena holds the AST and its tokens for one command, reset between
     commands. */
  let ast_arena = koshka::BumpArena{};
  koshka::AST_ARENA = &ast_arena;

  /* Function bodies outlive the command that defined them, so the function
     arena is never reset during the run. */
  let function_arena = koshka::BumpArena{};
  koshka::FUNCTION_ARENA = &function_arena;

  if (is_language_server)
    return koshka::language_server::run(context, ast_arena);
  if (FLAG_LINT.is_enabled() && FLAG_APPLY.is_enabled())
    return koshka::run_lint_apply_operation(
        file_names, FLAG_FORMAT.is_enabled(), context, ast_arena);
  if (FLAG_FORMAT.is_enabled())
    return koshka::run_format_operation(file_names, FLAG_APPLY.is_enabled(),
                                        FLAG_LINT.is_enabled(), session_mood,
                                        ast_arena, context);

  /* A lint report must not follow the aliases, functions, and search path of
     whoever invoked it. */
  if (has_elevated_identity || is_rescue_mode || FLAG_CLEAN.is_enabled() ||
      FLAG_LINT.is_enabled() || FLAG_FORMAT.is_enabled())
  {
    LOG(Info, "skipping every startup config file in %s mode",
        is_rescue_mode            ? "rescue"
        : FLAG_CLEAN.is_enabled() ? "clean"
        : FLAG_LINT.is_enabled()  ? "lint"
                                  : "privileged");
  } else {
    /* --no-init-diagnostics turns diagnostics and warnings off while the init
       files source, so a -W shell loads a lax bash config quietly, then
       restores them for the session. */
    let const saved_diagnostics_disabled = context.diagnostics_disabled();
    let const saved_warning_level = context.warning_level();
    if (FLAG_SUPPRESS_INIT_DIAGNOSTICS.is_enabled()) {
      context.set_diagnostics_disabled(true);
      context.set_warning_level(0);
    }
    koshka::source_init_moods(context, ast_arena, init_moods, is_login_shell,
                              should_be_interactive);
    if (FLAG_SUPPRESS_INIT_DIAGNOSTICS.is_enabled()) {
      context.set_diagnostics_disabled(saved_diagnostics_disabled);
      context.set_warning_level(saved_warning_level);
    }
  }

  if (should_be_interactive && !context.get_variable_value("PS1").has_value())
    context.set_shell_variable("PS1", toiletline::default_prompt_template());

  context.set_startup_finished();

  /* The session mood takes over and seeds its strictness once the config has
     loaded, unless the rc picked one with set --mood, which wins the way a
     command-line --mood would. */
  if (!context.was_mood_set_explicitly()) context.set_mood(session_mood);
  context.apply_strictness_for_mood();
  if (FLAG_LINT.is_enabled()) {
    context.set_warning_level(
        context.mood() == koshka::mimic_mood::Default ? 0 : 3);
  }

  /* The rc files retained a heap copy of their text and tree until the next
     top-level command clears them, dropped now rather than carried through the
     idle prompt. */
  context.clear_retained_sources();

  /* A plain return must not be used past this point, since toiletline needs its
     own cleanup that utils::quit() runs. */
  bool did_seed_interactive_path_map = false;
  bool did_lint_input_fail = false;
  usize next_file_index = 0;
  koshka::analysis_diagnostic_totals lint_diagnostic_totals{};

  loop
  {
    ASSERT(!koshka::os::is_child_process());

    let script_contents = koshka::String{koshka::heap_allocator()};
    /* The named script file flows into the diagnostics so an error reads
       path:line:col. A -c or interactive line carries no path. */
    koshka::Maybe<koshka::StringView> source_filename = koshka::None;
    /* The root frame caret underlines the operand that produced the script
       body, the -c flag and its argument for a command string, the file name
       for a script file. Stdin and interactive runs leave it empty. */
    koshka::Maybe<koshka::SourceLocation> root_frame_call_site = koshka::None;
    bool should_analyze_input = true;

    try {
      if (should_read_stdin) {
        bool is_driver_run = false;
#if !defined NDEBUG
        is_driver_run = FLAG_DEBUG_COMPLETE_AT.is_set() ||
                        FLAG_DEBUG_HIGHLIGHT_AT.is_set() ||
                        FLAG_DEBUG_GHOST_AT.is_set();
#endif
        if (!is_driver_run) {
          LOG(Info, "reading the whole standard input");
          script_contents = koshka::utils::read_entire_standard_input();
        }

        should_quit = true;
      } else if (should_execute_commands && !FLAG_COMMAND.at_end()) {
        koshka::StringView command_view = FLAG_COMMAND.next();
        script_contents = koshka::String{command_view};
        context.set_execution_string(command_view);
        LOG(Info, "taking the next -c command string, %zu bytes",
            script_contents.count());
        {
          /* The consumed -c is the Nth -c token, where N is how many
             commands FLAG_COMMAND has handed out so far. */
          let const consumed_command_index = FLAG_COMMAND.value_position();
          usize seen_dash_c_count = 0;
          usize flag_offset = 0;
          for (int a = 0; a < parse_argc; a++) {
            let const token_length = std::strlen(parse_argv[a]);
            const koshka::StringView token{parse_argv[a], token_length};
            let const quoted_length = koshka::shell_quoted_arg_length(token);
            if (token == "-c") {
              seen_dash_c_count++;
              if (seen_dash_c_count == consumed_command_index &&
                  a + 1 < parse_argc)
              {
                const usize argument_length =
                    koshka::shell_quoted_arg_length(koshka::StringView{
                        parse_argv[a + 1], std::strlen(parse_argv[a + 1])});
                let const span = quoted_length + 1 + argument_length;
                root_frame_call_site =
                    koshka::SourceLocation{flag_offset, span};
                break;
              }
            }
            flag_offset += quoted_length + 1;
          }
        }
        /* A debug driver clears should_read_files while the operands remain
           listed. */
        if (FLAG_COMMAND.at_end() &&
            (!FLAG_LINT.is_enabled() || !should_read_files))
        {
          should_quit = true;
        }
      } else if (should_read_files) {
        ASSERT(next_file_index < file_names.count());
        const koshka::String &file_name = file_names[next_file_index++];

        if (file_name == "-") {
          bool is_driver_run = false;
#if !defined NDEBUG
          is_driver_run = FLAG_DEBUG_COMPLETE_AT.is_set() ||
                          FLAG_DEBUG_HIGHLIGHT_AT.is_set() ||
                          FLAG_DEBUG_GHOST_AT.is_set();
#endif
          if (!is_driver_run) {
            LOG(Info, "reading the whole standard input");
            script_contents = koshka::utils::read_entire_standard_input();
          }
        } else {
          let const operand_offset = koshka::quoted_argv_offset_until(
              parse_argc, parse_argv, file_name.view());
          const koshka::SourceLocation operand_location{
              operand_offset,
              koshka::shell_quoted_arg_length(file_name.view())};
          const koshka::Path script_path{file_name.view()};

          if (script_path.is_directory()) {
            let const verb = FLAG_LINT.is_enabled()
                                 ? koshka::StringView{"analyze"}
                                 : koshka::StringView{"execute"};
            koshka::show_message(
                koshka::ErrorWithLocation{
                    operand_location, "Unable to " + verb + " `" +
                                          file_name.view() +
                                          "` because the file is a directory"}
                    .to_string(context.cli_invocation().view(), &context));
            if (!FLAG_LINT.is_enabled()) {
              koshka::utils::quit(126, koshka::utils::farewell_policy::Goodbye);
            }
            should_analyze_input = false;
          }

          if (should_analyze_input) {
            LOG(Info, "reading the script file '%s'", file_name.c_str());
            koshka::Maybe<koshka::String> contents =
                script_path.read_entire_file();
            if (!contents) {
              let const looks_like_command =
                  !FLAG_LINT.is_enabled() &&
                  !file_name.view().find_character('/').has_value();
              let hint = koshka::String{koshka::heap_allocator()};
              if (looks_like_command)
                hint = "Pass -c to run this as a command string";
              let const message =
                  "Could not open '" + file_name.view() +
                  "': " + koshka::os::last_system_error_message();
              if (hint.is_empty()) {
                koshka::show_message(
                    koshka::ErrorWithLocation{operand_location, message}
                        .to_string(context.cli_invocation().view(), &context));
              } else {
                koshka::show_message(
                    koshka::ErrorWithLocationAndDetails{operand_location,
                                                        message, hint.view()}
                        .to_string(context.cli_invocation().view(), &context));
              }
              if (!FLAG_LINT.is_enabled()) {
                koshka::utils::quit(127,
                                    koshka::utils::farewell_policy::Goodbye);
              }
              should_analyze_input = false;
            } else {
              script_contents = steal(*contents);
              source_filename = file_name.view();
              /* A script-file run bottoms FUNCNAME out at "main", while -c and
                 stdin runs leave it off. */
              context.set_script_run(true);
              root_frame_call_site = operand_location;
            }
          }
        }

        should_quit =
            !FLAG_LINT.is_enabled() || next_file_index == file_names.count();
      } else if (should_be_interactive) {
        if (!toiletline::is_active()) {
          LOG(Info, "initializing the line editor");
          toiletline::initialize();
          /* The set -b wake hook registers even under -T, since job reporting
             is not completion. */
          toiletline::enable_job_notifications(context);
          if (!FLAG_NO_COMPLETION.is_enabled())
            toiletline::enable_completion(context);

          let const should_highlight =
              !FLAG_NO_COMPLETION.is_enabled() &&
              !FLAG_NO_SYNTAX_HIGHLIGHTING.is_enabled();
          toiletline::set_highlight_enabled(should_highlight);
          toiletline::set_ghost_enabled(should_highlight);
          if (let const welcome = context.get_variable_value("KOSH_WELCOME");
              welcome.has_value())
          {
            if (!welcome->is_empty()) koshka::show_message(welcome->view());
          } else {
            koshka::show_message(
                session_mood == koshka::mimic_mood::Posix ? "POSIX me harder!"
                : (session_mood == koshka::mimic_mood::Bash ||
                   session_mood == koshka::mimic_mood::BashPosix)
                    ? "Bash me harder!"
                    : "Welcome :3");
          }
        } else {
          toiletline::enter_raw_mode();
        }

        context.notify_done_jobs();

        toiletline::set_idle_title();

        /* The PROMPT_COMMAND hook runs before the template is expanded, so a
           framework that assigns PS1 inside it is in place by then. */
        run_prompt_command(context, ast_arena);

        if (!did_seed_interactive_path_map && !is_rescue_mode &&
            !FLAG_NO_COMPLETION.is_enabled())
        {
          context.get_program_resolver().initialize_path_map();
          did_seed_interactive_path_map = true;
        }

        /* A command whose output did not end in a newline leaves the cursor off
           the first column. A marker, spaces to the line width, and a carriage
           return push the prompt to a fresh line, and on a clean line the
           prompt overwrites the marker so nothing shows. */
        if (should_be_interactive) {
          u32 marker_columns = 0, marker_rows = 0;
          if (koshka::os::terminal_size(marker_columns, marker_rows) &&
              marker_columns > 0)
          {
            koshka::String eol_marker{koshka::heap_allocator()};
            /* One allocation holds the glyph, the fill spaces, and the controls
               so the fill loop never regrows the buffer. */
            eol_marker.reserve(marker_columns + 12);
            if (koshka::colors::stdout_wants_color()) {
              eol_marker += koshka::colors::ansi::INVERSE;
              eol_marker += "\\n";
              eol_marker += koshka::colors::ansi::RESET;
            } else {
              eol_marker += "\\n";
            }
            /* The marker is the two-column \n glyph, so the fill starts at
               column two. */
            for (u32 column = 2; column < marker_columns; column++)
              eol_marker.push(' ');
            eol_marker.push('\r');
            koshka::print(eol_marker);
            koshka::flush();
          }
        }

        koshka::String prompt = toiletline::build_prompt(context);

        toiletline::set_edit_mode(context.vi_mode()
                                      ? toiletline::edit_mode::Vi
                                      : toiletline::edit_mode::Emacs);

        loop
        {
          let[code, input, accepted_history_event_number] =
              toiletline::get_input(prompt);

          switch (code) {
          case TL_PRESSED_TAB:
            /* This fires only when there was nothing to complete, so the line
               is re-fed rather than inserting a literal tab. */
            toiletline::set_input(input);
            continue;
          case TL_PRESSED_EOF:
            /* EOF logs out only on an empty line. On a non-empty line it is
               ignored so the user can finish the command. */
            if (input.is_empty()) {
              koshka::print("^D");
              koshka::flush();
              toiletline::emit_newlines(input);
              koshka::utils::quit(exit_code,
                                  koshka::utils::farewell_policy::Goodbye);
            } else {
              toiletline::set_input(input);
              continue;
            }
            break;
          case TL_PRESSED_QUIT:
            toiletline::emit_newlines(input);
            koshka::utils::quit(exit_code,
                                koshka::utils::farewell_policy::Goodbye);
            break;
          case TL_PRESSED_INTERRUPT:
            koshka::print("^C");
            koshka::flush();
            break;
          case TL_PRESSED_SUSPEND:
            koshka::print("^Z");
            koshka::flush();
            break;
          default:;
          }

          toiletline::emit_newlines(input);

          if (code == TL_PRESSED_ENTER && !input.is_empty()) {
            script_contents = steal(input);
            history_event_number = accepted_history_event_number;
            break;
          }
        }

        LOG(Info, "accepted an interactive line of %zu bytes",
            script_contents.count());
        toiletline::exit_raw_mode();
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

    /* A Ctrl-C used to clear the input line must not abort the command about to
       run, so a pending interrupt is dropped here. */
    koshka::os::INTERRUPT_REQUESTED = 0;

    /* On the final chunk a terminal external command may replace the shell
       process rather than fork, exec, and wait, the way dash execs the last
       command under EV_EXIT. An interactive prompt, an EXIT trap, or a pending
       trailer keeps the fork to regain control. */
    const bool should_print_post_run_trailer =
        context.show_exit_code() || context.stats_enabled();
    context.set_terminal_exec_allowed(
        should_quit && !context.shell_is_interactive() &&
        !context.has_exit_trap() && !should_print_post_run_trailer);

    if (context.shell_is_interactive() && !script_contents.is_empty()) {
      koshka::String ps0 = toiletline::render_ps0(context);
      if (!ps0.is_empty()) {
        koshka::print(ps0);
        koshka::flush();
      }
    }

    if (root_frame_call_site.has_value() && !should_suppress_root_source_trace)
    {
      context.push_root_source_frame(&context.cli_invocation(),
                                     *root_frame_call_site,
                                     FLAG_COMMAND.count() <= 1);
    }
    defer
    {
      if (root_frame_call_site.has_value() &&
          !should_suppress_root_source_trace)
      {
        context.pop_root_source_frame();
      }
    };

    if (should_analyze_input) {
      script_contents.normalize_crlf_line_endings();
      exit_code = run_script_contents(
          script_contents, context, ast_arena, source_filename, nullptr,
          nullptr, history_event_number,
          FLAG_LINT.is_enabled() ? &lint_diagnostic_totals : nullptr);
    } else {
      exit_code = EXIT_FAILURE;
    }
    if (FLAG_LINT.is_enabled()) {
      did_lint_input_fail = did_lint_input_fail || exit_code != EXIT_SUCCESS;
      if (should_quit && did_lint_input_fail) {
        exit_code = EXIT_FAILURE;
      }
    }

    /* A child process reaches here when its exec() failed and printed the error
       itself. */
    if (should_quit || koshka::os::is_child_process() ||
        (!FLAG_LINT.is_enabled() && FLAG_ERROR_EXIT.is_enabled() &&
         exit_code != 0))
    {
#if !defined NDEBUG
      /* The completion test driver runs after the staged chunks, so a -c that
         registered specs is visible to the engine. */
      if (FLAG_DEBUG_COMPLETE_AT.is_set() && !koshka::os::is_child_process()) {
        exit_code = koshka::run_debug_completion_driver(
            FLAG_DEBUG_COMPLETE_AT.value(), context);
      }
      if (FLAG_DEBUG_HIGHLIGHT_AT.is_set() && !koshka::os::is_child_process()) {
        exit_code = koshka::run_debug_highlight_driver(
            FLAG_DEBUG_HIGHLIGHT_AT.value(), context);
      }
      if (FLAG_DEBUG_GHOST_AT.is_set() && !koshka::os::is_child_process()) {
        exit_code = koshka::run_debug_ghost_driver(FLAG_DEBUG_GHOST_AT.value(),
                                                   context);
      }
#endif
      LOG(Info, "exiting after the final chunk with code %d", exit_code);
      if (!koshka::os::is_child_process()) context.run_exit_trap();
      if (FLAG_LINT.is_enabled()) {
        if (context.memory_stats_enabled()) {
          koshka::utils::print_memory_report();
          context.set_memory_stats_enabled(false);
        }
        koshka::print_analysis_diagnostic_summary(lint_diagnostic_totals);
      }
      koshka::utils::quit(exit_code,
                          FLAG_ERROR_EXIT.is_enabled()
                              ? koshka::utils::farewell_policy::Goodbye
                              : koshka::utils::farewell_policy::Silent);
    }
  }

  unreachable("the main command loop terminated without exiting");
}
