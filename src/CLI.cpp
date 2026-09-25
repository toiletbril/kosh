/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements command-line and builtin option parsing. It owns flag
 * declarations, help rendering, operand collection, validation, and located
 * usage errors.
 */

#include "CLI.hpp"

#include "CLIColors.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

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

fn FlagManyStrings::append(StringView v, usize position,
                           SourceLocation location) throws -> void
{
  m_values.push_managed(v);
  m_positions.push(position);
  m_locations.push(steal(location));
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

pure fn FlagManyStrings::get_position(usize i) const wontthrow -> usize
{
  ASSERT(i < m_positions.count());
  return m_positions[i];
}

pure fn FlagManyStrings::get_location(usize i) const wontthrow -> SourceLocation
{
  ASSERT(i < m_locations.count());
  return m_locations[i];
}

fn FlagManyStrings::take_next() wontthrow -> String
{
  ASSERT(m_value_position < m_values.count());
  return steal(m_values[m_value_position++]);
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
  m_positions.clear();
  m_locations.clear();
  m_value_position = 0;
}

FlagOptionalValue::FlagOptionalValue(char short_name, StringView long_name,
                                     flag_section section,
                                     StringView description,
                                     value_acceptor should_accept_value,
                                     StringView value_name)
    : Flag(Flag::Kind::OptionalValue, short_name, long_name, section,
           description),
      m_should_accept_value(should_accept_value), m_value_name(value_name)
{}

FlagOptionalValue::FlagOptionalValue(FlagList &flags, char short_name,
                                     StringView long_name, flag_section section,
                                     StringView description,
                                     value_acceptor should_accept_value,
                                     StringView value_name)
    : FlagOptionalValue(short_name, long_name, section, description,
                        should_accept_value, value_name)
{
  flags.push(this);
}

fn FlagOptionalValue::enable() wontthrow -> void
{
  m_value.clear();
  m_has_value = false;
  m_is_enabled = true;
}

fn FlagOptionalValue::set(StringView value) throws -> void
{
  m_value = value;
  m_has_value = true;
  m_is_enabled = true;
}

pure fn FlagOptionalValue::is_enabled() const wontthrow -> bool
{
  return m_is_enabled;
}

pure fn FlagOptionalValue::has_value() const wontthrow -> bool
{
  return m_has_value;
}

pure fn FlagOptionalValue::value() const wontthrow -> StringView
{
  return m_value.view();
}

pure fn FlagOptionalValue::value_name() const wontthrow -> StringView
{
  return m_value_name;
}

pure fn FlagOptionalValue::should_accept_value(StringView value) const wontthrow
    -> bool
{
  return m_should_accept_value(value);
}

fn FlagOptionalValue::reset() throws -> void
{
  m_position = 0;
  m_value_location = {};
  m_value.clear();
  m_is_enabled = false;
  m_has_value = false;
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
                   bool should_accept_negative_number_operand,
                   bool should_allow_options_after_operands,
                   bool should_accept_unknown_flag_operand,
                   Allocator allocator) throws -> ArrayList<String>
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
                       should_accept_negative_number_operand,
                       should_allow_options_after_operands,
                       should_accept_unknown_flag_operand, allocator);
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

  location.position += static_cast<u32>(value - argv[argument_index]);
  location.length = static_cast<u32>(std::strlen(value));
  return location;
}

static fn boolean_flag_location(
    const char *const *argv, usize argument_index, const char *flag_offset,
    bool is_long, usize base_position,
    const ArrayList<SourceLocation> *arg_locations) throws -> SourceLocation
{
  let location =
      argument_location(argv, argument_index, base_position, arg_locations);
  let const argument_length = std::strlen(argv[argument_index]);
  if (!is_long && argument_length > 2 && location.length == argument_length) {
    location.position += static_cast<u32>(flag_offset - argv[argument_index]);
    location.length = 1;
  }

  return location;
}

fn parse_flags(const FlagList &flags, int argc, const char *const *argv,
               usize base_position, const Flag *operand_value_flag,
               const ArrayList<SourceLocation> *arg_locations,
               ArrayList<SourceLocation> *operand_locations,
               StringView program_name,
               bool should_accept_negative_number_operand,
               bool should_allow_options_after_operands,
               bool should_accept_unknown_flag_operand,
               Allocator allocator) throws -> ArrayList<String>
{
  ASSERT(argc >= 0);

  if (argc == 0) return ArrayList<String>{allocator};

  ASSERT(argv != nullptr);

  LOG(Debug, "parsing %d command line arguments", argc);

  u32 position = 0;
  let args = ArrayList<String>{allocator};

  /* When the caller asks for operand locations, each surviving operand records
     the source span of the argv token it came from, so a builtin can caret the
     specific operand after flag parsing drops the flags. */
  let const do_record_operand =
      [argv, base_position, arg_locations, operand_locations](usize arg_index)
          throws -> void {
    if (operand_locations == nullptr) return;

    operand_locations->push(
        argument_location(argv, arg_index, base_position, arg_locations));
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
        let const value_position = ++position;
        let const value_location = argument_location(
            argv, static_cast<usize>(i), base_position, arg_locations);
        if (previous_flag->kind() == Flag::Kind::String)
          static_cast<FlagString *>(previous_flag)->set(argv[i]);
        else
          static_cast<FlagManyStrings *>(previous_flag)
              ->append(argv[i], value_position, value_location);
        previous_flag->set_position(value_position);
        previous_flag->set_value_location(value_location);

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
      if (!is_program_name && !should_allow_options_after_operands)
        should_ignore_rest = true;
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
          bool_flag->set_value_location(
              boolean_flag_location(argv, static_cast<usize>(i), flag_offset,
                                    is_long, base_position, arg_locations));
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
          repeated_flag->set_value_location(
              boolean_flag_location(argv, static_cast<usize>(i), flag_offset,
                                    is_long, base_position, arg_locations));
          LOG(All, "incremented the flag '%s'",
              flag_name(repeated_flag, is_long).c_str());

          if (!is_long && *value_offset != '\0') {
            ++flag_offset;
            should_repeat = true;
            continue;
          }
        } break;

        case Flag::Kind::OptionalValue: {
          let const optional_flag = static_cast<FlagOptionalValue *>(flag);
          optional_flag->enable();
          optional_flag->set_position(++position);
          optional_flag->set_value_location(
              boolean_flag_location(argv, static_cast<usize>(i), flag_offset,
                                    is_long, base_position, arg_locations));

          let value = StringView{value_offset};
          if (!value.is_empty() && value[0] == '=') value = value.substring(1);
          if (!value.is_empty() && optional_flag->should_accept_value(value)) {
            optional_flag->set(value);
            optional_flag->set_value_location(attached_flag_value_location(
                argv, static_cast<usize>(i), value.data, base_position,
                arg_locations));
          } else if (!value.is_empty() && is_long) {
            let error = ErrorWithLocation{
                argument_location(argv, static_cast<usize>(i), base_position,
                                  arg_locations),
                prefixed_message(program_name,
                                 "Invalid value '" + String{value} + "' for '" +
                                     flag_name(flag, true) + "'")};
            error.set_command_status(2);
            throw error;
          } else if (!value.is_empty()) {
            ++flag_offset;
            should_repeat = true;
          } else if (i + 1 < argc && optional_flag->should_accept_value(
                                         StringView{argv[i + 1]}))
          {
            i++;
            let const value_location = argument_location(
                argv, static_cast<usize>(i), base_position, arg_locations);
            optional_flag->set(StringView{argv[i]});
            optional_flag->set_value_location(value_location);
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
                let const value_position = ++position;
                if (flag->kind() == Flag::Kind::String)
                  static_cast<FlagString *>(flag)->set(value_offset);
                else {
                  let const value_location = attached_flag_value_location(
                      argv, static_cast<usize>(i), value_offset, base_position,
                      arg_locations);
                  static_cast<FlagManyStrings *>(flag)->append(
                      value_offset, value_position, value_location);
                }

                flag->set_position(value_position);
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
              let const value_position = ++position;
              if (flag->kind() == Flag::Kind::String)
                static_cast<FlagString *>(flag)->set(value_offset);
              else {
                let const value_location = attached_flag_value_location(
                    argv, static_cast<usize>(i), value_offset, base_position,
                    arg_locations);
                static_cast<FlagManyStrings *>(flag)->append(
                    value_offset, value_position, value_location);
              }

              flag->set_position(value_position);
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
        if (should_accept_unknown_flag_operand) {
          LOG(Debug, "taking the unknown flag '%s' as an operand", argv[i]);
          args.push_managed(StringView{argv[i]});
          do_record_operand(static_cast<usize>(i));
          should_ignore_rest = true;
          break;
        }

        if (*flag_offset == '-') {
          let error = Error{prefixed_message(
              program_name, "Missing space between '-' and other options")};
          error.set_command_status(2);
          throw error;
        } else {
          let error_message = String{heap_allocator()};
          error_message += "Unknown flag '-";

          if (!is_long) {
            let const flag_view = StringView{flag_offset};
            let const decoded = utils::decode_utf8(flag_view, 0, 0);
            let const byte_count = decoded.length > 1 ? decoded.length : 1;
            error_message += flag_view.substring_of_length(0, byte_count);
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

fn parse_util_operands(const FlagList &flags, const ArrayList<String> &args,
                       Allocator allocator,
                       const ArrayList<SourceLocation> *arg_locations,
                       bool should_accept_negative_number_operand,
                       bool should_allow_options_after_operands,
                       bool should_accept_unknown_flag_operand) throws
    -> util_operands_result
{
  let operand_locations = ArrayList<SourceLocation>{allocator};
  let operands = parse_flags_vec(flags, args, 0, nullptr, arg_locations,
                                 &operand_locations, {},
                                 should_accept_negative_number_operand,
                                 should_allow_options_after_operands,
                                 should_accept_unknown_flag_operand, allocator);
  ASSERT(operands.allocator() == allocator);
  ASSERT(operand_locations.allocator() == allocator);
  ASSERT(operands.count() == operand_locations.count());

  if (!operands.is_empty()) operands.remove(0);
  if (!operand_locations.is_empty()) operand_locations.remove(0);

  return util_operands_result{steal(operands), steal(operand_locations)};
}

fn parse_until_subcommand(const FlagList &flags, const ArrayList<String> &args,
                          const ArrayList<SourceLocation> *arg_locations,
                          ArrayList<SourceLocation> *operand_locations,
                          StringView program_name) throws -> usize
{
  let local_operand_locations = ArrayList<SourceLocation>{heap_allocator()};
  let operands =
      parse_flags_vec(flags, args, 0, nullptr, arg_locations,
                      operand_locations != nullptr ? operand_locations
                                                   : &local_operand_locations,
                      program_name);
  if (operands.count() <= 1) return args.count();

  let const &locations = operand_locations != nullptr ? *operand_locations
                                                      : local_operand_locations;
  let const subcommand_operand = operands[1].view();
  let const subcommand_location = locations[1];
  if (arg_locations != nullptr) {
    for (usize index = 0; index < arg_locations->count(); index++) {
      if ((*arg_locations)[index].position == subcommand_location.position &&
          (*arg_locations)[index].length == subcommand_location.length)
        return index;
    }
  }

  for (usize index = 0; index < args.count(); index++)
    if (args[index].view() == subcommand_operand) return index;

  return args.count();
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
    case Flag::Kind::OptionalValue:
      static_cast<FlagOptionalValue *>(flag)->reset();
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

fn parse_cli_color_mode(StringView text) wontthrow -> Maybe<cli_color_mode>
{
  constexpr static_string_entry<cli_color_mode> MODE_ENTRIES[] = {
      {SSK("auto"),   cli_color_mode::Auto  },
      {SSK("tty"),    cli_color_mode::Auto  },
      {SSK("if-tty"), cli_color_mode::Auto  },
      {SSK("always"), cli_color_mode::Always},
      {SSK("yes"),    cli_color_mode::Always},
      {SSK("force"),  cli_color_mode::Always},
      {SSK("never"),  cli_color_mode::Never },
      {SSK("no"),     cli_color_mode::Never },
      {SSK("none"),   cli_color_mode::Never },
  };
  constexpr StaticStringMap MODES{MODE_ENTRIES};
  return MODES.find(text);
}

fn stdout_wants_color(cli_color_mode mode) throws -> bool
{
  switch (mode) {
  case cli_color_mode::Always: return true;
  case cli_color_mode::Never: return false;
  case cli_color_mode::Auto: return colors::stdout_wants_color();
  }

  unreachable("invalid CLI color mode %d", ENUM(mode));
}

pure fn get_tree_connector(bool is_last) wontthrow -> tree_connector
{
  return is_last ? tree_connector{"└── ", "    "}
                 : tree_connector{"├── ", "│   "};
}

fn append_report_text(String &output, StringView text, StringView style,
                      bool should_color) throws -> void
{
  if (text.is_empty()) return;

  let const is_styled = should_color && !style.is_empty();
  if (is_styled) output += style;
  output += text;
  if (is_styled) output += colors::ansi::RESET;
}

fn append_report_column(String &output, StringView text, usize width,
                        bool is_right_aligned, StringView style,
                        bool should_color) throws -> void
{
  let const text_width = toiletline::get_display_width(text);
  let const padding_length = text_width < width ? width - text_width : 0;
  if (is_right_aligned) output.append_repeated(' ', padding_length);

  append_report_text(output, text, style, should_color);

  if (!is_right_aligned) output.append_repeated(' ', padding_length);
}

fn append_report_field(String &output, StringView name, StringView value,
                       StringView style, bool should_color) throws -> void
{
  if (value.is_empty()) return;

  append_report_text(output, name, style, should_color);
  output += ": ";
  output += value;
  output += '\n';
}

fn append_report_inline_field(String &output, StringView name, StringView value,
                              StringView style, bool should_color) throws
    -> void
{
  if (value.is_empty()) return;

  append_report_text(output, name, style, should_color);
  output += ": ";
  output += value;
}

fn ReportTable::add_column(StringView heading, report_table_alignment alignment,
                           StringView style) throws -> void
{
  m_columns.push({
      String{m_columns.allocator(), heading},
      alignment, style
  });
}

fn ReportTable::add_row(const ArrayList<report_table_cell_view> &cells) throws
    -> void
{
  let row = ArrayList<report_table_cell>{m_grid_rows.allocator()};
  row.reserve(cells.count());
  for (let const &cell : cells)
    row.push({
        String{row.allocator(), cell.text},
        cell.style
    });
  m_grid_rows.push(steal(row));
}

static fn append_report_grid(
    String &output, const ArrayList<report_table_column> &columns,
    const ArrayList<ArrayList<report_table_cell>> &rows, bool should_color,
    StringView indentation, bool should_show_header,
    usize column_gap_space_count) throws -> void
{
  let widths = ArrayList<usize>{columns.allocator()};
  widths.reserve(columns.count());
  for (let const &column : columns)
    widths.push(toiletline::get_display_width(column.heading.view()));

  for (let const &row : rows) {
    for (usize index = 0; index < columns.count(); index++) {
      if (index >= row.count()) continue;
      let const width = toiletline::get_display_width(row[index].text.view());
      if (widths[index] < width) widths[index] = width;
    }
  }

  let const append_grid_column = [&](String &target, StringView text,
                                     usize width, bool is_right_aligned,
                                     StringView style) throws -> void {
    let const text_width = toiletline::get_display_width(text);
    let const padding_length = text_width < width ? width - text_width : 0;
    if (is_right_aligned) target.append_repeated(' ', padding_length);
    append_report_text(target, text, style, should_color);
    if (!is_right_aligned) target.append_repeated(' ', padding_length);
  };

  let const append_row = [&](const ArrayList<report_table_cell> &row) throws {
    output += indentation;
    for (usize index = 0; index < columns.count(); index++) {
      let const text =
          index < row.count() ? row[index].text.view() : StringView{};
      let const style = index < row.count() && !row[index].style.is_empty()
                            ? row[index].style
                            : columns[index].style;
      append_grid_column(
          output, text, widths[index],
          columns[index].alignment == report_table_alignment::Right, style);
      if (index + 1 < columns.count())
        output.append_repeated(' ', column_gap_space_count);
    }
    output += '\n';
  };

  if (should_show_header) {
    let header = ArrayList<report_table_cell>{columns.allocator()};
    header.reserve(columns.count());
    for (let const &column : columns)
      header.push({
          String{header.allocator(), column.heading.view()},
          {}
      });
    append_row(header);
  }
  for (let const &row : rows)
    append_row(row);
}

fn ReportTable::to_string(bool should_color,
                          StringView indentation) const throws -> String
{
  let output = String{m_columns.allocator()};
  append_report_grid(output, m_columns, m_grid_rows, should_color, indentation,
                     m_should_show_header, m_column_gap_space_count);
  return output;
}

fn ReportTable::to_string() const throws -> String { return to_string(false); }

fn append_titled_report_table(String &output, StringView title,
                              const ReportTable &table,
                              bool should_color) throws -> void
{
  if (!output.is_empty()) {
    while (!output.is_empty() && output.back() == '\n')
      output.truncate(output.length() - 1);
    output += "\n\n";
  }
  append_report_text(output, title, colors::ansi::BOLD_BLUE, should_color);
  output += '\n';
  output += table.to_string(should_color, "  ").view();
}

static pure fn report_indentation_width(StringView indentation) wontthrow
    -> usize
{
  usize width = 0;
  for (usize index = 0; index < indentation.length; index++) {
    let const byte = indentation[index];
    if (byte == '\t') {
      width += 8 - (width % 8);
      continue;
    }

    width++;
  }
  return width;
}

fn append_report_name_section(String &output, StringView title,
                              const ArrayList<StringView> &names,
                              bool should_color, StringView indentation) throws
    -> void
{
  output += indentation;
  append_report_text(output, title, colors::ansi::BOLD_BLUE, should_color);
  output += '\n';

  usize longest_length = 0;
  for (let const name : names)
    if (name.length > longest_length) longest_length = name.length;

  let const column_width = longest_length + 2;
  let const indentation_width = report_indentation_width(indentation) + 2;
  let const available_width =
      indentation_width < 80 ? 80 - indentation_width : usize{1};
  let const column_count = column_width >= available_width
                               ? usize{1}
                               : available_width / column_width;
  for (usize index = 0; index < names.count(); index++) {
    let const name = names[index];
    if (index % column_count == 0) {
      output += indentation;
      output += "  ";
    }
    output += name;
    let const is_last_in_row =
        index % column_count == column_count - 1 || index + 1 == names.count();
    if (is_last_in_row) {
      output += '\n';
    } else {
      output.append_repeated(' ', column_width - name.length);
    }
  }
  output += '\n';
}

fn append_indented_report(String &output, StringView report,
                          StringView indentation) throws -> void
{
  usize line_start = 0;
  bool is_title = true;
  while (line_start < report.length) {
    usize line_end = line_start;
    while (line_end < report.length && report[line_end] != '\n')
      line_end++;

    if (!is_title && line_end > line_start) output += indentation;
    output += report.substring_of_length(line_start, line_end - line_start);
    if (line_end < report.length) output += '\n';

    is_title = false;
    line_start = line_end + 1;
  }
}

fn append_report_body(String &output, StringView body,
                      StringView indentation) throws -> void
{
  usize line_start = 0;
  while (line_start < body.length) {
    usize line_end = line_start;
    while (line_end < body.length && body[line_end] != '\n')
      line_end++;

    if (line_end > line_start) output += indentation;
    output += body.substring_of_length(line_start, line_end - line_start);
    if (line_end < body.length) output += '\n';
    line_start = line_end + 1;
  }
}

fn format_cli_help(StringView text, bool should_color) throws -> String
{
  if (!should_color) return String{text};

  enum class help_section : u8
  {
    Other,
    Description,
    Synopsis,
  };

  let output = String{heap_allocator()};
  help_section section = help_section::Other;
  usize line_start = 0;
  while (line_start < text.length) {
    usize line_end = line_start;
    while (line_end < text.length && text[line_end] != '\n')
      line_end++;
    let const line =
        text.substring_of_length(line_start, line_end - line_start);

    bool is_heading = !line.is_empty();
    bool has_heading_letter = false;
    for (usize index = 0; index < line.length && is_heading; index++) {
      let const byte = line[index];
      has_heading_letter = has_heading_letter || (byte >= 'A' && byte <= 'Z');
      is_heading = byte == ' ' || (byte >= 'A' && byte <= 'Z');
    }
    is_heading = is_heading && has_heading_letter;

    if (is_heading) {
      append_report_text(output, line, colors::ansi::BOLD_BLUE, true);
      if (line == "DESCRIPTION")
        section = help_section::Description;
      else if (line == "SYNOPSIS")
        section = help_section::Synopsis;
      else
        section = help_section::Other;
    } else if (!line.is_empty() && section == help_section::Description) {
      output += line;
    } else if (!line.is_empty() && section == help_section::Synopsis) {
      append_report_text(output, line, colors::ansi::GREEN, true);
    } else {
      output += line;
    }

    if (line_end < text.length) output += '\n';
    line_start = line_end + 1;
  }

  return output;
}

fn format_cli_help(StringView text) throws -> String
{
  return format_cli_help(text, colors::stdout_wants_color());
}

static fn write_alternate_screen_sequence(const ExecContext &ec,
                                          StringView sequence) wontthrow -> bool
{
  let descriptor = KOSH_STDOUT;
  if (ec.out_fd.has_value()) descriptor = *ec.out_fd;

  return os::write_all(descriptor, sequence.data, sequence.length);
}

fn enter_alternate_screen(const ExecContext &ec) wontthrow -> bool
{
  return write_alternate_screen_sequence(ec, "\x1b[?1049h");
}

fn leave_alternate_screen(const ExecContext &ec) wontthrow -> void
{
  unused(write_alternate_screen_sequence(ec, "\x1b[?1049l"));
}

fn hide_cursor(const ExecContext &ec) wontthrow -> bool
{
  return write_alternate_screen_sequence(ec, "\x1b[?25l");
}

fn show_cursor(const ExecContext &ec) wontthrow -> void
{
  unused(write_alternate_screen_sequence(ec, "\x1b[?25h"));
}

fn append_live_controls_bar(String &output, StringView sample_label,
                            StringView refresh_label, bool should_color) throws
    -> void
{
  append_report_text(output,
                     "ctrl+c to exit. cumulative stats over " + sample_label +
                         " every " + refresh_label,
                     colors::ansi::DIM, should_color);
  output += "\n";
  output += "\n";
}

fn format_live_duration(f64 seconds, Allocator allocator) throws -> String
{
  let const nanoseconds = static_cast<u64>(seconds * 1000000000.0 + 0.5);
  let result = String::from(nanoseconds / 1000000000ULL, allocator);
  u64 fraction = nanoseconds % 1000000000ULL;
  if (fraction != 0) {
    usize digit_count = 9;
    while (fraction % 10 == 0) {
      fraction /= 10;
      digit_count--;
    }
    let const fraction_text = String::from(fraction, allocator);
    result += ".";
    for (usize index = fraction_text.length(); index < digit_count; index++)
      result += "0";
    result += fraction_text.view();
  }

  result += "s";
  return result;
}

pure fn find_rolling_window_boundary(const ArrayList<u64> &timestamps,
                                     u64 window_start) wontthrow
    -> rolling_window_boundary
{
  ASSERT(!timestamps.is_empty());
  if (window_start <= timestamps[0]) return {0, 0, timestamps[0]};

  usize before = 0;
  while (before + 1 < timestamps.count() &&
         timestamps[before + 1] <= window_start)
    before++;

  if (before + 1 == timestamps.count())
    return {before, before, timestamps[before]};

  return {before, before + 1, window_start};
}

pure fn interpolate_rolling_counter(u64 before, u64 after, u64 before_timestamp,
                                    u64 after_timestamp,
                                    u64 target_timestamp) wontthrow
    -> Maybe<u64>
{
  if (after < before) return None;
  if (target_timestamp <= before_timestamp) return before;
  if (target_timestamp >= after_timestamp) return after;
  if (after_timestamp <= before_timestamp) return before;

  let const elapsed = after_timestamp - before_timestamp;
  let const passed = target_timestamp - before_timestamp;
  return before +
         static_cast<u64>(static_cast<u128>(after - before) * passed / elapsed);
}

pure fn rolling_window_start(u64 now_nanoseconds,
                             u64 window_nanoseconds) wontthrow -> u64
{
  return now_nanoseconds > window_nanoseconds
             ? now_nanoseconds - window_nanoseconds
             : 0;
}

cold fn make_flag_help(const FlagList &flags, bool should_color) throws
    -> String
{
  let s = String{heap_allocator()};

  static constexpr usize DESCRIPTION_COLUMN = 40;
  static constexpr usize TEXT_WIDTH = HELP_WRAP_WIDTH - DESCRIPTION_COLUMN;

  let const do_render_flag = [&](const koshka::Flag *f) throws {
    s += "\n";

    let flag_name = String{heap_allocator()};
    let flag_value = String{heap_allocator()};
    if (f->short_name() != '\0') {
      flag_name += "  -";
      flag_name += f->short_name();
      if (f->kind() == koshka::Flag::Kind::RepeatedBool) {
        flag_name += "[";
        flag_name += f->short_name();
        flag_name += "..]";
      }
      if (!f->long_name().is_empty()) flag_name += ", ";
    } else if (!f->long_name().is_empty()) {
      flag_name += "      ";
    } else {
      flag_name += "  ";
    }

    if (!f->long_name().is_empty()) {
      flag_name += "--";
      flag_name += f->long_name();
      switch (f->kind()) {
      case koshka::Flag::Kind::String: flag_value += "=<...>"; break;
      case koshka::Flag::Kind::ManyStrings: flag_value += "=<.., ..>"; break;
      case koshka::Flag::Kind::OptionalValue: {
        flag_value += "[=<";
        flag_value += static_cast<const FlagOptionalValue *>(f)->value_name();
        flag_value += ">]";
        break;
      }
      case koshka::Flag::Kind::Bool:
      case koshka::Flag::Kind::RepeatedBool: break;
      }
    }

    append_report_text(s, flag_name.view(), {}, should_color);
    append_report_text(s, flag_value.view(), colors::ansi::DIM, should_color);

    let const flag_width = flag_name.length() + flag_value.length();
    if (flag_width + 2 > DESCRIPTION_COLUMN) {
      s += '\n';
      for (usize i = 0; i < DESCRIPTION_COLUMN; i++)
        s += ' ';
    } else {
      for (usize i = flag_width; i < DESCRIPTION_COLUMN; i++)
        s += ' ';
    }

    let description_text = String{heap_allocator()};
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
          description_text += '\n';
          for (usize j = 0; j < DESCRIPTION_COLUMN; j++)
            description_text += ' ';
          line_used = 0;
        }
        if (line_used > 0) {
          description_text += ' ';
          line_used++;
        }
        description_text +=
            description.substring_of_length(word_start, word_length);
        line_used += word_length;
      }
      word_start = i + 1;
    }
    append_report_text(s, description_text.view(), {}, should_color);
  };

  static const StringView SECTION_HEADERS[] = {
      "OPTIONS",           "POSIX OPTIONS",
      "BASH OPTIONS",      "COMPATIBILITY OPTIONS",
      "AUXILIARY OPTIONS", "KOSHKA OPTIONS",
      "LIVE OPTIONS",      "DEBUG OPTIONS"};
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

fn show_warning(StringView warning) throws -> void
{
  show_message(Warning{warning}.to_string());
}

} /* namespace koshka */
