#include "Arena.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Lexer.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace lexer {

struct balanced_scan_heredoc
{
  String delimiter;
  bool should_strip_tabs;
};

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
    if (byte == '\\' && position + 1 < source.length) {
      position += 2;
      continue;
    }
    if (byte == '\'' || byte == '"') {
      quote = byte;
      position++;
      continue;
    }
    if (is_whitespace(byte) || is_shell_sentinel(byte)) break;
    position++;
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
      let line = source.substring_of_length(line_start, position - line_start);
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
  usize depth = 1;
  char quote = 0;
  char previous_byte = 0;
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
    } else if (byte == closing_byte) {
      depth--;
      if (depth == 0) return position;
    }
    previous_byte = byte;
  }

  return None;
}

} /* namespace lexer */

} /* namespace koshka */
