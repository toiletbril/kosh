#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-abefhkmnruvxBCEPTARWISG] [+abefhkmnuvxBCEPTARWISG] "
                   "[-o name] [+o name] [--options] [--] [arg ...]");

HELP_DESCRIPTION_DECL(
    "The set builtin sets the shell options and the positional parameters.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(OPTIONS, Bool, '\0', "options",
     "Display every option with its current state and description.");
/* The mood flags are parsed by hand in execute(), so these declarations only
   join the set builtin's flag list for completion and the help listing. */
FLAG(MOOD, String, 'M', "mood",
     "Set the runtime mood to shit, bash, or sh, or print it with no value.");
FLAG(INIT_MOODS, ManyStrings, 'L', "init-moods",
     "Source the startup files for the listed moods, or print the loaded ones "
     "with no value.");

REGISTER_BUILTIN_FLAGS(Set);

namespace shit {

namespace {

enum class set_option_behavior : u8
{
  Stored,
  Posix,
  Vi,
  Emacs,
  WarningLevel,
  AnnoyingDiagnostics,
  NoDiagnostics,
  Login,
  Rcfile,
};

struct option_text
{
  const char *data{nullptr};
  usize length{0};

  constexpr option_text() = default;
  template <usize Count>
  consteval option_text(const char (&text)[Count])
      : data(text), length(Count - 1)
  {}
  constexpr operator StringView() const wontthrow
  {
    return StringView{data, length};
  }
  constexpr fn is_empty() const wontthrow -> bool { return length == 0; }
};

struct set_option_descriptor
{
  shell_option_id id;
  set_option_behavior behavior;
  char letter;
  option_text name;
  option_text help;
  option_text alias{};
  bool is_in_shellopts{false};
  bool is_listed{true};
};

constexpr set_option_descriptor SET_OPTIONS[] = {
    {shell_option_id::Allexport, set_option_behavior::Stored, 'a', "allexport",
     "Mark every assigned variable for the environment.", "export-all", true},
    {shell_option_id::Notify,
     set_option_behavior::Stored,
     'b', "notify",
     "Report a background job's completion immediately when it finishes.", {},
     true},
    {shell_option_id::Errexit, set_option_behavior::Stored, 'e', "errexit",
     "Exit on the first command that fails.", "error-exit", true},
    {shell_option_id::Noglob, set_option_behavior::Stored, 'f', "noglob",
     "Disable pathname expansion.", "no-glob", true},
    {shell_option_id::Hashall,
     set_option_behavior::Stored,
     'h', "hashall",
     "Retain command hashing mode for compatible option queries.", {},
     true},
    {shell_option_id::Keyword,
     set_option_behavior::Stored,
     'k', "keyword",
     "Retain keyword assignment mode for compatible option queries.", {},
     true},
    {shell_option_id::Monitor,
     set_option_behavior::Stored,
     'm', "monitor",
     "Run background jobs in their own process group with notifications.", {},
     true},
    {shell_option_id::Noexec, set_option_behavior::Stored, 'n', "noexec",
     "Read and parse commands but do not run them.", "no-exec", true},
    {shell_option_id::Privileged,
     set_option_behavior::Stored,
     'p', "privileged",
     "Retain elevated ids and suppress environment startup files.", {},
     true},
    {shell_option_id::Nounset, set_option_behavior::Stored, 'u', "nounset",
     "Treat an unset variable as an error.", "no-unset", true},
    {shell_option_id::Verbose,
     set_option_behavior::Stored,
     'v', "verbose",
     "Write input to standard error as it is read.", {},
     true},
    {shell_option_id::Xtrace,
     set_option_behavior::Stored,
     'x', "xtrace",
     "Print each command after expansion before it runs.", {},
     true},
    {shell_option_id::Braceexpand,
     set_option_behavior::Stored,
     'B', "braceexpand",
     "Enable brace expansion.", {},
     true},
    {shell_option_id::Noclobber, set_option_behavior::Stored, 'C', "noclobber",
     "Refuse to overwrite an existing file through '>'.", "no-clobber", true},
    {shell_option_id::Errtrace,
     set_option_behavior::Stored,
     'E', "errtrace",
     "Retain ERR trap inheritance mode for compatible option queries.", {},
     true},
    {shell_option_id::Physical,
     set_option_behavior::Stored,
     'P', "physical",
     "Resolve symbolic links while changing directories.", {},
     true},
    {shell_option_id::Functrace,
     set_option_behavior::Stored,
     'T', "functrace",
     "Retain DEBUG and RETURN inheritance mode for compatible option queries.", {},
     true},
    {shell_option_id::Pipefail,
     set_option_behavior::Stored,
     '\0', "pipefail",
     "Report a pipeline's status as the rightmost stage that failed.", {},
     true},
    {shell_option_id::Failglob,
     set_option_behavior::Stored,
     '\0', "failglob",
     "Fail a command whose glob matches nothing.", {},
     true},
    {shell_option_id::Shitbox, set_option_behavior::Stored, '\0', "shitbox",
     "Resolve the bundled shitbox utility names directly as commands."},
    {shell_option_id::Vi,
     set_option_behavior::Vi,
     '\0', "vi",
     "Use vi-style command-line editing.", {},
     true},
    {shell_option_id::Emacs,
     set_option_behavior::Emacs,
     '\0', "emacs",
     "Use emacs-style command-line editing.", {},
     true},
    {shell_option_id::Count,
     set_option_behavior::Posix,
     '\0', "posix",
     "Switch to the bash posix mood.", {},
     true},
    {shell_option_id::ShowAst,
     set_option_behavior::Stored,
     'A', "show-ast",
     "Print the AST before each command runs.", {},
     false, false},
    {shell_option_id::ShowLexedWords,
     set_option_behavior::Stored,
     'R', "show-lexed-words",
     "Print the escape bitmap after each parse.", {},
     false, false},
    {shell_option_id::ShowExitCode,
     set_option_behavior::Stored,
     '\0', "show-exit-code",
     "Print the exit code after each command.", {},
     false, false},
    {shell_option_id::Count,
     set_option_behavior::WarningLevel,
     'W', {},
     "Step through the diagnostic tiers with -W, -WW, and -WWW.", {},
     false, false},
    {shell_option_id::Mimicry, set_option_behavior::Stored, 'I', "mimicry",
     "Mimic the shell named by a script's shebang."},
    {shell_option_id::Count, set_option_behavior::AnnoyingDiagnostics, '\0',
     "annoying-diagnostics", "Report the annoying diagnostic tier."},
    {shell_option_id::ShowStats,
     set_option_behavior::Stored,
     'S', "show-stats",
     "Print evaluation statistics after each run.", {},
     false, false},
    {shell_option_id::Count, set_option_behavior::NoDiagnostics, '\0',
     "no-diagnostics", "Skip the analysis stage before each chunk runs."},
    {shell_option_id::ShowMemory,
     set_option_behavior::Stored,
     'G', "show-memory",
     "Print a granular memory report at exit.", {},
     false, false},
    {shell_option_id::Count,
     set_option_behavior::Login,
     '\0', "login",
     "Whether the shell started as a login shell, fixed at startup.", {},
     false, false},
    {shell_option_id::Count,
     set_option_behavior::Rcfile,
     '\0', "rcfile",
     "Whether a custom rc file was named at startup, fixed at startup.", {},
     false, false},
};

consteval fn set_option_name_count() wontthrow -> usize
{
  usize result = 0;
  for (let const &option : SET_OPTIONS) {
    if (!option.name.is_empty()) result++;
    if (!option.alias.is_empty()) result++;
  }
  return result;
}

template <usize Count>
struct set_option_name_table
{
  static_string_entry<u8> entries[Count]{};
};

consteval fn make_set_option_name_table() wontthrow
    -> set_option_name_table<set_option_name_count()>
{
  set_option_name_table<set_option_name_count()> result{};
  usize entry_position = 0;
  for (usize option_position = 0; option_position < countof(SET_OPTIONS);
       option_position++)
  {
    let const &option = SET_OPTIONS[option_position];
    if (!option.name.is_empty())
      result.entries[entry_position++] = {
          PackedStringKey::from_literal(option.name.data),
          static_cast<u8>(option_position)};
    if (!option.alias.is_empty())
      result.entries[entry_position++] = {
          PackedStringKey::from_literal(option.alias.data),
          static_cast<u8>(option_position)};
  }
  return result;
}

consteval fn set_option_descriptors_are_valid() wontthrow -> bool
{
  if (countof(SET_OPTIONS) > 0xff) return false;
  for (usize left = 0; left < countof(SET_OPTIONS); left++) {
    let const &left_option = SET_OPTIONS[left];
    if (left_option.name.is_empty() &&
        (left_option.behavior != set_option_behavior::WarningLevel ||
         left_option.letter == '\0'))
    {
      return false;
    }
    if (left_option.name.length > PackedStringKey::BYTE_CAPACITY) return false;
    if (!left_option.alias.is_empty() &&
        left_option.alias.length > PackedStringKey::BYTE_CAPACITY)
      return false;
    for (usize right = left + 1; right < countof(SET_OPTIONS); right++)
      if (left_option.letter != '\0' &&
          left_option.letter == SET_OPTIONS[right].letter)
        return false;
  }
  let const names = make_set_option_name_table();
  for (usize left = 0; left < countof(names.entries); left++)
    for (usize right = left + 1; right < countof(names.entries); right++)
      if (names.entries[left].key == names.entries[right].key) return false;
  return true;
}

static_assert(set_option_descriptors_are_valid());
constexpr auto SET_OPTION_NAMES = make_set_option_name_table();
constexpr StaticStringMap SET_OPTION_BY_NAME{SET_OPTION_NAMES.entries};

constexpr u8 INTERACTIVE_COMMENTS_POSITION = countof(SET_OPTIONS);

consteval fn shellopts_position_count() wontthrow -> usize
{
  usize result = 1;
  for (let const &option : SET_OPTIONS)
    if (option.is_in_shellopts) result++;
  return result;
}

template <usize Count>
struct shellopts_position_table
{
  u8 positions[Count]{};
};

consteval fn shellopts_name(u8 position) wontthrow -> option_text
{
  if (position == INTERACTIVE_COMMENTS_POSITION)
    return option_text{"interactive-comments"};
  return SET_OPTIONS[position].name;
}

consteval fn option_text_is_before(option_text left,
                                   option_text right) wontthrow -> bool
{
  let const shared_length =
      left.length < right.length ? left.length : right.length;
  for (usize byte_position = 0; byte_position < shared_length; byte_position++)
    if (left.data[byte_position] != right.data[byte_position])
      return left.data[byte_position] < right.data[byte_position];
  return left.length < right.length;
}

consteval fn make_shellopts_positions() wontthrow
    -> shellopts_position_table<shellopts_position_count()>
{
  shellopts_position_table<shellopts_position_count()> result{};
  usize output_position = 0;
  for (usize option_position = 0; option_position < countof(SET_OPTIONS);
       option_position++)
    if (SET_OPTIONS[option_position].is_in_shellopts)
      result.positions[output_position++] = static_cast<u8>(option_position);
  result.positions[output_position] = INTERACTIVE_COMMENTS_POSITION;

  for (usize position = 1; position < countof(result.positions); position++) {
    let const moved = result.positions[position];
    usize slot = position;
    while (slot > 0 &&
           option_text_is_before(shellopts_name(moved),
                                 shellopts_name(result.positions[slot - 1])))
    {
      result.positions[slot] = result.positions[slot - 1];
      slot--;
    }
    result.positions[slot] = moved;
  }
  return result;
}

constexpr auto SHELLOPTS_POSITIONS = make_shellopts_positions();

consteval fn shellopts_positions_are_valid() wontthrow -> bool
{
  for (usize left = 0; left < countof(SHELLOPTS_POSITIONS.positions); left++) {
    let const left_position = SHELLOPTS_POSITIONS.positions[left];
    if (left_position > INTERACTIVE_COMMENTS_POSITION) return false;
    if (left_position != INTERACTIVE_COMMENTS_POSITION &&
        !SET_OPTIONS[left_position].is_in_shellopts)
      return false;
    if (left > 0 && !option_text_is_before(
                        shellopts_name(SHELLOPTS_POSITIONS.positions[left - 1]),
                        shellopts_name(left_position)))
      return false;
    for (usize right = left + 1; right < countof(SHELLOPTS_POSITIONS.positions);
         right++)
      if (left_position == SHELLOPTS_POSITIONS.positions[right]) return false;
  }
  return true;
}

static_assert(shellopts_positions_are_valid());

struct set_option_letter_table
{
  u8 positions[256];
};

consteval fn make_set_option_letter_table() wontthrow -> set_option_letter_table
{
  set_option_letter_table result{};
  for (usize position = 0; position < countof(result.positions); position++)
    result.positions[position] = 0xff;
  for (usize position = 0; position < countof(SET_OPTIONS); position++) {
    let const letter = SET_OPTIONS[position].letter;
    if (letter != '\0')
      result.positions[static_cast<u8>(letter)] = static_cast<u8>(position);
  }
  return result;
}

constexpr auto SET_OPTION_BY_LETTER = make_set_option_letter_table();

fn find_option_by_letter(char letter) throws -> const set_option_descriptor *
{
  let const position = SET_OPTION_BY_LETTER.positions[static_cast<u8>(letter)];
  return position == 0xff ? nullptr : &SET_OPTIONS[position];
}

fn find_option_by_name(StringView name) throws -> const set_option_descriptor *
{
  let const position = SET_OPTION_BY_NAME.find(name);
  return position.has_value() ? &SET_OPTIONS[*position] : nullptr;
}

fn option_is_available(const EvalContext &cxt,
                       const set_option_descriptor &option) wontthrow -> bool
{
  return option.id != shell_option_id::Physical || !cxt.is_posix_mode();
}

fn option_is_on(const EvalContext &cxt,
                const set_option_descriptor &option) throws -> bool
{
  if (!option_is_available(cxt, option)) return false;
  switch (option.behavior) {
  case set_option_behavior::Stored: return cxt.shell_option_state(option.id);
  case set_option_behavior::Posix: return cxt.is_posix_option_on();
  case set_option_behavior::Vi: return cxt.vi_mode();
  case set_option_behavior::Emacs: return cxt.emacs_mode();
  case set_option_behavior::WarningLevel: return cxt.warning_level() > 0;
  case set_option_behavior::AnnoyingDiagnostics:
    return cxt.annoying_diagnostics_enabled();
  case set_option_behavior::NoDiagnostics: return cxt.diagnostics_disabled();
  case set_option_behavior::Login: return cxt.is_login_shell();
  case set_option_behavior::Rcfile: return cxt.has_custom_rcfile();
  }
  unreachable("Unhandled set option behavior");
}

fn option_is_startup_fact(const set_option_descriptor &option) throws -> bool
{
  return option.behavior == set_option_behavior::Login ||
         option.behavior == set_option_behavior::Rcfile;
}

fn apply_or_reject_option(EvalContext &cxt, const set_option_descriptor &option,
                          bool enable,
                          bool should_step_warning_level = false) throws -> void
{
  if (!option_is_available(cxt, option))
    throw Error{
        "Unknown option '" + String{cxt.scratch_allocator(), option.name}
          +
        "'"
    };
  if (option_is_startup_fact(option))
    throw Error{
        "Unable to change '" + String{cxt.scratch_allocator(), option.name}
          +
        "' because it is fixed at shell startup"
    };
  LOG(Info, "set flipping option '%.*s' to %s",
      static_cast<int>(option.name.length), option.name.data,
      enable ? "on" : "off");
  switch (option.behavior) {
  case set_option_behavior::Stored:
    if (option.id == shell_option_id::Privileged && !enable &&
        os::is_running_setuid() && !os::drop_elevated_identity())
      throw Error{"Unable to drop elevated ids: " +
                  os::last_system_error_message()};
    cxt.note_shell_option_mutation(option.id);
    cxt.set_shell_option_state(option.id, enable);
    break;
  case set_option_behavior::Posix: cxt.set_posix_mode_via_option(enable); break;
  case set_option_behavior::Vi: cxt.set_vi_mode(enable); break;
  case set_option_behavior::Emacs: cxt.set_emacs_mode(enable); break;
  case set_option_behavior::WarningLevel:
    cxt.note_warning_option_mutation();
    if (should_step_warning_level)
      cxt.set_warnings_enabled(enable);
    else
      cxt.set_warning_level(enable ? 1 : 0);
    break;
  case set_option_behavior::AnnoyingDiagnostics:
    cxt.note_annoying_diagnostics_option_mutation();
    cxt.set_annoying_diagnostics_enabled(enable);
    break;
  case set_option_behavior::NoDiagnostics:
    cxt.note_diagnostics_option_mutation();
    cxt.set_diagnostics_disabled(enable);
    break;
  case set_option_behavior::Login:
  case set_option_behavior::Rcfile: unreachable("Startup fact was applied");
  }
  if (option.id == shell_option_id::Nounset)
    cxt.set_error_unset_explicit(enable);
  else if (option.id == shell_option_id::Failglob)
    cxt.set_failglob_explicit(enable);
  else if (option.id == shell_option_id::Pipefail)
    cxt.set_pipefail_explicit(enable);
}

fn list_options(const EvalContext &cxt) throws -> String
{
  let out = String{heap_allocator()};
  for (let const &option : SET_OPTIONS) {
    if (!option.is_listed || !option_is_available(cxt, option)) continue;
    out += option_is_on(cxt, option) ? "set -o " : "set +o ";
    out += option.name;
    out += '\n';
  }
  return out;
}

fn list_options_columnar(const EvalContext &cxt) throws -> String
{
  const usize name_field_width = 15;
  let out = String{heap_allocator()};
  for (let const &option : SET_OPTIONS) {
    if (!option.is_listed || !option_is_available(cxt, option)) continue;
    out += option.name;
    for (usize pad = option.name.length; pad < name_field_width; pad++)
      out.push(' ');
    out.push('\t');
    out += option_is_on(cxt, option) ? "on" : "off";
    out.push('\n');
  }
  return out;
}

fn apply_long_option_by_name(const ExecContext &ec, EvalContext &cxt,
                             const ArrayList<String> &args, usize &i,
                             bool enable) throws -> void
{
  if (i + 1 >= args.count()) {
    ec.print_to_stdout(enable ? list_options_columnar(cxt) : list_options(cxt));
    return;
  }
  let const &name = args[++i];
  let const option = find_option_by_name(name);
  if (option == nullptr)
    throw make_error_for_arg(ec, i,
                             StringView{"Unknown -o option '"} + name + "'");
  if (!option_is_available(cxt, *option)) {
    let error = make_error_for_arg(
        ec, i, StringView{"Unknown -o option '"} + name + "'");
    error.set_command_status(2);
    throw error;
  }
  apply_or_reject_option(cxt, *option, enable);
}

fn format_option_table(const EvalContext *cxt,
                       bool include_alias_spellings) throws -> String
{
  const usize name_field_width = include_alias_spellings ? 30 : 18;
  let out = String{heap_allocator()};
  for (let const &option : SET_OPTIONS) {
    if (cxt != nullptr && !option_is_available(*cxt, option)) continue;
    out += "  ";
    if (option.letter != '\0') {
      out.push('-');
      out.push(option.letter);
      out += "  ";
    } else {
      out += "    ";
    }
    let name_cell = String{option.name};
    if (include_alias_spellings && !option.alias.is_empty()) {
      name_cell += ", ";
      name_cell += option.alias;
    }
    out += name_cell.view();
    for (usize pad = name_cell.count(); pad < name_field_width; pad++)
      out.push(' ');
    if (cxt != nullptr) out += option_is_on(*cxt, option) ? "[on]  " : "[off] ";
    out += option.help;
    out.push('\n');
  }
  return out;
}

fn format_option_switches_help() throws -> String
{
  let section = String{"OPTION SWITCHES\n"};
  section += "  A letter after a minus enables the option and after a plus "
             "disables it.\n  -o NAME and +o NAME do the same by long "
             "name.\n\n";
  section += format_option_table(nullptr, true);
  section += "\n  The -o long names:\n";
  let listed_names = String{heap_allocator()};
  for (let const &option : SET_OPTIONS) {
    if (option.name.is_empty()) continue;
    if (!listed_names.is_empty()) listed_names += ", ";
    listed_names += option.name;
    if (!option.alias.is_empty()) {
      listed_names += ", ";
      listed_names += option.alias;
    }
  }
  section += wrap_text(listed_names.view(), 4, HELP_WRAP_WIDTH);
  section += '\n';
  return section;
}

} // namespace

fn query_shell_option(const EvalContext &cxt, StringView name) throws
    -> Maybe<bool>
{
  const set_option_descriptor *option = find_option_by_name(name);
  if (option == nullptr) return None;
  return option_is_on(cxt, *option);
}

fn shell_option_names(bool include_alias_spellings) throws
    -> const ArrayList<StringView> &
{
  static ArrayList<StringView> canonical = [] throws {
    let names = ArrayList<StringView>{heap_allocator()};
    for (let const &option : SET_OPTIONS)
      if (!option.name.is_empty()) names.push(option.name);
    return names;
  }();
  static ArrayList<StringView> with_aliases = [] throws {
    let names = ArrayList<StringView>{heap_allocator()};
    for (let const &option : SET_OPTIONS) {
      if (!option.name.is_empty()) names.push(option.name);
      if (!option.alias.is_empty()) names.push(option.alias);
    }
    return names;
  }();
  return include_alias_spellings ? with_aliases : canonical;
}

fn shell_option_letters() throws -> const String &
{
  static String letters = [] throws {
    let collected = String{heap_allocator()};
    for (let const &option : SET_OPTIONS) {
      if (option.letter != '\0') collected.push(option.letter);
      if (option.letter == 'h') collected.push('r');
    }
    return collected;
  }();
  return letters;
}

fn enabled_shell_option_names(const EvalContext &cxt) throws -> String
{
  let joined = String{heap_allocator()};
  let const do_append = [&](StringView name) throws {
    if (!joined.is_empty()) joined.push(':');
    joined.append(name);
  };
  for (let const position : SHELLOPTS_POSITIONS.positions) {
    if (position == INTERACTIVE_COMMENTS_POSITION) {
      if (cxt.is_shopt_enabled("interactive_comments"))
        do_append("interactive-comments");
      continue;
    }
    let const &option = SET_OPTIONS[position];
    if (option_is_on(cxt, option)) do_append(option.name);
  }
  return joined;
}

fn enabled_shell_option_letters(const EvalContext &cxt) throws -> String
{
  let letters = String{heap_allocator()};
  for (let const &option : SET_OPTIONS) {
    if (option.letter == 'h') {
      if (option_is_on(cxt, option)) letters.push('h');
      if (cxt.restricted_enforcement_active()) letters.push('r');
      if (cxt.shell_is_interactive()) letters.push('i');
      continue;
    }
    if (option.behavior == set_option_behavior::WarningLevel) {
      for (u8 warning_level = 0; warning_level < cxt.warning_level();
           warning_level++)
        letters.push('W');
      continue;
    }
    if (option.letter == '\0' || !option_is_on(cxt, option)) continue;
    letters.push(option.letter);
  }
  if (cxt.has_execution_string()) letters.push('c');
  return letters;
}

fn apply_shell_option(EvalContext &cxt, StringView name, bool enable) throws
    -> bool
{
  const set_option_descriptor *option = find_option_by_name(name);
  if (option == nullptr || !option_is_available(cxt, *option)) return false;
  apply_or_reject_option(cxt, *option, enable);
  return true;
}

Set::Set() = default;

pure fn Set::kind() const wontthrow -> Builtin::Kind { return Kind::Set; }

fn Set::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_EXTRA_AND_RETURN(ec,
                                       format_option_switches_help().view());
  }

  if (args.count() == 1) {
    let out = String{cxt.scratch_allocator()};
    for (let const &assignment : cxt.sorted_variable_assignments()) {
      out += assignment.view();
      out += "\n";
    }
    ec.print_to_stdout(out);
    return 0;
  }

  let operands = ArrayList<String>{heap_allocator()};
  bool is_collecting_operands = false;
  bool should_rebind = false;

  usize i = 1;
  let do_read_option_value =
      [&](const String &option_arg) -> Maybe<StringView> {
    if (let const eq = option_arg.view().find_character('='); eq.has_value())
      return option_arg.view().substring(*eq + 1);
    if (i + 1 < args.count()) return args[++i].view();
    return None;
  };

  for (; i < args.count(); i++) {
    let const &arg = args[i];

    if (is_collecting_operands) {
      operands.push_managed(arg);
      continue;
    }

    if (arg == "--") {
      is_collecting_operands = true;
      should_rebind = true;
      continue;
    }

    if (arg == "--mood" || arg == "-M" ||
        arg.view().starts_with(StringView{"--mood="}) ||
        arg.view().starts_with(StringView{"-M="}))
    {
      let const value = do_read_option_value(arg);
      if (!value.has_value()) {
        ec.print_to_stdout(
            String{cxt.scratch_allocator(), mood_name(cxt.mood())} + "\n");
        continue;
      }
      let const parsed = parse_mood_name(*value);
      if (!parsed.has_value())
        throw make_error_for_arg(
            ec, i,
            String{cxt.scratch_allocator(), "Unknown --mood value '"} + *value +
                "', expected 'shit', 'bash', 'sh', or 'bash-posix'");
      cxt.set_mood(*parsed);
      cxt.note_warning_option_mutation();
      cxt.set_warning_level(0);
      cxt.apply_strictness_for_mood();
      cxt.note_explicit_mood();
      continue;
    }

    if (arg == "--init-moods" || arg == "-L" ||
        arg.view().starts_with(StringView{"--init-moods="}) ||
        arg.view().starts_with(StringView{"-L="}))
    {
      let const value = do_read_option_value(arg);
      if (!value.has_value()) {
        let out = String{cxt.scratch_allocator()};
        for (mimic_mood listed : {mimic_mood::Default, mimic_mood::Posix,
                                  mimic_mood::Bash, mimic_mood::BashPosix})
        {
          if (!cxt.mood_initialized(listed)) continue;
          if (!out.is_empty()) out += " ";
          out += mood_name(listed);
        }
        out += "\n";
        ec.print_to_stdout(out);
        continue;
      }
      let moods = ArrayList<mimic_mood>{cxt.scratch_allocator()};
      usize name_start = 0;
      for (usize j = 0; j <= value->length; j++) {
        if (j != value->length && (*value)[j] != ',') continue;
        let const name = value->substring_of_length(name_start, j - name_start);
        name_start = j + 1;
        if (name.is_empty()) continue;
        let const parsed = parse_mood_name(name);
        if (!parsed.has_value())
          throw make_error_for_arg(
              ec, i,
              String{cxt.scratch_allocator(), "Unknown --init-moods value '"} +
                  name + "', expected 'shit', 'bash', 'sh', or 'bash-posix'");
        moods.push(*parsed);
      }
      if (AST_ARENA == nullptr)
        throw Error{"Unable to source the init moods outside of a parse"};
      let const previous_mood = cxt.mood();
      source_init_moods(cxt, *AST_ARENA, moods, cxt.is_login_shell(),
                        cxt.shell_is_interactive());
      cxt.set_mood(previous_mood);
      cxt.apply_strictness_for_mood();
      continue;
    }

    if (arg == "-o" || arg == "+o") {
      apply_long_option_by_name(ec, cxt, args, i, arg[0] == '-');
      continue;
    }

    if (arg == "--options") {
      ec.print_to_stdout(format_option_table(&cxt, false));
      continue;
    }

    if (arg == "-" || arg == "+") {
      if (arg[0] == '-') {
        apply_or_reject_option(cxt, *find_option_by_letter('x'), false);
        apply_or_reject_option(cxt, *find_option_by_letter('v'), false);
      }

      is_collecting_operands = true;
      should_rebind = i + 1 < args.count();
      continue;
    }

    if (arg.length() > 1 && (arg[0] == '-' || arg[0] == '+')) {
      let const enable = arg[0] == '-';
      for (usize c = 1; c < arg.length(); c++) {
        let const letter = arg[c];

        /* The o letter ends the bundle and names one option from the next
           argument, the way bash accepts set -euo pipefail. */
        if (letter == 'o') {
          apply_long_option_by_name(ec, cxt, args, i, enable);
          break;
        }
        if (letter == 'r') {
          if (!enable && cxt.restricted_enforcement_active())
            throw make_error_for_arg(ec, i,
                                     "Restricted mode cannot be disabled");
          if (enable) cxt.activate_restricted_mode();
          continue;
        }

        let const option = find_option_by_letter(letter);
        if (option == nullptr || !option_is_available(cxt, *option)) {
          let invalid_option = String{heap_allocator()};
          invalid_option += arg[0];
          invalid_option += letter;
          if (option == nullptr)
            throw make_error_for_arg(
                ec, i, StringView{"Unknown option '"} + invalid_option + "'");
          let unavailable_error = make_error_for_arg(
              ec, i, StringView{"Unknown option '"} + invalid_option + "'");
          unavailable_error.set_command_status(2);
          throw unavailable_error;
        }
        apply_or_reject_option(cxt, *option, enable, true);
      }
      continue;
    }

    is_collecting_operands = true;
    should_rebind = true;
    operands.push_managed(arg);
  }

  if (should_rebind) {
    LOG(Debug, "set rebinding %zu positional parameters", operands.count());
    cxt.set_positional_params(steal(operands));
  }

  return 0;
}

} // namespace shit
