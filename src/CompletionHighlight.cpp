/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements highlight for command completion. It scans tolerant
 * shell input and produces semantic candidates, highlighting, syntax, paths,
 * or command metadata.
 */

#include "Arena.hpp"
#include "Builtin.hpp"
#include "CliColors.hpp"
#include "Completion.hpp"
#include "CompletionInternal.hpp"
#include "Debug.hpp"
#include "HashSet.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace completion {

using namespace internal;

/* Reset at the top of highlight_line so the previous render stays valid until
   the editor drains it. */
BumpArena internal::HIGHLIGHT_ARENA{};
#if !defined NDEBUG
usize internal::DEBUG_HIGHLIGHT_INPUT_BYTE_COUNT = 0;
#endif

static fn first_word_resolves(StringView word, EvalContext &context) throws
    -> bool
{
  if (word == "!") return true;
  if (KEYWORDS.find(word).has_value()) return true;

  /* A path word resolves against the filesystem with a leading tilde expanded
     first. */
  if (os::has_directory_separator(word)) {
    let target = word;
    Maybe<String> expanded;
    if (!word.is_empty() && word[0] == '~') {
      if (Maybe<String> home_expanded = utils::expand_leading_tilde_path(word))
      {
        expanded = steal(*home_expanded);
        target = expanded->view();
      } else
        return false;
    }
    /* An existing regular file resolves even when not executable, permission
       is a runtime matter. */
    if (Maybe<Path> canonical = Path::canonicalize(target);
        canonical.has_value())
    {
      return canonical->is_regular_file() || canonical->is_directory();
    }
    return false;
  }

  if (search_builtin(word).has_value()) return true;
  if (context.find_function(word) != nullptr) return true;
  if (context.get_alias(word).has_value()) return true;

  let const path_status = context.get_program_resolver().get_status(word);
  let const resolves = path_status == ProgramResolver::Status::Runnable;
  LOG(All, "the path search resolves '%.*s' to %s",
      static_cast<int>(word.length), word.data, resolves ? "yes" : "no");
  if (path_status != ProgramResolver::Status::Missing) return resolves;

  return context.koshkit_utilities_are_reachable() &&
         koshkit::find_util(word).has_value();
}

static fn command_word_prefixes_any(StringView word,
                                    EvalContext &context) throws -> bool
{
  if (word.is_empty()) return false;
  if (os::has_directory_separator(word)) return false;

  let const do_has_prefix = [&](StringView name) -> bool {
    return utils::smart_case_prefix_matches(name, word);
  };

  for (let const &builtin_name : builtin_names())
    if (do_has_prefix(builtin_name.view())) return true;

  bool was_found = false;
  context.for_each_function_name([&](StringView name) {
    if (!was_found && do_has_prefix(name)) was_found = true;
  });
  if (was_found) return true;
  context.for_each_alias_name([&](StringView name) {
    if (!was_found && do_has_prefix(name)) was_found = true;
  });
  if (was_found) return true;

  if (context.get_program_resolver().command_name_has_prefix(word)) return true;

  if (context.koshkit_utilities_are_reachable()) {
    for (let const &util_name : koshkit::util_names())
      if (do_has_prefix(util_name.view())) return true;
  }

  return false;
}

fn command_word_resolves(StringView line, EvalContext &context) throws -> bool
{
  let const word = command_word_of(line);
  if (word.is_empty()) return true;
  return first_word_resolves(word, context);
}

static pure fn is_highlight_name_start(char c) wontthrow -> bool
{
  return lexer::is_variable_name_start(c);
}

static pure fn is_highlight_name_char(char c) wontthrow -> bool
{
  return lexer::is_variable_name(c);
}

static pure fn is_highlight_function_name_char(char c) wontthrow -> bool
{
  if (is_highlight_name_char(c)) return true;

  switch (c) {
  case '/':
  case '.':
  case '-':
  case '+':
  case ':':
  case '@':
  case '#':
  case '%':
  case '{':
  case '}': return true;

  default: return false;
  }
}

/* A brace pair holding `..` is a range expansion and an unpaired brace is a
   fragment, so neither one belongs to a name. */
static pure fn word_braces_are_literal(StringView word) wontthrow -> bool
{
  usize brace_depth = 0;
  for (usize i = 0; i < word.length; i++) {
    if (word[i] == '{') {
      brace_depth++;
      continue;
    }
    if (word[i] == '}') {
      if (brace_depth == 0) return false;

      brace_depth--;
      continue;
    }
    if (brace_depth > 0 && word[i] == '.' && i + 1 < word.length &&
        word[i + 1] == '.')
    {
      return false;
    }
  }

  return brace_depth == 0;
}

/* The shell looks a command word up in the function table before it treats the
   word as a path, so a name such as ble/util/put is a call and not a file. */
pure fn internal::word_is_function_name(StringView word) wontthrow -> bool
{
  if (word.is_empty() || !is_highlight_name_start(word[0])) return false;

  let has_brace = false;
  for (usize i = 1; i < word.length; i++) {
    if (!is_highlight_function_name_char(word[i])) return false;
    if (word[i] == '{' || word[i] == '}') has_brace = true;
  }

  return !has_brace || word_braces_are_literal(word);
}

pure fn internal::word_defines_function(StringView line, usize word_end,
                                        usize end) wontthrow -> bool
{
  let i = word_end;
  while (i < end && (line[i] == ' ' || line[i] == '\t'))
    i++;
  if (i >= end || line[i] != '(') return false;

  i++;
  while (i < end && (line[i] == ' ' || line[i] == '\t'))
    i++;
  return i < end && line[i] == ')';
}

/* '{' and '}' are left out so a brace word such as a{1,2} stays one word. */
static pure fn position_is_highlight_word_break(StringView line, usize position,
                                                usize end) wontthrow -> bool
{
  let const c = line[position];
  switch (c) {
  case ' ':
  case '\t':
  case '\n':
  case '|':
  case '&':
  case ';':
  case '<':
  case '>':
  case '(':
  case ')': return true;
  case '\r': return position + 1 < end && line[position + 1] == '\n';
  default: return false;
  }
}

/* The $(...) form is handled by the caller. */
static pure fn scan_dollar_expansion(StringView line, usize dollar,
                                     usize end) wontthrow -> usize
{
  let i = dollar + 1;
  if (i >= end) return i;

  let const c = line[i];
  if (c == '{') {
    usize depth = 0;
    for (; i < end; i++) {
      if (line[i] == '{')
        depth++;
      else if (line[i] == '}') {
        i++;
        depth--;
        if (depth == 0) break;
      }
    }
    return i <= line.length ? i : line.length;
  }
  if (c >= '0' && c <= '9') {
    while (i < end && line[i] >= '0' && line[i] <= '9')
      i++;
    return i;
  }
  if (lexer::is_special_parameter_char(c)) return i + 1;
  if (is_highlight_name_start(c)) {
    while (i < end && is_highlight_name_char(line[i]))
      i++;
    return i;
  }
  return i;
}

static pure fn word_contains_url_scheme(StringView word) wontthrow -> bool
{
  for (usize i = 0; i + 2 < word.length; i++) {
    if (word[i] == ':' && word[i + 1] == '/' && word[i + 2] == '/') return true;
  }
  return false;
}

static pure fn word_looks_like_ssh_remote_path(StringView word) wontthrow
    -> bool
{
  let const colon_position = word.find_character(':');
  if (!colon_position.has_value() || *colon_position == 0) return false;

  for (usize byte_index = 0; byte_index < *colon_position; byte_index++)
    if (word[byte_index] == '/' || word[byte_index] == '\\') return false;

  if (*colon_position == 1 && word.length > 2 &&
      (word[2] == '/' || word[2] == '\\'))
  {
    return false;
  }

  let const authority = word.substring_of_length(0, *colon_position);
  if (let const at_position = authority.find_character('@');
      at_position.has_value() &&
      (*at_position == 0 || *at_position + 1 == authority.length))
  {
    return false;
  }
  return true;
}

namespace {

enum class keyword_role : u8
{
  open,
  check,
  close,
  plain,
  misplaced_in,
};

struct keyword_spec
{
  keyword_role role;
  highlight_construct construct = highlight_construct::if_;
  highlight_construct construct_alt = highlight_construct::if_;
  bool has_alt = false;
  bool is_next_command = false;
  bool should_expect_in = false;
  bool should_expect_for_variable = false;
  bool should_set_function_pending = false;
  bool is_non_posix_only = false;
};

constexpr static_string_entry<keyword_spec> HIGHLIGHT_KEYWORD_ENTRIES[] = {
    {SSK("if"),
     {.role = keyword_role::open,
      .construct = highlight_construct::if_,
      .is_next_command = true}                                             },
    {SSK("while"),
     {.role = keyword_role::open,
      .construct = highlight_construct::while_until,
      .is_next_command = true}                                             },
    {SSK("until"),
     {.role = keyword_role::open,
      .construct = highlight_construct::while_until,
      .is_next_command = true}                                             },
    {SSK("for"),
     {.role = keyword_role::open,
      .construct = highlight_construct::for_,
      .should_expect_in = true,
      .should_expect_for_variable = true}                                  },
    {SSK("case"),
     {.role = keyword_role::open,
      .construct = highlight_construct::case_,
      .should_expect_in = true}                                            },
    {SSK("[["),
     {.role = keyword_role::open,
      .construct = highlight_construct::conditional,
      .is_non_posix_only = true}                                           },
    {SSK("function"),
     {.role = keyword_role::open,
      .construct = highlight_construct::function,
      .should_set_function_pending = true}                                 },
    {SSK("then"),
     {.role = keyword_role::check, .construct = highlight_construct::if_}  },
    {SSK("else"),
     {.role = keyword_role::check, .construct = highlight_construct::if_}  },
    {SSK("elif"),
     {.role = keyword_role::check, .construct = highlight_construct::if_}  },
    {SSK("do"),
     {.role = keyword_role::check,
      .construct = highlight_construct::while_until,
      .construct_alt = highlight_construct::for_,
      .has_alt = true}                                                     },
    {SSK("fi"),
     {.role = keyword_role::close, .construct = highlight_construct::if_}  },
    {SSK("done"),
     {.role = keyword_role::close,
      .construct = highlight_construct::while_until,
      .construct_alt = highlight_construct::for_,
      .has_alt = true}                                                     },
    {SSK("esac"),
     {.role = keyword_role::close, .construct = highlight_construct::case_}},
    {SSK("time"),     {.role = keyword_role::plain}                        },
    {SSK("when"),     {.role = keyword_role::plain}                        },
    {SSK("in"),       {.role = keyword_role::misplaced_in}                 },
};

constexpr StaticStringMap HIGHLIGHT_KEYWORDS{HIGHLIGHT_KEYWORD_ENTRIES};

/* The highlight table carries `[[` and `in`, which the lexer keyword table does
   not hold, so the coupling is a subset test in one direction. */
consteval fn every_lexer_keyword_has_a_highlight_spec() wontthrow -> bool
{
  for (let const &keyword : KEYWORD_ENTRIES) {
    bool has_spec = false;
    for (let const &spec : HIGHLIGHT_KEYWORD_ENTRIES) {
      if (spec.key == keyword.key) has_spec = true;
    }

    if (!has_spec) return false;
  }

  return true;
}

static_assert(every_lexer_keyword_has_a_highlight_spec(),
              "A lexer keyword reaches the highlighter with no spec, so its "
              "construct never opens.");

} /* namespace */

fn internal::advance_shell_keyword_state(StringView word, usize frame_depth,
                                         shell_lexical_state &state) throws
    -> Maybe<bool>
{
  let const spec = HIGHLIGHT_KEYWORDS.find(word);
  if (!spec.has_value()) return None;
  LOG(All, "advancing the lexical keyword state for '%.*s'",
      static_cast<int>(word.length), word.data);

  switch (spec->role) {
  case keyword_role::open: {
    if (spec->construct == highlight_construct::case_)
      return spec->is_next_command;

    let phase = highlight_construct_phase::condition;
    if (spec->construct == highlight_construct::for_)
      phase = highlight_construct_phase::for_variable;
    else if (spec->construct == highlight_construct::function)
      phase = highlight_construct_phase::function_name;
    else if (spec->construct == highlight_construct::conditional)
      phase = highlight_construct_phase::body;
    state.constructs.push(
        shell_lexical_construct{frame_depth, spec->construct, phase});
    return spec->is_next_command;
  }

  case keyword_role::check:
    if (!state.constructs.is_empty()) {
      let &top = state.constructs.back();
      if (top.frame_depth == frame_depth &&
          (top.kind == spec->construct ||
           (spec->has_alt && top.kind == spec->construct_alt)))
      {
        top.phase = word == "elif" ? highlight_construct_phase::condition
                                   : highlight_construct_phase::body;
      }
    }
    return true;

  case keyword_role::close:
    if (!state.constructs.is_empty()) {
      let const &top = state.constructs.back();
      if (top.frame_depth == frame_depth &&
          (top.kind == spec->construct ||
           (spec->has_alt && top.kind == spec->construct_alt)))
      {
        state.constructs.pop_back();
      }
    }
    return true;

  case keyword_role::plain: return true;
  case keyword_role::misplaced_in: return false;
  }

  return None;
}

static fn
color_arithmetic(StringView line, usize begin, usize end, EvalContext &context,
                 ArrayList<highlight_span> &spans, HashSet &line_variable_names,
                 const HashSet *known_function_names,
                 bool should_stop_at_closing_parentheses) throws -> usize;

static fn word_names_existing_path(StringView word) throws -> bool
{
  if (word.is_empty()) return false;
  if (word[0] == '~') {
    if (Maybe<String> expanded = utils::expand_leading_tilde_path(word);
        expanded.has_value())
      return Path{expanded->view()}.exists();
    return false;
  }

  return Path{word}.exists();
}

static fn path_partial_prefixes_entry(StringView word, usize existing_end,
                                      StringView partial, bool has_tilde,
                                      bool should_only_include_directories,
                                      StringView expanded_tilde_prefix,
                                      usize tilde_prefix_length) throws -> bool
{
  if (partial.is_empty()) return false;

  String directory{bump_allocator(HIGHLIGHT_ARENA)};
  if (existing_end > 0) {
    let const prefix = word.substring_of_length(0, existing_end);
    if (has_tilde) {
      if (expanded_tilde_prefix.is_empty()) return false;
      directory.append(expanded_tilde_prefix);
      directory.append(prefix.substring(tilde_prefix_length));
    } else {
      directory = String{bump_allocator(HIGHLIGHT_ARENA), prefix};
    }
  } else if (let const root_length = os::path_root_length(word);
             root_length > 0)
  {
    directory = String{bump_allocator(HIGHLIGHT_ARENA),
                       word.substring_of_length(0, root_length)};
  } else {
    directory = String{bump_allocator(HIGHLIGHT_ARENA), "."};
  }

  let const listing_directory = Path{directory.view()};
  let const entries = utils::read_directory_cached(
      listing_directory, utils::directory_validation::Cached,
      utils::directory_listing_order::FoldedName);
  if (entries == nullptr) return false;

  let const do_name_starts_with = [&](StringView name) wontthrow {
    if (name.starts_with(partial)) return true;
    return !os::FILESYSTEM_IS_CASE_SENSITIVE;
  };

  let entry_position =
      utils::directory_entry_name_lower_bound(*entries, partial);
  while (entry_position < entries->count() &&
         utils::directory_entry_name_has_casefold_prefix(
             (*entries)[entry_position].name.view(), partial))
  {
    let const &entry = (*entries)[entry_position];
    if (do_name_starts_with(entry.name.view()) &&
        (!should_only_include_directories ||
         utils::directory_entry_kind(listing_directory, entry) ==
             Path::entry_kind::Directory))
    {
      return true;
    }
    entry_position++;
  }

  return false;
}

/* True when the byte after a word finishes it, so no keystroke can grow it. */
static fn word_is_terminated_by_separator(StringView line, usize word_end,
                                          usize line_length) wontthrow -> bool
{
  if (word_end >= line_length) return false;

  let const next_byte = line[word_end];
  return next_byte == ' ' || next_byte == '\t' || next_byte == '\n' ||
         next_byte == ';' || next_byte == '|' || next_byte == '&' ||
         next_byte == '<' || next_byte == '>' || next_byte == '(' ||
         next_byte == ')' ||
         (next_byte == '\r' && word_end + 1 < line_length &&
          line[word_end + 1] == '\n');
}

/* Path coloring receives source spelling rather than an expanded word. On
   Windows an unquoted backslash looks like a native separator but the shell
   grammar removes it when it escapes an ordinary byte. */
static fn word_has_erased_directory_separator(StringView word) wontthrow -> bool
{
  char quote_character = 0;
  for (usize position = 0; position + 1 < word.length; position++) {
    let const byte = word[position];
    if (byte == '\\' && quote_character != '\'') {
      let const escaped_byte = word[position + 1];
      let const is_escape = quote_character != '"' || escaped_byte == '$' ||
                            escaped_byte == '`' || escaped_byte == '"' ||
                            escaped_byte == '\\' || escaped_byte == '\n';
      if (!is_escape) continue;
      if (os::is_directory_separator(byte) &&
          !os::is_directory_separator(escaped_byte))
        return true;
      position++;
      continue;
    }
    if (quote_character == 0 && (byte == '\'' || byte == '"')) {
      quote_character = byte;
    } else if (byte == quote_character) {
      quote_character = 0;
    }
  }

  return false;
}

/* Returns whether the word was treated as a path. */
static fn color_path_argument(usize word_start, StringView word,
                              bool word_is_terminated,
                              bool should_only_include_directories,
                              bool is_leading_tilde_active,
                              ArrayList<highlight_span> &spans) throws -> bool
{
  if (word.is_empty() || word[0] == '-') return false;

  let const has_separator = os::has_directory_separator(word);
  let const has_tilde = is_leading_tilde_active;
  let const has_dot_prefix = word.length >= 2 && word[0] == '.' &&
                             (os::is_directory_separator(word[1]) ||
                              (word.length >= 3 && word[1] == '.' &&
                               os::is_directory_separator(word[2])));
  let expanded_tilde_prefix = String{bump_allocator(HIGHLIGHT_ARENA)};
  usize tilde_prefix_length = word.length;
  if (has_tilde) {
    for (usize position = 1; position < word.length; position++)
      if (os::is_directory_separator(word[position])) {
        tilde_prefix_length = position;
        break;
      }
    let const tilde_prefix = word.substring_of_length(0, tilde_prefix_length);
    if (Maybe<String> expanded = utils::expand_leading_tilde_path(tilde_prefix))
      expanded_tilde_prefix = steal(*expanded);
  }
  let expanded_path = String{bump_allocator(HIGHLIGHT_ARENA)};
  if (!expanded_tilde_prefix.is_empty()) {
    expanded_path.append(expanded_tilde_prefix.view());
    expanded_path.append(word.substring(tilde_prefix_length));
  }

  let is_prefix_non_directory = false;
  let const do_prefix_is_valid = [&](usize prefix_end, bool should_be_directory)
                                     throws -> bool {
    is_prefix_non_directory = false;
    let target = word.substring_of_length(0, prefix_end);
    if (has_tilde) {
      if (expanded_tilde_prefix.is_empty()) return false;
      ASSERT(prefix_end >= tilde_prefix_length);
      target = expanded_path.view().substring_of_length(
          0, expanded_tilde_prefix.count() + prefix_end - tilde_prefix_length);
    }

    let status = os::file_status{};
    if (!os::stat_path_following(target, status)) return false;
    is_prefix_non_directory = os::file_type_letter(status.mode) != 'd';
    return !should_be_directory || !is_prefix_non_directory;
  };

  let const has_no_path_shape = !has_separator && !has_tilde && !has_dot_prefix;

  usize existing_end = 0;
  if (has_no_path_shape) {
    if (!should_only_include_directories) {
      if (!word_names_existing_path(word)) return false;
      existing_end = word.length;
    } else {
      existing_end = do_prefix_is_valid(word.length, true) ? word.length : 0;
    }
  } else {
    let const word_requires_directory =
        should_only_include_directories ||
        os::is_directory_separator(word[word.length - 1]);
    if (do_prefix_is_valid(word.length, word_requires_directory)) {
      existing_end = word.length;
    } else if (is_prefix_non_directory) {
      spans.push(highlight_span{word_start, word_start + word.length,
                                highlight_role::invalid_path});
      return true;
    } else {
      usize component_end = os::path_root_length(word);
      existing_end = component_end;
      while (component_end < word.length) {
        while (component_end < word.length &&
               os::is_directory_separator(word[component_end]))
          component_end++;
        if (component_end >= word.length) {
          existing_end = word.length;
          break;
        }
        while (component_end < word.length &&
               !os::is_directory_separator(word[component_end]))
          component_end++;
        let const should_be_directory =
            should_only_include_directories || component_end < word.length;
        if (!do_prefix_is_valid(component_end, should_be_directory)) {
          if (is_prefix_non_directory) {
            spans.push(highlight_span{word_start, word_start + word.length,
                                      highlight_role::invalid_path});
            return true;
          }
          break;
        }
        existing_end = component_end;
        while (existing_end < word.length &&
               os::is_directory_separator(word[existing_end]))
          existing_end++;
      }
    }
  }

  if (existing_end > 0)
    spans.push(highlight_span{word_start, word_start + existing_end,
                              highlight_role::existing_path});

  if (existing_end >= word.length) return true;

  usize segment_end = existing_end;
  while (segment_end < word.length &&
         !os::is_directory_separator(word[segment_end]))
    segment_end++;

  let const partial =
      word.substring_of_length(existing_end, segment_end - existing_end);
  let const tail_could_complete =
      !word_is_terminated &&
      path_partial_prefixes_entry(word, existing_end, partial, has_tilde,
                                  should_only_include_directories,
                                  expanded_tilde_prefix.view(),
                                  tilde_prefix_length);
  let const tail_role = tail_could_complete ? highlight_role::partial_path
                                            : highlight_role::invalid_path;
  spans.push(highlight_span{word_start + existing_end, word_start + segment_end,
                            tail_role});
  if (segment_end < word.length)
    spans.push(highlight_span{word_start + segment_end,
                              word_start + word.length,
                              highlight_role::invalid_path});

  return true;
}

/* None when the expansion carries an operator such as ${x:-y} or a form like
   ${#x}. */
static fn simple_dollar_name(StringView line, usize i,
                             usize expansion_end) wontthrow -> Maybe<StringView>
{
  if (i + 1 >= expansion_end) return koshka::None;
  if (line[i + 1] == '{') {
    if (expansion_end < i + 3 || line[expansion_end - 1] != '}') {
      return koshka::None;
    }
    let inner = line.substring_of_length(i + 2, expansion_end - (i + 2) - 1);
    if (inner.is_empty()) return koshka::None;
    for (usize k = 0; k < inner.length; k++)
      if (!is_highlight_name_char(inner[k])) return koshka::None;
    return inner;
  }
  return line.substring_of_length(i + 1, expansion_end - (i + 1));
}

/* Read without side effect so the highlighter never advances RANDOM or reads
   the clock. */
static fn dollar_name_is_set(StringView name,
                             const HashSet &line_variable_names,
                             EvalContext &context) throws -> bool
{
  if (name.is_empty()) return true;
  if (name.length == 1 && !is_highlight_name_start(name[0])) return true;

  if (name.is_all_decimal_digits()) return true;

  if (line_variable_names.contains(name) || context.has_variable_name(name)) {
    return true;
  }
  return os::get_environment_variable(name).has_value();
}

static fn color_dollar(StringView line, usize i, usize end,
                       ArrayList<highlight_span> &spans, EvalContext &context,
                       HashSet &line_variable_names,
                       const HashSet *known_function_names) throws -> usize
{
  /* $(( ... )) frames an arithmetic expression, so its inside colors as bare
     names, numbers, and operators. */
  if (i + 2 < end && line[i + 1] == '(' && line[i + 2] == '(') {
    let const inner_begin = i + 3 < end ? i + 3 : end;
    return color_arithmetic(line, inner_begin, end, context, spans,
                            line_variable_names, known_function_names, true);
  }

  if (i + 1 < end && line[i + 1] == '(') {
    let const inner_begin = i + 2 < end ? i + 2 : end;
    return scan_highlight_range(line, inner_begin, end, context, spans,
                                line_variable_names, known_function_names,
                                true);
  }
  let const expansion_end = scan_dollar_expansion(line, i, end);
  if (expansion_end > i) {
    let role = highlight_role::variable;
    if (Maybe<StringView> name = simple_dollar_name(line, i, expansion_end);
        name.has_value() &&
        !dollar_name_is_set(*name, line_variable_names, context))
      role = highlight_role::unset_variable;
    spans.push(highlight_span{i, expansion_end, role});
  }
  return expansion_end;
}

static fn
color_arithmetic(StringView line, usize begin, usize end, EvalContext &context,
                 ArrayList<highlight_span> &spans, HashSet &line_variable_names,
                 const HashSet *known_function_names,
                 bool should_stop_at_closing_parentheses) throws -> usize
{
  usize i = begin;
  usize parenthesis_depth = 0;
  while (i < end) {
    let const c = line[i];

    if (c == '$') {
      let const next = color_dollar(line, i, end, spans, context,
                                    line_variable_names, known_function_names);
      i = next > i ? next : i + 1;
      continue;
    }

    if (c == '(') {
      parenthesis_depth++;
      spans.push(highlight_span{i, i + 1, highlight_role::operator_});
      i++;
      continue;
    }

    if (c == ')') {
      if (parenthesis_depth == 0 && should_stop_at_closing_parentheses &&
          i + 1 < end && line[i + 1] == ')')
      {
        return i + 2;
      }
      if (parenthesis_depth > 0) parenthesis_depth--;
      spans.push(highlight_span{i, i + 1, highlight_role::operator_});
      i++;
      continue;
    }

    if (is_highlight_name_start(c)) {
      let const name_start = i;
      while (i < end && is_highlight_name_char(line[i]))
        i++;
      let const name = line.substring_of_length(name_start, i - name_start);
      spans.push(
          highlight_span{name_start, i,
                         dollar_name_is_set(name, line_variable_names, context)
                             ? highlight_role::variable
                             : highlight_role::unset_variable});
      continue;
    }

    if (c >= '0' && c <= '9') {
      while (i < end && (is_highlight_name_char(line[i]) || line[i] == '.'))
        i++;
      continue;
    }

    if (lexer::is_whitespace(c) || c == '\n') {
      i++;
      continue;
    }

    let const operator_start = i;
    while (i < end && line[i] != '$' && !is_highlight_name_start(line[i]) &&
           line[i] != '(' && line[i] != ')' &&
           !(line[i] >= '0' && line[i] <= '9') &&
           !lexer::is_whitespace(line[i]))
      i++;
    spans.push(highlight_span{operator_start, i, highlight_role::operator_});
  }

  return i;
}

struct heredoc_pending_highlight
{
  String delimiter;
  bool should_strip_tabs;
};

/* A <<- delimiter is matched once its leading tabs are skipped, the way the
   lexer strips them. */
static fn
scan_heredoc_bodies(StringView line, usize position, usize end,
                    const ArrayList<heredoc_pending_highlight> &pending,
                    ArrayList<highlight_span> &spans) throws -> usize
{
  let i = position;

  for (let const &heredoc : pending) {
    let const body_start = i;
    let was_closed = false;

    while (i < end) {
      let line_end = i;
      while (line_end < end && line[line_end] != '\n')
        line_end++;

      let content_start = i;
      if (heredoc.should_strip_tabs) {
        while (content_start < line_end && line[content_start] == '\t')
          content_start++;
      }

      let const content = lexer::heredoc_line_content(
          line.substring_of_length(content_start, line_end - content_start));
      let const content_end = content_start + content.length;
      let const next = (line_end < end) ? line_end + 1 : line_end;

      if (content == heredoc.delimiter.view()) {
        if (body_start < content_start)
          spans.push(highlight_span{body_start, content_start,
                                    highlight_role::heredoc});
        if (content_start < content_end)
          spans.push(highlight_span{content_start, content_end,
                                    highlight_role::heredoc_delimiter});
        i = next;
        was_closed = true;
        break;
      }

      i = next;
    }

    if (!was_closed) {
      if (body_start < end)
        spans.push(highlight_span{body_start, end, highlight_role::heredoc});
      i = end;
      break;
    }
  }

  return i;
}

enum class name_operand_role : u8
{
  binds_variable,
  reads_variable,
  names_command,
};

static constexpr static_string_entry<name_operand_role>
    NAME_OPERAND_COMMAND_ENTRIES[] = {
        {SSK("alias"),    name_operand_role::names_command },
        {SSK("declare"),  name_operand_role::binds_variable},
        {SSK("export"),   name_operand_role::binds_variable},
        {SSK("local"),    name_operand_role::binds_variable},
        {SSK("readonly"), name_operand_role::binds_variable},
        {SSK("typeset"),  name_operand_role::binds_variable},
        {SSK("unalias"),  name_operand_role::names_command },
        {SSK("unset"),    name_operand_role::reads_variable},
};
static constexpr StaticStringMap NAME_OPERAND_COMMANDS{
    NAME_OPERAND_COMMAND_ENTRIES};

/* An alias name reaches as wide as a function name, so the operand keeps every
   byte before the equals sign. */
static pure fn command_name_operand_of(StringView word) wontthrow -> StringView
{
  let name = word;
  if (Maybe<usize> assignment_position = name.find_character('=');
      assignment_position.has_value())
    name = name.substring_of_length(0, assignment_position.value());

  return word_is_function_name(name) ? name : StringView{};
}

static pure fn variable_name_operand_of(StringView word) wontthrow -> StringView
{
  let name = word;

  if (Maybe<usize> assignment_position = name.find_character('=');
      assignment_position.has_value())
  {
    let name_length = assignment_position.value();
    if (name_length > 0 && name[name_length - 1] == '+') name_length--;
    name = name.substring_of_length(0, name_length);
  }

  if (Maybe<usize> bracket_position = name.find_character('[');
      bracket_position.has_value())
  {
    name = name.substring_of_length(0, bracket_position.value());
  }

  return lexer::word_is_variable_name(name) ? name : StringView{};
}

/* A command substitution recurses with its own command-position and construct
   state, so a nested command line colors on its own. */
fn internal::scan_highlight_range(
    StringView line, usize begin, usize end, EvalContext &context,
    ArrayList<highlight_span> &spans, HashSet &line_variable_names,
    const HashSet *known_function_names,
    bool should_stop_at_closing_parenthesis) throws -> usize
{
  let const do_push = [&](usize start, usize stop, highlight_role role)
                          throws -> void {
    if (start < stop) spans.push(highlight_span{start, stop, role});
  };

  let pending_assignment_names =
      ArrayList<StringView>{bump_allocator(HIGHLIGHT_ARENA)};
  let const do_commit_pending_assignments = [&]() throws -> void {
    for (let const &name : pending_assignment_names)
      line_variable_names.add(name);
    pending_assignment_names.clear();
  };

  let stack = ArrayList<highlight_construct>{bump_allocator(HIGHLIGHT_ARENA)};
  let pending_heredocs =
      ArrayList<heredoc_pending_highlight>{bump_allocator(HIGHLIGHT_ARENA)};
  let is_command_position = true;
  let is_time_option_pending = false;
  let highlight_command_word = StringView{};
  let expecting_in = false;
  let for_variable_pending = false;
  let for_do_expected = false;
  let function_name_pending = false;
  let is_case_pattern_expected = false;
  let line_functions = HashSet{bump_allocator(HIGHLIGHT_ARENA)};
  let decoded_command_word =
      utils::decoded_shell_word{bump_allocator(HIGHLIGHT_ARENA)};
  let is_in_array_value = false;
  usize parenthesis_depth = 0;
  usize array_value_parenthesis_depth = 0;

  let const do_color_backtick =
      [&](usize backtick_position, ArrayList<highlight_span> &word_spans)
          throws -> usize {
    let const inner_begin = backtick_position + 1;
    let position = inner_begin;
    while (position < end && line[position] != '`') {
      if (line[position] == '\\' && position + 1 < end &&
          (line[position + 1] == '`' || line[position + 1] == '$' ||
           line[position + 1] == '\\'))
      {
        position += 2;
      } else {
        position++;
      }
    }
    let const inner_end = position;
    if (position < end) position++;
    scan_highlight_range(line, inner_begin, inner_end, context, word_spans,
                         line_variable_names, known_function_names);
    return position;
  };

  let i = begin;
  while (i < end) {
    let const c = line[i];

    if (c == ' ' || c == '\t' || c == '\n' ||
        (c == '\r' && i + 1 < end && line[i + 1] == '\n'))
    {
      /* A newline ends a command the way a ';' does. */
      if (c == '\n') {
        do_commit_pending_assignments();
        is_command_position = true;
        is_time_option_pending = false;
        highlight_command_word = StringView{};
        i++;
        if (!pending_heredocs.is_empty()) {
          i = scan_heredoc_bodies(line, i, end, pending_heredocs, spans);
          pending_heredocs.clear();
        }
        continue;
      }
      i++;
      continue;
    }

    if (c == '#') {
      let comment_end = i;
      while (comment_end < end && line[comment_end] != '\n')
        comment_end++;
      do_push(i, comment_end, highlight_role::comment);
      i = comment_end;
      continue;
    }

    if (is_command_position && c == '(' && i + 1 < end && line[i + 1] == '(') {
      spans.push(highlight_span{i, i + 2, highlight_role::operator_});
      i = color_arithmetic(line, i + 2, end, context, spans,
                           line_variable_names, known_function_names, true);
      is_command_position = false;
      continue;
    }

    /* <<< is a one-line here-string and falls through to the operator scan. */
    if (c == '<' && i + 1 < end && line[i + 1] == '<' &&
        !(i + 2 < end && line[i + 2] == '<'))
    {
      let const operator_start = i;
      i += 2;
      let should_strip_tabs = false;
      if (i < end && line[i] == '-') {
        should_strip_tabs = true;
        i++;
      }
      do_push(operator_start, i, highlight_role::operator_);

      while (i < end && (line[i] == ' ' || line[i] == '\t'))
        i++;

      let const delimiter_start = i;
      while (i < end && !position_is_highlight_word_break(line, i, end))
        i++;

      let const delimiter_word =
          line.substring_of_length(delimiter_start, i - delimiter_start);
      if (!delimiter_word.is_empty()) {
        do_push(delimiter_start, i, highlight_role::heredoc_delimiter);

        let delimiter = lexer::unquote_heredoc_delimiter(
            delimiter_word, bump_allocator(HIGHLIGHT_ARENA));
        pending_heredocs.push(
            heredoc_pending_highlight{steal(delimiter), should_strip_tabs});
      }
      continue;
    }

    /* A separator or an opener moves the next word back to command position, a
       redirection does not. */
    if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' ||

        c == ')' || c == '{' || c == '}')
    {
      let const operator_start = i;
      let has_separator = false;
      let has_redirect = false;
      let has_opener = false;
      let has_closer = false;
      while (i < end) {
        let const o = line[i];
        if (o == '|' || o == '&' || o == ';') {
          has_separator = true;
          i++;
        } else if (o == '<' || o == '>') {
          has_redirect = true;
          i++;
        } else if (o == '(' || o == '{') {
          has_opener = true;
          i++;
          break;
        } else if (o == ')' || o == '}') {
          has_closer = true;
          i++;
          break;
        } else {
          break;
        }
      }
      let const closes_range = has_closer && parenthesis_depth == 0 &&
                               should_stop_at_closing_parenthesis &&
                               !is_case_pattern_expected;
      if (!closes_range) do_push(operator_start, i, highlight_role::operator_);

      if (has_separator || has_opener || has_closer) {
        do_commit_pending_assignments();
      }

      if (has_opener) parenthesis_depth++;
      if (has_closer) {
        if (is_case_pattern_expected && !stack.is_empty() &&
            stack.back() == highlight_construct::case_)
        {
          is_case_pattern_expected = false;
          is_command_position = true;
        } else if (parenthesis_depth > 0)
          parenthesis_depth--;
        else if (should_stop_at_closing_parenthesis)
          return i;
      }
      if (is_in_array_value &&
          parenthesis_depth <= array_value_parenthesis_depth)
      {
        is_in_array_value = false;
      }
      if (has_separator && operator_start + 1 < i &&
          line[operator_start] == ';' &&
          (line[operator_start + 1] == ';' ||
           line[operator_start + 1] == '&') &&
          !stack.is_empty() && stack.back() == highlight_construct::case_)
      {
        is_case_pattern_expected = true;
      }

      if (!is_in_array_value &&
          (has_opener || (has_separator && !has_redirect)))
      {
        is_command_position = true;
        is_time_option_pending = false;
        highlight_command_word = StringView{};
        expecting_in = false;
      }
      continue;
    }

    let const word_start = i;
    let word_spans = ArrayList<highlight_span>{bump_allocator(HIGHLIGHT_ARENA)};
    let word_has_shell_syntax = false;
    while (i < end && !position_is_highlight_word_break(line, i, end)) {
      let const d = line[i];
      if (d == '\'') {
        let const string_start = i;
        i++;
        while (i < end && line[i] != '\'')
          i++;
        if (i < end) i++;
        word_spans.push(
            highlight_span{string_start, i, highlight_role::string});
      } else if (d == '"') {
        /* literal_start tracks the current yellow run, which resumes after
           every expansion. */
        i++;
        let literal_start = i - 1;
        while (i < end && line[i] != '"') {
          if (line[i] == '\\' && i + 1 < end) {
            i += 2;
            continue;
          }
          if (line[i] == '$') {
            if (i > literal_start)
              word_spans.push(
                  highlight_span{literal_start, i, highlight_role::string});
            i = color_dollar(line, i, end, word_spans, context,
                             line_variable_names, known_function_names);
            literal_start = i;
            continue;
          }
          if (line[i] == '`') {
            if (i > literal_start)
              word_spans.push(
                  highlight_span{literal_start, i, highlight_role::string});
            i = do_color_backtick(i, word_spans);
            literal_start = i;
            continue;
          }
          i++;
        }
        if (i < end) i++;
        if (i > literal_start)
          word_spans.push(
              highlight_span{literal_start, i, highlight_role::string});
      } else if (d == '`') {
        i = do_color_backtick(i, word_spans);
      } else if (d == '$' && i + 1 < end && line[i + 1] == '\'') {
        let const string_start = i;
        i += 2;
        while (i < end && line[i] != '\'') {
          if (line[i] == '\\' && i + 1 < end) {
            i += 2;
            continue;
          }
          i++;
        }
        if (i < end) i++;
        word_spans.push(
            highlight_span{string_start, i, highlight_role::string});
      } else if (d == '$') {
        i = color_dollar(line, i, end, word_spans, context, line_variable_names,
                         known_function_names);
      } else if (d == '\\' && i + 1 < end) {
        word_has_shell_syntax = true;
        i += 2;
      } else {
        i++;
      }
    }
    let const word_end = i;
    let const word =
        line.substring_of_length(word_start, word_end - word_start);
    let const plain = word_spans.is_empty() && !word_has_shell_syntax;
    let const is_assignment = lexer::word_looks_like_assignment(word);
    let const is_word_terminated =
        word_is_terminated_by_separator(line, word_end, end);
    let const do_color_mixed_path = [&](bool should_only_include_directories)
                                        throws -> bool {
      let string_coverage_end = word_start;
      for (let const &inner : word_spans) {
        if (inner.role != highlight_role::string ||
            inner.start > string_coverage_end)
          break;
        if (inner.end > string_coverage_end) string_coverage_end = inner.end;
      }
      if (string_coverage_end == word_end) {
        return false;
      }

      let const decoded =
          utils::decode_shell_word(word, bump_allocator(HIGHLIGHT_ARENA), true);
      if (word_spans.is_empty() && word_has_erased_directory_separator(word) &&
          !os::has_directory_separator(decoded.text.view()))
      {
        return false;
      }
      let path_spans =
          ArrayList<highlight_span>{bump_allocator(HIGHLIGHT_ARENA)};
      if (decoded.glob_active.any()) {
        let mapped_start = word_start;
        for (let const &inner : word_spans) {
          if (inner.end <= mapped_start) continue;
          if (mapped_start < inner.start)
            path_spans.push(highlight_span{mapped_start, inner.start,
                                           highlight_role::glob});
          if (inner.end > mapped_start) mapped_start = inner.end;
        }
        if (mapped_start < word_end)
          path_spans.push(
              highlight_span{mapped_start, word_end, highlight_role::glob});
      } else {
        let has_only_leading_tilde_range =
            decoded.is_leading_tilde_active &&
            decoded.opaque_ranges.count() == 1 &&
            decoded.opaque_ranges[0].decoded_start == 0 &&
            decoded.opaque_ranges[0].decoded_length == 1 &&
            decoded.opaque_ranges[0].raw_start == 0 &&
            decoded.opaque_ranges[0].raw_length == 1;
        if ((!decoded.opaque_ranges.is_empty() &&
             !has_only_leading_tilde_range) ||
            !os::has_directory_separator(decoded.text.view()))
        {
          return false;
        }

        let decoded_path_spans =
            ArrayList<highlight_span>{bump_allocator(HIGHLIGHT_ARENA)};
        if (!color_path_argument(0, decoded.text.view(), is_word_terminated,
                                 should_only_include_directories,
                                 decoded.is_leading_tilde_active,
                                 decoded_path_spans))
          return false;

        for (let const &decoded_span : decoded_path_spans) {
          ASSERT(decoded_span.end < decoded.raw_positions.count());
          let mapped_start =
              word_start + decoded.raw_positions[decoded_span.start];
          let const mapped_end =
              word_start + decoded.raw_positions[decoded_span.end];

          for (let const &inner : word_spans) {
            if (inner.end <= mapped_start) continue;
            if (inner.start >= mapped_end) break;
            if (mapped_start < inner.start)
              path_spans.push(
                  highlight_span{mapped_start, inner.start, decoded_span.role});
            if (inner.end > mapped_start) mapped_start = inner.end;
            if (mapped_start >= mapped_end) break;
          }
          if (mapped_start < mapped_end)
            path_spans.push(
                highlight_span{mapped_start, mapped_end, decoded_span.role});
        }
      }

      usize path_span_index = 0;
      usize word_span_index = 0;
      while (path_span_index < path_spans.count() ||
             word_span_index < word_spans.count())
      {
        if (word_span_index >= word_spans.count() ||
            (path_span_index < path_spans.count() &&
             path_spans[path_span_index].start <
                 word_spans[word_span_index].start))
        {
          spans.push(path_spans[path_span_index++]);
        } else {
          spans.push(word_spans[word_span_index++]);
        }
      }

      return true;
    };
    let const do_names_known_function = [&](StringView command) throws -> bool {
      return line_functions.contains(command) ||
             (known_function_names != nullptr &&
              known_function_names->contains(command));
    };
    let const do_command_role = [&](StringView command)
                                    throws -> highlight_role {
      let const is_command_resolved = first_word_resolves(command, context);
      let const command_has_prefix =
          !is_command_resolved && !is_word_terminated
              ? command_word_prefixes_any(command, context)
              : false;
      if (is_command_resolved || do_names_known_function(command)) {
        return highlight_role::resolved_command;
      }
      if (command_has_prefix) return highlight_role::partial_command;
      return highlight_role::unknown_command;
    };

    if (is_command_position && is_time_option_pending) {
      if (word == "-p" || word == "--posix" || word == "-R" || word == "--help")
      {
        do_push(word_start, word_end, highlight_role::flag);
        continue;
      }
      is_time_option_pending = false;
    }

    if (!is_command_position && word.length > 1 && word[0] == '-') {
      do_push(word_start, word_end, highlight_role::flag);
      if (highlight_command_word == "koshkit")
        highlight_command_word = StringView{};
      continue;
    }

    if (!is_command_position && highlight_command_word == "koshkit") {
      highlight_command_word = StringView{};
      if (plain && !is_assignment) {
        do_push(word_start, word_end,
                koshkit::find_util(word).has_value()
                    ? highlight_role::resolved_command
                    : highlight_role::unknown_command);
        continue;
      }
    }

    if (is_assignment && is_command_position) {
      let const assigned_name = variable_name_operand_of(word);

      if (!assigned_name.is_empty()) {
        pending_assignment_names.push(assigned_name);
        do_push(word_start, word_start + assigned_name.length,
                highlight_role::assignment_name);
      }
    }

    /* The words inside `name=(...)` are element values, so the list is left out
       of command position until it closes. A declare or local operand carries
       the same form outside command position. */
    if (is_assignment && word[word.length - 1] == '=' && word_end < end &&
        line[word_end] == '(')
    {
      is_in_array_value = true;
      array_value_parenthesis_depth = parenthesis_depth;
      is_command_position = false;
    }

    if (!is_command_position) {
      let const operand_role =
          NAME_OPERAND_COMMANDS.find(highlight_command_word);

      if (operand_role.has_value() &&
          *operand_role == name_operand_role::names_command)
      {
        let const name = command_name_operand_of(word);

        if (!name.is_empty()) {
          do_push(word_start, word_start + name.length,
                  highlight_role::function_name);
          line_functions.add(name);
          if (name.length == word.length) continue;
        }
      } else if (operand_role.has_value() && plain) {
        let const name = variable_name_operand_of(word);

        if (!name.is_empty()) {
          if (*operand_role == name_operand_role::binds_variable) {
            do_push(word_start, word_start + name.length,
                    highlight_role::assignment_name);
            line_variable_names.add(name);
          } else {
            do_push(word_start, word_start + name.length,
                    dollar_name_is_set(name, line_variable_names, context)
                        ? highlight_role::variable
                        : highlight_role::unset_variable);
          }

          continue;
        }
      }
    }

    if (plain && word == "]]" && !stack.is_empty() &&
        stack.back() == highlight_construct::conditional)
    {
      do_push(word_start, word_end, highlight_role::keyword);
      stack.pop_back();
      is_command_position = false;
      continue;
    }

    if (expecting_in && plain && word == "in") {
      do_push(word_start, word_end, highlight_role::keyword);
      expecting_in = false;
      for_variable_pending = false;
      is_command_position = false;
      /* A case takes patterns, so this only arms for a for. */
      if (!stack.is_empty() && stack.back() == highlight_construct::for_) {
        for_do_expected = true;
      }
      if (!stack.is_empty() && stack.back() == highlight_construct::case_) {
        is_case_pattern_expected = true;
      }
      continue;
    }

    /* The word right after for is the loop variable, which must be a plain
       identifier, the way the parser rejects for $f. */
    if (for_variable_pending) {
      for_variable_pending = false;
      is_command_position = false;
      if (!plain || !lexer::word_is_variable_name(word)) {
        do_push(word_start, word_end, highlight_role::invalid_syntax);
      } else {
        do_push(word_start, word_end, highlight_role::variable);
        line_variable_names.add(word);
      }
      continue;
    }

    if (function_name_pending) {
      function_name_pending = false;
      is_command_position = false;
      if (plain && word_is_function_name(word)) {
        do_push(word_start, word_end, highlight_role::function_name);
        line_functions.add(word);
      } else {
        do_push(word_start, word_end, highlight_role::invalid_syntax);
      }
      continue;
    }

    /* A word other than do once the for word list ends is misplaced, shown
       red. */
    if (for_do_expected && is_command_position) {
      for_do_expected = false;
      if (word != "do") {
        do_push(word_start, word_end, highlight_role::invalid_syntax);
        is_command_position = false;
        continue;
      }
    }

    if (is_command_position && !plain && !is_assignment &&
        do_color_mixed_path(false))
    {
      pending_assignment_names.clear();
      is_command_position = false;
      continue;
    }

    if (is_command_position && word_spans.is_empty() && word_has_shell_syntax &&
        !is_assignment)
    {
      decoded_command_word =
          utils::decode_shell_word(word, bump_allocator(HIGHLIGHT_ARENA));
      highlight_command_word = decoded_command_word.text.view();
      pending_assignment_names.clear();
      do_push(word_start, word_end,
              do_command_role(decoded_command_word.text.view()));
      is_command_position = false;
      continue;
    }

    if (is_command_position && plain && !is_assignment) {
      pending_assignment_names.clear();

      let is_keyword = true;
      let keyword_ok = true;
      let is_next_command = true;
      let should_expect_in = false;
      let should_expect_for_variable = false;
      if (Maybe<keyword_spec> spec = HIGHLIGHT_KEYWORDS.find(word);
          spec.has_value() &&
          !(spec.value().is_non_posix_only && context.is_posix_mode()))
      {
        let const &keyword = spec.value();
        switch (keyword.role) {
        case keyword_role::open:
          stack.push(keyword.construct);
          is_next_command = keyword.is_next_command;
          should_expect_in = keyword.should_expect_in;
          should_expect_for_variable = keyword.should_expect_for_variable;
          function_name_pending = keyword.should_set_function_pending;
          break;

        case keyword_role::check:
          keyword_ok =
              !stack.is_empty() &&
              (stack.back() == keyword.construct ||
               (keyword.has_alt && stack.back() == keyword.construct_alt));
          break;

        case keyword_role::close:
          keyword_ok =
              !stack.is_empty() &&
              (stack.back() == keyword.construct ||
               (keyword.has_alt && stack.back() == keyword.construct_alt));
          if (keyword_ok) stack.pop_back();
          break;

        case keyword_role::plain: break;

        case keyword_role::misplaced_in:
          keyword_ok = false;
          is_next_command = false;
          break;
        }
      } else {
        is_keyword = false;
      }

      if (is_keyword) {
        do_push(word_start, word_end,
                keyword_ok ? highlight_role::keyword
                           : highlight_role::invalid_syntax);
        is_command_position = is_next_command;
        is_time_option_pending = word == "time";
        if (should_expect_in) expecting_in = true;
        if (should_expect_for_variable) for_variable_pending = true;
        continue;
      }

      highlight_command_word = word;
      if (word == "~" && !is_word_terminated) {
        do_push(word_start, word_end, highlight_role::partial_path);
      } else if (word_defines_function(line, word_end, end)) {
        do_push(word_start, word_end, highlight_role::function_name);
        line_functions.add(word);
      } else if (!do_names_known_function(word) &&
                 os::has_directory_separator(word) &&
                 !word_has_erased_directory_separator(word))
      {
        color_path_argument(word_start, word, is_word_terminated, false,
                            word[0] == '~', spans);
      } else {
        do_push(word_start, word_end, do_command_role(word));
      }
      is_command_position = false;
      continue;
    }

    if (!is_command_position && plain && !is_assignment) {
      if (word_contains_url_scheme(word) ||
          word_looks_like_ssh_remote_path(word))
      {
        do_push(word_start, word_end, highlight_role::url);
      } else if (token_has_glob_metacharacter(word)) {
        /* The word is plain here so the metacharacter is live. */
        do_push(word_start, word_end, highlight_role::glob);
      } else {
        if (!word_has_erased_directory_separator(word))
          color_path_argument(word_start, word, is_word_terminated,
                              highlight_command_word == "cd", word[0] == '~',
                              spans);
      }
    }
    if (!is_command_position && !plain && !is_assignment &&
        do_color_mixed_path(highlight_command_word == "cd"))
      continue;
    for (let const &inner : word_spans)
      do_push(inner.start, inner.end, inner.role);
    if (is_command_position && !is_assignment) {
      pending_assignment_names.clear();
      is_command_position = false;
    }
  }

  do_commit_pending_assignments();
  return i;
}

} /* namespace completion */

} /* namespace koshka */
