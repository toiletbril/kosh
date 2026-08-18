#pragma once

#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"

#define FLAG_LIST T__FLAG_LIST

#define HELP_SYNOPSIS T__FLAG_HELP_SYNOPSIS

#define HELP_SYNOPSIS_DECL(...)                                                \
  static koshka::SynopsisList HELP_SYNOPSIS { __VA_ARGS__ }

#define HELP_DESCRIPTION T__FLAG_HELP_DESCRIPTION

#define HELP_DESCRIPTION_DECL(text)                                            \
  [[maybe_unused]] static constexpr koshka::StringView HELP_DESCRIPTION { text }

#define FLAG_LIST_DECL() static koshka::FlagList FLAG_LIST

/* FLAG takes an optional flag_section argument before the description. The
   section is named unqualified, such as Compat, and the macro prepends
   koshka::flag_section. The five-argument form defaults to NoSection, which
   renders the flag at the top of --help with no section heading. The
   six-argument form names the section the flag renders under. */
#define T__FLAG_SELECT(_1, _2, _3, _4, _5, _6, name, ...) name
#define FLAG(...)                                         T__FLAG_SELECT(__VA_ARGS__, T__FLAG6, T__FLAG5)(__VA_ARGS__)
#define T__FLAG5(var_name, kind, short_name, long_name, description)           \
  T__FLAG6(var_name, kind, short_name, long_name, NoSection, description)
#define T__FLAG6(var_name, kind, short_name, long_name, section, description)  \
  static koshka::Flag##kind concat_literal(FLAG_, var_name)                    \
  {                                                                            \
    FLAG_LIST, short_name, long_name, koshka::flag_section::section,           \
        description                                                            \
  }

namespace koshka {

class Flag;

class SynopsisList
{
public:
  SynopsisList(std::initializer_list<StringView> lines) wontthrow
  {
    if (lines.size() > countof(m_lines))
      TRAP("the help synopsis exceeds its fixed capacity");
    for (let const line : lines)
      m_lines[m_count++] = line;
  }

  pure fn count() const wontthrow -> usize { return m_count; }
  pure fn begin() const wontthrow -> const StringView * { return m_lines; }
  pure fn end() const wontthrow -> const StringView *
  {
    return m_lines + m_count;
  }
  pure fn operator[](usize index) const wontthrow->StringView
  {
    ASSERT(index < m_count);
    return m_lines[index];
  }

private:
  StringView m_lines[4]{};
  usize m_count{0};
};

class FlagList
{
public:
  fn push(Flag *flag) wontthrow -> void
  {
    if (m_count >= countof(m_flags))
      TRAP("the flag registry exceeds its fixed capacity");
    m_flags[m_count++] = flag;
  }

  pure fn count() const wontthrow -> usize { return m_count; }
  pure fn begin() const wontthrow -> Flag *const * { return m_flags; }
  pure fn end() const wontthrow -> Flag *const * { return m_flags + m_count; }
  pure fn operator[](usize index) const wontthrow->Flag *
  {
    ASSERT(index < m_count);
    return m_flags[index];
  }

private:
  Flag *m_flags[64]{};
  usize m_count{0};
};

/* The order here is the order the sections print in. */
enum class flag_section : u8
{
  NoSection,
  Posix,
  Bash,
  Compat,
  Auxiliary,
  Kosh,
  Debug,
};

extern const usize HELP_WRAP_WIDTH;
extern const usize HELP_INDENT;

class Flag
{
public:
  enum class Kind : u8
  {
    Bool,
    RepeatedBool,
    String,
    ManyStrings,
  };

  pure fn kind() const wontthrow -> Kind;
  pure fn position() const wontthrow -> usize;
  fn set_position(u32 position) throws -> void;
  pure fn value_location() const wontthrow -> SourceLocation;
  fn set_value_location(SourceLocation location) wontthrow -> void;
  pure fn short_name() const wontthrow -> char;
  pure fn long_name() const wontthrow -> StringView;
  pure fn section() const wontthrow -> flag_section;
  pure fn description() const wontthrow -> StringView;

protected:
  Flag(Kind type, char short_name, StringView long_name, flag_section section,
       StringView description);

  Kind m_kind;
  usize m_position{0}; /* 0 if it wasn't specified. */
  SourceLocation m_value_location{};
  char m_short_name;
  flag_section m_section;
  StringView m_long_name;
  StringView m_description;
};

class FlagBool : public Flag
{
public:
  FlagBool(FlagList &flags, char short_name, StringView long_name,
           flag_section section, StringView description);
  FlagBool(char short_name, StringView long_name, flag_section section,
           StringView description);

  fn enable() wontthrow -> void;
  fn toggle() throws -> void;
  pure fn is_enabled() const wontthrow -> bool;

  fn reset() throws -> void;

private:
  bool m_value{false};
};

class FlagRepeatedBool : public Flag
{
public:
  FlagRepeatedBool(FlagList &flags, char short_name, StringView long_name,
                   flag_section section, StringView description);
  FlagRepeatedBool(char short_name, StringView long_name, flag_section section,
                   StringView description);

  fn increment() throws -> void;
  pure fn count() const wontthrow -> usize;

  fn reset() throws -> void;

private:
  usize m_count{0};
};

class FlagString : public Flag
{
public:
  FlagString(FlagList &flags, char short_name, StringView long_name,
             flag_section section, StringView description);
  FlagString(char short_name, StringView long_name, flag_section section,
             StringView description);

  fn set(StringView v) throws -> void;
  pure fn is_set() const wontthrow -> bool;
  pure fn value() const wontthrow -> StringView;

  fn reset() throws -> void;

private:
  bool m_is_set{false};
  String m_value{heap_allocator()};
};

class FlagManyStrings : public Flag
{
public:
  FlagManyStrings(FlagList &flags, char short_name, StringView long_name,
                  flag_section section, StringView description);
  FlagManyStrings(char short_name, StringView long_name, flag_section section,
                  StringView description);

  fn append(StringView v) throws -> void;
  pure fn count() const wontthrow -> usize;
  pure fn is_empty() const wontthrow -> bool;

  pure fn get(usize i) const wontthrow -> StringView;

  fn next() throws -> StringView;
  pure fn at_end() const wontthrow -> bool;
  pure fn value_position() const wontthrow -> usize { return m_value_position; }

  fn reset() throws -> void;

private:
  ArrayList<String> m_values{heap_allocator()};
  usize m_value_position{0};
};

/* operand_value_flag names the one flag whose value is read from the first
   non-option operand the way the shell's -c command is, so a recognized boolean
   flag that follows it is parsed as a flag rather than swallowed as the value.
   It is null for every builtin, which take an option value from the next
   argument verbatim the way bash's getopt does. */
fn parse_flags_vec(const FlagList &flags, const ArrayList<String> &args,
                   usize base_position = 0,
                   const Flag *operand_value_flag = nullptr,
                   const ArrayList<SourceLocation> *arg_locations = nullptr,
                   ArrayList<SourceLocation> *operand_locations = nullptr,
                   StringView program_name = StringView{},
                   bool should_accept_negative_number_operand = false) throws
    -> ArrayList<String>;
fn parse_flags(const FlagList &flags, int argc, const char *const *argv,
               usize base_position = 0,
               const Flag *operand_value_flag = nullptr,
               const ArrayList<SourceLocation> *arg_locations = nullptr,
               ArrayList<SourceLocation> *operand_locations = nullptr,
               StringView program_name = StringView{},
               bool should_accept_negative_number_operand = false) throws
    -> ArrayList<String>;

fn join_command_line(int argc, const char *const *argv) throws -> String;

pure fn arg_needs_shell_quoting(StringView arg) wontthrow -> bool;
pure fn shell_quoted_arg_length(StringView arg) wontthrow -> usize;
fn append_shell_quoted_arg(String &out, StringView arg,
                           bool should_always_quote = false) throws -> void;

fn reset_flags(const FlagList &flags) throws -> void;

fn show_version() throws -> void;
fn short_version_string(Allocator allocator) throws -> String;
fn show_short_version() throws -> void;

fn make_synopsis(StringView program_name, const SynopsisList &lines) throws
    -> String;
fn make_flag_help(const FlagList &flags) throws -> String;

fn wrap_text(StringView text, usize indent, usize width,
             const Maybe<usize> &continuation_indent = {}) throws -> String;

fn show_message(StringView err) throws -> void;

/* Arm a one-shot leading newline on the next show_message, so a diagnostic
   raised while the editor sits mid-line starts on its own line instead of
   joining the prompt. The first message consumes the arming. */
fn arm_message_leading_newline(bool armed) wontthrow -> void;

fn print(StringView text) throws -> void;
fn print_error(StringView text) throws -> void;
fn flush() throws -> void;

} /* namespace koshka */
