#include "Cli.hpp"

#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

constexpr usize HELP_WRAP_WIDTH = 80;
constexpr usize HELP_INDENT = 2;

Flag::Flag(Flag::Kind kind, char short_name, StringView long_name,
           flag_section section, StringView description)
    : m_kind(kind), m_short_name(short_name), m_section(section),
      m_long_name(long_name), m_description(description)
{}

pure fn Flag::kind() const wontthrow -> Flag::Kind { return m_kind; }

fn Flag::set_position(u32 position) throws -> void { m_position = position; }

pure fn Flag::position() const wontthrow -> usize { return m_position; }

pure fn Flag::value_location() const wontthrow -> SourceLocation
{
  return m_value_location;
}

fn Flag::set_value_location(SourceLocation location) wontthrow -> void
{
  m_value_location = steal(location);
}

pure fn Flag::short_name() const wontthrow -> char { return m_short_name; }

pure fn Flag::long_name() const wontthrow -> StringView { return m_long_name; }

pure fn Flag::section() const wontthrow -> flag_section { return m_section; }

pure fn Flag::description() const wontthrow -> StringView
{
  return m_description;
}

FlagBool::FlagBool(char short_name, StringView long_name, flag_section section,
                   StringView description)
    : Flag(Flag::Kind::Bool, short_name, long_name, section, description)
{}

FlagBool::FlagBool(FlagList &flags, char short_name, StringView long_name,
                   flag_section section, StringView description)
    : FlagBool(short_name, long_name, section, description)
{
  flags.push(this);
}

fn FlagBool::toggle() throws -> void { m_value = !m_value; }

fn FlagBool::enable() wontthrow -> void { m_value = true; }

pure fn FlagBool::is_enabled() const wontthrow -> bool { return m_value; }

fn FlagBool::reset() throws -> void
{
  m_position = 0;
  m_value_location = {};
  m_value = false;
}

FlagRepeatedBool::FlagRepeatedBool(char short_name, StringView long_name,
                                   flag_section section, StringView description)
    : Flag(Flag::Kind::RepeatedBool, short_name, long_name, section,
           description)
{
  ASSERT(long_name.is_empty());
}

FlagRepeatedBool::FlagRepeatedBool(FlagList &flags, char short_name,
                                   StringView long_name, flag_section section,
                                   StringView description)
    : FlagRepeatedBool(short_name, long_name, section, description)
{
  flags.push(this);
}

fn FlagRepeatedBool::increment() throws -> void { m_count++; }

pure fn FlagRepeatedBool::count() const wontthrow -> usize { return m_count; }

fn FlagRepeatedBool::reset() throws -> void
{
  m_position = 0;
  m_value_location = {};
  m_count = 0;
}

FlagString::FlagString(char short_name, StringView long_name,
                       flag_section section, StringView description)
    : Flag(Flag::Kind::String, short_name, long_name, section, description)
{}

FlagString::FlagString(FlagList &flags, char short_name, StringView long_name,
                       flag_section section, StringView description)
    : FlagString(short_name, long_name, section, description)
{
  flags.push(this);
}

fn FlagString::set(StringView v) throws -> void
{
  m_value = v;
  m_is_set = true;
}

pure fn FlagString::is_set() const wontthrow -> bool { return m_is_set; }

pure fn FlagString::value() const wontthrow -> StringView
{
  return m_value.view();
}

fn FlagString::reset() throws -> void
{
  m_position = 0;
  m_value_location = {};
  m_value.clear();
  m_is_set = false;
}

FlagManyStrings::FlagManyStrings(char short_name, StringView long_name,
                                 flag_section section, StringView description)
    : Flag(Flag::Kind::ManyStrings, short_name, long_name, section, description)
{}

FlagManyStrings::FlagManyStrings(FlagList &flags, char short_name,
                                 StringView long_name, flag_section section,
                                 StringView description)
    : FlagManyStrings(short_name, long_name, section, description)
{
  flags.push(this);
}

fn FlagManyStrings::append(StringView v) throws -> void
{
  m_values.push_managed(v);
}

pure fn FlagManyStrings::is_empty() const wontthrow -> bool
{
  return m_values.is_empty();
}

pure fn FlagManyStrings::count() const wontthrow -> usize
{
  return m_values.count();
}

pure fn FlagManyStrings::get(usize i) const wontthrow -> StringView
{
  ASSERT(i < m_values.count());
  return m_values[i].view();
}

fn FlagManyStrings::next() throws -> StringView
{
  ASSERT(m_value_position < m_values.count());
  let const &value = m_values[m_value_position++];
  return value.view();
}

pure fn FlagManyStrings::at_end() const wontthrow -> bool
{
  return m_value_position == count();
}

fn FlagManyStrings::reset() throws -> void
{
  m_position = 0;
  m_value_location = {};
  m_values.clear();
  m_value_position = 0;
}

static fn find_flag(const FlagList &flags, const char *flag_start, bool is_long,
                    Flag **result_flag, const char **value_start) throws -> bool
{
  usize longest_length = 0;
  let const flag_start_length = std::strlen(flag_start);

  *value_start = nullptr;
  *result_flag = nullptr;

  for (usize i = 0; i < flags.count(); ++i) {
    if (!is_long) {
      if (flags[i]->short_name() == *flag_start) {
        *result_flag = flags[i];
        *value_start = flag_start + 1;
        return true;
      }
    } else {
      if (!flags[i]->long_name().is_empty()) {
        let const flag_length = flags[i]->long_name().length;

        /* strncmp stops at the argument's NUL, so a short argument such as --f
           against the flag --foobar does not read past it. */
        if (flag_length > flag_start_length) continue;

        let const after_name = flag_start[flag_length];
        if (flag_length > longest_length &&
            std::strncmp(flags[i]->long_name().data, flag_start, flag_length) ==
                0 &&
            (after_name == '\0' || after_name == '='))
        {
          *result_flag = flags[i];
          *value_start = flag_start + flag_length;
          longest_length = flag_length;
        }
      }
    }
  }

  return longest_length > 0;
}

fn parse_flags_vec(const FlagList &flags, const ArrayList<String> &args,
                   usize base_position, const Flag *operand_value_flag,
                   const ArrayList<SourceLocation> *arg_locations,
                   ArrayList<SourceLocation> *operand_locations,
                   StringView program_name,
                   bool should_accept_negative_number_operand) throws
    -> ArrayList<String>
{
  reset_flags(flags);

  constexpr usize local_argument_capacity = 128;
  const char *local_arguments[local_argument_capacity];
  let overflow_arguments = ArrayList<const char *>{heap_allocator()};
  const char **argument_data = local_arguments;

  if (args.count() > local_argument_capacity) {
    overflow_arguments.reserve(args.count());

    for (let const &arg : args)
      overflow_arguments.push(arg.c_str());

    argument_data = overflow_arguments.begin();
  } else {
    for (usize index = 0; index < args.count(); index++)
      local_arguments[index] = args[index].c_str();
  }

  try {
    return parse_flags(flags, static_cast<int>(args.count()), argument_data,
                       base_position, operand_value_flag, arg_locations,
                       operand_locations, program_name,
                       should_accept_negative_number_operand);
  } catch (...) {
    reset_flags(flags);
    throw;
  }
}

static fn flag_name(const Flag *f, bool is_long) throws -> String
{
  let name = String{heap_allocator()};
  name += "-";
  if (is_long) {
    name += "-";
    name += f->long_name();
  } else {
    name.push(f->short_name());
  }
  return name;
}

static fn prefixed_message(StringView program_name, StringView message) throws
    -> String
{
  if (program_name.is_empty()) return String{heap_allocator(), message};
  return program_name + ": " + message;
}

static fn argument_location(
    const char *const *argv, usize argument_index, usize base_position,
    const ArrayList<SourceLocation> *arg_locations) throws -> SourceLocation
{
  if (arg_locations != nullptr && argument_index < arg_locations->count())
    return (*arg_locations)[argument_index];

  usize caret_offset = 0;
  for (usize prior_index = 0; prior_index < argument_index; prior_index++)
    caret_offset += shell_quoted_arg_length(StringView{argv[prior_index]}) + 1;
  return SourceLocation{base_position + caret_offset,
                        std::strlen(argv[argument_index])};
}

static fn attached_flag_value_location(
    const char *const *argv, usize argument_index, const char *value,
    usize base_position, const ArrayList<SourceLocation> *arg_locations) throws
    -> SourceLocation
{
  let location =
      argument_location(argv, argument_index, base_position, arg_locations);
  if (location.length != std::strlen(argv[argument_index])) return location;

  location.position += static_cast<usize>(value - argv[argument_index]);
  location.length = std::strlen(value);
  return location;
}

fn parse_flags(const FlagList &flags, int argc, const char *const *argv,
               usize base_position, const Flag *operand_value_flag,
               const ArrayList<SourceLocation> *arg_locations,
               ArrayList<SourceLocation> *operand_locations,
               StringView program_name,
               bool should_accept_negative_number_operand) throws
    -> ArrayList<String>
{
  ASSERT(argc >= 0);

  if (argc == 0) return ArrayList<String>{heap_allocator()};

  ASSERT(argv != nullptr);

  LOG(Debug, "parsing %d command line arguments", argc);

  u32 position = 0;
  let args = ArrayList<String>{heap_allocator()};

  /* When the caller asks for operand locations, each surviving operand records
     the source span of the argv token it came from, so a builtin can caret the
     specific operand after flag parsing drops the flags. */
  let const do_record_operand =
      [arg_locations, operand_locations](usize arg_index) wontthrow -> void {
    if (operand_locations == nullptr) return;
    if (arg_locations != nullptr && arg_index < arg_locations->count())
      operand_locations->push((*arg_locations)[arg_index]);
    else
      operand_locations->push(SourceLocation{});
  };

  Flag *previous_flag{};
  bool should_take_next_argument_as_value = false;
  bool was_previous_flag_long = false;
  bool should_ignore_rest = false;

  for (int i = 0; i < argc; i++) {
    ASSERT(argv[i] != nullptr);

    if (should_take_next_argument_as_value) {
      /* operand_value_flag alone lets a recognized boolean flag after it parse
         as a flag, so `-c -l command` runs command under -l. Every other flag
         takes the next argument verbatim, keeping a dash-led value intact. */
      bool is_next_known_boolean_flag = false;
      if (previous_flag == operand_value_flag &&
          operand_value_flag != nullptr && argv[i][0] == '-' &&
          argv[i][1] != '\0')
      {
        let const is_long_token = argv[i][1] == '-';
        let const token_offset = is_long_token ? &argv[i][2] : &argv[i][1];
        if (*token_offset != '\0') {
          Flag *probe_flag = nullptr;
          const char *probe_value = nullptr;
          if (find_flag(flags, token_offset, is_long_token, &probe_flag,
                        &probe_value) &&
              (probe_flag->kind() == Flag::Kind::Bool ||
               probe_flag->kind() == Flag::Kind::RepeatedBool))
          {
            is_next_known_boolean_flag = true;
          }
        }
      }

      if (!is_next_known_boolean_flag) {
        should_take_next_argument_as_value = false;

        ASSERT(previous_flag != nullptr);
        LOG(All,
            "attaching the next argument '%s' as the value of the flag '%s'",
            argv[i], flag_name(previous_flag, was_previous_flag_long).c_str());
        if (previous_flag->kind() == Flag::Kind::String)
          static_cast<FlagString *>(previous_flag)->set(argv[i]);
        else
          static_cast<FlagManyStrings *>(previous_flag)->append(argv[i]);
        previous_flag->set_position(++position);
        previous_flag->set_value_location(argument_location(
            argv, static_cast<usize>(i), base_position, arg_locations));

        continue;
      }
    }

    /* argv[0] is the invocation name even when it opens with a dash, the login
       convention that spawns a shell as -bash, so it is never a flag bundle. */
    let const argument = StringView{argv[i]};
    let const is_negative_number_operand =
        should_accept_negative_number_operand && argument.length > 1 &&
        argument[0] == '-' && argument.substring(1).is_all_decimal_digits();
    if (should_ignore_rest || argv[i][0] != '-' || i == 0 ||
        is_negative_number_operand)
    {
      /* The next operand is the script, after which every argument is a
         positional parameter for the script, the way `sh script -x` does. */
      let const is_program_name = args.is_empty();
      LOG(Debug, "taking '%s' as an operand", argv[i]);
      args.push_managed(StringView{argv[i]});
      do_record_operand(static_cast<usize>(i));
      if (!is_program_name) should_ignore_rest = true;
      continue;
    }

    bool is_long = false;
    const char *flag_offset{};

    if (argv[i][1] != '-') {
      flag_offset = &argv[i][1];
    } else {
      flag_offset = &argv[i][2];
      is_long = true;
    }

    if (*flag_offset == '\0') {
      if (is_long) {
        LOG(Debug, "stopping option parsing at '--'");
        should_ignore_rest = true;
      } else {
        args.push_managed(StringView{argv[i]});
        do_record_operand(static_cast<usize>(i));
      }

      continue;
    }

    bool should_repeat = true;

    Flag *flag{};
    const char *value_offset{};

    while (should_repeat) {
      should_repeat = false;

      let const found =
          find_flag(flags, flag_offset, is_long, &flag, &value_offset);

      if (found) {
        switch (flag->kind()) {
        case Flag::Kind::Bool: {
          let const bool_flag = static_cast<FlagBool *>(flag);

          if (is_long && *value_offset != '\0') {
            let error = ErrorWithLocation{
                argument_location(argv, static_cast<usize>(i), base_position,
                                  arg_locations),
                prefixed_message(program_name, "The flag '" +
                                                   flag_name(flag, is_long) +
                                                   "' does not take a value")};
            error.set_command_status(2);
            throw error;
          }

          bool_flag->enable();
          bool_flag->set_position(++position);
          LOG(All, "enabled the flag '%s'",
              flag_name(bool_flag, is_long).c_str());

          if (!is_long && *value_offset != '\0') {
            ++flag_offset;
            should_repeat = true;
            continue;
          }
        } break;

        case Flag::Kind::RepeatedBool: {
          let const repeated_flag = static_cast<FlagRepeatedBool *>(flag);

          repeated_flag->increment();
          repeated_flag->set_position(++position);
          LOG(All, "incremented the flag '%s'",
              flag_name(repeated_flag, is_long).c_str());

          if (!is_long && *value_offset != '\0') {
            ++flag_offset;
            should_repeat = true;
            continue;
          }
        } break;

        case Flag::Kind::String:
        case Flag::Kind::ManyStrings: {
          if (*value_offset == '\0') {
            LOG(All, "the flag '%s' expects the next argument as its value",
                flag_name(flag, is_long).c_str());
            should_take_next_argument_as_value = true;
            previous_flag = flag;
            was_previous_flag_long = is_long;
            flag->set_value_location(argument_location(
                argv, static_cast<usize>(i), base_position, arg_locations));
          } else {
            if (*value_offset == '=') {
              value_offset++;

              if (*value_offset != '\0') {
                if (flag->kind() == Flag::Kind::String)
                  static_cast<FlagString *>(flag)->set(value_offset);
                else
                  static_cast<FlagManyStrings *>(flag)->append(value_offset);

                flag->set_position(++position);
                flag->set_value_location(attached_flag_value_location(
                    argv, static_cast<usize>(i), value_offset, base_position,
                    arg_locations));
                LOG(All, "set the flag '%s' to '%s'",
                    flag_name(flag, is_long).c_str(), value_offset);
              } else {
                let error = ErrorWithLocation{
                    argument_location(argv, static_cast<usize>(i),
                                      base_position, arg_locations),
                    "No value provided for '" + flag_name(flag, is_long) +
                        "' flag"};
                error.set_command_status(2);
                throw error;
              }
            } else if (!is_long) {
              if (flag->kind() == Flag::Kind::String)
                static_cast<FlagString *>(flag)->set(value_offset);
              else
                static_cast<FlagManyStrings *>(flag)->append(value_offset);

              flag->set_position(++position);
              flag->set_value_location(attached_flag_value_location(
                  argv, static_cast<usize>(i), value_offset, base_position,
                  arg_locations));
              LOG(All, "set the flag '%s' to the attached value '%s'",
                  flag_name(flag, is_long).c_str(), value_offset);
            } else {
              let error = ErrorWithLocation{
                  argument_location(argv, static_cast<usize>(i), base_position,
                                    arg_locations),
                  "Long flags require a separator between the flag and the "
                  "value. Try using '" +
                      flag_name(flag, is_long) + "=" + value_offset + "'"};
              error.set_command_status(2);
              throw error;
            }
          }
        } break;
        }
      } else {
        if (*flag_offset == '-') {
          let error = Error{prefixed_message(
              program_name, "Missing space between '-' and other options")};
          error.set_command_status(2);
          throw error;
        } else {
          let error_message = String{heap_allocator()};
          error_message += "Unknown flag '-";

          if (!is_long) {
            error_message.push(*flag_offset);
          } else {
            error_message += "-";

            const StringView flag_view = flag_offset;
            let const equals_position = flag_view.find_character('=');

            if (equals_position.has_value())
              error_message +=
                  flag_view.substring_of_length(0, *equals_position);
            else
              error_message += flag_view;
          }
          error_message += "'";

          LOG(Debug, "rejecting the unknown flag in '%s'", argv[i]);

          let const arg_index = static_cast<usize>(i);
          let error = ErrorWithLocationAndDetails{
              argument_location(argv, arg_index, base_position, arg_locations),
              prefixed_message(program_name, error_message),
              "Use `--` before an operand that begins with a dash"};
          error.set_command_status(2);
          throw error;
        }
      }
    }
  }

  if (should_take_next_argument_as_value) {
    ASSERT(previous_flag != nullptr);
    let error = ErrorWithLocation{
        previous_flag->value_location(),
        prefixed_message(program_name,
                         "No value provided for '" +
                             flag_name(previous_flag, was_previous_flag_long) +
                             "' flag")};
    error.set_command_status(2);
    throw error;
  }

  return args;
}

pure fn arg_needs_shell_quoting(StringView arg) wontthrow -> bool
{
  if (arg.is_empty()) return true;

  for (usize i = 0; i < arg.length; i++) {
    let const character = arg[i];
    let const is_ascii_alphanumeric = (character >= 'a' && character <= 'z') ||
                                      (character >= 'A' && character <= 'Z') ||
                                      (character >= '0' && character <= '9');
    switch (character) {
    case '_':
    case '@':
    case '%':
    case '+':
    case '=':
    case ':':
    case ',':
    case '.':
    case '/':
    case '-': break;
    default:
      if (!is_ascii_alphanumeric) return true;
      break;
    }
  }

  return false;
}

pure fn shell_quoted_arg_length(StringView arg) wontthrow -> usize
{
  if (!arg_needs_shell_quoting(arg)) return arg.length;
  usize count = 2;
  for (usize i = 0; i < arg.length; i++) {
    count += arg[i] == '\'' ? 4 : 1;
  }
  return count;
}

fn append_shell_quoted_arg(String &out, StringView arg,
                           bool should_always_quote) throws -> void
{
  if (!should_always_quote && !arg_needs_shell_quoting(arg)) {
    out.append(arg);
    return;
  }
  out.push('\'');
  for (usize i = 0; i < arg.length; i++) {
    if (arg[i] == '\'')
      out += "'\\''";
    else
      out.push(arg[i]);
  }
  out.push('\'');
}

fn join_command_line(int argc, const char *const *argv) throws -> String
{
  let s = String{heap_allocator()};
  for (int i = 0; i < argc; i++) {
    if (i > 0) s.push(' ');
    append_shell_quoted_arg(s, StringView{argv[i], std::strlen(argv[i])});
  }
  return s;
}

fn reset_flags(const FlagList &flags) throws -> void
{
  for (let const flag : flags)
    switch (flag->kind()) {
    case Flag::Kind::Bool: static_cast<FlagBool *>(flag)->reset(); break;
    case Flag::Kind::RepeatedBool:
      static_cast<FlagRepeatedBool *>(flag)->reset();
      break;
    case Flag::Kind::String: static_cast<FlagString *>(flag)->reset(); break;
    case Flag::Kind::ManyStrings:
      static_cast<FlagManyStrings *>(flag)->reset();
      break;
    }
}

cold fn append_version_triple(String &out, Allocator allocator) throws -> void
{
  out += String::from(KOSH_VER_MAJOR, allocator);
  out += '.';
  out += String::from(KOSH_VER_MINOR, allocator);
  out += '.';
  out += String::from(KOSH_VER_PATCH, allocator);
  out += '-';
  out += KOSH_VER_EXTRA;
}

cold fn show_version() throws -> void
{
  let s = String{heap_allocator()};
  s += "Koshka Shell ";
  append_version_triple(s, heap_allocator());
  s += '\n';
  s += "Built on ";
  s += KOSH_BUILD_DATE;
  s += '\n';
  s += '\n';
  s += "MODE=";
  s += KOSH_BUILD_MODE;
  s += '\n';
  s += "HEAD=";
  s += KOSH_COMMIT_HASH;
  s += '\n';
  s += "CXX=";
  s += KOSH_COMPILER;
  s += '\n';
  s += "ENVCXXFLAGS=";
  s += (*KOSH_ENVCXXFLAGS == '\0' ? "<none>" : KOSH_ENVCXXFLAGS);
  s += '\n';
  s += "OS=";
  s += KOSH_OS_INFO;
  s += '\n';
  s += "RUNTIME=";
  s += os::executable_system_name();
  s += ' ';
  s += os::executable_machine_name();
  s += '\n';
  s += '\n';
  s += KOSH_SHORT_LICENSE;
  s += '\n';
  s += "(c) toiletbril <https://github.com/toiletbril>";
  s += '\n';
  s += '\n';
  s += "Report bugs at <https://github.com/toiletbril/kosh>";
  s += '\n';

  print(s);
  flush();
}

cold fn short_version_string(Allocator allocator) throws -> String
{
  let s = String{allocator};
  append_version_triple(s, allocator);
  s += '-';
  s += KOSH_BUILD_MODE;
  s += '+';
  s += StringView{KOSH_COMMIT_HASH}.substring_of_length(0, 7);

  return s;
}

cold fn show_short_version() throws -> void
{
  let s = short_version_string(heap_allocator());
  s += '\n';

  print(s);
  flush();
}

cold fn make_synopsis(StringView program_name, const SynopsisList &lines) throws
    -> String
{
  let s = String{heap_allocator()};

  s += "SYNOPSIS\n";

  for (let const line : lines) {
    s += "  ";
    s += program_name;
    s += ' ';
    s += line;
    s += '\n';
  }

  return s;
}

cold fn wrap_text(StringView text, usize indent, usize width,
                  const Maybe<usize> &continuation_indent) throws -> String
{
  let out = String{heap_allocator()};
  let line_indent = indent;
  let text_width = width > line_indent ? width - line_indent : 1;
  usize line_used = 0;
  usize word_start = 0;
  bool is_line_started = false;
  for (usize i = 0; i <= text.length; i++) {
    let const is_at_end = i == text.length;
    if (!is_at_end && text[i] != ' ') {
      continue;
    }
    let const word_length = i - word_start;
    if (word_length > 0) {
      if (is_line_started && line_used + 1 + word_length > text_width) {
        out += '\n';
        is_line_started = false;
        line_indent = continuation_indent.value_or(indent);
        text_width = width > line_indent ? width - line_indent : 1;
        line_used = 0;
      }
      if (!is_line_started) {
        out.append_repeated(' ', line_indent);
        is_line_started = true;
      } else {
        out += ' ';
        line_used++;
      }
      out += text.substring_of_length(word_start, word_length);
      line_used += word_length;
    }
    word_start = i + 1;
  }
  return out;
}

cold fn make_flag_help(const FlagList &flags) throws -> String
{
  let s = String{heap_allocator()};

  static constexpr usize DESCRIPTION_COLUMN = 26;
  static constexpr usize TEXT_WIDTH = HELP_WRAP_WIDTH - DESCRIPTION_COLUMN;

  let const do_render_flag = [&](const koshka::Flag *f) throws {
    s += "\n";

    let left = String{heap_allocator()};
    if (f->short_name() != '\0') {
      left += "  -";
      left += f->short_name();
      if (f->kind() == koshka::Flag::Kind::RepeatedBool) {
        left += "[";
        left += f->short_name();
        left += "..]";
      }
      if (!f->long_name().is_empty()) left += ", ";
    } else if (!f->long_name().is_empty()) {
      left += "      ";
    } else {
      left += "  ";
    }

    if (!f->long_name().is_empty()) {
      left += "--";
      left += f->long_name();
      switch (f->kind()) {
      case koshka::Flag::Kind::String: left += "=<...>"; break;
      case koshka::Flag::Kind::ManyStrings: left += "=<.., ..>"; break;
      case koshka::Flag::Kind::Bool:
      case koshka::Flag::Kind::RepeatedBool: break;
      }
    }

    s += left;

    if (left.length() + 2 > DESCRIPTION_COLUMN) {
      s += '\n';
      for (usize i = 0; i < DESCRIPTION_COLUMN; i++)
        s += ' ';
    } else {
      for (usize i = left.length(); i < DESCRIPTION_COLUMN; i++)
        s += ' ';
    }

    let const description = f->description();
    usize line_used = 0;
    usize word_start = 0;
    for (usize i = 0; i <= description.length; i++) {
      let const is_at_end = i == description.length;
      if (!is_at_end && description[i] != ' ') {
        continue;
      }

      let const word_length = i - word_start;
      if (word_length > 0) {
        if (line_used > 0 && line_used + 1 + word_length > TEXT_WIDTH) {
          s += '\n';
          for (usize j = 0; j < DESCRIPTION_COLUMN; j++)
            s += ' ';
          line_used = 0;
        }
        if (line_used > 0) {
          s += ' ';
          line_used++;
        }
        s += description.substring_of_length(word_start, word_length);
        line_used += word_length;
      }
      word_start = i + 1;
    }
  };

  static const StringView SECTION_HEADERS[] = {
      "OPTIONS",           "POSIX OPTIONS",
      "BASH OPTIONS",      "COMPATIBILITY OPTIONS",
      "AUXILIARY OPTIONS", "KOSHKA OPTIONS",
      "DEBUG OPTIONS"};
  for (u8 section = 0; section < countof(SECTION_HEADERS); section++) {
    bool was_header_printed = false;
    for (let const flag : flags) {
      if (static_cast<u8>(flag->section()) != section) continue;
      if (!was_header_printed) {
        if (!s.is_empty()) s += "\n\n";
        s += SECTION_HEADERS[section];
        was_header_printed = true;
      }
      do_render_flag(flag);
    }
  }

  return s;
}

fn print(StringView text) throws -> void
{
  if (!os::write_all(KOSH_STDOUT, text.data, text.count()))
    throw Error{"Unable to write to standard output: " +
                os::last_system_error_message()};
}

fn print_error(StringView text) throws -> void
{
  usize written_count = 0;
  while (written_count < text.count()) {
    let const written = os::write_fd(KOSH_STDERR, text.data + written_count,
                                     text.count() - written_count);
    if (!written.has_value() || *written == 0) {
      return;
    }
    written_count += *written;
  }
}

fn flush() throws -> void
{
  if (std::fflush(stdout) != 0)
    throw Error{"Unable to flush standard output: " +
                os::last_system_error_message()};
}

/* The first show_message consumes it, so only the leading message of a
   completion run breaks to its own line, not every message after it. */
static thread_local bool MESSAGE_LEADING_NEWLINE_ARMED = false;

fn arm_message_leading_newline(bool armed) wontthrow -> void
{
  MESSAGE_LEADING_NEWLINE_ARMED = armed;
}

cold fn show_message(StringView err) throws -> void
{
  if (err.is_empty()) return;

  os::signal_internal_diagnostic();

  if (MESSAGE_LEADING_NEWLINE_ARMED) {
    print_error("\n");
    MESSAGE_LEADING_NEWLINE_ARMED = false;
  }
  print_error(err);
  print_error("\n");
}

} /* namespace koshka */
