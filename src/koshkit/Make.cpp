#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-f file] [target ...]");

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
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Make);

namespace koshka {

namespace koshkit {

namespace {

/* The variables make predefines, so a makefile that reads one without assigning
   it still finds a sane default. These sit at the lowest precedence, below a
   makefile assignment and the environment, which the expander checks first. */
constexpr static_string_entry<const char *> BUILTIN_VARIABLE_ENTRIES[] = {
    {SSK("MAKE"),    "koshkit make"},
    {SSK("CC"),      "cc"          },
    {SSK("CXX"),     "c++"         },
    {SSK("CPP"),     "cc -E"       },
    {SSK("AR"),      "ar"          },
    {SSK("ARFLAGS"), "rv"          },
    {SSK("RM"),      "rm -f"       },
};
constexpr StaticStringMap BUILTIN_VARIABLES{BUILTIN_VARIABLE_ENTRIES};

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
        default_goal(allocator), variable_index(allocator)
  {}
  ArrayList<make_variable> variables;
  ArrayList<make_rule> rules;
  ArrayList<make_rule> pattern_rules;
  /* The first ordinary explicit target, the bare-make goal. A target-specific
     variable line does not set it, the way GNU make picks the first real rule.
   */
  String default_goal;
  StringMap<usize> variable_index;

  fn find_variable(StringView name) const throws -> const String *
  {
    if (let const *index = variable_index.find(name))
      return &variables[*index].value;
    return nullptr;
  }

  fn find_rule(StringView target) const throws -> const make_rule *
  {
    for (const make_rule &rule : rules)
      if (rule.target == target) return &rule;
    return nullptr;
  }

  fn find_mutable_rule(StringView target) throws -> make_rule *
  {
    for (make_rule &rule : rules)
      if (rule.target == target) return &rule;
    return nullptr;
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
static fn substitute_automatic(StringView text, StringView target,
                               StringView first_prereq, StringView all_prereqs,
                               StringView stem, Allocator allocator) throws
    -> String
{
  String out{allocator};
  usize i = 0;
  while (i < text.length) {
    if (text[i] == '$' && i + 1 < text.length) {
      let const next = text[i + 1];
      if (next == '$') {
        out += "$$";
        i += 2;
        continue;
      }
      if (next == '@') {
        out += target;
        i += 2;
        continue;
      }
      if (next == '<') {
        out += first_prereq;
        i += 2;
        continue;
      }
      if (next == '^') {
        out += all_prereqs;
        i += 2;
        continue;
      }
      if (next == '*') {
        out += stem;
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

static fn expand(EvalContext &cxt, const makefile &mk, StringView text,
                 usize depth) throws -> String;

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
  if (const String *stored = mk.find_variable(variable_name); stored != nullptr)
    value = expand(cxt, mk, stored->view(), depth + 1);
  else if (Maybe<String> from_env = os::get_environment_variable(variable_name);
           from_env.has_value())
    value = steal(*from_env);

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
      } else if (const String *value = mk.find_variable(name); value != nullptr)
      {
        result += expand(cxt, mk, value->view(), depth + 1).view();
      } else if (Maybe<String> from_env = os::get_environment_variable(name);
                 from_env.has_value())
      {
        result += from_env->view();
      } else if (Maybe<const char *> builtin = BUILTIN_VARIABLES.find(name);
                 builtin.has_value())
      {
        result += StringView{*builtin};
      }
      i = j < text.length ? j + 1 : j;
    } else if (text[i] == '$' && i + 1 < text.length && text[i + 1] == '$') {
      result += '$';
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
                           StringView operator_and_value) throws -> void
{
  StringView value = operator_and_value.substring(1);
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
  let const physical = split_keep_newlines(source);
  ArrayList<String> logical{allocator};
  usize i = 0;
  while (i < physical.count()) {
    let const raw = physical[i].without_trailing_newline();
    let line = String{allocator, raw};

    while (ends_with_continuation(line.view()) && i + 1 < physical.count()) {
      line = String{allocator,
                    line.view().substring_of_length(0, line.view().length - 1)};
      i++;
      let const next = physical[i].without_trailing_newline();
      line += ' ';
      line += trim(next);
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

static fn parse_makefile(EvalContext &cxt, StringView source) throws -> makefile
{
  makefile mk{cxt.scratch_allocator()};
  make_rule *current = nullptr;
  ArrayList<conditional_state> conditionals{cxt.scratch_allocator()};

  let const do_is_active = [&]() -> bool {
    for (const conditional_state &state : conditionals)
      if (!state.is_branch_active) return false;
    return true;
  };

  for (const String &logical :
       join_continuations(source, cxt.scratch_allocator()))
  {
    StringView line = logical.view();

    /* A recipe line is kept verbatim and expanded only at build time. */
    if (!line.is_empty() && line[0] == '\t') {
      if (do_is_active() && current != nullptr)
        current->recipe_lines.push(
            String{cxt.scratch_allocator(), line.substring(1)});
      continue;
    }

    /* A comment runs to the end of a non-recipe line. */
    if (let const hash = line.find_character('#'); hash.has_value())
      line = line.substring_of_length(0, *hash);

    let const trimmed = trim(line);
    if (trimmed.is_empty()) {
      current = nullptr;
      continue;
    }

    let const directive = leading_word(trimmed);

    /* An inactive branch still tracks nested directives so the matching endif
       pops the right one. */
    if (directive == "ifeq" || directive == "ifneq" || directive == "ifdef" ||
        directive == "ifndef")
    {
      let const is_parent_active = do_is_active();
      let const is_taken =
          is_parent_active &&
          evaluate_conditional(cxt, mk, directive,
                               trimmed.substring(directive.length));
      conditionals.push(
          conditional_state{is_taken, is_taken, is_parent_active});
      current = nullptr;
      continue;
    }
    if (directive == "else") {
      if (!conditionals.is_empty()) {
        conditional_state &top = conditionals[conditionals.count() - 1];
        let const rest = trim(trimmed.substring(directive.length));
        let const else_directive = leading_word(rest);
        if (else_directive == "ifeq" || else_directive == "ifneq" ||
            else_directive == "ifdef" || else_directive == "ifndef")
        {
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
      current = nullptr;
      continue;
    }
    if (directive == "endif") {
      if (!conditionals.is_empty()) conditionals.pop_back();
      current = nullptr;
      continue;
    }

    if (!do_is_active()) {
      current = nullptr;
      continue;
    }

    /* override re-asserts a value, so its prefix is stripped and the assignment
       parses as usual. undefine, unexport, define, and a bare export are not
       modelled, so they are skipped rather than read as a malformed rule. An
       export that prefixes an assignment keeps the assignment. */
    StringView statement = trimmed;
    if (directive == "override")
      statement = trim(statement.substring(directive.length));

    let const statement_word = leading_word(statement);
    if (statement_word == "undefine" || statement_word == "unexport" ||
        statement_word == "define")
    {
      current = nullptr;
      continue;
    }
    if (statement_word == "export") {
      let const after_export = trim(statement.substring(statement_word.length));
      if (after_export.is_empty()) {
        current = nullptr;
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
        current = nullptr;
        for (const String &target :
             split_words(targets.view(), cxt.scratch_allocator()))
        {
          make_rule *rule = mk.find_mutable_rule(target.view());
          if (rule == nullptr) {
            make_rule fresh{cxt.scratch_allocator()};
            fresh.target = target.clone();
            mk.rules.push(steal(fresh));
            rule = &mk.rules[mk.rules.count() - 1];
          }
          rule->variable_assignments.push(
              String{cxt.scratch_allocator(), trim(after_colon)});
        }
        continue;
      }

      /* The targets and prerequisites expand when the rule is read. */
      let const targets =
          expand(cxt, mk, trim(statement.substring_of_length(0, *colon)), 0);
      let const prerequisites =
          expand(cxt, mk, statement.substring(*colon + 1), 0);
      current = nullptr;
      for (const String &target :
           split_words(targets.view(), cxt.scratch_allocator()))
      {
        let new_prerequisites =
            split_words(prerequisites.view(), cxt.scratch_allocator());
        if (mk.default_goal.is_empty() &&
            !target.view().find_character('%').has_value())
          mk.default_goal = target.clone();
        /* A recipe-less rule already standing for this target, such as one a
           prior target-specific assignment created, takes this line's
           prerequisites and recipe rather than a second rule find_rule would
           never reach. */
        if (!target.view().find_character('%').has_value())
          if (make_rule *existing = mk.find_mutable_rule(target.view());
              existing != nullptr && existing->recipe_lines.is_empty())
          {
            for (String &prerequisite : new_prerequisites)
              existing->prerequisites.push(steal(prerequisite));
            current = existing;
            continue;
          }

        make_rule rule{cxt.scratch_allocator()};
        rule.target = target.clone();
        rule.prerequisites = steal(new_prerequisites);
        if (target.view().find_character('%').has_value()) {
          mk.pattern_rules.push(steal(rule));
          current = &mk.pattern_rules[mk.pattern_rules.count() - 1];
        } else {
          mk.rules.push(steal(rule));
          current = &mk.rules[mk.rules.count() - 1];
        }
      }
      continue;
    }

    if (equals.has_value()) {
      apply_assignment(cxt, mk, statement.substring_of_length(0, *equals),
                       statement.substring(*equals));
      current = nullptr;
    }
  }
  return mk;
}

struct saved_make_variable
{
  String name;
  bool was_present;
  String old_value;
};

static fn build_target(const ExecContext &ec, EvalContext &cxt, makefile &mk,
                       StringView goal, ArrayList<String> &visiting,
                       ArrayList<String> &built) throws -> void
{
  if (built.find(goal).has_value()) return;

  if (visiting.find(goal).has_value())
    throw Error{
        "The target '" + String{cxt.scratch_allocator(), goal}
          +
        "' is part of a dependency cycle"
    };

  const ArrayList<String> *recipe_lines = nullptr;
  ArrayList<String> prerequisites{cxt.scratch_allocator()};
  const ArrayList<String> *target_assignments = nullptr;
  String target_stem{cxt.scratch_allocator()};

  if (const make_rule *rule = mk.find_rule(goal); rule != nullptr) {
    for (const String &prerequisite : rule->prerequisites) {
      let const expanded = expand(cxt, mk, prerequisite.view(), 0);
      for (const String &word :
           split_words(expanded.view(), cxt.scratch_allocator()))
        prerequisites.push(word.clone());
    }
    recipe_lines = &rule->recipe_lines;
    target_assignments = &rule->variable_assignments;
  } else {
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
      break;
    }

    if (recipe_lines == nullptr) {
      if (Path{goal}.exists()) return;
      throw Error{
          "There is no rule to make the target '" +
          String{cxt.scratch_allocator(), goal}
          + "'"
      };
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

  visiting.push(String{cxt.scratch_allocator(), goal});
  for (const String &prerequisite : normal_prerequisites)
    build_target(ec, cxt, mk, prerequisite.view(), visiting, built);
  for (const String &prerequisite : order_only_prerequisites)
    build_target(ec, cxt, mk, prerequisite.view(), visiting, built);
  visiting.pop_back();

  let const first_prereq = normal_prerequisites.is_empty()
                               ? StringView{}
                               : normal_prerequisites[0].view();
  ArrayList<String> seen_prerequisites{cxt.scratch_allocator()};
  String all_prereqs{cxt.scratch_allocator()};
  for (const String &prerequisite : normal_prerequisites) {
    if (seen_prerequisites.find(prerequisite.view()).has_value()) continue;

    seen_prerequisites.push(prerequisite.clone());
    if (!all_prereqs.is_empty()) all_prereqs += ' ';
    all_prereqs += prerequisite.view();
  }

  for (const String &recipe : *recipe_lines) {
    StringView body = recipe.view();
    bool is_silent = false;
    bool should_ignore_errors = false;
    /* A leading @ or - applies in either order. */
    while (!body.is_empty() && (body[0] == '@' || body[0] == '-')) {
      if (body[0] == '@') is_silent = true;
      if (body[0] == '-') should_ignore_errors = true;
      body = body.substring(1);
    }

    /* The automatic variables are filled on the raw recipe first, then the
       $(NAME) expansion runs, so a $$ stays an escape and a $@ that the
       expansion would not touch is resolved here. */
    let const with_autos =
        substitute_automatic(body, goal, first_prereq, all_prereqs.view(),
                             target_stem.view(), cxt.scratch_allocator());
    let const command = expand(cxt, mk, with_autos.view(), 0);
    if (command.is_empty()) continue;
    if (!is_silent) ec.print_to_stdout(command + "\n");

    /* A recipe runs with the strict toggles off so an unmatched glob or an
       unset variable does not abort the build. */
    let const saved_runtime = RuntimeState::capture(cxt);
    RuntimeState recipe_runtime = saved_runtime;
    recipe_runtime.set_option(shell_option_id::Failglob, false);
    recipe_runtime.set_option(shell_option_id::Nounset, false);
    recipe_runtime.warning_level = 0;
    recipe_runtime.restore(cxt);
    defer { saved_runtime.restore(cxt); };

    /* Each recipe line runs in its own subshell, the way GNU make spawns a
       shell per line. The parentheses also keep the tail-command exec
       optimization from replacing the make process when the recipe is a single
       external command, which would otherwise abandon the remaining recipe
       lines and targets. The newlines guard the closing paren against a
       trailing comment in the line. */
    let subshell_command = String{cxt.scratch_allocator(), "(\n"};
    subshell_command += command.view();
    subshell_command += "\n)";
    let const status = cxt.run_source(subshell_command.view(), "make",
                                      return_handling::Consume,
                                      ec.source_location(), StringView{"make"});
    if (status != 0 && !should_ignore_errors) {
      throw Error{
          "The recipe for the target '" +
          String{cxt.scratch_allocator(), goal}
          + "' failed with status " +
          String::from(status, cxt.scratch_allocator())
      };
    }
  }

  built.push(String{cxt.scratch_allocator(), goal});
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
  /* The -j flag accepts an optional job count, with no value meaning
     unlimited, so it is dropped here before the flag parser demands a value. */
  ArrayList<String> filtered{cxt.scratch_allocator()};
  for (const String &arg : args) {
    let const text = arg.view();
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
  let const operands = parse_util_operands(FLAG_LIST, filtered);
  defer { reset_flags(FLAG_LIST); };

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

  String makefile_path{cxt.scratch_allocator()};
  if (FLAG_MAKE_FILE.is_set()) {
    makefile_path = String{cxt.scratch_allocator(), FLAG_MAKE_FILE.value()};
  } else if (Path{"Makefile"}.exists()) {
    makefile_path = "Makefile";
  } else if (Path{"makefile"}.exists()) {
    makefile_path = "makefile";
  } else {
    throw ErrorWithDetails{"Unable to find a Makefile in the current directory",
                           "Create a `Makefile` or pass `-f <file>`"};
  }

  Maybe<String> source = Path{makefile_path.view()}.read_entire_file();
  if (!source.has_value())
    throw ErrorWithDetails{"Unable to read the makefile '" + makefile_path +
                               "': " + os::last_system_error_message(),
                           "Check the path passed to `-f`"};

  let mk = parse_makefile(cxt, source->view());

  ArrayList<String> goals{cxt.scratch_allocator()};
  if (operands.is_empty()) {
    if (mk.default_goal.is_empty())
      throw ErrorWithDetails{
          "The makefile defines no targets and no default goal",
          "Add a rule or name a target on the command line"};
    goals.push(mk.default_goal.clone());
  } else {
    for (const String &operand : operands)
      goals.push(operand.clone());
  }

  ArrayList<String> visiting{cxt.scratch_allocator()};
  ArrayList<String> built{cxt.scratch_allocator()};
  try {
    for (const String &goal : goals)
      build_target(ec, cxt, mk, goal.view(), visiting, built);
  } catch (const InterruptErrorWithLocation &) {
    throw;
  } catch (const ErrorWithLocation &) {
    throw;
  } catch (Error &error) {
    error.set_command_status(2);
    throw;
  }

  return 0;
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

  let const mk = parse_makefile(cxt, source->view());
  for (const make_rule &rule : mk.rules) {
    let const name = rule.target.view();
    if (name.is_empty() || name[0] == '.') continue;
    targets.push(rule.target.clone());
  }
  return targets;
}

} /* namespace koshkit */

} /* namespace koshka */
