#include "Arena.hpp"
#include "Builtin.hpp"
#include "Colors.hpp"
#include "Completion.hpp"
#include "CompletionInternal.hpp"
#include "CompletionPolicy.hpp"
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

static pure fn is_word_separator(char c) wontthrow -> bool
{
  return lexer::is_whitespace(c) || c == '\n';
}

static pure fn is_command_separator(char c) wontthrow -> bool
{
  return c == ';' || c == '|' || c == '&' || c == '(' || c == '\n';
}

static pure fn is_unmatched_closing_paren(StringView line,
                                          usize position) wontthrow -> bool
{
  usize depth = 0;
  for (usize k = 0; k < position; k++) {
    if (line[k] == '(') {
      depth++;
    } else if (line[k] == ')' && depth > 0) {
      depth--;
    }
  }
  return depth == 0;
}

pure fn quoted_run_end(StringView line, usize position) wontthrow -> usize
{
  let const opener = line[position];
  if (opener == '\\')
    return position + 1 < line.length ? position + 1 : position;
  if (opener != '\'' && opener != '"') return position;

  usize k = position + 1;
  while (k < line.length && line[k] != opener) {
    if (opener == '"' && line[k] == '\\' && k + 1 < line.length) k++;
    k++;
  }
  return k < line.length ? k : line.length - 1;
}

pure fn token_has_glob_metacharacter(StringView token) wontthrow -> bool
{
  for (usize i = 0; i < token.length; i++) {
    let const c = token[i];
    if (c == '*' || c == '?' || c == '[') return true;
  }
  return false;
}

static pure fn is_token_boundary(char c) wontthrow -> bool
{
  return is_word_separator(c) || is_command_separator(c);
}

pure fn is_active_token_boundary(StringView line, usize position) wontthrow
    -> bool
{
  let const c = line[position];
  if (!is_token_boundary(c)) return false;

  if ((c == '(' || c == ')') && position > 0 &&
      !is_token_boundary(line[position - 1]))
  {
    return false;
  }

  return true;
}

/* A forward scan honors single and double quotes and a backslash escape. A
   quoted or escaped separator stays part of the word. A paren glued to the
   preceding byte is literal. A name like burner (3).log completes without
   opening a subshell. */
pure fn find_token_bounds(StringView line, usize cursor) wontthrow
    -> token_bounds
{
  usize start = 0;
  usize i = 0;
  while (i < cursor && i < line.length) {
    let const c = line[i];
    if (c == '\\' || c == '\'' || c == '"') {
      i = quoted_run_end(line, i) + 1;
      continue;
    }

    if (is_active_token_boundary(line, i)) start = i + 1;
    i++;
  }

  usize end = cursor;
  while (end < line.length) {
    let const c = line[end];
    if (c == '\\' || c == '\'' || c == '"') {
      end = quoted_run_end(line, end) + 1;
      continue;
    }

    if (is_active_token_boundary(line, end)) break;
    end++;
  }

  return token_bounds{start, end};
}

static pure fn is_transparent_command_prefix(StringView word) wontthrow -> bool
{
  if (word.is_empty()) return false;
  if (word[0] == '-') return true;
  if (lexer::is_variable_name_start(word[0]) &&
      word.find_character('=').has_value())
  {
    return true;
  }
  return TRANSPARENT_PREFIXES.contains(word);
}

static pure fn next_completion_prefix_word(StringView line,
                                           usize &position) wontthrow
    -> Maybe<StringView>
{
  position = skip_blanks(line, position);
  if (position >= line.length) return None;

  let const start = position;
  while (position < line.length && line[position] != ' ' &&
         line[position] != '\t')
  {
    if (line[position] == '\'' || line[position] == '"' ||
        line[position] == '\\')
    {
      position = quoted_run_end(line, position) + 1;
    } else {
      position++;
    }
  }

  return line.substring_of_length(start, position - start);
}

static fn timeout_flag_takes_value(char short_name,
                                   StringView long_name) wontthrow -> bool
{
  let const flags =
      koshkit::koshkit_util_flag_list(koshkit::Utility::Kind::Timeout);
  if (flags == nullptr) return false;

  for (let const flag : *flags) {
    if (short_name != '\0' && flag->short_name() != short_name) continue;
    if (!long_name.is_empty() && flag->long_name() != long_name) continue;
    return flag->kind() == Flag::Kind::String ||
           flag->kind() == Flag::Kind::ManyStrings;
  }
  return false;
}

static fn timeout_option_takes_next_word(StringView word) wontthrow -> bool
{
  if (word.length < 2 || word[0] != '-') return false;

  if (word[1] == '-') {
    let const separator = word.find_character('=');
    let const name_length =
        separator.has_value() ? *separator - 2 : word.length - 2;
    let const name = word.substring_of_length(2, name_length);
    return !separator.has_value() && timeout_flag_takes_value('\0', name);
  }

  for (usize flag_index = 1; flag_index < word.length; flag_index++) {
    if (timeout_flag_takes_value(word[flag_index], {}))
      return flag_index + 1 == word.length;
  }
  return false;
}

static pure fn timeout_managed_command_start(StringView line,
                                             usize position) wontthrow
    -> Maybe<usize>
{
  let should_skip_value = false;
  loop
  {
    let const word = next_completion_prefix_word(line, position);
    if (!word.has_value()) return None;
    let const decoded_word =
        utils::decode_shell_word(*word, completion_allocator());

    if (should_skip_value) {
      should_skip_value = false;
      continue;
    }

    if (decoded_word.text == "-") return None;

    if (decoded_word.text == "--") {
      let const duration = next_completion_prefix_word(line, position);
      if (!duration.has_value()) return None;
      return skip_blanks(line, position);
    }

    if (decoded_word.text.length() > 1 && decoded_word.text[0] == '-') {
      should_skip_value =
          timeout_option_takes_next_word(decoded_word.text.view());
      continue;
    }

    return skip_blanks(line, position);
  }
}

static pure fn timeout_command_start(StringView line) wontthrow -> Maybe<usize>
{
  usize position = 0;
  loop
  {
    let const word = next_completion_prefix_word(line, position);
    if (!word.has_value()) return None;
    let const decoded_word =
        utils::decode_shell_word(*word, completion_allocator());

    if (decoded_word.text == "timeout")
      return timeout_managed_command_start(line, position);

    if (decoded_word.text == "koshkit") {
      let const utility = next_completion_prefix_word(line, position);
      if (!utility.has_value()) return None;
      let const decoded_utility =
          utils::decode_shell_word(*utility, completion_allocator());
      if (decoded_utility.text == "timeout")
        return timeout_managed_command_start(line, position);
      return None;
    }

    if (!is_transparent_command_prefix(decoded_word.text.view())) return None;
  }
}

pure fn is_in_command_position(StringView line, usize token_start) wontthrow
    -> bool
{
  if (let const managed_start = timeout_command_start(line);
      managed_start.has_value())
  {
    return token_start == *managed_start;
  }

  let i = token_start;
  loop
  {
    while (i > 0 && is_word_separator(line[i - 1]))
      i--;
    if (i == 0) return true;
    if (is_command_separator(line[i - 1])) return true;
    if (line[i - 1] == ')' && is_unmatched_closing_paren(line, i - 1)) {
      return true;
    }

    let word_start = i;
    while (word_start > 0 && !is_word_separator(line[word_start - 1]) &&
           !is_command_separator(line[word_start - 1]))
      word_start--;
    if (!is_transparent_command_prefix(
            line.substring_of_length(word_start, i - word_start)))
      return false;
    i = word_start;
  }
}
fn command_word_of(StringView line) wontthrow -> StringView
{
  if (let const managed_start = timeout_command_start(line);
      managed_start.has_value())
  {
    usize position = *managed_start;
    let const command = next_completion_prefix_word(line, position);
    return command.has_value() ? *command : StringView{};
  }

  usize i = 0;
  usize open_paren_depth = 0;
  for (usize k = 0; k < line.length; k++) {
    let const c = line[k];
    if (c == '\'' || c == '"' || c == '\\') {
      k = quoted_run_end(line, k);
      continue;
    }

    if (c == '(') {
      open_paren_depth++;
    } else if (c == ')') {
      /* An unmatched paren closes a case pattern and starts the arm's body. */
      if (open_paren_depth > 0)
        open_paren_depth--;
      else
        i = k + 1;
    } else if (c == ';' || c == '|' || c == '&') {
      i = k + 1;
    }
  }
  loop
  {
    i = skip_blanks(line, i);
    let const start = i;
    while (i < line.length && line[i] != ' ' && line[i] != '\t')
      i++;
    let const word = line.substring_of_length(start, i - start);
    if (word.is_empty() || !is_transparent_command_prefix(word)) return word;
  }
}

pure fn command_segment_start(StringView line, usize cursor) wontthrow -> usize
{
  usize start = 0;
  usize open_paren_depth = 0;
  let const limit = cursor < line.length ? cursor : line.length;
  for (usize k = 0; k < limit; k++) {
    let const c = line[k];
    if (c == '\'' || c == '"' || c == '\\') {
      k = quoted_run_end(line, k);
      continue;
    }

    if (c == '(') {
      open_paren_depth++;
    } else if (c == ')') {
      if (open_paren_depth > 0)
        open_paren_depth--;
      else
        start = k + 1;
    } else if (c == ';' || c == '|' || c == '&' || c == '\n') {
      start = k + 1;
    }
  }
  return start;
}

/* Symlinks are left alone so a name that dispatches on its argv[0], such as a
   busybox or rustup link, keeps the surface name the user typed. */
fn resolve_completion_alias(StringView command, EvalContext &context) throws
    -> String
{
  let name = String{command};
  for (usize depth = 0; depth < 8; depth++) {
    let const expansion = context.get_alias(name.view());
    if (!expansion.has_value()) break;
    let const expanded = expansion->view();
    usize i = 0;
    i = skip_blanks(expanded, i);
    let const start = i;
    while (i < expanded.length && expanded[i] != ' ' && expanded[i] != '\t')
      i++;
    let const first = expanded.substring_of_length(start, i - start);
    if (first.is_empty() || first == name.view()) break;
    name = String{first};
  }
  return name;
}

fn resolve_completion_command(StringView command, EvalContext &context) throws
    -> String
{
  let name = resolve_completion_alias(command, context);
  let const located = context.get_program_resolver().search(
      name.view(), ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (!located.is_empty()) {
    if (let const canonical = os::canonical_path(located.front());
        canonical.has_value())
    {
      let resolved_name = String{canonical->filename()};
      let const name_info = os::normalize_program_name(resolved_name);
      return String{
          resolved_name.substring_of_length(0, name_info.stem_length)};
    }
  }
  return name;
}

fn split_completion_words(StringView line, usize cursor, usize &cword) throws
    -> ArrayList<String>
{
  let words = ArrayList<String>{completion_allocator()};
  usize i = 0;
  let is_found = false;
  while (i < line.length) {
    i = skip_blanks(line, i);
    if (i >= line.length) break;
    let const start = i;
    while (i < line.length && line[i] != ' ' && line[i] != '\t')
      i++;
    if (cursor >= start && cursor <= i) {
      cword = words.count();
      is_found = true;
    }
    words.push(String{completion_allocator(),
                      line.substring_of_length(start, i - start)});
  }
  if (!is_found) {
    cword = words.count();
    words.push(String{completion_allocator()});
  }
  return words;
}

} /* namespace completion */

} /* namespace koshka */
