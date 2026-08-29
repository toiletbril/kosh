#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

#include <cstdio>

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

enum class make_function_kind : u8
{
  Abspath,
  Addprefix,
  Addsuffix,
  And,
  Basename,
  Call,
  Dir,
  Error,
  Filter,
  FilterOut,
  Findstring,
  Firstword,
  Flavor,
  Foreach,
  If,
  Join,
  Lastword,
  Notdir,
  Or,
  Origin,
  Patsubst,
  Realpath,
  Shell,
  Sort,
  Strip,
  Subst,
  Suffix,
  Value,
  Warning,
  Wildcard,
  Word,
  Wordlist,
  Words,
};

struct make_function_spec
{
  make_function_kind kind;
  u8 minimum_argument_count;
  u8 maximum_argument_count;
};

constexpr static_string_entry<make_function_spec> MAKE_FUNCTION_ENTRIES[] = {
    {SSK("abspath"),    {make_function_kind::Abspath, 0, 1}     },
    {SSK("addprefix"),  {make_function_kind::Addprefix, 2, 2}   },
    {SSK("addsuffix"),  {make_function_kind::Addsuffix, 2, 2}   },
    {SSK("and"),        {make_function_kind::And, 1, UINT8_MAX} },
    {SSK("basename"),   {make_function_kind::Basename, 0, 1}    },
    {SSK("call"),       {make_function_kind::Call, 1, UINT8_MAX}},
    {SSK("dir"),        {make_function_kind::Dir, 0, 1}         },
    {SSK("error"),      {make_function_kind::Error, 0, 1}       },
    {SSK("filter"),     {make_function_kind::Filter, 2, 2}      },
    {SSK("filter-out"), {make_function_kind::FilterOut, 2, 2}   },
    {SSK("findstring"), {make_function_kind::Findstring, 2, 2}  },
    {SSK("firstword"),  {make_function_kind::Firstword, 0, 1}   },
    {SSK("flavor"),     {make_function_kind::Flavor, 0, 1}      },
    {SSK("foreach"),    {make_function_kind::Foreach, 3, 3}     },
    {SSK("if"),         {make_function_kind::If, 2, 3}          },
    {SSK("join"),       {make_function_kind::Join, 2, 2}        },
    {SSK("lastword"),   {make_function_kind::Lastword, 0, 1}    },
    {SSK("notdir"),     {make_function_kind::Notdir, 0, 1}      },
    {SSK("or"),         {make_function_kind::Or, 1, UINT8_MAX}  },
    {SSK("origin"),     {make_function_kind::Origin, 0, 1}      },
    {SSK("patsubst"),   {make_function_kind::Patsubst, 3, 3}    },
    {SSK("realpath"),   {make_function_kind::Realpath, 0, 1}    },
    {SSK("shell"),      {make_function_kind::Shell, 0, 1}       },
    {SSK("sort"),       {make_function_kind::Sort, 0, 1}        },
    {SSK("strip"),      {make_function_kind::Strip, 0, 1}       },
    {SSK("subst"),      {make_function_kind::Subst, 3, 3}       },
    {SSK("suffix"),     {make_function_kind::Suffix, 0, 1}      },
    {SSK("value"),      {make_function_kind::Value, 0, 1}       },
    {SSK("warning"),    {make_function_kind::Warning, 0, 1}     },
    {SSK("wildcard"),   {make_function_kind::Wildcard, 0, 1}    },
    {SSK("word"),       {make_function_kind::Word, 2, 2}        },
    {SSK("wordlist"),   {make_function_kind::Wordlist, 3, 3}    },
    {SSK("words"),      {make_function_kind::Words, 0, 1}       },
};
constexpr StaticStringMap MAKE_FUNCTIONS{MAKE_FUNCTION_ENTRIES};

constexpr PackedStringKey SPECIAL_TARGET_KEYS[] = {
    SSK(".DEFAULT"),  SSK(".IGNORE"),   SSK(".PHONY"),  SSK(".POSIX"),
    SSK(".PRECIOUS"), SSK(".SCCS_GET"), SSK(".SILENT"), SSK(".SUFFIXES"),
};
constexpr StaticStringSet SPECIAL_TARGETS{SPECIAL_TARGET_KEYS};

constexpr PackedStringKey CONDITIONAL_DIRECTIVE_KEYS[] = {
    SSK("ifeq"), SSK("ifneq"), SSK("ifdef"), SSK("ifndef")};
constexpr StaticStringSet CONDITIONAL_DIRECTIVES{CONDITIONAL_DIRECTIVE_KEYS};

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

enum class make_variable_origin : u8
{
  Automatic,
  CommandLine,
  Default,
  Environment,
  File,
  Undefined,
};

enum class make_variable_flavor : u8
{
  Recursive,
  Simple,
  Undefined,
};

struct make_variable
{
  make_variable(String name, String value, make_variable_origin origin,
                make_variable_flavor flavor)
      : name(steal(name)), value(steal(value)), origin(origin), flavor(flavor)
  {}
  String name;
  String value;
  make_variable_origin origin;
  make_variable_flavor flavor;
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
        exported_variable_names(allocator),
        unexported_variable_names(allocator), suffixes(allocator),
        phony_targets(allocator), precious_targets(allocator),
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
  StringMap<bool> exported_variable_names;
  StringMap<bool> unexported_variable_names;
  ArrayList<String> suffixes;
  StringMap<bool> phony_targets;
  StringMap<bool> precious_targets;
  StringMap<bool> ignored_targets;
  StringMap<bool> silent_targets;
  bool does_environment_override{false};
  bool should_ignore_errors{false};
  bool is_silent{false};
  bool is_every_target_precious{false};
  usize call_max_argument_count{0};

  fn find_variable(StringView name) const throws -> const String *
  {
    if (let const *index = variable_index.find(name); index != nullptr)
      return &variables[*index].value;
    return nullptr;
  }

  fn find_variable_record(StringView name) const throws -> const make_variable *
  {
    if (let const *index = variable_index.find(name); index != NULL)
      return &variables[*index];
    return NULL;
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

  fn remove_variable(StringView name) throws -> void
  {
    let const *stored_index = variable_index.find(name);
    if (stored_index == NULL) return;
    let const index = *stored_index;
    let const last_index = variables.count() - 1;
    variable_index.erase(name);
    if (index != last_index) {
      variables[index] = steal(variables[last_index]);
      variable_index.set(variables[index].name.view(), index);
    }
    variables.pop_back();
  }
};

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

static fn substitute_make_words(StringView source, StringView pattern_text,
                                StringView replacement, Allocator allocator,
                                bool is_suffix_without_wildcard) throws
    -> String;

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
        usize close_position = i + 2;
        while (close_position < text.length && text[close_position] != close)
          close_position++;
        if (close_position < text.length) {
          let const contents =
              text.substring_of_length(i + 2, close_position - (i + 2));
          if (!contents.is_empty() &&
              StringView{"@%<^+?*"}.find_character(contents[0]).has_value())
          {
            usize contents_position = 1;
            let value = String{allocator, do_value_for(contents[0])};
            if (contents_position < contents.length &&
                (contents[contents_position] == 'D' ||
                 contents[contents_position] == 'F'))
            {
              value = automatic_path_part(
                  value.view(), contents[contents_position] == 'F', allocator);
              contents_position++;
            }
            if (contents_position == contents.length) {
              out += value.view();
              i = close_position + 1;
              continue;
            }
            if (contents[contents_position] == ':') {
              let const substitution =
                  contents.substring(contents_position + 1);
              if (let const equals = substitution.find_character('=');
                  equals.has_value())
              {
                out += substitute_make_words(
                           value.view(),
                           substitution.substring_of_length(0, *equals),
                           substitution.substring(*equals + 1), allocator, true)
                           .view();
                i = close_position + 1;
                continue;
              }
            }
          }
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

static fn is_command_line_assignment(StringView operand) wontthrow -> bool;

struct make_runtime_flags
{
  bool should_always_make{false};
  bool does_environment_override{false};
  bool should_ignore_errors{false};
  bool should_keep_going{false};
  bool is_dry_run{false};
  bool is_query{false};
  bool is_print_database{false};
  bool should_disable_builtin_rules{false};
  bool is_silent{false};
  bool should_touch{false};
};

static fn apply_makeflags(StringView text, make_runtime_flags &flags,
                          Allocator allocator) throws -> void
{
  for (const String &word : split_makeflags_words(text, allocator)) {
    if (word.is_empty() || is_command_line_assignment(word.view())) continue;
    let letters = word.view();
    if (letters == "--") break;
    if (letters[0] == '-') letters = letters.substring(1);
    if (letters.is_empty() || letters[0] == '-') continue;

    for (usize letter_position = 0; letter_position < letters.length;
         letter_position++)
    {
      let const letter = letters[letter_position];
      switch (letter) {
      case 'B': flags.should_always_make = true; break;
      case 'e': flags.does_environment_override = true; break;
      case 'i': flags.should_ignore_errors = true; break;
      case 'k': flags.should_keep_going = true; break;
      case 'S': flags.should_keep_going = false; break;
      case 'n': flags.is_dry_run = true; break;
      case 'p': flags.is_print_database = true; break;
      case 'q': flags.is_query = true; break;
      case 'r': flags.should_disable_builtin_rules = true; break;
      case 's': flags.is_silent = true; break;
      case 't': flags.should_touch = true; break;
      default: break;
      }
    }
  }
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
  StringView long_names;
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
    } else if (member_name == StringView{"//"}) {
      long_names = contents->view().substring_of_length(
          content_position, static_cast<usize>(member_size.value()));
    } else if (member_name == StringView{"/"} ||
               member_name == StringView{"/SYM64/"})
    {
      member_name = StringView{};
    } else if (member_name.length > 1 && member_name[0] == '/') {
      let const parsed_offset =
          utils::parse_decimal_u64(member_name.substring(1));
      if (parsed_offset.is_error() ||
          parsed_offset.value() >= long_names.length)
        return None;
      let const name_start = static_cast<usize>(parsed_offset.value());
      usize name_end = name_start;
      while (name_end < long_names.length && long_names[name_end] != '\n')
        name_end++;
      member_name =
          long_names.substring_of_length(name_start, name_end - name_start);
      if (!member_name.is_empty() && member_name[member_name.length - 1] == '/')
        member_name =
            member_name.substring_of_length(0, member_name.length - 1);
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

static fn expand(EvalContext &cxt, makefile &mk, StringView text,
                 usize depth) throws -> String;

struct make_variable_lookup
{
  explicit make_variable_lookup(Allocator allocator) : owned_value(allocator) {}
  String owned_value;
  StringView value;
  make_variable_origin origin{make_variable_origin::Undefined};
  make_variable_flavor flavor{make_variable_flavor::Undefined};
  bool is_value_owned{false};

  fn set_owned_value(String value_to_own) wontthrow -> void
  {
    owned_value = steal(value_to_own);
    value = owned_value.view();
    is_value_owned = true;
  }

  fn set_borrowed_value(StringView value_to_borrow) wontthrow -> void
  {
    value = value_to_borrow;
    is_value_owned = false;
  }

  fn into_string(Allocator allocator) throws -> String
  {
    return is_value_owned ? steal(owned_value) : String{allocator, value};
  }
};

static fn lookup_make_variable(EvalContext &cxt, const makefile &mk,
                               StringView name) throws -> make_variable_lookup
{
  make_variable_lookup lookup{cxt.scratch_allocator()};
  let const do_use_stored = [&](const make_variable &variable) {
    lookup.set_borrowed_value(variable.value.view());
    lookup.origin = variable.origin;
    lookup.flavor = variable.flavor;
  };

  if (mk.command_variable_names.find(name) != NULL)
    if (const make_variable *variable = mk.find_variable_record(name);
        variable != NULL)
    {
      do_use_stored(*variable);
      return lookup;
    }

  if (name.length == 2 && (name[1] == 'D' || name[1] == 'F') &&
      StringView{"@%<^+?*"}.find_character(name[0]).has_value())
  {
    let const base_name = name.substring_of_length(0, 1);
    if (const make_variable *variable = mk.find_variable_record(base_name);
        variable != NULL && variable->origin == make_variable_origin::Automatic)
    {
      lookup.set_owned_value(automatic_path_part(
          variable->value.view(), name[1] == 'F', cxt.scratch_allocator()));
      lookup.origin = make_variable_origin::Automatic;
      lookup.flavor = make_variable_flavor::Simple;
      return lookup;
    }
  }

  if (name == StringView{"CURDIR"}) {
    lookup.set_owned_value(Path::current_directory().text().clone());
    lookup.origin = make_variable_origin::File;
    lookup.flavor = make_variable_flavor::Simple;
    return lookup;
  }

  if (name == StringView{"SHELL"}) {
    if (const make_variable *variable = mk.find_variable_record(name);
        variable != NULL)
    {
      do_use_stored(*variable);
      return lookup;
    }
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
    lookup.set_borrowed_value(cxt.shell_executable_path());
#else
    lookup.set_borrowed_value("/bin/sh");
#endif
    lookup.origin = make_variable_origin::Default;
    lookup.flavor = make_variable_flavor::Recursive;
    return lookup;
  }

  if (mk.does_environment_override)
    if (Maybe<String> from_environment = os::get_environment_variable(name);
        from_environment.has_value())
    {
      lookup.set_owned_value(steal(*from_environment));
      lookup.origin = make_variable_origin::Environment;
      lookup.flavor = make_variable_flavor::Recursive;
      return lookup;
    }

  if (const make_variable *variable = mk.find_variable_record(name);
      variable != NULL)
  {
    do_use_stored(*variable);
    return lookup;
  }

  if (Maybe<String> from_environment = os::get_environment_variable(name);
      from_environment.has_value())
  {
    lookup.set_owned_value(steal(*from_environment));
    lookup.origin = make_variable_origin::Environment;
    lookup.flavor = make_variable_flavor::Recursive;
    return lookup;
  }

  if (Maybe<const char *> builtin = BUILTIN_VARIABLES.find(name);
      builtin.has_value())
  {
    lookup.set_borrowed_value(*builtin);
    lookup.origin = make_variable_origin::Default;
    lookup.flavor = make_variable_flavor::Recursive;
  }

  return lookup;
}

static fn expand_variable(EvalContext &cxt, makefile &mk, StringView name,
                          usize depth) throws -> String
{
  let lookup = lookup_make_variable(cxt, mk, name);
  if (lookup.flavor == make_variable_flavor::Simple)
    return lookup.into_string(cxt.scratch_allocator());
  if (lookup.flavor == make_variable_flavor::Undefined)
    return String{cxt.scratch_allocator()};
  let recursive_value = lookup.into_string(cxt.scratch_allocator());
  return expand(cxt, mk, recursive_value.view(), depth + 1);
}

static fn make_variable_origin_text(make_variable_origin origin) wontthrow
    -> const char *
{
  switch (origin) {
  case make_variable_origin::Automatic: return "automatic";
  case make_variable_origin::CommandLine: return "command line";
  case make_variable_origin::Default: return "default";
  case make_variable_origin::Environment: return "environment";
  case make_variable_origin::File: return "file";
  case make_variable_origin::Undefined: return "undefined";
  }
  unreachable();
}

static fn make_variable_flavor_text(make_variable_flavor flavor) wontthrow
    -> const char *
{
  switch (flavor) {
  case make_variable_flavor::Recursive: return "recursive";
  case make_variable_flavor::Simple: return "simple";
  case make_variable_flavor::Undefined: return "undefined";
  }
  unreachable();
}

static fn split_word_views(StringView text, Allocator allocator) throws
    -> ArrayList<StringView>
{
  ArrayList<StringView> words{allocator};
  usize text_position = 0;

  while (text_position < text.length) {
    while (text_position < text.length && is_blank(text[text_position]))
      text_position++;
    let const word_start = text_position;
    while (text_position < text.length && !is_blank(text[text_position]))
      text_position++;
    if (text_position > word_start)
      words.push(
          text.substring_of_length(word_start, text_position - word_start));
  }

  return words;
}

static fn append_make_word(String &result, StringView word,
                           bool &has_word) throws -> void
{
  if (has_word) result += ' ';
  result += word;
  has_word = true;
}

static fn find_make_text(StringView text, StringView wanted) wontthrow
    -> Maybe<usize>
{
  if (wanted.is_empty()) return 0;
  if (wanted.length > text.length) return None;

  for (usize position = 0; position + wanted.length <= text.length; position++)
  {
    if (text[position] != wanted[0]) continue;
    if (text.substring_of_length(position, wanted.length) == wanted)
      return position;
  }

  return None;
}

static pure fn recipe_references_make_variable(StringView recipe) wontthrow
    -> bool
{
  return find_make_text(recipe, "$(MAKE)").has_value() ||
         find_make_text(recipe, "${MAKE}").has_value();
}

struct make_pattern
{
  explicit make_pattern(Allocator allocator)
      : prefix(allocator), suffix(allocator)
  {}
  String prefix;
  String suffix;
  bool has_wildcard{false};
};

struct make_percent_text
{
  explicit make_percent_text(Allocator allocator) : text(allocator) {}
  String text;
  Maybe<usize> wildcard_position;
};

static fn decode_make_percent_text(StringView source,
                                   Allocator allocator) throws
    -> make_percent_text
{
  make_percent_text decoded{allocator};
  usize source_position = 0;
  while (source_position < source.length) {
    if (source[source_position] == '%') {
      if (!decoded.wildcard_position.has_value())
        decoded.wildcard_position = decoded.text.count();
      else
        decoded.text.push('%');
      source_position++;
      continue;
    }
    if (source[source_position] != '\\') {
      decoded.text.push(source[source_position++]);
      continue;
    }

    let const backslash_start = source_position;
    while (source_position < source.length && source[source_position] == '\\')
      source_position++;
    let const backslash_count = source_position - backslash_start;
    if (source_position == source.length || source[source_position] != '%') {
      for (usize backslash_position = 0; backslash_position < backslash_count;
           backslash_position++)
        decoded.text.push('\\');
      continue;
    }

    for (usize backslash_position = 0; backslash_position < backslash_count / 2;
         backslash_position++)
      decoded.text.push('\\');
    if ((backslash_count & 1u) != 0)
      decoded.text.push('%');
    else if (!decoded.wildcard_position.has_value())
      decoded.wildcard_position = decoded.text.count();
    else
      decoded.text.push('%');
    source_position++;
  }
  return decoded;
}

static fn parse_make_pattern(StringView pattern, Allocator allocator) throws
    -> make_pattern
{
  make_pattern parsed{allocator};
  let decoded = decode_make_percent_text(pattern, allocator);
  if (decoded.wildcard_position.has_value()) {
    parsed.has_wildcard = true;
    parsed.prefix = String{allocator, decoded.text.view().substring_of_length(
                                          0, *decoded.wildcard_position)};
    parsed.suffix = String{
        allocator, decoded.text.view().substring(*decoded.wildcard_position)};
  } else {
    parsed.prefix = steal(decoded.text);
  }

  return parsed;
}

static fn match_make_pattern(const make_pattern &pattern,
                             StringView word) wontthrow -> Maybe<StringView>
{
  if (!pattern.has_wildcard)
    return word == pattern.prefix.view() ? Maybe<StringView>{StringView{}}
                                         : None;
  if (word.length < pattern.prefix.count() + pattern.suffix.count())
    return None;
  if (!word.starts_with(pattern.prefix.view())) return None;
  if (word.substring(word.length - pattern.suffix.count()) !=
      pattern.suffix.view())
    return None;

  return word.substring_of_length(pattern.prefix.count(),
                                  word.length - pattern.prefix.count() -
                                      pattern.suffix.count());
}

static fn append_make_replacement(String &result, StringView replacement,
                                  StringView stem, bool has_wildcard) throws
    -> void
{
  let decoded = decode_make_percent_text(replacement, result.allocator());
  if (has_wildcard && decoded.wildcard_position.has_value()) {
    result +=
        decoded.text.view().substring_of_length(0, *decoded.wildcard_position);
    result += stem;
    result += decoded.text.view().substring(*decoded.wildcard_position);
  } else {
    result += decoded.text.view();
  }
}

static fn substitute_make_words(StringView source, StringView pattern_text,
                                StringView replacement, Allocator allocator,
                                bool is_suffix_without_wildcard) throws
    -> String
{
  let const pattern = parse_make_pattern(pattern_text, allocator);
  let result = String{allocator};
  bool has_word = false;
  for (StringView word : split_word_views(source, allocator)) {
    if (has_word) result += ' ';
    if (pattern.has_wildcard) {
      if (let const stem = match_make_pattern(pattern, word); stem.has_value())
        append_make_replacement(result, replacement, *stem, true);
      else
        result += word;
    } else if (is_suffix_without_wildcard &&
               word.length >= pattern.prefix.count() &&
               word.substring(word.length - pattern.prefix.count()) ==
                   pattern.prefix.view())
    {
      result +=
          word.substring_of_length(0, word.length - pattern.prefix.count());
      result += replacement;
    } else if (!is_suffix_without_wildcard && word == pattern.prefix.view()) {
      append_make_replacement(result, replacement, StringView{}, false);
    } else {
      result += word;
    }
    has_word = true;
  }
  return result;
}

struct make_variable_snapshot
{
  explicit make_variable_snapshot(Allocator allocator)
      : name(allocator), value(allocator)
  {}
  String name;
  String value;
  make_variable_origin origin{make_variable_origin::Undefined};
  make_variable_flavor flavor{make_variable_flavor::Undefined};
  bool was_present{false};
};

static fn save_make_variable(const makefile &mk, StringView name,
                             Allocator allocator) throws
    -> make_variable_snapshot
{
  make_variable_snapshot snapshot{allocator};
  snapshot.name = String{allocator, name};
  if (const make_variable *variable = mk.find_variable_record(name);
      variable != NULL)
  {
    snapshot.value = String{allocator, variable->value.view()};
    snapshot.origin = variable->origin;
    snapshot.flavor = variable->flavor;
    snapshot.was_present = true;
  }
  return snapshot;
}

static fn set_scoped_make_variable(makefile &mk, StringView name,
                                   StringView value, Allocator allocator) throws
    -> void
{
  if (let const *index = mk.variable_index.find(name); index != NULL) {
    make_variable &variable = mk.variables[*index];
    variable.value = String{allocator, value};
    variable.origin = make_variable_origin::Automatic;
    variable.flavor = make_variable_flavor::Simple;
    return;
  }

  mk.variable_index.set(name, mk.variables.count());
  mk.variables.push(make_variable{
      String{allocator, name },
      String{allocator, value},
      make_variable_origin::Automatic, make_variable_flavor::Simple
  });
}

static fn restore_make_variable(makefile &mk,
                                const make_variable_snapshot &snapshot,
                                Allocator allocator) throws -> void
{
  if (!snapshot.was_present) {
    ASSERT(mk.variable_index.find(snapshot.name.view()) != NULL);
    ASSERT(*mk.variable_index.find(snapshot.name.view()) + 1 ==
           mk.variables.count());
    mk.variable_index.erase(snapshot.name.view());
    mk.variables.pop_back();
    return;
  }

  let const *index = mk.variable_index.find(snapshot.name.view());
  ASSERT(index != NULL);
  make_variable &variable = mk.variables[*index];
  variable.value = String{allocator, snapshot.value.view()};
  variable.origin = snapshot.origin;
  variable.flavor = snapshot.flavor;
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

static fn run_make_shell_function(EvalContext &cxt, makefile &mk,
                                  StringView command) throws -> String
{
  let source = String{cxt.scratch_allocator(), command};
  if (const String *shell_value = mk.find_variable("SHELL");
      shell_value != nullptr)
  {
    let const shell = expand(cxt, mk, shell_value->view(), 0);
    if (!shell.is_empty()) {
      source = String{cxt.scratch_allocator()};
      append_shell_quoted_arg(source, shell.view());
      source += " -c ";
      append_shell_quoted_arg(source, command);
    }
  }

  let const captured =
      cxt.capture_command_substitution(source.view(), StringView{"make"});
  let folded = String{cxt.scratch_allocator()};
  for (usize byte_position = 0; byte_position < captured.count();
       byte_position++)
  {
    if (captured[byte_position] == '\r' &&
        byte_position + 1 < captured.count() &&
        captured[byte_position + 1] == '\n')
      continue;
    folded.push(captured[byte_position] == '\n' ? ' '
                                                : captured[byte_position]);
  }
  return folded;
}

/* None means the name is not a substitution reference. */
static fn try_substitution_reference(EvalContext &cxt, makefile &mk,
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

  let value = expand_variable(cxt, mk, variable_name, depth + 1);
  return substitute_make_words(value.view(), pattern.view(), replacement.view(),
                               cxt.scratch_allocator(), true);
}

static fn split_make_function_arguments(StringView text,
                                        u8 maximum_argument_count,
                                        Allocator allocator) throws
    -> ArrayList<StringView>
{
  ArrayList<StringView> arguments{allocator};
  if (text.is_empty()) return arguments;
  if (maximum_argument_count == 1) {
    arguments.push(text);
    return arguments;
  }

  usize argument_start = 0;
  usize nested_depth = 0;
  for (usize position = 0; position < text.length; position++) {
    if (text[position] == '$' && position + 1 < text.length &&
        (text[position + 1] == '(' || text[position + 1] == '{'))
    {
      nested_depth++;
      position++;
    } else if ((text[position] == ')' || text[position] == '}') &&
               nested_depth > 0)
    {
      nested_depth--;
    } else if (text[position] == ',' && nested_depth == 0 &&
               arguments.count() + 1 < maximum_argument_count)
    {
      arguments.push(
          text.substring_of_length(argument_start, position - argument_start));
      argument_start = position + 1;
    }
  }
  arguments.push(text.substring(argument_start));
  return arguments;
}

static fn require_make_function_arguments(
    StringView function_name, const make_function_spec &spec,
    const ArrayList<StringView> &arguments) throws -> void
{
  if (arguments.count() >= spec.minimum_argument_count &&
      arguments.count() <= spec.maximum_argument_count)
    return;
  throw Error{"The make function '" + String{function_name} +
              "' received an invalid number of arguments"};
}

static fn make_word_number(StringView text, StringView function_name) throws
    -> usize
{
  let const parsed = utils::parse_decimal_u64(trim(text));
  if (parsed.is_error() || parsed.value() == 0 ||
      static_cast<u64>(static_cast<usize>(parsed.value())) != parsed.value())
    throw Error{"The make function '" + String{function_name} +
                "' requires a positive word index"};
  return static_cast<usize>(parsed.value());
}

static fn last_path_separator(StringView path) wontthrow -> Maybe<usize>
{
  Maybe<usize> separator = None;
  for (usize position = 0; position < path.length; position++)
    if (os::is_directory_separator(path[position])) separator = position;
  return separator;
}

static fn last_suffix_position(StringView path) wontthrow -> Maybe<usize>
{
  let const separator = last_path_separator(path);
  Maybe<usize> suffix = None;
  for (usize position = separator.has_value() ? *separator + 1 : 0;
       position < path.length; position++)
    if (path[position] == '.') suffix = position;
  return suffix;
}

static fn
evaluate_make_function(EvalContext &cxt, makefile &mk, StringView function_name,
                       const make_function_spec &spec,
                       const ArrayList<StringView> &arguments, usize depth,
                       bool are_arguments_expanded = false) throws -> String
{
  require_make_function_arguments(function_name, spec, arguments);
  let const allocator = cxt.scratch_allocator();
  let const do_expand = [&](usize argument_index) -> String {
    if (argument_index >= arguments.count()) return String{allocator};
    if (are_arguments_expanded)
      return String{allocator, arguments[argument_index]};
    return expand(cxt, mk, arguments[argument_index], depth + 1);
  };

  switch (spec.kind) {
  case make_function_kind::Subst: {
    let const from = do_expand(0);
    let const to = do_expand(1);
    let const source = do_expand(2);
    let result = String{allocator};
    if (from.is_empty()) {
      result = source.clone();
      result += to.view();
      return result;
    }

    usize source_position = 0;
    while (source_position < source.count()) {
      let const match =
          find_make_text(source.view().substring(source_position), from.view());
      if (!match.has_value()) {
        result += source.view().substring(source_position);
        break;
      }
      result += source.view().substring_of_length(source_position, *match);
      result += to.view();
      source_position += *match + from.count();
    }
    return result;
  }
  case make_function_kind::Patsubst: {
    let const pattern_text = do_expand(0);
    let const replacement = do_expand(1);
    let const source = do_expand(2);
    return substitute_make_words(source.view(), pattern_text.view(),
                                 replacement.view(), allocator, false);
  }
  case make_function_kind::Strip: {
    let const source = do_expand(0);
    let result = String{allocator};
    bool has_word = false;
    for (StringView word : split_word_views(source.view(), allocator))
      append_make_word(result, word, has_word);
    return result;
  }
  case make_function_kind::Findstring: {
    let const wanted = do_expand(0);
    let const source = do_expand(1);
    return find_make_text(source.view(), wanted.view()).has_value()
               ? wanted.clone()
               : String{allocator};
  }
  case make_function_kind::Filter:
  case make_function_kind::FilterOut: {
    let const pattern_text = do_expand(0);
    let const source = do_expand(1);
    let literal_patterns = StringMap<bool>{allocator};
    let wildcard_patterns = ArrayList<make_pattern>{allocator};
    for (StringView pattern_text_value :
         split_word_views(pattern_text.view(), allocator))
    {
      let pattern = parse_make_pattern(pattern_text_value, allocator);
      if (pattern.has_wildcard)
        wildcard_patterns.push(steal(pattern));
      else
        literal_patterns.set(pattern.prefix.view(), true);
    }
    let result = String{allocator};
    bool has_word = false;
    for (StringView word : split_word_views(source.view(), allocator)) {
      bool is_matched = literal_patterns.find(word) != nullptr;
      if (!is_matched)
        for (const make_pattern &pattern : wildcard_patterns)
          if (match_make_pattern(pattern, word).has_value()) {
            is_matched = true;
            break;
          }
      if (is_matched == (spec.kind == make_function_kind::Filter))
        append_make_word(result, word, has_word);
    }
    return result;
  }
  case make_function_kind::Sort: {
    let const source = do_expand(0);
    let words = split_word_views(source.view(), allocator);
    words.sort([](StringView left, StringView right) { return left < right; });
    let result = String{allocator};
    bool has_word = false;
    for (usize word_index = 0; word_index < words.count(); word_index++) {
      if (word_index > 0 && words[word_index] == words[word_index - 1])
        continue;
      append_make_word(result, words[word_index], has_word);
    }
    return result;
  }
  case make_function_kind::Word:
  case make_function_kind::Wordlist:
  case make_function_kind::Words:
  case make_function_kind::Firstword:
  case make_function_kind::Lastword: {
    usize source_argument_index = 0;
    if (spec.kind == make_function_kind::Word) source_argument_index = 1;
    if (spec.kind == make_function_kind::Wordlist) source_argument_index = 2;
    let const source = do_expand(source_argument_index);
    let const words = split_word_views(source.view(), allocator);
    if (spec.kind == make_function_kind::Words) {
      char count_text[32];
      let const count_length = static_cast<usize>(
          std::snprintf(count_text, sizeof(count_text), "%zu", words.count()));
      return String{
          allocator, StringView{count_text, count_length}
      };
    }
    if (words.is_empty()) return String{allocator};
    if (spec.kind == make_function_kind::Firstword)
      return String{allocator, words[0]};
    if (spec.kind == make_function_kind::Lastword)
      return String{allocator, words[words.count() - 1]};
    let const first_word_number =
        make_word_number(do_expand(0).view(), function_name);
    if (spec.kind == make_function_kind::Word)
      return first_word_number <= words.count()
                 ? String{allocator, words[first_word_number - 1]}
                 : String{allocator};
    let const last_word_number =
        make_word_number(do_expand(1).view(), function_name);
    if (first_word_number > last_word_number ||
        first_word_number > words.count())
      return String{allocator};
    let result = String{allocator};
    bool has_word = false;
    let const last_index =
        last_word_number < words.count() ? last_word_number : words.count();
    for (usize word_index = first_word_number - 1; word_index < last_index;
         word_index++)
      append_make_word(result, words[word_index], has_word);
    return result;
  }
  case make_function_kind::Dir:
  case make_function_kind::Notdir:
  case make_function_kind::Suffix:
  case make_function_kind::Basename: {
    let const source = do_expand(0);
    let result = String{allocator};
    bool has_word = false;
    for (StringView word : split_word_views(source.view(), allocator)) {
      if (spec.kind == make_function_kind::Dir) {
        let const separator = last_path_separator(word);
        append_make_word(result,
                         separator.has_value()
                             ? word.substring_of_length(0, *separator + 1)
                             : StringView{"./"},
                         has_word);
      } else if (spec.kind == make_function_kind::Notdir) {
        let const separator = last_path_separator(word);
        append_make_word(result,
                         separator.has_value() ? word.substring(*separator + 1)
                                               : word,
                         has_word);
      } else if (spec.kind == make_function_kind::Suffix) {
        if (let const suffix = last_suffix_position(word); suffix.has_value())
          append_make_word(result, word.substring(*suffix), has_word);
      } else {
        let const suffix = last_suffix_position(word);
        append_make_word(
            result,
            suffix.has_value() ? word.substring_of_length(0, *suffix) : word,
            has_word);
      }
    }
    return result;
  }
  case make_function_kind::Addprefix:
  case make_function_kind::Addsuffix: {
    let const affix = do_expand(0);
    let const source = do_expand(1);
    let result = String{allocator};
    bool has_word = false;
    for (StringView word : split_word_views(source.view(), allocator)) {
      let combined = String{allocator};
      if (spec.kind == make_function_kind::Addprefix) combined += affix.view();
      combined += word;
      if (spec.kind == make_function_kind::Addsuffix) combined += affix.view();
      append_make_word(result, combined.view(), has_word);
    }
    return result;
  }
  case make_function_kind::Join: {
    let const left_text = do_expand(0);
    let const right_text = do_expand(1);
    let const left = split_word_views(left_text.view(), allocator);
    let const right = split_word_views(right_text.view(), allocator);
    let const joined_count =
        left.count() > right.count() ? left.count() : right.count();
    let result = String{allocator};
    bool has_word = false;
    for (usize word_index = 0; word_index < joined_count; word_index++) {
      let joined = String{allocator};
      if (word_index < left.count()) joined += left[word_index];
      if (word_index < right.count()) joined += right[word_index];
      append_make_word(result, joined.view(), has_word);
    }
    return result;
  }
  case make_function_kind::If: {
    let const condition = do_expand(0);
    if (!condition.is_empty()) return do_expand(1);
    return arguments.count() == 3 ? do_expand(2) : String{allocator};
  }
  case make_function_kind::Or: {
    for (usize argument_index = 0; argument_index < arguments.count();
         argument_index++)
    {
      let const value = do_expand(argument_index);
      if (!value.is_empty()) return value;
    }
    return String{allocator};
  }
  case make_function_kind::And: {
    let result = String{allocator};
    for (usize argument_index = 0; argument_index < arguments.count();
         argument_index++)
    {
      result = do_expand(argument_index);
      if (result.is_empty()) return String{allocator};
    }
    return result;
  }
  case make_function_kind::Foreach: {
    let const variable_name_text = do_expand(0);
    let const variable_name = trim(variable_name_text.view());
    let const values = do_expand(1);
    let const snapshot = save_make_variable(mk, variable_name, allocator);
    defer { restore_make_variable(mk, snapshot, allocator); };
    let result = String{allocator};
    bool has_value = false;
    for (StringView value : split_word_views(values.view(), allocator)) {
      set_scoped_make_variable(mk, variable_name, value, allocator);
      append_make_word(result, do_expand(2).view(), has_value);
    }
    return result;
  }
  case make_function_kind::Call: {
    let const called_name_text = do_expand(0);
    let const called_name = trim(called_name_text.view());
    let expanded_arguments = ArrayList<String>{allocator};
    expanded_arguments.reserve(arguments.count());
    expanded_arguments.push(String{allocator, called_name});
    for (usize argument_index = 1; argument_index < arguments.count();
         argument_index++)
      expanded_arguments.push(do_expand(argument_index));

    let const saved_call_max_argument_count = mk.call_max_argument_count;
    let const binding_count = arguments.count() > mk.call_max_argument_count
                                  ? arguments.count()
                                  : mk.call_max_argument_count;
    let snapshots = ArrayList<make_variable_snapshot>{allocator};
    snapshots.reserve(binding_count);
    for (usize argument_index = 0; argument_index < binding_count;
         argument_index++)
    {
      char numeric_name[32];
      let const name_length = static_cast<usize>(std::snprintf(
          numeric_name, sizeof(numeric_name), "%zu", argument_index));
      let const name = StringView{numeric_name, name_length};
      snapshots.push(save_make_variable(mk, name, allocator));
      let const value = argument_index < expanded_arguments.count()
                            ? expanded_arguments[argument_index].view()
                            : StringView{};
      set_scoped_make_variable(mk, name, value, allocator);
    }
    mk.call_max_argument_count = binding_count;
    defer
    {
      mk.call_max_argument_count = saved_call_max_argument_count;
      for (usize snapshot_index = snapshots.count(); snapshot_index-- > 0;)
        restore_make_variable(mk, snapshots[snapshot_index], allocator);
    };

    if (let const called_spec = MAKE_FUNCTIONS.find(called_name);
        called_spec.has_value())
    {
      ArrayList<StringView> called_arguments{allocator};
      for (usize argument_index = 1;
           argument_index < expanded_arguments.count(); argument_index++)
        called_arguments.push(expanded_arguments[argument_index].view());
      return evaluate_make_function(cxt, mk, called_name, *called_spec,
                                    called_arguments, depth + 1, true);
    }
    return expand_variable(cxt, mk, called_name, depth + 1);
  }
  case make_function_kind::Value:
  case make_function_kind::Origin:
  case make_function_kind::Flavor: {
    let const variable_name_text = do_expand(0);
    let const variable_name = trim(variable_name_text.view());
    let lookup = lookup_make_variable(cxt, mk, variable_name);
    if (spec.kind == make_function_kind::Value)
      return lookup.into_string(allocator);
    if (spec.kind == make_function_kind::Origin)
      return String{allocator, make_variable_origin_text(lookup.origin)};
    return String{allocator, make_variable_flavor_text(lookup.flavor)};
  }
  case make_function_kind::Abspath:
  case make_function_kind::Realpath: {
    let const source = do_expand(0);
    let result = String{allocator};
    bool has_word = false;
    for (StringView word : split_word_views(source.view(), allocator)) {
      if (spec.kind == make_function_kind::Abspath) {
        let const path = Path{word}.to_absolute();
        append_make_word(result, path.text().view(), has_word);
      } else if (let path = Path::canonicalize(word); path.has_value()) {
        append_make_word(result, path->text().view(), has_word);
      }
    }
    return result;
  }
  case make_function_kind::Shell: {
    if (cxt.make_shell_suppressed()) return String{allocator};
    let const command = do_expand(0);
    return run_make_shell_function(cxt, mk, command.view());
  }
  case make_function_kind::Wildcard: {
    let const patterns = do_expand(0);
    return make_wildcard(cxt, patterns.view());
  }
  case make_function_kind::Error:
    throw Error{"The makefile stopped the build with the message '" +
                do_expand(0) + "'"};
  case make_function_kind::Warning:
    show_message("make: warning: " + do_expand(0));
    return String{allocator};
  }
  unreachable();
}

static fn expand(EvalContext &cxt, makefile &mk, StringView text,
                 usize depth) throws -> String
{
  if (depth >= 128)
    throw Error{"Make variable expansion exceeds the supported depth"};

  String result{cxt.scratch_allocator()};
  usize text_position = 0;
  while (text_position < text.length) {
    if (text[text_position] == '$' && text_position + 1 < text.length &&
        (text[text_position + 1] == '(' || text[text_position + 1] == '{'))
    {
      ArrayList<char> close_stack{cxt.scratch_allocator()};
      close_stack.push(text[text_position + 1] == '(' ? ')' : '}');
      usize close_position = text_position + 2;
      while (close_position < text.length && !close_stack.is_empty()) {
        if (text[close_position] == '$' && close_position + 1 < text.length &&
            (text[close_position + 1] == '(' ||
             text[close_position + 1] == '{'))
        {
          close_stack.push(text[close_position + 1] == '(' ? ')' : '}');
          close_position++;
        } else if (text[close_position] == close_stack.back()) {
          close_stack.pop_back();
          if (close_stack.is_empty()) break;
        }
        close_position++;
      }
      if (!close_stack.is_empty())
        throw Error{"The make variable reference is unterminated"};

      let const expression = text.substring_of_length(
          text_position + 2, close_position - (text_position + 2));
      usize function_name_length = 0;
      while (function_name_length < expression.length &&
             !is_blank(expression[function_name_length]) &&
             expression[function_name_length] != ',')
        function_name_length++;
      let const function_name =
          expression.substring_of_length(0, function_name_length);
      let const function_spec = MAKE_FUNCTIONS.find(function_name);
      if (function_name_length < expression.length && function_spec.has_value())
      {
        usize arguments_position = function_name_length;
        while (arguments_position < expression.length &&
               is_blank(expression[arguments_position]))
          arguments_position++;
        let const arguments = split_make_function_arguments(
            expression.substring(arguments_position),
            function_spec->maximum_argument_count, cxt.scratch_allocator());
        result += evaluate_make_function(cxt, mk, function_name, *function_spec,
                                         arguments, depth)
                      .view();
      } else if (Maybe<String> substitution =
                     try_substitution_reference(cxt, mk, expression, depth);
                 substitution.has_value())
      {
        result += substitution->view();
      } else {
        let const expanded_name = expand(cxt, mk, expression, depth + 1);
        result +=
            expand_variable(cxt, mk, expanded_name.view(), depth + 1).view();
      }
      text_position =
          close_position < text.length ? close_position + 1 : close_position;
    } else if (text[text_position] == '$' && text_position + 1 < text.length &&
               text[text_position + 1] == '$')
    {
      result += '$';
      text_position += 2;
    } else if (text[text_position] == '$' && text_position + 1 < text.length) {
      result += expand_variable(cxt, mk,
                                text.substring_of_length(text_position + 1, 1),
                                depth + 1)
                    .view();
      text_position += 2;
    } else {
      result.push(text[text_position]);
      text_position++;
    }
  }
  return result;
}

/* The first colon not immediately followed by '=' opens the rule. */
static fn rule_colon(StringView line) wontthrow -> Maybe<usize>
{
  usize token_start = 0;
  for (usize i = 0; i < line.length; i++) {
    if (is_blank(line[i])) {
      token_start = i + 1;
      continue;
    }
    if (line[i] == ':' && i == token_start + 1 && i + 1 < line.length &&
        ((line[token_start] >= 'a' && line[token_start] <= 'z') ||
         (line[token_start] >= 'A' && line[token_start] <= 'Z')) &&
        (line[i + 1] == '/' || line[i + 1] == '\\'))
      continue;
    if (line[i] == ':' && !(i + 1 < line.length && line[i + 1] == '='))
      return i;
  }
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
  if (let const *index = mk.variable_index.find(name); index != nullptr) {
    make_variable &variable = mk.variables[*index];
    if (operator_character == '?') return;
    if (operator_character == '+') {
      let const appended_value =
          variable.flavor == make_variable_flavor::Simple
              ? expand(cxt, mk, trimmed_value, 0)
              : String{cxt.scratch_allocator(), trimmed_value};
      variable.value += " ";
      variable.value += appended_value.view();
    } else {
      let const value_to_store =
          operator_character == ':'
              ? expand(cxt, mk, trimmed_value, 0)
              : String{cxt.scratch_allocator(), trimmed_value};
      variable.value = String{cxt.scratch_allocator(), value_to_store.view()};
      variable.origin = is_command_line ? make_variable_origin::CommandLine
                                        : make_variable_origin::File;
      variable.flavor = operator_character == ':'
                            ? make_variable_flavor::Simple
                            : make_variable_flavor::Recursive;
    }
    return;
  }
  let const value_to_store =
      operator_character == ':'
          ? expand(cxt, mk, trimmed_value, 0)
          : String{cxt.scratch_allocator(), trimmed_value};
  mk.variable_index.set(name, mk.variables.count());
  mk.variables.push(make_variable{
      String{cxt.scratch_allocator(), name                 },
      String{cxt.scratch_allocator(), value_to_store.view()},
      is_command_line ? make_variable_origin::CommandLine
                      : make_variable_origin::File,
      operator_character == ':' ? make_variable_flavor::Simple
                                : make_variable_flavor::Recursive
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

static fn evaluate_conditional(EvalContext &cxt, makefile &mk,
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
  explicit make_source_frame(ArrayList<String> &&new_lines,
                             usize new_include_depth = 0)
      : lines(steal(new_lines)), include_depth(new_include_depth)
  {}
  ArrayList<String> lines;
  usize line_position{0};
  usize include_depth{0};
};

static fn parse_makefile(EvalContext &cxt, StringView source,
                         const ArrayList<String> &command_assignments,
                         bool does_environment_override,
                         bool should_use_builtin_rules,
                         StringView initial_makeflags = {}) throws -> makefile
{
  makefile mk{cxt.scratch_allocator()};
  mk.does_environment_override = does_environment_override;
  if (!initial_makeflags.is_empty()) {
    mk.variable_index.set("MAKEFLAGS", mk.variables.count());
    mk.variables.push(make_variable{
        String{cxt.scratch_allocator(), "MAKEFLAGS"      },
        String{cxt.scratch_allocator(), initial_makeflags},
        make_variable_origin::Environment, make_variable_flavor::Recursive
    });
  }
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

    if (directive == "define") {
      let const name = trim(trimmed.substring(directive.length));
      if (name.is_empty()) throw Error{"A define line must name a variable"};

      String value{cxt.scratch_allocator()};
      usize define_depth = 1;
      while (source_frame.line_position < source_frame.lines.count()) {
        let const definition_line =
            source_frame.lines[source_frame.line_position++].view();
        let const definition_directive = leading_word(trim(definition_line));
        if (definition_directive == "define") {
          define_depth++;
        } else if (definition_directive == "endef") {
          define_depth--;
          if (define_depth == 0) break;
        }
        if (!value.is_empty()) value += '\n';
        value += definition_line;
      }
      if (define_depth != 0)
        throw Error{"The define directive is unterminated"};
      if (do_is_active()) {
        String assignment{cxt.scratch_allocator(), "="};
        assignment += value.view();
        apply_assignment(cxt, mk, name, assignment.view());
      }
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
      if (include_paths.is_empty()) {
        current_rule_indices.clear();
        current_pattern_indices.clear();
        continue;
      }
      let const include_depth = source_frame.include_depth + 1;
      if (include_depth >= 17)
        throw Error{"Makefile includes exceed the supported nesting depth"};
      current_rule_indices.clear();
      current_pattern_indices.clear();
      for (usize include_position = include_paths.count(); include_position > 0;
           include_position--)
      {
        let const &include_path = include_paths[include_position - 1];
        let const included_source =
            Path{include_path.view()}.read_entire_file();
        if (!included_source.has_value())
          throw Error{"Unable to read the included makefile '" + include_path +
                      "': " + os::last_system_error_message()};
        source_frames.push(
            make_source_frame{join_continuations(included_source->view(),
                                                 cxt.scratch_allocator()),
                              include_depth});
      }
      continue;
    }

    /* override re-asserts a value, so its prefix is stripped and the assignment
       parses as usual. */
    let statement = trimmed;
    if (directive == "override")
      statement = trim(statement.substring(directive.length));

    let const statement_word = leading_word(statement);
    if (statement_word == "undefine") {
      let const names =
          expand(cxt, mk, trim(statement.substring(statement_word.length)), 0);
      for (const String &name :
           split_words(names.view(), cxt.scratch_allocator()))
        if (mk.command_variable_names.find(name.view()) == NULL)
          mk.remove_variable(name.view());
      current_rule_indices.clear();
      current_pattern_indices.clear();
      continue;
    }
    if (statement_word == "unexport") {
      let const names = trim(statement.substring(statement_word.length));
      for (const String &name : split_words(names, cxt.scratch_allocator())) {
        mk.exported_variable_names.erase(name.view());
        mk.unexported_variable_names.set(name.view(), true);
      }
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
      if (!after_export.find_character('=').has_value()) {
        for (const String &name :
             split_words(after_export, cxt.scratch_allocator()))
        {
          mk.exported_variable_names.set(name.view(), true);
          mk.unexported_variable_names.erase(name.view());
        }
        current_rule_indices.clear();
        current_pattern_indices.clear();
        continue;
      }
      let const exported_name = assignment_variable_name(after_export);
      if (!exported_name.is_empty()) {
        mk.exported_variable_names.set(exported_name, true);
        mk.unexported_variable_names.erase(exported_name);
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
        let const parsed_target =
            parse_make_pattern(target.view(), cxt.scratch_allocator());
        let normalized_target = parsed_target.prefix.clone();
        if (parsed_target.has_wildcard) {
          normalized_target.push('%');
          normalized_target += parsed_target.suffix.view();
        }
        let new_prerequisites =
            split_words(prerequisites.view(), cxt.scratch_allocator());
        if (normalized_target.view() == StringView{".PHONY"}) {
          for (const String &phony : new_prerequisites)
            mk.phony_targets.set(phony.view(), true);
          continue;
        }
        if (normalized_target.view() == StringView{".IGNORE"}) {
          if (new_prerequisites.is_empty())
            mk.should_ignore_errors = true;
          else
            for (const String &ignored : new_prerequisites)
              mk.ignored_targets.set(ignored.view(), true);
        }
        if (normalized_target.view() == StringView{".SILENT"}) {
          if (new_prerequisites.is_empty())
            mk.is_silent = true;
          else
            for (const String &silent : new_prerequisites)
              mk.silent_targets.set(silent.view(), true);
        }
        if (normalized_target.view() == StringView{".PRECIOUS"}) {
          if (new_prerequisites.is_empty())
            mk.is_every_target_precious = true;
          else
            for (const String &precious : new_prerequisites)
              mk.precious_targets.set(precious.view(), true);
        }
        if (normalized_target.view() == StringView{".SUFFIXES"}) {
          if (new_prerequisites.is_empty()) {
            mk.suffixes.clear();
          } else {
            for (const String &suffix : new_prerequisites)
              if (!mk.suffixes.find(suffix.view()).has_value())
                mk.suffixes.push(suffix.clone());
          }
        }
        let is_inference_rule = false;
        if (!normalized_target.view().find_character('/').has_value())
          for (const String &source_suffix : mk.suffixes) {
            if (normalized_target.view() == source_suffix.view()) {
              is_inference_rule = true;
              break;
            }
            if (!normalized_target.view().starts_with(source_suffix.view()))
              continue;

            let const target_suffix =
                normalized_target.view().substring(source_suffix.view().length);
            for (const String &known_suffix : mk.suffixes)
              if (target_suffix == known_suffix.view()) {
                is_inference_rule = true;
                break;
              }
            if (is_inference_rule) break;
          }

        if (mk.default_goal.is_empty() && normalized_target[0] != '.' &&
            !parsed_target.has_wildcard &&
            !SPECIAL_TARGETS.contains(normalized_target.view()) &&
            !is_inference_rule)
          mk.default_goal = normalized_target.clone();
        if (!parsed_target.has_wildcard)
          if (make_rule *existing =
                  mk.find_mutable_rule(normalized_target.view());
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
        rule.target = parsed_target.has_wildcard ? target.clone()
                                                 : steal(normalized_target);
        rule.prerequisites = steal(new_prerequisites);
        if (parsed_target.has_wildcard) {
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

  ArrayList<make_rule> retained_pattern_rules{cxt.scratch_allocator()};
  for (usize pattern_position = 0; pattern_position < mk.pattern_rules.count();
       pattern_position++)
  {
    make_rule &pattern = mk.pattern_rules[pattern_position];
    if (pattern.recipe_lines.is_empty()) continue;
    bool is_canceled = false;
    for (usize later_position = pattern_position + 1;
         later_position < mk.pattern_rules.count(); later_position++)
    {
      let const &later = mk.pattern_rules[later_position];
      if (!later.recipe_lines.is_empty() ||
          later.target.view() != pattern.target.view() ||
          later.prerequisites.count() != pattern.prerequisites.count())
        continue;

      is_canceled = true;
      for (usize prerequisite_position = 0;
           prerequisite_position < pattern.prerequisites.count();
           prerequisite_position++)
        if (later.prerequisites[prerequisite_position].view() !=
            pattern.prerequisites[prerequisite_position].view())
        {
          is_canceled = false;
          break;
        }
      if (is_canceled) break;
    }
    if (!is_canceled) retained_pattern_rules.push(steal(pattern));
  }
  mk.pattern_rules = steal(retained_pattern_rules);

  if (mk.find_variable_record(".DEFAULT_GOAL") != NULL)
    mk.default_goal = expand_variable(cxt, mk, ".DEFAULT_GOAL", 0);
  make_runtime_flags makefile_flags;
  if (const String *makeflags = mk.find_variable("MAKEFLAGS");
      makeflags != nullptr)
  {
    let const expanded_makeflags = expand(cxt, mk, makeflags->view(), 0);
    apply_makeflags(expanded_makeflags.view(), makefile_flags,
                    cxt.scratch_allocator());
  }
  if (should_use_builtin_rules && !makefile_flags.should_disable_builtin_rules)
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

static fn is_make_target_supplyable(EvalContext &cxt, makefile &mk,
                                    StringView goal,
                                    StringMap<bool> &active_targets,
                                    StringMap<bool> &supplyability_cache) throws
    -> bool
{
  if (active_targets.find(goal) != nullptr) return false;
  if (let const *cached = supplyability_cache.find(goal); cached != NULL)
    return *cached;
  if (Path{goal}.exists() || mk.find_rule(goal) != nullptr) {
    supplyability_cache.set(goal, true);
    return true;
  }
  active_targets.set(goal, true);
  defer { active_targets.erase(goal); };

  for (const make_rule &pattern : mk.pattern_rules) {
    if (pattern.recipe_lines.is_empty()) continue;
    let const parsed_pattern =
        parse_make_pattern(pattern.target.view(), cxt.scratch_allocator());
    let const stem = match_make_pattern(parsed_pattern, goal);
    if (!stem.has_value()) continue;

    bool are_prerequisites_supplyable = true;
    for (const String &prerequisite : pattern.prerequisites) {
      let substituted = String{cxt.scratch_allocator()};
      append_make_replacement(substituted, prerequisite.view(), *stem, true);
      let const expanded = expand(cxt, mk, substituted.view(), 0);
      for (const String &word :
           split_words(expanded.view(), cxt.scratch_allocator()))
        if (!is_make_target_supplyable(cxt, mk, word.view(), active_targets,
                                       supplyability_cache))
        {
          are_prerequisites_supplyable = false;
          break;
        }
      if (!are_prerequisites_supplyable) break;
    }
    if (are_prerequisites_supplyable) {
      supplyability_cache.set(goal, true);
      return true;
    }
  }

  StringView target_suffix;
  for (const String &suffix : mk.suffixes) {
    let const candidate = suffix.view();
    if (goal.length >= candidate.length &&
        goal.substring(goal.length - candidate.length) == candidate)
    {
      target_suffix = candidate;
      break;
    }
  }
  for (const String &source_suffix_string : mk.suffixes) {
    let rule_name = source_suffix_string.clone();
    rule_name += target_suffix;
    if (mk.find_rule(rule_name.view()) == nullptr) continue;

    let source =
        String{cxt.scratch_allocator(),
               goal.substring_of_length(0, goal.length - target_suffix.length)};
    source += source_suffix_string.view();
    if (is_make_target_supplyable(cxt, mk, source.view(), active_targets,
                                  supplyability_cache))
    {
      supplyability_cache.set(goal, true);
      return true;
    }
  }
  supplyability_cache.set(goal, false);
  return false;
}

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
    StringMap<bool> supply_active_targets{cxt.scratch_allocator()};
    StringMap<bool> supplyability_cache{cxt.scratch_allocator()};
    usize best_stem_length = ~usize{0};
    for (const make_rule &pattern : mk.pattern_rules) {
      if (pattern.recipe_lines.is_empty()) continue;
      let const parsed_pattern =
          parse_make_pattern(pattern.target.view(), cxt.scratch_allocator());
      let const stem = match_make_pattern(parsed_pattern, goal);
      if (!stem.has_value() || stem->length >= best_stem_length) continue;

      ArrayList<String> candidate{cxt.scratch_allocator()};
      for (const String &prerequisite : pattern.prerequisites) {
        let substituted = String{cxt.scratch_allocator()};
        append_make_replacement(substituted, prerequisite.view(), *stem, true);
        let const expanded = expand(cxt, mk, substituted.view(), 0);
        for (const String &word :
             split_words(expanded.view(), cxt.scratch_allocator()))
          candidate.push(word.clone());
      }
      bool are_prerequisites_supplyable = true;
      for (const String &prerequisite : candidate)
        if (!is_make_target_supplyable(cxt, mk, prerequisite.view(),
                                       supply_active_targets,
                                       supplyability_cache))
        {
          are_prerequisites_supplyable = false;
          break;
        }
      if (!are_prerequisites_supplyable) continue;

      best_stem_length = stem->length;
      prerequisites = steal(candidate);
      recipe_lines = &pattern.recipe_lines;
      target_stem = String{cxt.scratch_allocator(), *stem};
    }
    if (!prerequisites.is_empty())
      inferred_first_prerequisite = prerequisites[0].clone();

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
        if (!is_make_target_supplyable(cxt, mk, source.view(),
                                       supply_active_targets,
                                       supplyability_cache))
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
  let saved_variables =
      ArrayList<make_variable_snapshot>{cxt.scratch_allocator()};
  if (target_assignments != nullptr)
    for (const String &assignment : *target_assignments) {
      let const name = assignment_variable_name(assignment.view());
      saved_variables.push(
          save_make_variable(mk, name, cxt.scratch_allocator()));
      let const equals = assignment.view().find_character('=');
      apply_assignment(cxt, mk,
                       assignment.view().substring_of_length(0, *equals),
                       assignment.view().substring(*equals));
    }
  defer
  {
    for (usize i = saved_variables.count(); i-- > 0;) {
      restore_make_variable(mk, saved_variables[i], cxt.scratch_allocator());
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
  let is_out_of_date = options.should_always_make ||
                       mk.phony_targets.find(goal) != nullptr ||
                       !was_target_existing || has_outdated_prerequisite;
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

  struct automatic_variable_value
  {
    StringView name;
    StringView value;
  };
  const automatic_variable_value automatic_values[] = {
      {"@", automatic_target.view()},
      {"%", archive_member.view()  },
      {"<", first_prereq           },
      {"^", all_prereqs.view()     },
      {"+", repeated_prereqs.view()},
      {"?", newer_prereqs.view()   },
      {"*", target_stem.view()     },
  };
  let automatic_snapshots =
      ArrayList<make_variable_snapshot>{cxt.scratch_allocator()};
  automatic_snapshots.reserve(countof(automatic_values));
  for (let const &automatic : automatic_values) {
    automatic_snapshots.push(
        save_make_variable(mk, automatic.name, cxt.scratch_allocator()));
    set_scoped_make_variable(mk, automatic.name, automatic.value,
                             cxt.scratch_allocator());
  }
  defer
  {
    for (usize snapshot_position = automatic_snapshots.count();
         snapshot_position-- > 0;)
      restore_make_variable(mk, automatic_snapshots[snapshot_position],
                            cxt.scratch_allocator());
  };

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
    should_force_run |= recipe_references_make_variable(body);

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
        recipe_source += " -c ";
        append_shell_quoted_arg(recipe_source, command.view());
      }
    }

    let subshell_command = String{cxt.scratch_allocator(), "(\n"};
    subshell_command += recipe_source.view();
    subshell_command += "\n)";
    i32 status = 0;
    try {
      status = cxt.run_source(subshell_command.view(), "make",
                              return_handling::Consume, ec.source_location(),
                              StringView{"make"});
      if (os::INTERRUPT_REQUESTED || status == 130) {
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
  ArrayList<String> filtered{cxt.scratch_allocator()};
  ArrayList<SourceLocation> filtered_locations{cxt.scratch_allocator()};
  ArrayList<String> requested_makefiles{cxt.scratch_allocator()};
  ArrayList<SourceLocation> requested_makefile_locations{
      cxt.scratch_allocator()};
  ArrayList<String> option_order{cxt.scratch_allocator()};
  ArrayList<String> command_assignments{cxt.scratch_allocator()};
  let const do_argument_location = [&](usize argument_position) {
    return argument_position < arg_locations.count()
               ? arg_locations[argument_position]
               : ec.source_location();
  };
  let const do_attached_location = [&](usize argument_position,
                                       usize value_offset) {
    let location = do_argument_location(argument_position);
    if (location.length == args[argument_position].count()) {
      location.position += static_cast<u32>(value_offset);
      location.length -= static_cast<u32>(value_offset);
    }
    return location;
  };
  if (!args.is_empty()) {
    filtered.push(args[0].clone());
    filtered_locations.push(do_argument_location(0));
  }
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
      filtered_locations.push(ec.source_location());
      option_order.push(steal(inherited_options));
    }

  for (usize arg_index = 1; arg_index < args.count(); arg_index++) {
    const String &arg = args[arg_index];
    let const text = arg.view();
    option_order.push(arg.clone());
    if ((text == "-f" || text == "--file") && arg_index + 1 < args.count()) {
      arg_index++;
      requested_makefiles.push(args[arg_index].clone());
      requested_makefile_locations.push(do_argument_location(arg_index));
      continue;
    }
    if (text.starts_with(StringView{"--file="})) {
      requested_makefiles.push(
          String{cxt.scratch_allocator(), text.substring(7)});
      requested_makefile_locations.push(do_attached_location(arg_index, 7));
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
        if (option_position > 1)
          filtered_locations.push(do_argument_location(arg_index));
        if (option_position + 1 < text.length) {
          requested_makefiles.push(String{cxt.scratch_allocator(),
                                          text.substring(option_position + 1)});
          requested_makefile_locations.push(
              do_attached_location(arg_index, option_position + 1));
        } else if (arg_index + 1 < args.count()) {
          arg_index++;
          requested_makefiles.push(args[arg_index].clone());
          requested_makefile_locations.push(do_argument_location(arg_index));
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
    if (is_jobs_flag && text == StringView{"-j"} &&
        arg_index + 1 < args.count() &&
        args[arg_index + 1].view().is_all_decimal_digits())
      arg_index++;
    if (!is_jobs_flag) {
      filtered.push(arg.clone());
      filtered_locations.push(do_argument_location(arg_index));
    }
  }

  /* A recipe's $(MAKE) re-enters this util while the outer call is still on the
     stack, and the flag list is shared, so the inherited -C or -f is cleared
     before parsing. The outer call already read its flags into locals, so the
     reset does not disturb it. */
  reset_flags(FLAG_LIST);
  ArrayList<SourceLocation> operand_locations{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, filtered, &filtered_locations,
                          &operand_locations, false, true);
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
    if (Path::set_current_directory(Path{FLAG_MAKE_DIR.value()}).is_error()) {
      report_soft_koshkit_util_error(
          ec, cxt, FLAG_MAKE_DIR.value_location(), args[0].view(),
          "unable to change to the directory '" +
              String{cxt.scratch_allocator(), FLAG_MAKE_DIR.value()} +
              "': " + os::last_system_error_message());
      return 2;
    }
  }
  defer
  {
    if (saved_directory.has_value())
      static_cast<void>(Path::set_current_directory(*saved_directory));
  };

  if (requested_makefiles.is_empty()) {
    if (Path{"makefile"}.exists()) {
      requested_makefiles.push(String{cxt.scratch_allocator(), "makefile"});
      requested_makefile_locations.push(ec.source_location());
    } else if (Path{"Makefile"}.exists()) {
      requested_makefiles.push(String{cxt.scratch_allocator(), "Makefile"});
      requested_makefile_locations.push(ec.source_location());
    }
  }

  String source{cxt.scratch_allocator()};
  for (usize makefile_position = 0;
       makefile_position < requested_makefiles.count(); makefile_position++)
  {
    let const &makefile_path = requested_makefiles[makefile_position];
    if (makefile_path.view() == StringView{"-"}) {
      source += utils::read_entire_standard_input().view();
      source += '\n';
      continue;
    }

    let const part = Path{makefile_path.view()}.read_entire_file();
    if (!part.has_value()) {
      report_soft_koshkit_util_error(
          ec, cxt, requested_makefile_locations[makefile_position],
          args[0].view(),
          "unable to read the makefile '" + makefile_path +
              "': " + os::last_system_error_message());
      return 2;
    }
    source += part->view();
    source += '\n';
  }

  ArrayList<String> goals{cxt.scratch_allocator()};
  ArrayList<SourceLocation> goal_locations{cxt.scratch_allocator()};
  StringMap<bool> command_line_variable_names{cxt.scratch_allocator()};
  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    let const &operand = operands[operand_position];
    if (is_command_line_assignment(operand.view())) {
      command_assignments.push(operand.clone());
      command_line_variable_names.set(assignment_variable_name(operand.view()),
                                      true);
    } else {
      goals.push(operand.clone());
      goal_locations.push(operand_locations[operand_position]);
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
  if (FLAG_MAKE_PRINT_DATABASE.is_enabled()) makeflags += 'p';
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

  let previous_makeflags = os::get_environment_variable("MAKEFLAGS");
  os::set_environment_variable("MAKEFLAGS", makeflags.view());
  defer
  {
    if (previous_makeflags.has_value())
      os::set_environment_variable("MAKEFLAGS", previous_makeflags->view());
    else
      os::unset_environment_variable("MAKEFLAGS");
  };

  let mk =
      parse_makefile(cxt, source.view(), command_assignments,
                     FLAG_MAKE_ENVIRONMENT_OVERRIDES.is_enabled(),
                     !FLAG_MAKE_NO_BUILTINS.is_enabled(), makeflags.view());

  make_runtime_flags runtime_flags{FLAG_MAKE_ALWAYS_MAKE.is_enabled(),
                                   FLAG_MAKE_ENVIRONMENT_OVERRIDES.is_enabled(),
                                   FLAG_MAKE_IGNORE_ERRORS.is_enabled(),
                                   should_keep_going,
                                   FLAG_MAKE_DRY_RUN.is_enabled(),
                                   FLAG_MAKE_QUESTION.is_enabled(),
                                   FLAG_MAKE_PRINT_DATABASE.is_enabled(),
                                   FLAG_MAKE_NO_BUILTINS.is_enabled(),
                                   FLAG_MAKE_SILENT.is_enabled(),
                                   FLAG_MAKE_TOUCH.is_enabled()};
  if (const String *parsed_makeflags = mk.find_variable("MAKEFLAGS");
      parsed_makeflags != nullptr)
  {
    makeflags = expand(cxt, mk, parsed_makeflags->view(), 0);
    apply_makeflags(makeflags.view(), runtime_flags, cxt.scratch_allocator());
    os::set_environment_variable("MAKEFLAGS", makeflags.view());
  }

  ArrayList<saved_recipe_environment> saved_recipe_environment_values{
      cxt.scratch_allocator()};
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
  mk.unexported_variable_names.for_each(
      [&](StringView name, bool is_unexported) throws {
        unused(is_unexported);
        let old_value = os::get_environment_variable(name);
        if (!old_value.has_value()) return;
        saved_recipe_environment_values.push(saved_recipe_environment{
            String{cxt.scratch_allocator(), name},
            Maybe<String>{old_value->clone()}
        });
        os::unset_environment_variable(name);
      });
  for (const make_variable &variable : mk.variables) {
    if (variable.name.view() == StringView{"MAKEFLAGS"}) continue;
    if (variable.name.view() == StringView{"SHELL"} &&
        mk.exported_variable_names.find(variable.name.view()) == nullptr)
    {
      continue;
    }
    if (mk.unexported_variable_names.find(variable.name.view()) != nullptr)
      continue;
    let old_value = os::get_environment_variable(variable.name.view());
    let const is_command_variable =
        mk.command_variable_names.find(variable.name.view()) != nullptr;
    let const is_command_line_variable =
        command_line_variable_names.find(variable.name.view()) != NULL;
    let const is_exported_variable =
        mk.exported_variable_names.find(variable.name.view()) != nullptr;
    if (!is_command_line_variable && !is_exported_variable &&
        !old_value.has_value())
      continue;
    if (!is_command_variable && old_value.has_value() &&
        runtime_flags.does_environment_override)
      continue;

    saved_recipe_environment_values.push(saved_recipe_environment{
        variable.name.clone(), old_value.has_value()
                                   ? Maybe<String>{old_value->clone()}
                                   : Maybe<String>{}});
    let const expanded_value = expand(cxt, mk, variable.value.view(), 0);
    os::set_environment_variable(variable.name.view(), expanded_value.view());
  }

  if (runtime_flags.is_print_database) {
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
    if (mk.default_goal.is_empty() && runtime_flags.is_print_database) return 0;
    if (mk.default_goal.is_empty())
      throw ErrorWithDetails{
          "The makefile defines no targets and no default goal",
          "Add a rule or name a target on the command line"};
    goals.push(mk.default_goal.clone());
    goal_locations.push(ec.source_location());
  }

  StringMap<bool> active_targets{cxt.scratch_allocator()};
  StringMap<bool> completed_target_results{cxt.scratch_allocator()};
  let const options = make_build_options{
      runtime_flags.should_always_make,
      runtime_flags.is_query,
      runtime_flags.should_ignore_errors || mk.should_ignore_errors,
      runtime_flags.should_keep_going,
      runtime_flags.is_dry_run,
      runtime_flags.is_print_database,
      runtime_flags.is_silent || mk.is_silent,
      runtime_flags.should_touch};
  let has_outdated_goal = false;
  let did_fail = false;
  try {
    for (usize goal_position = 0; goal_position < goals.count();
         goal_position++)
    {
      let const &goal = goals[goal_position];
      try {
        has_outdated_goal |=
            build_target(ec, cxt, mk, goal.view(), active_targets,
                         completed_target_results, options);
      } catch (const ErrorWithLocation &) {
        throw;
      } catch (Error &error) {
        did_fail = true;
        report_soft_koshkit_util_error(ec, cxt, goal_locations[goal_position],
                                       args[0].view(), error.message().view());
        if (!runtime_flags.should_keep_going) return 2;
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
