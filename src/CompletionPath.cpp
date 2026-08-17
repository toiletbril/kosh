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

pure fn split_path_token(StringView token) wontthrow -> path_token
{
  let last_separator = token.length;
  for (usize i = 0; i < token.length; i++) {
    if (os::is_directory_separator(token[i])) last_separator = i;
  }
  if (last_separator == token.length) {
    return path_token{StringView{}, token};
  }
  return path_token{
      token.substring_of_length(0, last_separator + 1),
      token.substring(last_separator + 1),
  };
}

/* The tilde is excluded since it expands a home the user wants. */
static pure fn byte_needs_quoting(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '*':
  case '?':
  case '[':
  case ']':
  case '(':
  case ')':
  case '{':
  case '}':
  case '\'':
  case '"':
  case '`':
  case '$':
  case '&':
  case '|':
  case ';':
  case '<':
  case '>':
  case '\\':
  case '!':
  case '#': return true;
  default: return false;
  }
}

pure fn path_candidate_needs_quoting(StringView candidate) wontthrow -> bool
{
  for (usize i = 0; i < candidate.length; i++)
    if (byte_needs_quoting(candidate[i])) return true;

  return false;
}

static pure fn byte_needs_double_quote_escape(char byte) wontthrow -> bool
{
  return byte == '"' || byte == '\\' || byte == '$' || byte == '`';
}

fn quote_path_candidate(StringView candidate) throws -> String
{
  let quoted = String{completion_allocator()};

  let const has_single_quote = candidate.find_character('\'').has_value();
  let const has_bang = candidate.find_character('!').has_value();

  if (!has_single_quote) {
    quoted.push('\'');
    quoted += candidate;
    quoted.push('\'');
    return quoted;
  }

  if (!has_bang) {
    quoted.push('"');
    for (usize i = 0; i < candidate.length; i++) {
      let const byte = candidate[i];
      if (byte_needs_double_quote_escape(byte)) quoted.push('\\');
      quoted.push(byte);
    }
    quoted.push('"');
    return quoted;
  }

  for (usize i = 0; i < candidate.length; i++) {
    let const byte = candidate[i];
    if (byte_needs_quoting(byte)) quoted.push('\\');
    quoted.push(byte);
  }

  return quoted;
}

fn escape_path_candidate(StringView candidate) throws -> String
{
  let escaped = String{completion_allocator()};

  for (usize position = 0; position < candidate.length; position++) {
    let const byte = candidate[position];
    if (byte_needs_quoting(byte)) escaped.push('\\');
    escaped.push(byte);
  }

  return escaped;
}

static fn append_open_quote_candidate(String &candidate, StringView text,
                                      char quote_character) throws -> void
{
  for (usize position = 0; position < text.length; position++) {
    let const byte = text[position];
    if (quote_character == '\'' && byte == '\'') {
      candidate.push('\'');
      candidate.push('\\');
      candidate.push('\'');
      candidate.push('\'');
      continue;
    }
    if (quote_character == '"' && byte_needs_double_quote_escape(byte)) {
      candidate.push('\\');
    }
    candidate.push(byte);
  }
}

static fn open_quote_candidate_boundary(StringView typed, usize typed_boundary,
                                        StringView candidate) wontthrow -> usize
{
  let const is_case_sensitive = utils::token_has_uppercase(typed);
  usize typed_position = 0;
  usize candidate_position = 0;
  while (candidate_position < candidate.length &&
         typed_position < typed_boundary)
  {
    let const is_equal =
        is_case_sensitive
            ? candidate[candidate_position] == typed[typed_position]
            : utils::ascii_to_lower(candidate[candidate_position]) ==
                  utils::ascii_to_lower(typed[typed_position]);
    candidate_position++;
    if (is_equal) typed_position++;
  }
  if (typed_position == typed_boundary) return candidate_position;
  return typed_boundary < candidate.length ? typed_boundary : candidate.length;
}

static fn
append_candidate_suffix(String &candidate, StringView suffix,
                        const utils::decoded_shell_word &decoded_word) throws
    -> void
{
  if (suffix.is_empty()) return;

  if (decoded_word.is_leading_variable_active) {
    candidate += escape_path_candidate(suffix);
    return;
  }

  if (path_candidate_needs_quoting(suffix))
    candidate += quote_path_candidate(suffix);
  else
    candidate += suffix;
}

fn rebuild_shell_syntax_candidate(StringView raw_token,
                                  const utils::decoded_shell_word &decoded_word,
                                  StringView decoded_candidate) throws -> String
{
  let candidate = String{completion_allocator()};
  if (decoded_candidate.starts_with(decoded_word.text.view())) {
    let const suffix = decoded_candidate.substring(decoded_word.text.length());
    let const can_extend_closed_quote =
        decoded_word.quote_character == 0 &&
        decoded_word.last_quote_character != 0 && !raw_token.is_empty() &&
        raw_token[raw_token.length - 1] == decoded_word.last_quote_character;
    if (can_extend_closed_quote) {
      candidate.append(raw_token.substring_of_length(0, raw_token.length - 1));
      append_open_quote_candidate(candidate, suffix,
                                  decoded_word.last_quote_character);
      candidate.push(decoded_word.last_quote_character);
    } else if (decoded_word.quote_character != 0) {
      candidate.append(raw_token);
      append_open_quote_candidate(candidate, suffix,
                                  decoded_word.quote_character);
    } else {
      candidate.append(raw_token);
      append_candidate_suffix(candidate, suffix, decoded_word);
    }
    return candidate;
  }

  if (decoded_word.last_quote_character == 0) {
    if (decoded_word.is_leading_tilde_active &&
        decoded_candidate.starts_with("~"))
    {
      candidate.push('~');
      append_candidate_suffix(candidate, decoded_candidate.substring(1),
                              decoded_word);
      return candidate;
    }

    if (decoded_word.is_leading_variable_active &&
        decoded_word.leading_variable_expansion_end <= decoded_candidate.length)
    {
      candidate.append(raw_token.substring_of_length(
          0, decoded_word.leading_variable_expansion_end));
      candidate += escape_path_candidate(decoded_candidate.substring(
          decoded_word.leading_variable_expansion_end));
      return candidate;
    }

    append_candidate_suffix(candidate, decoded_candidate, decoded_word);
    return candidate;
  }

  let const candidate_boundary = open_quote_candidate_boundary(
      decoded_word.text.view(), decoded_word.last_quote_decoded_start,
      decoded_candidate);
  let const decoded_prefix = decoded_word.text.view().substring_of_length(
      0, decoded_word.last_quote_decoded_start);
  let const candidate_prefix =
      decoded_candidate.substring_of_length(0, candidate_boundary);
  if (decoded_prefix == candidate_prefix) {
    candidate.append(raw_token.substring_of_length(
        0, decoded_word.last_quote_content_start));
  } else {
    if (!candidate_prefix.is_empty()) {
      if (path_candidate_needs_quoting(candidate_prefix))
        candidate.append(quote_path_candidate(candidate_prefix).view());
      else
        candidate.append(candidate_prefix);
    }
    candidate.push(decoded_word.last_quote_character);
  }
  append_open_quote_candidate(candidate,
                              decoded_candidate.substring(candidate_boundary),
                              decoded_word.last_quote_character);
  if (decoded_word.quote_character == 0)
    candidate.push(decoded_word.last_quote_character);
  return candidate;
}

/* A leading $NAME or ${NAME} in the directory prefix is expanded to its value
   so the listing reads the real directory, while the offered candidate keeps
   the unexpanded prefix. None means no leading variable, so the caller falls
   back to the literal path. */
static fn expand_leading_variable_path(StringView directory_part,
                                       usize expansion_end,
                                       EvalContext &context) throws
    -> Maybe<String>
{
  if (directory_part.is_empty() || directory_part[0] != '$' ||
      expansion_end > directory_part.length)
  {
    return None;
  }

  let const expansion = directory_part.substring_of_length(0, expansion_end);

  usize name_start = 1;
  let const is_braced =
      name_start < expansion.length && expansion[name_start] == '{';
  if (is_braced) name_start++;

  let name_end = expansion.length;
  if (is_braced) {
    if (name_end <= name_start || expansion[name_end - 1] != '}') return None;
    name_end--;
  }

  let const name =
      expansion.substring_of_length(name_start, name_end - name_start);
  if (name.is_empty()) return None;

  let const value = context.get_variable_value(name);
  if (!value.has_value()) return None;

  let expanded = String{completion_allocator(), value->view()};
  expanded.append(directory_part.substring(expansion_end));
  return expanded;
}

fn resolve_listing_directory(StringView directory_part,
                             const Path &base_directory, EvalContext &context,
                             bool is_leading_tilde_active,
                             bool is_leading_variable_active,
                             usize leading_variable_expansion_end) throws
    -> Path
{
  if (directory_part.is_empty()) return base_directory;

  if (is_leading_tilde_active)
    if (Maybe<String> expanded =
            utils::expand_leading_tilde_path(directory_part);
        expanded.has_value())
      return Path{expanded->view()};

  if (is_leading_variable_active)
    if (Maybe<String> expanded = expand_leading_variable_path(
            directory_part, leading_variable_expansion_end, context);
        expanded.has_value())
    {
      let directory = Path{expanded->view()};
      if (directory.is_absolute()) return directory;
      let resolved_path = base_directory.clone();
      resolved_path.push_component(expanded->view());
      return resolved_path;
    }

  let directory = Path{directory_part};
  if (directory.is_absolute()) return directory;

  let resolved_path = base_directory.clone();
  resolved_path.push_component(directory_part);
  return resolved_path;
}

} /* namespace completion */

} /* namespace koshka */
