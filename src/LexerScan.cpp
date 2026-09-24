/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file scans balanced shell regions without producing tokens. Lexing,
 * formatting, and highlighting use the separate scanner to skip nested
 * substitutions and heredoc bodies without running the full parser.
 */

#include "Errors.hpp"
#include "Lexer.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace lexer {

struct balanced_scan_heredoc
{
  String delimiter;
  bool should_strip_tabs;
};

enum struct balanced_scan_keyword : u8
{
  case_word,
  in_word,
  esac_word,
  command_word,
};

/* A command word keeps the next word at command position, so a case header is
   still recognized after it. */
constexpr static_string_entry<balanced_scan_keyword>
    BALANCED_SCAN_KEYWORD_ENTRIES[] = {
        {SSK("case"),   balanced_scan_keyword::case_word   },
        {SSK("coproc"), balanced_scan_keyword::command_word},
        {SSK("do"),     balanced_scan_keyword::command_word},
        {SSK("elif"),   balanced_scan_keyword::command_word},
        {SSK("else"),   balanced_scan_keyword::command_word},
        {SSK("esac"),   balanced_scan_keyword::esac_word   },
        {SSK("if"),     balanced_scan_keyword::command_word},
        {SSK("in"),     balanced_scan_keyword::in_word     },
        {SSK("then"),   balanced_scan_keyword::command_word},
        {SSK("time"),   balanced_scan_keyword::command_word},
        {SSK("until"),  balanced_scan_keyword::command_word},
        {SSK("while"),  balanced_scan_keyword::command_word},
};

constexpr StaticStringMap BALANCED_SCAN_KEYWORDS{BALANCED_SCAN_KEYWORD_ENTRIES};

fn unquote_heredoc_delimiter(StringView word, Allocator allocator) throws
    -> String
{
  let delimiter = String{allocator};
  char quote = 0;

  for (usize position = 0; position < word.length; position++) {
    let const byte = word[position];
    if (quote != 0) {
      if (byte == quote) {
        quote = 0;
      } else if (byte == '\\' && quote == '"' && position + 1 < word.length) {
        delimiter.push(word[++position]);
      } else {
        delimiter.push(byte);
      }
      continue;
    }
    if (byte == '\'' || byte == '"') {
      quote = byte;
    } else if (byte == '\\' && position + 1 < word.length) {
      delimiter.push(word[++position]);
    } else {
      delimiter.push(byte);
    }
  }

  return delimiter;
}

pure fn heredoc_line_content(StringView line) wontthrow -> StringView
{
  if (!line.is_empty() && line[line.length - 1] == '\r')
    return line.substring_of_length(0, line.length - 1);

  return line;
}

pure fn balanced_scan_delimiter_end(StringView source, usize position) wontthrow
    -> usize
{
  char quote = 0;

  while (position < source.length) {
    let const byte = source[position];
    if (quote != 0) {
      if (byte == '\\' && quote == '"' && position + 1 < source.length) {
        position += 2;
        continue;
      }
      position++;
      if (byte == quote) quote = 0;
      continue;
    }
    switch (byte) {
    case '\\': position += position + 1 < source.length ? 2 : 1; continue;
    case '\'':
    case '"':
      quote = byte;
      position++;
      continue;
    case ' ':
    case '\t':
    case '\n':
    case '|':
    case '(':
    case ')':
    case '&':
    case ';':
    case '<':
    case '>': return position;
    default: position++; continue;
    }
  }

  return position;
}

fn skip_balanced_scan_heredoc_bodies(
    StringView source, usize position,
    const ArrayList<balanced_scan_heredoc> &pending) wontthrow -> usize
{
  for (let const &heredoc : pending) {
    while (position < source.length) {
      let const line_start = position;
      while (position < source.length && source[position] != '\n')
        position++;
      let line = heredoc_line_content(
          source.substring_of_length(line_start, position - line_start));
      if (heredoc.should_strip_tabs)
        while (!line.is_empty() && line[0] == '\t')
          line = line.substring(1);
      let const is_terminator = line == heredoc.delimiter.view();
      if (position < source.length) position++;
      if (is_terminator) break;
    }
  }

  return position;
}

fn scan_balanced_shell_region(StringView source, usize position,
                              char closing_byte) throws -> Maybe<usize>
{
  let pending_heredocs = ArrayList<balanced_scan_heredoc>{heap_allocator()};
  let case_pattern_depths = ArrayList<usize>{heap_allocator()};
  usize depth = 1;
  char quote = 0;
  char previous_byte = 0;
  bool has_seen_case_keyword = false;
  bool is_case_pattern_expected = false;
  bool is_command_position = true;
  let const opening_byte = closing_byte == ')' ? '(' : '{';

  while (position < source.length) {
    let const byte = source[position++];
    if (quote != 0) {
      if (byte == '\\' && quote != '\'' && position < source.length) {
        position++;
        previous_byte = byte;
        continue;
      }
      if (byte == '$' && quote == '"' && position < source.length &&
          source[position] == '(')
      {
        let const nested =
            scan_balanced_shell_region(source, position + 1, ')');
        if (!nested.has_value()) return None;
        position = *nested;
        previous_byte = ')';
        continue;
      }
      if (byte == quote) quote = 0;
      previous_byte = byte;
      continue;
    }

    /* A case pattern ends with a parenthesis that closes no region. Keywords
       are recognized so a pattern position is known. */
    if (byte >= 'a' && byte <= 'z' &&
        (previous_byte == 0 || is_whitespace(previous_byte) ||
         is_shell_sentinel(previous_byte)))
    {
      let const word_start = position - 1;
      let const word_end = balanced_scan_delimiter_end(source, word_start);
      let const word =
          source.substring_of_length(word_start, word_end - word_start);
      let const keyword = BALANCED_SCAN_KEYWORDS.find(word);
      bool did_match_keyword = keyword.has_value();
      bool is_next_command = false;

      if (did_match_keyword) {
        switch (*keyword) {
        case balanced_scan_keyword::case_word:
          did_match_keyword = is_command_position;
          has_seen_case_keyword = did_match_keyword;
          break;

        case balanced_scan_keyword::in_word:
          did_match_keyword = has_seen_case_keyword;
          if (did_match_keyword) {
            has_seen_case_keyword = false;
            case_pattern_depths.push(depth);
            is_case_pattern_expected = true;
          }
          break;

        case balanced_scan_keyword::esac_word:
          did_match_keyword = !case_pattern_depths.is_empty() &&
                              (is_command_position || is_case_pattern_expected);
          if (did_match_keyword) {
            case_pattern_depths.pop_back();
            is_case_pattern_expected = false;
          }
          break;

        case balanced_scan_keyword::command_word:
          did_match_keyword = is_command_position;
          is_next_command = did_match_keyword;
          break;
        }
      }

      if (did_match_keyword) {
        position = word_end;
        previous_byte = source[word_end - 1];
        is_command_position = is_next_command;
        continue;
      }
    }

    switch (byte) {
    case '\\':
      if (position < source.length) {
        position++;
        previous_byte = byte;
        continue;
      }
      break;

    case '\'':
    case '"':
    case '`':
      quote = byte;
      previous_byte = byte;
      continue;

    case '#':
      if (closing_byte != '}' && (previous_byte == 0 || previous_byte == '\n' ||
                                  is_whitespace(previous_byte)))
      {
        while (position < source.length && source[position] != '\n')
          position++;
        previous_byte = '#';
        continue;
      }
      break;

    case '\n':
      if (!pending_heredocs.is_empty()) {
        position = skip_balanced_scan_heredoc_bodies(source, position,
                                                     pending_heredocs);
        pending_heredocs.clear();
      }
      previous_byte = '\n';
      is_command_position = true;
      continue;

    case '<':
      if (position < source.length && source[position] == '<' &&
          (previous_byte == 0 || previous_byte == '\n' ||
           is_whitespace(previous_byte) || is_shell_sentinel(previous_byte)))
      {
        if (position + 1 < source.length && source[position + 1] == '<') {
          position += 2;
          previous_byte = '<';
          continue;
        }

        position++;
        bool should_strip_tabs = false;
        if (position < source.length && source[position] == '-') {
          should_strip_tabs = true;
          position++;
        }

        while (position < source.length && is_whitespace(source[position]))
          position++;

        let const delimiter_start = position;
        position = balanced_scan_delimiter_end(source, position);
        let delimiter = unquote_heredoc_delimiter(
            source.substring_of_length(delimiter_start,
                                       position - delimiter_start),
            heap_allocator());
        if (!delimiter.is_empty()) {
          pending_heredocs.push(
              balanced_scan_heredoc{steal(delimiter), should_strip_tabs});
        }

        previous_byte = position > 0 ? source[position - 1] : '<';
        continue;
      }
      break;

    default: break;
    }

    if (byte == opening_byte) {
      depth++;
      is_command_position = true;
    } else if (byte == closing_byte) {
      if (is_case_pattern_expected && !case_pattern_depths.is_empty() &&
          depth == case_pattern_depths.back())
      {
        is_case_pattern_expected = false;
        is_command_position = true;
        previous_byte = byte;
        continue;
      }

      depth--;
      if (depth == 0) return position;
    }

    if (byte == ';' || byte == '&' || byte == '|') {
      is_command_position = true;

      if (byte == ';' && !case_pattern_depths.is_empty() &&
          position < source.length &&
          (source[position] == ';' || source[position] == '&'))
      {
        is_case_pattern_expected = true;
      }
    } else if (byte == '{' || byte == '}' || byte == '!') {
      is_command_position = true;
    } else if (!is_whitespace(byte) && byte != opening_byte &&
               byte != closing_byte)
    {
      is_command_position = false;
    }

    previous_byte = byte;
  }

  return None;
}

} /* namespace lexer */

} /* namespace koshka */
