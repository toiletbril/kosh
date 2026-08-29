#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL(
    "[-BeiknpqrSst] [-C directory] [-f makefile]... [macro=value ...] "
    "[target ...]");

HELP_DESCRIPTION_DECL(
    "The make utility runs the recipe of each requested target.");

FLAG(MAKE_FILE, String, 'f', "file",
     "Read the named file instead of Makefile.");
FLAG(MAKE_DIR, String, 'C', "directory",
     "Change to this directory before reading the Makefile.");
FLAG(MAKE_ALWAYS_MAKE, Bool, 'B', "always-make",
     "Rebuild every target unconditionally.");
FLAG(MAKE_KEEP_GOING, Bool, 'k', "keep-going",
     "Keep going after a target fails.");
FLAG(MAKE_ENVIRONMENT_OVERRIDES, Bool, 'e', "environment-overrides",
     "Let environment variables override makefile assignments.");
FLAG(MAKE_IGNORE_ERRORS, Bool, 'i', "ignore-errors",
     "Treat recipe errors as successful results.");
FLAG(MAKE_STOP, Bool, 'S', "stop",
     "Stop after an error, cancelling keep-going mode.");
FLAG(MAKE_DRY_RUN, Bool, 'n', "just-print",
     "Print recipes without running them.");
FLAG(MAKE_PRINT_DATABASE, Bool, 'p', "print-data-base",
     "Print macro definitions and target descriptions.");
FLAG(MAKE_QUESTION, Bool, 'q', "question",
     "Return whether any target is out of date without running recipes.");
FLAG(MAKE_NO_BUILTINS, Bool, 'r', "no-builtin-rules",
     "Disable built-in inference rules.");
FLAG(MAKE_SILENT, Bool, 's', "silent", "Do not print recipes before running.");
FLAG(MAKE_TOUCH, Bool, 't', "touch",
     "Update targets without running their recipes.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Make);

namespace koshka {

namespace koshkit {

namespace {

/* The variables make predefines, so a makefile that reads one without assigning
   it still finds a sane default. These sit at the lowest precedence, below a
   makefile assignment and the environment, which the expander checks first. */
constexpr static_string_entry<const char *> BUILTIN_VARIABLE_ENTRIES[] = {
    {SSK("MAKE"),         "koshkit make"},
    {SSK("SHELL"),        "/bin/sh"     },
    {SSK("AR"),           "ar"          },
    {SSK("ARFLAGS"),      "-rv"         },
    {SSK("YACC"),         "yacc"        },
    {SSK("YFLAGS"),       ""            },
    {SSK("LEX"),          "lex"         },
    {SSK("LFLAGS"),       ""            },
    {SSK("LDFLAGS"),      ""            },
    {SSK("CC"),           "c99"         },
    {SSK("CFLAGS"),       "-O 1"        },
    {SSK("FC"),           "fort77"      },
    {SSK("FFLAGS"),       "-O 1"        },
    {SSK("GET"),          "get"         },
    {SSK("GFLAGS"),       ""            },
    {SSK("SCCSFLAGS"),    ""            },
    {SSK("SCCSGETFLAGS"), "-s"          },
    {SSK("CXX"),          "c++"         },
    {SSK("CPP"),          "c99 -E"      },
    {SSK("RM"),           "rm -f"       },
};
constexpr StaticStringMap BUILTIN_VARIABLES{BUILTIN_VARIABLE_ENTRIES};

constexpr PackedStringKey SPECIAL_TARGET_KEYS[] = {
    SSK(".DEFAULT"),  SSK(".IGNORE"), SSK(".POSIX"),    SSK(".PRECIOUS"),
    SSK(".SCCS_GET"), SSK(".SILENT"), SSK(".SUFFIXES"),
};
constexpr StaticStringSet SPECIAL_TARGETS{SPECIAL_TARGET_KEYS};

constexpr PackedStringKey CONDITIONAL_DIRECTIVE_KEYS[] = {
    SSK("ifeq"), SSK("ifneq"), SSK("ifdef"), SSK("ifndef")};
constexpr StaticStringSet CONDITIONAL_DIRECTIVES{CONDITIONAL_DIRECTIVE_KEYS};

constexpr PackedStringKey IGNORED_STATEMENT_KEYS[] = {
    SSK("undefine"), SSK("unexport"), SSK("define")};
constexpr StaticStringSet IGNORED_STATEMENTS{IGNORED_STATEMENT_KEYS};

struct builtin_rule_entry
{
  const char *target;
  const char *recipe_lines[4];
};

constexpr builtin_rule_entry BUILTIN_RULE_ENTRIES[] = {
    {".c",   {"$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<", nullptr, nullptr, nullptr}},
    {".f",   {"$(FC) $(FFLAGS) $(LDFLAGS) -o $@ $<", NULL, NULL, NULL}         },
    {".sh",  {"cp $< $@", "chmod a+x $@", nullptr, nullptr}                    },
    {".c.o", {"$(CC) $(CFLAGS) -c $<", nullptr, nullptr, nullptr}              },
    {".f.o", {"$(FC) $(FFLAGS) -c $<", NULL, NULL, NULL}                       },
    {".y.o",
     {"$(YACC) $(YFLAGS) $<", "$(CC) $(CFLAGS) -c y.tab.c", "rm -f y.tab.c",
      "mv y.tab.o $@"}                                                         },
    {".l.o",
     {"$(LEX) $(LFLAGS) $<", "$(CC) $(CFLAGS) -c lex.yy.c", "rm -f lex.yy.c",
      "mv lex.yy.o $@"}                                                        },
    {".y.c", {"$(YACC) $(YFLAGS) $<", "mv y.tab.c $@", nullptr, nullptr}       },
    {".l.c", {"$(LEX) $(LFLAGS) $<", "mv lex.yy.c $@", nullptr, nullptr}       },
    {".c.a",
     {"$(CC) -c $(CFLAGS) $<", "$(AR) $(ARFLAGS) $@ $*.o", "rm -f $*.o",
      nullptr}                                                                 },
    {".f.a",
     {"$(FC) -c $(FFLAGS) $<", "$(AR) $(ARFLAGS) $@ $*.o", "rm -f $*.o", NULL} },
};

struct make_variable
{
  make_variable(String name, String value)
      : name(steal(name)), value(steal(value))
  {}
  String name;
  String value;
};

struct make_rule
{
  explicit make_rule(Allocator allocator)
      : target(allocator), prerequisites(allocator), recipe_lines(allocator),
        variable_assignments(allocator)
  {}
  String target;
  ArrayList<String> prerequisites;
  ArrayList<String> recipe_lines;
  /* Each entry is the raw `NAME op= value` text. */
  ArrayList<String> variable_assignments;
};

struct makefile
{
  explicit makefile(Allocator allocator)
      : variables(allocator), rules(allocator), pattern_rules(allocator),
        default_goal(allocator), variable_index(allocator),
        rule_index(allocator), command_variable_names(allocator),
        suffixes(allocator), precious_targets(allocator),
        ignored_targets(allocator), silent_targets(allocator)
  {}
  ArrayList<make_variable> variables;
  ArrayList<make_rule> rules;
  ArrayList<make_rule> pattern_rules;
  /* The first ordinary explicit target, the bare-make goal. A target-specific
     variable line does not set it, the way GNU make picks the first real rule.
   */
  String default_goal;
  StringMap<usize> variable_index;
  StringMap<usize> rule_index;
  StringMap<bool> command_variable_names;
  ArrayList<String> suffixes;
  StringMap<bool> precious_targets;
  StringMap<bool> ignored_targets;
  StringMap<bool> silent_targets;
  bool does_environment_override{false};
  bool should_ignore_errors{false};
  bool is_silent{false};
  bool is_every_target_precious{false};

  fn find_variable(StringView name) const throws -> const String *
  {
    if (let const *index = variable_index.find(name); index != nullptr)
      return &variables[*index].value;
    return nullptr;
  }

  fn find_rule(StringView target) const throws -> const make_rule *
  {
    let const *index = rule_index.find(target);
    return index == nullptr ? nullptr : &rules[*index];
  }

  fn find_mutable_rule(StringView target) throws -> make_rule *
  {
    let const *index = rule_index.find(target);
    return index == nullptr ? nullptr : &rules[*index];
  }

  fn add_rule(make_rule &&rule) throws -> usize
  {
    let const index = rules.count();
    rule_index.set(rule.target.view(), index);
    rules.push(steal(rule));
    return index;
  }
};

static fn match_pattern(StringView pattern, StringView goal,
                        Allocator allocator) throws -> Maybe<String>
{
  let const percent = pattern.find_character('%');
  if (!percent.has_value()) return None;
  let const prefix = pattern.substring_of_length(0, *percent);
  let const suffix = pattern.substring(*percent + 1);
  if (goal.length < prefix.length + suffix.length) return None;
  if (!goal.starts_with(prefix)) return None;
  if (goal.substring(goal.length - suffix.length) != suffix) return None;
  return String{allocator, goal.substring_of_length(
                               prefix.length,
                               goal.length - prefix.length - suffix.length)};
}

static fn substitute_stem(StringView text, StringView stem,
                          Allocator allocator) throws -> String
{
  String out{allocator};
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '%')
      out += stem;
    else
      out.push(text[i]);
  }
  return out;
}

/* This runs on the raw recipe before the $(NAME) expansion, and a $$ escape is
   carried through untouched so the later expansion collapses it to a single $
   without the following byte being read as an automatic variable. */
static fn automatic_path_part(StringView text, bool is_filename,
                              Allocator allocator) throws -> String
{
  String out{allocator};
  usize word_position = 0;
  while (word_position < text.length) {
    while (word_position < text.length &&
           (text[word_position] == ' ' || text[word_position] == '\t'))
      word_position++;
    let const word_start = word_position;
    usize separator_position = word_start;
    bool has_separator = false;
    while (word_position < text.length && text[word_position] != ' ' &&
           text[word_position] != '\t')
    {
      if (os::is_directory_separator(text[word_position])) {
        separator_position = word_position;
        has_separator = true;
      }
      word_position++;
    }
    if (word_position == word_start) continue;
    if (!out.is_empty()) out += ' ';

    if (is_filename) {
      let const filename_start =
          has_separator ? separator_position + 1 : word_start;
      out += text.substring_of_length(filename_start,
                                      word_position - filename_start);
    } else if (!has_separator) {
      out += '.';
    } else if (separator_position == word_start) {
      out += text.substring_of_length(word_start, 1);
    } else {
      out +=
          text.substring_of_length(word_start, separator_position - word_start);
    }
  }
  return out;
}

static fn substitute_automatic(StringView text, StringView target,
                               StringView archive_member,
                               StringView first_prereq, StringView all_prereqs,
                               StringView repeated_prereqs,
                               StringView newer_prereqs, StringView stem,
                               Allocator allocator) throws -> String
{
  String out{allocator};
  let const do_value_for = [&](char name) -> StringView {
    switch (name) {
    case '@': return target;
    case '%': return archive_member;
    case '<': return first_prereq;
    case '^': return all_prereqs;
    case '+': return repeated_prereqs;
    case '?': return newer_prereqs;
    case '*': return stem;
    default: return StringView{};
    }
  };
  usize i = 0;
  while (i < text.length) {
    if (text[i] == '$' && i + 1 < text.length) {
      let const next = text[i + 1];
      if (next == '$') {
        out += "$$";
        i += 2;
        continue;
      }
      if ((next == '(' || next == '{') && i + 3 < text.length) {
        let const close = next == '(' ? ')' : '}';
        let const name = text[i + 2];
        let const value = do_value_for(name);
        if (text[i + 3] == close &&
            StringView{"@%<^+?*"}.find_character(name).has_value())
        {
          out += value;
          i += 4;
          continue;
        }
        if (i + 4 < text.length && text[i + 4] == close &&
            (text[i + 3] == 'D' || text[i + 3] == 'F') &&
            StringView{"@%<^+?*"}.find_character(name).has_value())
        {
          out += automatic_path_part(value, text[i + 3] == 'F', allocator);
          i += 5;
          continue;
        }
      }
      if (StringView{"@%<^+?*"}.find_character(next).has_value()) {
        out += do_value_for(next);
        i += 2;
        continue;
      }
    }
    out.push(text[i]);
    i++;
  }
  return out;
}

static fn is_blank(char c) wontthrow -> bool
{
  return c == ' ' || c == '\t' || c == '\r';
}

static fn trim(StringView text) wontthrow -> StringView
{
  usize start = 0;
  usize end = text.length;
  while (start < end && is_blank(text[start]))
    start++;
  while (end > start && is_blank(text[end - 1]))
    end--;
  return text.substring_of_length(start, end - start);
}

static fn split_words(StringView text, Allocator allocator) throws
    -> ArrayList<String>
{
  ArrayList<String> words{allocator};
  usize i = 0;
  while (i < text.length) {
    while (i < text.length && is_blank(text[i]))
      i++;
    let const start = i;
    while (i < text.length && !is_blank(text[i]))
      i++;
    if (i > start)
      words.push(String{allocator, text.substring_of_length(start, i - start)});
  }
  return words;
}

static fn split_makeflags_words(StringView text, Allocator allocator) throws
    -> ArrayList<String>
{
  ArrayList<String> words{allocator};
  usize text_position = 0;

  while (text_position < text.length) {
    while (text_position < text.length && is_blank(text[text_position]))
      text_position++;
    if (text_position == text.length) break;

    String word{allocator};
    while (text_position < text.length && !is_blank(text[text_position])) {
      if (text[text_position] == '\\' && text_position + 1 < text.length)
        text_position++;
      word.push(text[text_position++]);
    }
    words.push(steal(word));
  }

  return words;
}

static fn archive_member_modification_time(const Path &archive,
                                           StringView wanted_member) throws
    -> Maybe<i64>
{
  let const contents = archive.read_entire_file();
  if (!contents.has_value() || contents->view().length < 8 ||
      contents->view().substring_of_length(0, 8) != StringView{"!<arch>\n"})
    return None;

  usize header_position = 8;
  while (header_position <= contents->view().length &&
         contents->view().length - header_position >= 60)
  {
    let const header =
        contents->view().substring_of_length(header_position, 60);
    if (header[58] != '`' || header[59] != '\n') {
      return None;
    }

    let member_name = header.substring_of_length(0, 16).trim_blanks();
    let const timestamp = utils::parse_decimal_i64(
        header.substring_of_length(16, 12).trim_blanks());
    let const member_size = utils::parse_decimal_u64(
        header.substring_of_length(48, 10).trim_blanks());
    if (timestamp.is_error() || member_size.is_error()) return None;

    let const content_position = header_position + 60;
    if (member_size.value() > contents->view().length - content_position)
      return None;

    u64 extended_name_length = 0;
    if (member_name.starts_with(StringView{"#1/"})) {
      let const parsed_name_length =
          utils::parse_decimal_u64(member_name.substring(3));
      if (parsed_name_length.is_error() ||
          parsed_name_length.value() > member_size.value())
        return None;
      extended_name_length = parsed_name_length.value();
      member_name = contents->view().substring_of_length(
          content_position, static_cast<usize>(extended_name_length));
    } else if (!member_name.is_empty() &&
               member_name[member_name.length - 1] == '/')
    {
      member_name = member_name.substring_of_length(0, member_name.length - 1);
    }

    if (member_name == wanted_member) return timestamp.value();

    let const padded_size = member_size.value() + (member_size.value() & 1u);
    if (padded_size > contents->view().length - content_position) return None;
    header_position = content_position + static_cast<usize>(padded_size);
  }

  return None;
}

static fn expand(EvalContext &cxt, const makefile &mk, StringView text,
                 usize depth) throws -> String;

static fn expand_variable(EvalContext &cxt, const makefile &mk, StringView name,
                          usize depth) throws -> String
{
  if (mk.command_variable_names.find(name) != nullptr)
    if (const String *value = mk.find_variable(name); value != nullptr)
      return expand(cxt, mk, value->view(), depth + 1);

  if (name == StringView{"SHELL"}) {
    if (const String *value = mk.find_variable(name); value != nullptr)
      return expand(cxt, mk, value->view(), depth + 1);
    return String{cxt.scratch_allocator(), "/bin/sh"};
  }

  if (mk.does_environment_override)
    if (Maybe<String> from_env = os::get_environment_variable(name);
        from_env.has_value())
      return steal(*from_env);

  if (const String *value = mk.find_variable(name); value != nullptr)
    return expand(cxt, mk, value->view(), depth + 1);

  if (Maybe<String> from_env = os::get_environment_variable(name);
      from_env.has_value())
    return steal(*from_env);

  if (Maybe<const char *> builtin = BUILTIN_VARIABLES.find(name);
      builtin.has_value())
    return String{cxt.scratch_allocator(), *builtin};

  return String{cxt.scratch_allocator()};
}

static fn make_wildcard(EvalContext &cxt, StringView patterns) throws -> String
{
  let result = String{cxt.scratch_allocator()};
  for (const String &pattern : split_words(patterns, cxt.scratch_allocator())) {
    for (const String &match :
         os::glob_matches(pattern.view(), cxt.scratch_allocator()))
    {
      if (!result.is_empty()) result += ' ';
      result += match.view();
    }
  }
  return result;
}

/* None means the name is not a substitution reference. */
static fn try_substitution_reference(EvalContext &cxt, const makefile &mk,
                                     StringView name, usize depth) throws
    -> Maybe<String>
{
  let const colon = name.find_character(':');
  if (!colon.has_value() || *colon == 0) {
    return None;
  }

  let const variable_name = name.substring_of_length(0, *colon);
  if (variable_name.find_character(' ').has_value() ||
      variable_name.find_character('\t').has_value())
  {
    return None;
  }

  let const rest = name.substring(*colon + 1);
  let const equals = rest.find_character('=');
  if (!equals.has_value()) return None;

  let const pattern =
      expand(cxt, mk, rest.substring_of_length(0, *equals), depth + 1);
  let const replacement =
      expand(cxt, mk, rest.substring(*equals + 1), depth + 1);

  let value = String{cxt.scratch_allocator()};
  if (mk.command_variable_names.find(variable_name) != nullptr) {
    if (const String *stored = mk.find_variable(variable_name);
        stored != nullptr)
      value = expand(cxt, mk, stored->view(), depth + 1);
  } else if (mk.does_environment_override) {
    if (Maybe<String> from_env = os::get_environment_variable(variable_name);
        from_env.has_value())
      value = steal(*from_env);
    else if (const String *stored = mk.find_variable(variable_name);
             stored != nullptr)
      value = expand(cxt, mk, stored->view(), depth + 1);
  } else if (const String *stored = mk.find_variable(variable_name);
             stored != nullptr)
  {
    value = expand(cxt, mk, stored->view(), depth + 1);
  } else if (Maybe<String> from_env =
                 os::get_environment_variable(variable_name);
             from_env.has_value())
  {
    value = steal(*from_env);
  }

  let const has_percent = pattern.view().find_character('%').has_value();
  let out = String{cxt.scratch_allocator()};
  for (const String &word : split_words(value.view(), cxt.scratch_allocator()))
  {
    if (!out.is_empty()) out += ' ';
    if (has_percent) {
      if (let const stem = match_pattern(pattern.view(), word.view(),
                                         cxt.scratch_allocator());
          stem.has_value())
        out += substitute_stem(replacement.view(), stem->view(),
                               cxt.scratch_allocator())
                   .view();
      else
        out += word.view();
    } else if (word.view().length >= pattern.view().length &&
               word.view().substring(word.view().length -
                                     pattern.view().length) == pattern.view())
    {
      out += word.view().substring_of_length(0, word.view().length -
                                                    pattern.view().length);
      out += replacement.view();
    } else {
      out += word.view();
    }
  }
  return out;
}

static fn expand(EvalContext &cxt, const makefile &mk, StringView text,
                 usize depth) throws -> String
{
  /* A self-referential reference such as A = $(wildcard $(A)) recurses without
     bound, and the function call, the substitution reference, and the plain
     variable all recurse, so the cap sits at the entry and leaves the text
     unexpanded once it is hit rather than guarding one branch. */
  if (depth >= 16) return String{cxt.scratch_allocator(), text};

  String result{cxt.scratch_allocator()};
  usize i = 0;
  while (i < text.length) {
    if (text[i] == '$' && i + 1 < text.length &&
        (text[i + 1] == '(' || text[i + 1] == '{'))
    {
      /* The close scan balances nested parentheses so a $(dir $(VAR)) or a
         $(shell cmd $(VAR)) reads to its own close rather than the first one.
       */
      let const open = text[i + 1];
      let const close = open == '(' ? ')' : '}';
      usize j = i + 2;
      usize nesting = 1;
      while (j < text.length) {
        if (text[j] == open)
          nesting++;
        else if (text[j] == close && --nesting == 0)
          break;
        j++;
      }
      let const name = text.substring_of_length(i + 2, j - (i + 2));
      if (name.length > 6 && name.substring_of_length(0, 6) == "shell ") {
        /* Completion suppresses the run so listing targets never forks the
           makefile's commands. */
        if (!cxt.make_shell_suppressed()) {
          let const command = expand(cxt, mk, name.substring(6), depth + 1);
          result +=
              cxt.capture_command_substitution(command, StringView{"make"})
                  .view();
        }
      } else if (name.length > 9 &&
                 name.substring_of_length(0, 9) == "wildcard ")
      {
        let const patterns = expand(cxt, mk, name.substring(9), depth + 1);
        result += make_wildcard(cxt, patterns.view()).view();
      } else if (name.length > 6 && name.substring_of_length(0, 6) == "error ")
      {
        throw Error{"The makefile stopped the build with the message '" +
                    expand(cxt, mk, name.substring(6), depth + 1) + "'"};
      } else if (name.length > 8 &&
                 name.substring_of_length(0, 8) == "warning ")
      {
        /* $(warning text) is informational only. */
      } else if (Maybe<String> subst =
                     try_substitution_reference(cxt, mk, name, depth);
                 subst.has_value())
      {
        result += subst->view();
      } else {
        let const expanded_name = expand(cxt, mk, name, depth + 1);
        result +=
            expand_variable(cxt, mk, expanded_name.view(), depth + 1).view();
      }
      i = j < text.length ? j + 1 : j;
    } else if (text[i] == '$' && i + 1 < text.length && text[i + 1] == '$') {
      result += '$';
      i += 2;
    } else if (text[i] == '$' && i + 1 < text.length) {
      result += expand_variable(cxt, mk, text.substring_of_length(i + 1, 1),
                                depth + 1)
                    .view();
      i += 2;
    } else {
      result.push(text[i]);
      i++;
    }
  }
  return result;
}

/* The first colon not immediately followed by '=' opens the rule. */
static fn rule_colon(StringView line) wontthrow -> Maybe<usize>
{
  for (usize i = 0; i < line.length; i++)
    if (line[i] == ':' && !(i + 1 < line.length && line[i + 1] == '='))
      return i;
  return None;
}

static fn assignment_variable_name(StringView assignment) wontthrow
    -> StringView
{
  let const equals = assignment.find_character('=');
  if (!equals.has_value()) return StringView{};
  let name = assignment.substring_of_length(0, *equals);
  if (!name.is_empty()) {
    let const last = name[name.length - 1];
    if (last == '+' || last == '?' || last == ':') {
      name = name.substring_of_length(0, name.length - 1);
    }
  }
  return trim(name);
}

static fn is_command_line_assignment(StringView operand) wontthrow -> bool
{
  let const equals = operand.find_character('=');
  if (!equals.has_value() || *equals == 0) return false;
  let const name = operand.substring_of_length(0, *equals);
  for (usize i = 0; i < name.length; i++)
    if (is_blank(name[i])) return false;
  return true;
}

static fn is_target_variable_assignment(StringView after_colon) wontthrow
    -> bool
{
  if (!after_colon.find_character('=').has_value()) return false;
  let const name = assignment_variable_name(after_colon);
  if (name.is_empty()) return false;
  for (usize i = 0; i < name.length; i++)
    if (name[i] == ' ' || name[i] == '\t') return false;
  return true;
}

static fn apply_assignment(EvalContext &cxt, makefile &mk, StringView name_part,
                           StringView operator_and_value,
                           bool is_command_line = false) throws -> void
{
  let value = operator_and_value.substring(1);
  char operator_character = ' ';
  if (!name_part.is_empty()) {
    let const last = name_part[name_part.length - 1];
    if (last == ':' || last == '?' || last == '+') {
      operator_character = last;
      name_part = name_part.substring_of_length(0, name_part.length - 1);
    }
  }

  let const name = trim(name_part);
  let const trimmed_value = trim(value);
  if (!is_command_line && mk.command_variable_names.find(name) != nullptr)
    return;
  if (is_command_line) mk.command_variable_names.set(name, true);

  /* A := assignment is immediate, so its right-hand side expands now against
     the values defined so far, the way GNU make evaluates a simple variable. A
     later
     $(NAME) then reads the finished string. Expanding here also breaks the
     self-reference in MAKE := $(MAKE) -j$(shell nproc), since $(MAKE) resolves
     to the make program name before the variable is stored rather than
     recursing on itself to the expansion-depth cap. A plain = stays lazy and
     keeps its raw text. */
  let const value_to_store =
      operator_character == ':'
          ? expand(cxt, mk, trimmed_value, 0)
          : String{cxt.scratch_allocator(), trimmed_value};

  if (let const *index = mk.variable_index.find(name); index != nullptr) {
    make_variable &variable = mk.variables[*index];
    if (operator_character == '?') return;
    if (operator_character == '+') {
      variable.value += " ";
      variable.value += value_to_store.view();
    } else {
      variable.value = String{cxt.scratch_allocator(), value_to_store.view()};
    }
    return;
  }
  mk.variable_index.set(name, mk.variables.count());
  mk.variables.push(make_variable{
      String{cxt.scratch_allocator(), name                 },
      String{cxt.scratch_allocator(), value_to_store.view()}
  });
}

/* An odd run of trailing backslashes continues the line, a doubled \\ is a
   literal backslash. */
static fn ends_with_continuation(StringView line) wontthrow -> bool
{
  usize backslash_count = 0;
  usize k = line.length;
  while (k > 0 && line[k - 1] == '\\') {
    backslash_count++;
    k--;
  }
  return (backslash_count % 2) == 1;
}

static fn join_continuations(StringView source, Allocator allocator) throws
    -> ArrayList<String>
{
  let const physical = utils::split_lines(source, allocator, true);
  ArrayList<String> logical{allocator};
  usize i = 0;
  while (i < physical.count()) {
    let const raw = physical[i].without_trailing_newline();
    let line = String{allocator, raw};

    if (!raw.is_empty() && raw[0] == '\t') {
      while (ends_with_continuation(line.view()) && i + 1 < physical.count()) {
        line += '\n';
        i++;
        let next = physical[i].without_trailing_newline();
        if (!next.is_empty() && next[0] == '\t') next = next.substring(1);
        line += next;
      }
    } else {
      while (ends_with_continuation(line.view()) && i + 1 < physical.count()) {
        line =
            String{allocator,
                   line.view().substring_of_length(0, line.view().length - 1)};
        i++;
        let const next = physical[i].without_trailing_newline();
        line += ' ';
        line += trim(next);
      }
    }

    logical.push(steal(line));
    i++;
  }
  return logical;
}

static fn leading_word(StringView text) wontthrow -> StringView
{
  usize start = 0;
  while (start < text.length && is_blank(text[start]))
    start++;
  usize end = start;
  while (end < text.length && !is_blank(text[end]))
    end++;
  return text.substring_of_length(start, end - start);
}

/* The comma split honors nested parentheses. */
static fn split_conditional_arguments(StringView rest, StringView &first,
                                      StringView &second) wontthrow -> bool
{
  rest = trim(rest);
  if (rest.is_empty() || rest[0] != '(') return false;

  usize close = rest.length;
  while (close > 0 && rest[close - 1] != ')')
    close--;
  if (close <= 1) return false;

  let const content = rest.substring_of_length(1, close - 2);
  usize depth = 0;
  Maybe<usize> comma = None;
  for (usize k = 0; k < content.length; k++) {
    if (content[k] == '(')
      depth++;
    else if (content[k] == ')' && depth > 0)
      depth--;
    else if (content[k] == ',' && depth == 0) {
      comma = k;
      break;
    }
  }
  if (!comma.has_value()) return false;

  first = trim(content.substring_of_length(0, *comma));
  second = trim(content.substring(*comma + 1));
  return true;
}

static fn evaluate_conditional(EvalContext &cxt, const makefile &mk,
                               StringView directive, StringView rest) throws
    -> bool
{
  if (directive == "ifdef" || directive == "ifndef") {
    let const name = expand(cxt, mk, trim(rest), 0);
    bool is_defined = false;
    if (const String *stored = mk.find_variable(name.view());
        stored != nullptr && !stored->is_empty())
      is_defined = true;
    else if (Maybe<String> from_env = os::get_environment_variable(name.view());
             from_env.has_value() && !from_env->is_empty())
      is_defined = true;
    return directive == "ifdef" ? is_defined : !is_defined;
  }

  let first = StringView{};
  let second = StringView{};
  if (!split_conditional_arguments(rest, first, second)) return false;
  let const expanded_first = expand(cxt, mk, first, 0);
  let const expanded_second = expand(cxt, mk, second, 0);
  let const is_equal = expanded_first.view() == expanded_second.view();
  return directive == "ifeq" ? is_equal : !is_equal;
}

struct conditional_state
{
  bool is_branch_active;
  bool was_any_branch_taken;
  bool is_parent_active;
};

struct make_source_frame
{
  explicit make_source_frame(ArrayList<String> &&new_lines)
      : lines(steal(new_lines))
  {}
  ArrayList<String> lines;
  usize line_position{0};
};

static fn parse_makefile(EvalContext &cxt, StringView source,
                         const ArrayList<String> &command_assignments,
                         bool does_environment_override,
                         bool should_use_builtin_rules) throws -> makefile
{
  makefile mk{cxt.scratch_allocator()};
  mk.does_environment_override = does_environment_override;
  if (should_use_builtin_rules)
    for (const char *suffix : {".o", ".c", ".y", ".l", ".a", ".sh", ".f", ".c~",
                               ".y~", ".l~", ".sh~", ".f~"})
      mk.suffixes.push(String{cxt.scratch_allocator(), suffix});
  for (const String &assignment : command_assignments) {
    let const equals = assignment.view().find_character('=');
    ASSERT(equals.has_value());
    apply_assignment(cxt, mk, assignment.view().substring_of_length(0, *equals),
                     assignment.view().substring(*equals), true);
  }
  ArrayList<usize> current_rule_indices{cxt.scratch_allocator()};
  ArrayList<usize> current_pattern_indices{cxt.scratch_allocator()};
  ArrayList<conditional_state> conditionals{cxt.scratch_allocator()};

  let const do_is_active = [&]() -> bool {
    for (const conditional_state &state : conditionals)
      if (!state.is_branch_active) return false;
    return true;
  };

  ArrayList<make_source_frame> source_frames{cxt.scratch_allocator()};
  source_frames.push(
      make_source_frame{join_continuations(source, cxt.scratch_allocator())});
  while (!source_frames.is_empty()) {
    make_source_frame &source_frame = source_frames[source_frames.count() - 1];
    if (source_frame.line_position == source_frame.lines.count()) {
      source_frames.pop_back();
      continue;
    }

    const String &logical = source_frame.lines[source_frame.line_position++];
    let line = logical.view();

    /* A recipe line is kept verbatim and expanded only at build time. */
    if (!line.is_empty() && line[0] == '\t') {
      if (do_is_active()) {
        for (usize index : current_rule_indices)
          mk.rules[index].recipe_lines.push(
              String{cxt.scratch_allocator(), line.substring(1)});
        for (usize index : current_pattern_indices)
          mk.pattern_rules[index].recipe_lines.push(
              String{cxt.scratch_allocator(), line.substring(1)});
      }
      continue;
    }

    String uncommented{cxt.scratch_allocator()};
    for (usize line_position = 0; line_position < line.length; line_position++)
    {
      if (line[line_position] == '\\' && line_position + 1 < line.length &&
          line[line_position + 1] == '#')
      {
        uncommented += '#';
        line_position++;
        continue;
      }
      if (line[line_position] == '#') break;
      uncommented += line[line_position];
    }
    line = uncommented.view();

    let const trimmed = trim(line);
    if (trimmed.is_empty()) {
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }

    let const directive = leading_word(trimmed);

    /* An inactive branch still tracks nested directives so the matching endif
       pops the right one. */
    if (CONDITIONAL_DIRECTIVES.contains(directive)) {
      let const is_parent_active = do_is_active();
      let const is_taken =
          is_parent_active &&
          evaluate_conditional(cxt, mk, directive,
                               trimmed.substring(directive.length));
      conditionals.push(
          conditional_state{is_taken, is_taken, is_parent_active});
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }
    if (directive == "else") {
      if (!conditionals.is_empty()) {
        conditional_state &top = conditionals[conditionals.count() - 1];
        let const rest = trim(trimmed.substring(directive.length));
        let const else_directive = leading_word(rest);
        if (CONDITIONAL_DIRECTIVES.contains(else_directive)) {
          let const is_taken =
              top.is_parent_active && !top.was_any_branch_taken &&
              evaluate_conditional(cxt, mk, else_directive,
                                   rest.substring(else_directive.length));
          top.is_branch_active = is_taken;
          if (is_taken) top.was_any_branch_taken = true;
        } else {
          top.is_branch_active =
              top.is_parent_active && !top.was_any_branch_taken;
          top.was_any_branch_taken = true;
        }
      }
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }
    if (directive == "endif") {
      if (!conditionals.is_empty()) conditionals.pop_back();
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }

    if (!do_is_active()) {
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }

    if (directive == "include") {
      let const expanded_path =
          expand(cxt, mk, trim(trimmed.substring(directive.length)), 0);
      let const include_paths =
          split_words(expanded_path.view(), cxt.scratch_allocator());
      if (include_paths.count() != 1)
        throw Error{"An include line must name exactly one makefile"};
      if (source_frames.count() >= 17)
        throw Error{"Makefile includes exceed the supported nesting depth"};

      let const included_source =
          Path{include_paths[0].view()}.read_entire_file();
      if (!included_source.has_value())
        throw Error{"Unable to read the included makefile '" +
                    include_paths[0] + "': " + os::last_system_error_message()};
      current_rule_indices.clear();
      current_pattern_indices.clear();
      source_frames.push(make_source_frame{join_continuations(
          included_source->view(), cxt.scratch_allocator())});
      continue;
    }

    /* override re-asserts a value, so its prefix is stripped and the assignment
       parses as usual. undefine, unexport, define, and a bare export are not
       modelled, so they are skipped rather than read as a malformed rule. An
       export that prefixes an assignment keeps the assignment. */
    let statement = trimmed;
    if (directive == "override")
      statement = trim(statement.substring(directive.length));

    let const statement_word = leading_word(statement);
    if (IGNORED_STATEMENTS.contains(statement_word)) {
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }
    if (statement_word == "export") {
      let const after_export = trim(statement.substring(statement_word.length));
      if (after_export.is_empty()) {
        current_rule_indices.clear();
        current_pattern_indices.clear();
        continue;
      }
      statement = after_export;
    }

    let const colon = rule_colon(statement);
    let const equals = statement.find_character('=');
    let const is_rule =
        colon.has_value() && (!equals.has_value() || *colon < *equals);

    if (is_rule) {
      let const after_colon = statement.substring(*colon + 1);
      if (is_target_variable_assignment(after_colon)) {
        let const targets =
            expand(cxt, mk, trim(statement.substring_of_length(0, *colon)), 0);
        current_rule_indices.clear();
        current_pattern_indices.clear();
        for (const String &target :
             split_words(targets.view(), cxt.scratch_allocator()))
        {
          make_rule *rule = mk.find_mutable_rule(target.view());
          if (rule == nullptr) {
            make_rule fresh{cxt.scratch_allocator()};
            fresh.target = target.clone();
            rule = &mk.rules[mk.add_rule(steal(fresh))];
          }
          rule->variable_assignments.push(
              String{cxt.scratch_allocator(), trim(after_colon)});
        }
        continue;
      }

      /* The targets and prerequisites expand when the rule is read. */
      let const targets =
          expand(cxt, mk, trim(statement.substring_of_length(0, *colon)), 0);
      let prerequisite_text = after_colon;
      let inline_recipe = StringView{};
      if (let const semicolon = after_colon.find_character(';');
          semicolon.has_value())
      {
        prerequisite_text = after_colon.substring_of_length(0, *semicolon);
        inline_recipe = trim(after_colon.substring(*semicolon + 1));
      }
      let const prerequisites = expand(cxt, mk, prerequisite_text, 0);
      current_rule_indices.clear();
      current_pattern_indices.clear();
      for (const String &target :
           split_words(targets.view(), cxt.scratch_allocator()))
      {
        let new_prerequisites =
            split_words(prerequisites.view(), cxt.scratch_allocator());
        if (target.view() == StringView{".IGNORE"}) {
          if (new_prerequisites.is_empty())
            mk.should_ignore_errors = true;
          else
            for (const String &ignored : new_prerequisites)
              mk.ignored_targets.set(ignored.view(), true);
        }
        if (target.view() == StringView{".SILENT"}) {
          if (new_prerequisites.is_empty())
            mk.is_silent = true;
          else
            for (const String &silent : new_prerequisites)
              mk.silent_targets.set(silent.view(), true);
        }
        if (target.view() == StringView{".PRECIOUS"}) {
          if (new_prerequisites.is_empty())
            mk.is_every_target_precious = true;
          else
            for (const String &precious : new_prerequisites)
              mk.precious_targets.set(precious.view(), true);
        }
        if (target.view() == StringView{".SUFFIXES"}) {
          if (new_prerequisites.is_empty()) {
            mk.suffixes.clear();
          } else {
            for (const String &suffix : new_prerequisites)
              if (!mk.suffixes.find(suffix.view()).has_value())
                mk.suffixes.push(suffix.clone());
          }
        }
        let is_inference_rule = false;
        if (!target.view().find_character('/').has_value())
          for (const String &source_suffix : mk.suffixes) {
            if (target.view() == source_suffix.view()) {
              is_inference_rule = true;
              break;
            }
            if (!target.view().starts_with(source_suffix.view())) continue;

            let const target_suffix =
                target.view().substring(source_suffix.view().length);
            for (const String &known_suffix : mk.suffixes)
              if (target_suffix == known_suffix.view()) {
                is_inference_rule = true;
                break;
              }
            if (is_inference_rule) break;
          }

        if (mk.default_goal.is_empty() &&
            !target.view().find_character('%').has_value() &&
            !SPECIAL_TARGETS.contains(target.view()) && !is_inference_rule)
          mk.default_goal = target.clone();
        if (!target.view().find_character('%').has_value())
          if (make_rule *existing = mk.find_mutable_rule(target.view());
              existing != nullptr)
          {
            if (is_inference_rule) {
              existing->prerequisites = steal(new_prerequisites);
              existing->recipe_lines.clear();
            } else {
              for (String &prerequisite : new_prerequisites)
                existing->prerequisites.push(steal(prerequisite));
            }
            current_rule_indices.push(
                static_cast<usize>(existing - mk.rules.begin()));
            continue;
          }

        make_rule rule{cxt.scratch_allocator()};
        rule.target = target.clone();
        rule.prerequisites = steal(new_prerequisites);
        if (target.view().find_character('%').has_value()) {
          mk.pattern_rules.push(steal(rule));
          current_pattern_indices.push(mk.pattern_rules.count() - 1);
        } else {
          current_rule_indices.push(mk.add_rule(steal(rule)));
        }
      }
      if (!inline_recipe.is_empty()) {
        for (usize index : current_rule_indices)
          mk.rules[index].recipe_lines.push(
              String{cxt.scratch_allocator(), inline_recipe});
        for (usize index : current_pattern_indices)
          mk.pattern_rules[index].recipe_lines.push(
              String{cxt.scratch_allocator(), inline_recipe});
      }
      continue;
    }

    if (equals.has_value()) {
      let const expanded_name =
          expand(cxt, mk, statement.substring_of_length(0, *equals), 0);
      apply_assignment(cxt, mk, expanded_name.view(),
                       statement.substring(*equals));
      current_rule_indices.clear();
      current_pattern_indices.clear();
    }
  }
  if (should_use_builtin_rules)
    for (const builtin_rule_entry &entry : BUILTIN_RULE_ENTRIES) {
      if (mk.find_rule(entry.target) != nullptr) continue;

      make_rule rule{cxt.scratch_allocator()};
      rule.target = entry.target;
      for (const char *recipe_line : entry.recipe_lines) {
        if (recipe_line == nullptr) break;
        rule.recipe_lines.push(String{cxt.scratch_allocator(), recipe_line});
      }
      mk.add_rule(steal(rule));
    }
  return mk;
}

struct saved_make_variable
{
  String name;
  bool was_present;
  String old_value;
};

struct saved_recipe_environment
{
  String name;
  Maybe<String> old_value;
};

struct make_build_options
{
  bool should_always_make;
  bool is_query;
  bool should_ignore_errors;
  bool should_keep_going;
  bool is_dry_run;
  bool is_print_database;
  bool is_silent;
  bool should_touch;
};

static fn build_target(const ExecContext &ec, EvalContext &cxt, makefile &mk,
                       StringView goal, StringMap<bool> &active_targets,
                       StringMap<bool> &completed_target_results,
                       const make_build_options &options) throws -> bool
{
  if (let const *result = completed_target_results.find(goal);
      result != nullptr)
    return *result;

  if (active_targets.find(goal) != nullptr)
    throw Error{
        "The target '" + String{cxt.scratch_allocator(), goal}
          +
        "' is part of a dependency cycle"
    };

  const ArrayList<String> *recipe_lines = nullptr;
  ArrayList<String> prerequisites{cxt.scratch_allocator()};
  const ArrayList<String> *target_assignments = nullptr;
  String target_stem{cxt.scratch_allocator()};
  String inferred_first_prerequisite{cxt.scratch_allocator()};
  const make_rule *explicit_rule = mk.find_rule(goal);
  String automatic_target{cxt.scratch_allocator(), goal};
  String archive_member{cxt.scratch_allocator()};
  if (let const open = goal.find_character('('); open.has_value() &&
                                                 goal.length > *open + 1 &&
                                                 goal[goal.length - 1] == ')')
  {
    automatic_target =
        String{cxt.scratch_allocator(), goal.substring_of_length(0, *open)};
    archive_member =
        String{cxt.scratch_allocator(),
               goal.substring_of_length(*open + 1, goal.length - *open - 2)};
  }

  if (explicit_rule != nullptr) {
    for (const String &prerequisite : explicit_rule->prerequisites) {
      let const expanded = expand(cxt, mk, prerequisite.view(), 0);
      for (const String &word :
           split_words(expanded.view(), cxt.scratch_allocator()))
        prerequisites.push(word.clone());
    }
    if (!explicit_rule->recipe_lines.is_empty())
      recipe_lines = &explicit_rule->recipe_lines;
    target_assignments = &explicit_rule->variable_assignments;
  }

  if (recipe_lines == nullptr) {
    for (const make_rule &pattern : mk.pattern_rules) {
      let const stem =
          match_pattern(pattern.target.view(), goal, cxt.scratch_allocator());
      if (!stem.has_value()) continue;

      ArrayList<String> candidate{cxt.scratch_allocator()};
      for (const String &prerequisite : pattern.prerequisites) {
        let const substituted = substitute_stem(prerequisite.view(), *stem,
                                                cxt.scratch_allocator());
        let const expanded = expand(cxt, mk, substituted.view(), 0);
        for (const String &word :
             split_words(expanded.view(), cxt.scratch_allocator()))
          candidate.push(word.clone());
      }
      /* make chooses a pattern rule only when its prerequisite can be supplied,
         so the first prerequisite must already be a file or be a target with
         its own rule. */
      if (!candidate.is_empty() && !Path{candidate[0].view()}.exists() &&
          mk.find_rule(candidate[0].view()) == nullptr)
        continue;

      prerequisites = steal(candidate);
      recipe_lines = &pattern.recipe_lines;
      target_stem = String{cxt.scratch_allocator(), stem->view()};
      if (!prerequisites.is_empty())
        inferred_first_prerequisite = prerequisites[0].clone();
      break;
    }

    if (recipe_lines == nullptr) {
      let const inference_target =
          archive_member.is_empty() ? goal : automatic_target.view();
      let target_suffix = StringView{};
      for (const String &suffix : mk.suffixes) {
        let const candidate = suffix.view();
        if (inference_target.length >= candidate.length &&
            inference_target.substring(inference_target.length -
                                       candidate.length) == candidate)
        {
          target_suffix = candidate;
          break;
        }
      }

      for (const String &source_suffix_string : mk.suffixes) {
        let const source_suffix = source_suffix_string.view();
        let rule_name = source_suffix_string.clone();
        rule_name += target_suffix;
        let const *suffix_rule = mk.find_rule(rule_name.view());
        if (suffix_rule == nullptr) continue;

        String stem{cxt.scratch_allocator()};
        if (archive_member.is_empty()) {
          stem = String{cxt.scratch_allocator(),
                        inference_target.substring_of_length(
                            0, inference_target.length - target_suffix.length)};
        } else {
          let const member_extension = Path{archive_member.view()}.extension();
          stem = String{
              cxt.scratch_allocator(),
              archive_member.view().substring_of_length(
                  0, archive_member.view().length - member_extension.length)};
        }
        let source = String{cxt.scratch_allocator(), stem.view()};
        source += source_suffix;
        if (!Path{source.view()}.exists() &&
            mk.find_rule(source.view()) == nullptr)
          continue;

        inferred_first_prerequisite = source.clone();
        prerequisites.push(steal(source));
        recipe_lines = &suffix_rule->recipe_lines;
        target_stem = stem.clone();
        break;
      }
    }

    if (recipe_lines == nullptr && explicit_rule != nullptr) {
      recipe_lines = &explicit_rule->recipe_lines;
    } else if (recipe_lines == nullptr) {
      if (const make_rule *fallback = mk.find_rule(".DEFAULT");
          fallback != nullptr)
      {
        recipe_lines = &fallback->recipe_lines;
        inferred_first_prerequisite = String{cxt.scratch_allocator(), goal};
      } else {
        if (Path{goal}.exists()) {
          completed_target_results.set(goal, false);
          return false;
        }
        throw Error{
            "There is no rule to make the target '" +
            String{cxt.scratch_allocator(), goal}
            + "'"
        };
      }
    }
  }

  if (target_stem.is_empty()) {
    let const stem_source = archive_member.is_empty() ? automatic_target.view()
                                                      : archive_member.view();
    for (const String &suffix : mk.suffixes) {
      let const suffix_text = suffix.view();
      if (stem_source.length <= suffix_text.length ||
          stem_source.substring(stem_source.length - suffix_text.length) !=
              suffix_text)
        continue;
      target_stem = String{cxt.scratch_allocator(),
                           stem_source.substring_of_length(
                               0, stem_source.length - suffix_text.length)};
      break;
    }
  }

  /* The saved values restore in reverse so a repeated += unwinds cleanly. */
  let saved_variables = ArrayList<saved_make_variable>{cxt.scratch_allocator()};
  if (target_assignments != nullptr)
    for (const String &assignment : *target_assignments) {
      let const name = assignment_variable_name(assignment.view());
      saved_make_variable snapshot{
          String{cxt.scratch_allocator(), name},
          false,
          String{cxt.scratch_allocator()}
      };
      if (const String *current_value = mk.find_variable(name);
          current_value != nullptr)
      {
        snapshot.was_present = true;
        snapshot.old_value =
            String{cxt.scratch_allocator(), current_value->view()};
      }
      saved_variables.push(steal(snapshot));
      let const equals = assignment.view().find_character('=');
      apply_assignment(cxt, mk,
                       assignment.view().substring_of_length(0, *equals),
                       assignment.view().substring(*equals));
    }
  defer
  {
    for (usize i = saved_variables.count(); i-- > 0;) {
      const saved_make_variable &snapshot = saved_variables[i];
      if (snapshot.was_present) {
        if (let const *index = mk.variable_index.find(snapshot.name.view());
            index != nullptr)
          mk.variables[*index].value =
              String{cxt.scratch_allocator(), snapshot.old_value.view()};
      } else {
        /* A variable the assignment created is unset again, not left empty, so
           a later ?= still applies its default. */
        mk.variable_index.erase(snapshot.name.view());
      }
    }
  };

  ArrayList<String> normal_prerequisites{cxt.scratch_allocator()};
  ArrayList<String> order_only_prerequisites{cxt.scratch_allocator()};
  bool is_order_only_section = false;
  for (const String &prerequisite : prerequisites) {
    if (prerequisite.view() == "|") {
      is_order_only_section = true;
      continue;
    }

    if (is_order_only_section)
      order_only_prerequisites.push(prerequisite.clone());
    else
      normal_prerequisites.push(prerequisite.clone());
  }

  active_targets.set(goal, true);
  defer { active_targets.erase(goal); };
  let has_outdated_prerequisite = false;
  let did_prerequisite_fail = false;
  for (const String &prerequisite : normal_prerequisites) {
    try {
      has_outdated_prerequisite |=
          build_target(ec, cxt, mk, prerequisite.view(), active_targets,
                       completed_target_results, options);
    } catch (Error &error) {
      if (!options.should_keep_going) throw;
      did_prerequisite_fail = true;
      report_soft_koshkit_error(ec, cxt, error.message().view());
    }
  }
  for (const String &prerequisite : order_only_prerequisites) {
    try {
      build_target(ec, cxt, mk, prerequisite.view(), active_targets,
                   completed_target_results, options);
    } catch (Error &error) {
      if (!options.should_keep_going) throw;
      did_prerequisite_fail = true;
      report_soft_koshkit_error(ec, cxt, error.message().view());
    }
  }
  if (did_prerequisite_fail)
    throw Error{
        "The target '" + String{cxt.scratch_allocator(), goal}
          +
        "' was not remade because a prerequisite failed"
    };

  let const target_path = Path{automatic_target.view()};
  let const archive_member_time = archive_member.is_empty()
                                      ? Maybe<i64>{}
                                      : archive_member_modification_time(
                                            target_path, archive_member.view());
  let const was_target_existing = archive_member.is_empty()
                                      ? target_path.exists()
                                      : archive_member_time.has_value();
  let is_out_of_date = options.should_always_make || !was_target_existing ||
                       has_outdated_prerequisite;
  if (!is_out_of_date)
    for (const String &prerequisite : normal_prerequisites) {
      let const prerequisite_path = Path{prerequisite.view()};
      let is_prerequisite_newer = prerequisite_path.is_newer_than(target_path);
      if (!archive_member.is_empty()) {
        let const prerequisite_time = prerequisite_path.modification_time();
        is_prerequisite_newer = prerequisite_time.has_value() &&
                                *prerequisite_time > *archive_member_time;
      }
      if (!prerequisite_path.exists() || is_prerequisite_newer) {
        is_out_of_date = true;
        break;
      }
    }

  if (!is_out_of_date) {
    completed_target_results.set(goal, false);
    return false;
  }

  if (options.should_touch && !recipe_lines->is_empty()) {
    if (!options.is_silent)
      ec.print_to_stdout("touch " + String{cxt.scratch_allocator(), goal} +
                         "\n");
    if (!target_path.exists()) {
      let const fd = os::open_file_descriptor(automatic_target.view(),
                                              os::file_open_mode::Append);
      if (!fd.has_value())
        throw Error{
            "Unable to touch the target '" +
            String{cxt.scratch_allocator(), goal}
            + "'"
        };
      os::close_fd(*fd);
    }
    if (!os::touch_file_times(automatic_target.view()))
      throw Error{
          "Unable to touch the target '" +
          String{cxt.scratch_allocator(), goal}
          + "'"
      };
  }

  let const first_prereq = !inferred_first_prerequisite.is_empty()
                               ? inferred_first_prerequisite.view()
                           : normal_prerequisites.is_empty()
                               ? StringView{}
                               : normal_prerequisites[0].view();
  StringMap<bool> seen_prerequisites{cxt.scratch_allocator()};
  String all_prereqs{cxt.scratch_allocator()};
  String repeated_prereqs{cxt.scratch_allocator()};
  String newer_prereqs{cxt.scratch_allocator()};
  for (const String &prerequisite : normal_prerequisites) {
    if (!repeated_prereqs.is_empty()) repeated_prereqs += ' ';
    repeated_prereqs += prerequisite.view();

    if (seen_prerequisites.find(prerequisite.view()) != nullptr) continue;

    seen_prerequisites.set(prerequisite.view(), true);
    if (!all_prereqs.is_empty()) all_prereqs += ' ';
    all_prereqs += prerequisite.view();

    let const prerequisite_path = Path{prerequisite.view()};
    let is_prerequisite_newer = prerequisite_path.is_newer_than(target_path);
    if (!archive_member.is_empty()) {
      let const prerequisite_time = prerequisite_path.modification_time();
      is_prerequisite_newer = archive_member_time.has_value() &&
                              prerequisite_time.has_value() &&
                              *prerequisite_time > *archive_member_time;
    }
    if (!was_target_existing || !prerequisite_path.exists() ||
        is_prerequisite_newer)
    {
      if (!newer_prereqs.is_empty()) newer_prereqs += ' ';
      newer_prereqs += prerequisite.view();
    }
  }

  let const is_target_silent =
      options.is_silent || mk.silent_targets.find(goal) != nullptr;
  let const should_ignore_target_errors =
      options.should_ignore_errors || mk.ignored_targets.find(goal) != nullptr;

  for (const String &recipe : *recipe_lines) {
    let body = recipe.view();
    bool is_silent = false;
    bool should_ignore_errors = false;
    bool should_force_run = false;
    while (!body.is_empty() &&
           (body[0] == '@' || body[0] == '-' || body[0] == '+'))
    {
      if (body[0] == '@') is_silent = true;
      if (body[0] == '-') should_ignore_errors = true;
      if (body[0] == '+') should_force_run = true;
      body = body.substring(1);
    }

    if ((options.is_query || options.should_touch) && !should_force_run)
      continue;

    /* The automatic variables are filled on the raw recipe first, then the
       $(NAME) expansion runs, so a $$ stays an escape and a $@ that the
       expansion would not touch is resolved here. */
    let const with_autos = substitute_automatic(
        body, automatic_target.view(), archive_member.view(), first_prereq,
        all_prereqs.view(), repeated_prereqs.view(), newer_prereqs.view(),
        target_stem.view(), cxt.scratch_allocator());
    let const command = expand(cxt, mk, with_autos.view(), 0);
    if (command.is_empty()) continue;
    if ((!is_silent && !is_target_silent) || options.is_dry_run)
      ec.print_to_stdout(command + "\n");
    if (options.is_dry_run && !should_force_run) continue;

    /* A recipe runs with the strict toggles off so an unmatched glob or an
       unset variable does not abort the build. */
    let const saved_runtime = RuntimeState::capture(cxt);
    RuntimeState recipe_runtime = saved_runtime;
    recipe_runtime.mood = mimic_mood::Posix;
    recipe_runtime.set_option(shell_option_id::Failglob, false);
    recipe_runtime.set_option(shell_option_id::Nounset, false);
    recipe_runtime.set_option(shell_option_id::Errexit, false);
    recipe_runtime.warning_level = 0;
    recipe_runtime.restore(cxt);
    defer { saved_runtime.restore(cxt); };

    /* Each recipe line runs in its own subshell, the way GNU make spawns a
       shell per line. The parentheses also keep the tail-command exec
       optimization from replacing the make process when the recipe is a single
       external command, which would otherwise abandon the remaining recipe
       lines and targets. The newlines guard the closing paren against a
       trailing comment in the line. */
    let recipe_source = command.clone();
    if (const String *shell_value = mk.find_variable("SHELL");
        shell_value != nullptr)
    {
      let const shell = expand(cxt, mk, shell_value->view(), 0);
      if (!shell.is_empty()) {
        recipe_source = String{cxt.scratch_allocator()};
        append_shell_quoted_arg(recipe_source, shell.view());
        if (!should_ignore_errors && !should_ignore_target_errors)
          recipe_source += " -e";
        recipe_source += " -c ";
        append_shell_quoted_arg(recipe_source, command.view());
      }
    }

    let subshell_command = String{cxt.scratch_allocator(), "(\n"};
    if (!should_ignore_errors && !should_ignore_target_errors)
      subshell_command += "set -e\n";
    subshell_command += recipe_source.view();
    subshell_command += "\n)";
    i32 status = 0;
    try {
      status = cxt.run_source(subshell_command.view(), "make",
                              return_handling::Consume, ec.source_location(),
                              StringView{"make"});
      if (os::INTERRUPT_REQUESTED) {
        os::INTERRUPT_REQUESTED = 0;
        let interrupt_error = InterruptErrorWithLocation{ec.source_location()};
        interrupt_error.set_command_status(130);
        throw interrupt_error;
      }
    } catch (InterruptErrorWithLocation &interrupt_error) {
      if (!options.is_dry_run && !options.is_print_database &&
          !options.is_query && target_path.exists() &&
          !target_path.is_directory() && !mk.is_every_target_precious &&
          mk.precious_targets.find(goal) == NULL)
        unused(os::remove_file(goal));
      interrupt_error.set_command_status(130);
      throw;
    }
    if (status != 0 && !should_ignore_errors && !should_ignore_target_errors) {
      if (!was_target_existing && target_path.exists() &&
          !mk.is_every_target_precious &&
          mk.precious_targets.find(goal) == nullptr)
        unused(os::remove_file(goal));

      throw Error{
          "The recipe for the target '" +
          String{cxt.scratch_allocator(), goal}
          + "' failed with status " +
          String::from(status, cxt.scratch_allocator())
      };
    }
  }

  completed_target_results.set(goal, true);
  return true;
}

} /* namespace */

Make::Make() = default;

pure fn Make::kind() const wontthrow -> Utility::Kind { return Kind::Make; }

fn Make::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  unused(arg_locations);
  ArrayList<String> filtered{cxt.scratch_allocator()};
  ArrayList<String> requested_makefiles{cxt.scratch_allocator()};
  ArrayList<String> option_order{cxt.scratch_allocator()};
  ArrayList<String> command_assignments{cxt.scratch_allocator()};
  if (!args.is_empty()) filtered.push(args[0].clone());
  if (let inherited_makeflags = os::get_environment_variable("MAKEFLAGS");
      inherited_makeflags.has_value())
    for (const String &word : split_makeflags_words(inherited_makeflags->view(),
                                                    cxt.scratch_allocator()))
    {
      if (word.is_empty()) continue;
      if (is_command_line_assignment(word.view())) {
        command_assignments.push(word.clone());
        continue;
      }
      let letters = word.view();
      if (letters == "--") break;
      if (letters[0] == '-') letters = letters.substring(1);
      if (letters.is_empty() || letters[0] == '-') continue;

      bool has_only_make_flags = true;
      for (usize letter_position = 0; letter_position < letters.length;
           letter_position++)
        if (!StringView{"BeiknpqrSst"}
                 .find_character(letters[letter_position])
                 .has_value())
        {
          has_only_make_flags = false;
          break;
        }
      if (!has_only_make_flags) continue;

      let inherited_options = String{cxt.scratch_allocator(), "-"};
      inherited_options += letters;
      filtered.push(inherited_options.clone());
      option_order.push(steal(inherited_options));
    }

  for (usize arg_index = 1; arg_index < args.count(); arg_index++) {
    const String &arg = args[arg_index];
    let const text = arg.view();
    option_order.push(arg.clone());
    if ((text == "-f" || text == "--file") && arg_index + 1 < args.count()) {
      requested_makefiles.push(args[++arg_index].clone());
      continue;
    }
    if (text.starts_with(StringView{"--file="})) {
      requested_makefiles.push(
          String{cxt.scratch_allocator(), text.substring(7)});
      continue;
    }
    bool was_makefile_option = false;
    if (text.length > 2 && text[0] == '-' && text[1] != '-') {
      for (usize option_position = 1; option_position < text.length;
           option_position++)
      {
        if (text[option_position] == 'C' || text[option_position] == 'j') break;
        if (text[option_position] != 'f') continue;

        if (option_position > 1)
          filtered.push(String{cxt.scratch_allocator(),
                               text.substring_of_length(0, option_position)});
        if (option_position + 1 < text.length) {
          requested_makefiles.push(String{cxt.scratch_allocator(),
                                          text.substring(option_position + 1)});
        } else if (arg_index + 1 < args.count()) {
          requested_makefiles.push(args[++arg_index].clone());
        }
        was_makefile_option = true;
        break;
      }
    }
    if (was_makefile_option) continue;
    bool is_jobs_flag = text == "-j";
    if (!is_jobs_flag && text.length > 2 && text[0] == '-' && text[1] == 'j') {
      is_jobs_flag = true;
      for (usize k = 2; k < text.length; k++)
        if (text[k] < '0' || text[k] > '9') {
          is_jobs_flag = false;
          break;
        }
    }
    if (!is_jobs_flag) filtered.push(arg.clone());
  }

  /* A recipe's $(MAKE) re-enters this util while the outer call is still on the
     stack, and the flag list is shared, so the inherited -C or -f is cleared
     before parsing. The outer call already read its flags into locals, so the
     reset does not disturb it. */
  reset_flags(FLAG_LIST);
  let const operands =
      parse_util_operands(FLAG_LIST, filtered, NULL, NULL, false, true);
  defer { reset_flags(FLAG_LIST); };

  let should_keep_going = false;
  for (const String &arg : option_order) {
    let const text = arg.view();
    if (text == "--keep-going") {
      should_keep_going = true;
      continue;
    }
    if (text == "--stop") {
      should_keep_going = false;
      continue;
    }
    if (text.length < 2 || text[0] != '-' || text[1] == '-') continue;

    for (usize option_position = 1; option_position < text.length;
         option_position++)
    {
      if (text[option_position] == 'k') should_keep_going = true;
      if (text[option_position] == 'S') should_keep_going = false;
      if (text[option_position] == 'f' || text[option_position] == 'C') break;
    }
  }

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  Maybe<Path> saved_directory;
  if (FLAG_MAKE_DIR.is_set()) {
    saved_directory = Path::current_directory();
    if (Path::set_current_directory(Path{FLAG_MAKE_DIR.value()}).is_error())
      throw ErrorWithDetails{
          "Unable to change to the directory '" +
              String{cxt.scratch_allocator(), FLAG_MAKE_DIR.value()}
              +
              "': " + os::last_system_error_message(),
          "Verify the `-C` path exists and is a directory"
      };
  }
  defer
  {
    if (saved_directory.has_value())
      static_cast<void>(Path::set_current_directory(*saved_directory));
  };

  if (requested_makefiles.is_empty()) {
    if (Path{"makefile"}.exists())
      requested_makefiles.push(String{cxt.scratch_allocator(), "makefile"});
    else if (Path{"Makefile"}.exists())
      requested_makefiles.push(String{cxt.scratch_allocator(), "Makefile"});
  }

  String source{cxt.scratch_allocator()};
  for (const String &makefile_path : requested_makefiles) {
    if (makefile_path.view() == StringView{"-"}) {
      source += utils::read_entire_standard_input().view();
      source += '\n';
      continue;
    }

    let const part = Path{makefile_path.view()}.read_entire_file();
    if (!part.has_value())
      throw ErrorWithDetails{"Unable to read the makefile '" + makefile_path +
                                 "': " + os::last_system_error_message(),
                             "Check the path passed to `-f`"};
    source += part->view();
    source += '\n';
  }

  ArrayList<String> goals{cxt.scratch_allocator()};
  StringMap<bool> command_line_variable_names{cxt.scratch_allocator()};
  for (const String &operand : operands) {
    if (is_command_line_assignment(operand.view())) {
      command_assignments.push(operand.clone());
      command_line_variable_names.set(assignment_variable_name(operand.view()),
                                      true);
    } else {
      goals.push(operand.clone());
    }
  }

  String makeflags{cxt.scratch_allocator()};
  if (FLAG_MAKE_ALWAYS_MAKE.is_enabled()) makeflags += 'B';
  if (FLAG_MAKE_ENVIRONMENT_OVERRIDES.is_enabled()) makeflags += 'e';
  if (FLAG_MAKE_IGNORE_ERRORS.is_enabled()) makeflags += 'i';
  if (should_keep_going)
    makeflags += 'k';
  else if (FLAG_MAKE_STOP.is_enabled())
    makeflags += 'S';
  if (FLAG_MAKE_DRY_RUN.is_enabled()) makeflags += 'n';
  if (FLAG_MAKE_QUESTION.is_enabled()) makeflags += 'q';
  if (FLAG_MAKE_NO_BUILTINS.is_enabled()) makeflags += 'r';
  if (FLAG_MAKE_SILENT.is_enabled()) makeflags += 's';
  if (FLAG_MAKE_TOUCH.is_enabled()) makeflags += 't';

  for (const String &assignment : command_assignments) {
    if (assignment_variable_name(assignment.view()) == StringView{"MAKEFLAGS"})
      continue;
    if (!makeflags.is_empty()) makeflags += ' ';
    for (usize byte_position = 0; byte_position < assignment.view().length;
         byte_position++)
    {
      let const byte = assignment.view()[byte_position];
      if (byte == '\\' || is_blank(byte)) makeflags += '\\';
      makeflags += byte;
    }
  }

  let has_command_makeflags = false;
  for (const String &assignment : command_assignments)
    if (assignment_variable_name(assignment.view()) == StringView{"MAKEFLAGS"})
    {
      has_command_makeflags = true;
      break;
    }
  if (!has_command_makeflags)
    command_assignments.push("MAKEFLAGS=" + makeflags);

  let previous_makeflags = os::get_environment_variable("MAKEFLAGS");
  os::set_environment_variable("MAKEFLAGS", makeflags.view());
  defer
  {
    if (previous_makeflags.has_value())
      os::set_environment_variable("MAKEFLAGS", previous_makeflags->view());
    else
      os::unset_environment_variable("MAKEFLAGS");
  };

  let mk = parse_makefile(cxt, source.view(), command_assignments,
                          FLAG_MAKE_ENVIRONMENT_OVERRIDES.is_enabled(),
                          !FLAG_MAKE_NO_BUILTINS.is_enabled());

  ArrayList<saved_recipe_environment> saved_recipe_environment_values{
      cxt.scratch_allocator()};
  for (const make_variable &variable : mk.variables) {
    if (variable.name.view() == StringView{"MAKEFLAGS"} ||
        variable.name.view() == StringView{"SHELL"})
      continue;
    let old_value = os::get_environment_variable(variable.name.view());
    let const is_command_variable =
        mk.command_variable_names.find(variable.name.view()) != nullptr;
    let const is_command_line_variable =
        command_line_variable_names.find(variable.name.view()) != NULL;
    if (!is_command_line_variable && !old_value.has_value()) continue;
    if (!is_command_variable && old_value.has_value() &&
        mk.does_environment_override)
      continue;

    saved_recipe_environment_values.push(saved_recipe_environment{
        variable.name.clone(), old_value.has_value()
                                   ? Maybe<String>{old_value->clone()}
                                   : Maybe<String>{}});
    let const expanded_value = expand(cxt, mk, variable.value.view(), 0);
    os::set_environment_variable(variable.name.view(), expanded_value.view());
  }
  defer
  {
    for (usize environment_index = saved_recipe_environment_values.count();
         environment_index-- > 0;)
    {
      const saved_recipe_environment &saved =
          saved_recipe_environment_values[environment_index];
      if (saved.old_value.has_value())
        os::set_environment_variable(saved.name.view(),
                                     saved.old_value->view());
      else
        os::unset_environment_variable(saved.name.view());
    }
  };

  if (FLAG_MAKE_PRINT_DATABASE.is_enabled()) {
    let suffix_description = String{cxt.scratch_allocator(), ".SUFFIXES:"};
    for (const String &suffix : mk.suffixes)
      suffix_description += " " + suffix;
    suffix_description += '\n';
    ec.print_to_stdout(suffix_description);
    for (const auto &entry : BUILTIN_VARIABLE_ENTRIES)
      ec.print_to_stdout(entry.key.to_string() + " = " + entry.value + "\n");
    for (const make_variable &variable : mk.variables)
      ec.print_to_stdout(variable.name + " = " + variable.value + "\n");
    for (const make_rule &rule : mk.rules) {
      let description = rule.target + ":";
      for (const String &prerequisite : rule.prerequisites)
        description += " " + prerequisite;
      description += '\n';
      ec.print_to_stdout(description);
      for (const String &recipe : rule.recipe_lines)
        ec.print_to_stdout("\t" + recipe + "\n");
    }
    for (const make_rule &rule : mk.pattern_rules) {
      let description = rule.target + ":";
      for (const String &prerequisite : rule.prerequisites)
        description += " " + prerequisite;
      description += '\n';
      ec.print_to_stdout(description);
      for (const String &recipe : rule.recipe_lines)
        ec.print_to_stdout("\t" + recipe + "\n");
    }
  }

  if (goals.is_empty()) {
    if (mk.default_goal.is_empty() && FLAG_MAKE_PRINT_DATABASE.is_enabled())
      return 0;
    if (mk.default_goal.is_empty())
      throw ErrorWithDetails{
          "The makefile defines no targets and no default goal",
          "Add a rule or name a target on the command line"};
    goals.push(mk.default_goal.clone());
  }

  StringMap<bool> active_targets{cxt.scratch_allocator()};
  StringMap<bool> completed_target_results{cxt.scratch_allocator()};
  let const options = make_build_options{
      FLAG_MAKE_ALWAYS_MAKE.is_enabled(),
      FLAG_MAKE_QUESTION.is_enabled(),
      FLAG_MAKE_IGNORE_ERRORS.is_enabled() || mk.should_ignore_errors,
      should_keep_going,
      FLAG_MAKE_DRY_RUN.is_enabled(),
      FLAG_MAKE_PRINT_DATABASE.is_enabled(),
      FLAG_MAKE_SILENT.is_enabled() || mk.is_silent,
      FLAG_MAKE_TOUCH.is_enabled()};
  let has_outdated_goal = false;
  let did_fail = false;
  try {
    for (const String &goal : goals) {
      try {
        has_outdated_goal |=
            build_target(ec, cxt, mk, goal.view(), active_targets,
                         completed_target_results, options);
      } catch (Error &error) {
        if (!should_keep_going) throw;
        did_fail = true;
        report_soft_koshkit_error(ec, cxt, error.message().view());
      }
    }
  } catch (const InterruptErrorWithLocation &) {
    throw;
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (Error &error) {
    error.set_command_status(2);
    throw;
  }

  if (did_fail) return 2;
  if (!options.is_query && !has_outdated_goal)
    ec.print_to_stdout("make: No action was needed.\n");

  return options.is_query && has_outdated_goal ? 1 : 0;
}

fn collect_makefile_targets(EvalContext &cxt, const Path &makefile) throws
    -> ArrayList<String>
{
  let targets = ArrayList<String>{cxt.scratch_allocator()};
  let const source = makefile.read_entire_file();
  if (!source.has_value()) return targets;

  /* Completion leaves the makefile's $(shell ...) functions unrun. */
  let const saved_suppressed = cxt.make_shell_suppressed();
  cxt.set_make_shell_suppressed(true);
  defer { cxt.set_make_shell_suppressed(saved_suppressed); };

  let const command_assignments = ArrayList<String>{cxt.scratch_allocator()};
  let const mk =
      parse_makefile(cxt, source->view(), command_assignments, false, true);
  for (const make_rule &rule : mk.rules) {
    let const name = rule.target.view();
    if (name.is_empty() || name[0] == '.') continue;
    targets.push(rule.target.clone());
  }
  return targets;
}

} /* namespace koshkit */

} /* namespace koshka */
