#include "Formatter.hpp"

#include "Debug.hpp"
#include "Errors.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "StaticStringMap.hpp"
#include "Toiletline.hpp"

namespace koshka {

namespace {

enum class format_piece_kind : u8
{
  Word,
  Operator,
  Newline,
  Comment,
  Raw,
};

struct format_piece
{
  StringView text;
  u32 source_position;
  format_piece_kind kind;
  bool is_at_line_start{false};

  format_piece(StringView text, usize source_position, format_piece_kind kind,
               bool is_at_line_start) wontthrow
      : text{text},
        source_position{static_cast<u32>(source_position)},
        kind{kind},
        is_at_line_start{is_at_line_start}
  {}
};

struct pending_heredoc
{
  String delimiter;
  bool should_strip_tabs{false};
};

enum formatter_keyword_flag : u8
{
  formatter_keyword_closes = 1,
  formatter_keyword_vertical = 1 << 1,
  formatter_keyword_indents = 1 << 2,
  formatter_keyword_prefix = 1 << 3,
  formatter_keyword_case = 1 << 4,
  formatter_keyword_elif = 1 << 5,
  formatter_keyword_esac = 1 << 6,
  formatter_keyword_conditional = 1 << 7,
};

constexpr static_string_entry<u8> FORMATTER_KEYWORD_ENTRIES[] = {
    {SSK("!"),     formatter_keyword_prefix                              },
    {SSK("[["),    formatter_keyword_conditional                         },
    {SSK("["),     formatter_keyword_conditional                         },
    {SSK("case"),  formatter_keyword_case                                },
    {SSK("do"),    formatter_keyword_vertical | formatter_keyword_indents},
    {SSK("done"),  formatter_keyword_closes | formatter_keyword_vertical },
    {SSK("elif"),  formatter_keyword_closes | formatter_keyword_elif     },
    {SSK("else"),  formatter_keyword_closes | formatter_keyword_vertical |
                      formatter_keyword_indents         },
    {SSK("esac"),  formatter_keyword_closes | formatter_keyword_vertical |
                      formatter_keyword_esac            },
    {SSK("fi"),    formatter_keyword_closes | formatter_keyword_vertical },
    {SSK("if"),    formatter_keyword_prefix                              },
    {SSK("test"),  formatter_keyword_conditional                         },
    {SSK("then"),  formatter_keyword_vertical | formatter_keyword_indents},
    {SSK("time"),  formatter_keyword_prefix                              },
    {SSK("until"), formatter_keyword_prefix                              },
    {SSK("while"), formatter_keyword_prefix                              },
};
constexpr StaticStringMap FORMATTER_KEYWORDS{FORMATTER_KEYWORD_ENTRIES};

constexpr PackedStringKey FORMATTER_COMPOUND_OPENER_KEYS[] = {
    SSK("case"),   SSK("for"),   SSK("if"),
    SSK("select"), SSK("until"), SSK("while"),
};
constexpr StaticStringSet FORMATTER_COMPOUND_OPENERS{
    FORMATTER_COMPOUND_OPENER_KEYS};

enum class format_operator : u8
{
  Other,
  OpenBrace,
  OpenParen,
  CloseBrace,
  CaseTerminator,
  Pipe,
  Continuation,
  CommandEnd,
  CloseParen,
  CloseConditional,
};

constexpr static_string_entry<format_operator> FORMAT_OPERATOR_ENTRIES[] = {
    {SSK("{"),   format_operator::OpenBrace       },
    {SSK("("),   format_operator::OpenParen       },
    {SSK("}"),   format_operator::CloseBrace      },
    {SSK(";;"),  format_operator::CaseTerminator  },
    {SSK(";&"),  format_operator::CaseTerminator  },
    {SSK(";;&"), format_operator::CaseTerminator  },
    {SSK("|"),   format_operator::Pipe            },
    {SSK("&&"),  format_operator::Continuation    },
    {SSK("||"),  format_operator::Continuation    },
    {SSK("|&"),  format_operator::Continuation    },
    {SSK(";"),   format_operator::CommandEnd      },
    {SSK("&"),   format_operator::CommandEnd      },
    {SSK(")"),   format_operator::CloseParen      },
    {SSK("]]"),  format_operator::CloseConditional},
};
constexpr StaticStringMap FORMAT_OPERATORS{FORMAT_OPERATOR_ENTRIES};

pure fn is_format_blank(char byte) wontthrow -> bool
{
  return lexer::is_whitespace(byte);
}

pure fn is_format_operator_start(char byte) wontthrow -> bool
{
  switch (byte) {
  case ';':
  case '&':
  case '|':
  case '<':
  case '>':
  case '(':
  case ')':
  case '{':
  case '}': return true;
  default: return false;
  }
}

pure fn is_format_operator_at(StringView source, usize position) wontthrow
    -> bool
{
  let const byte = source[position];
  if (byte != '{' && byte != '}') return is_format_operator_start(byte);
  let const previous_is_boundary =
      position == 0 || is_format_blank(source[position - 1]) ||
      source[position - 1] == '\n' || source[position - 1] == ';' ||
      source[position - 1] == ')';
  let const next_is_boundary =
      position + 1 == source.length || is_format_blank(source[position + 1]) ||
      source[position + 1] == '\n' || source[position + 1] == ';' ||
      source[position + 1] == '<' || source[position + 1] == '>';

  return previous_is_boundary && next_is_boundary;
}

fn scan_quoted_region(StringView source, usize &position, char quote) wontthrow
    -> void
{
  position++;

  while (position < source.length) {
    let const byte = source[position++];
    if (byte == '\\' && quote != '\'' && position < source.length) {
      position++;
      continue;
    }
    if (byte == quote) return;
  }
}

fn scan_balanced_region(StringView source, usize &position, char closing) throws
    -> void
{
  let const end = lexer::scan_balanced_shell_region(source, position, closing);
  position = end.value_or(source.length);
}

fn scan_format_word_end(StringView source, usize position) throws -> usize
{
  while (position < source.length) {
    let const byte = source[position];
    if (byte == '\\' && position + 1 < source.length) {
      position += 2;
      continue;
    }
    if (byte == '\'' || byte == '"' || byte == '`') {
      scan_quoted_region(source, position, byte);
      continue;
    }
    if (byte == '$' && position + 1 < source.length) {
      if (source[position + 1] == '(') {
        position += 2;
        scan_balanced_region(source, position, ')');
        continue;
      }
      if (source[position + 1] == '{') {
        position += 2;
        scan_balanced_region(source, position, '}');
        continue;
      }
    }
    if ((byte == '@' || byte == '!' || byte == '?' || byte == '+' ||
         byte == '*') &&
        position + 1 < source.length && source[position + 1] == '(')
    {
      position += 2;
      scan_balanced_region(source, position, ')');
      continue;
    }
    if (byte == '<' && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      position += 2;
      scan_balanced_region(source, position, ')');
      continue;
    }
    if (byte == '>' && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      position += 2;
      scan_balanced_region(source, position, ')');
      continue;
    }
    if (is_format_blank(byte) || byte == '\n' ||
        is_format_operator_at(source, position))
      break;
    position++;
  }

  return position;
}

fn scan_format_operator_end(StringView source, usize position) wontthrow
    -> usize
{
  let const next = position + 1 < source.length ? source[position + 1] : '\0';
  let const after_next =
      position + 2 < source.length ? source[position + 2] : '\0';
  usize length = 1;
  switch (source[position]) {
  case ';':
    if (next == ';')
      length = after_next == '&' ? 3 : 2;
    else if (next == '&')
      length = 2;
    break;
  case '&':
    if (next == '>')
      length = after_next == '>' ? 3 : 2;
    else if (next == '&')
      length = 2;
    break;
  case '|':
    if (next == '|' || next == '&') {
      length = 2;
    }
    break;
  case '<':
    if (next == '<')
      length = after_next == '-' || after_next == '<' ? 3 : 2;
    else if (next == '&' || next == '>') {
      length = 2;
    }
    break;
  case '>':
    if (next == '>' || next == '&' || next == '|') {
      length = 2;
    }
    break;
  default: break;
  }

  return position + length;
}

fn scan_format_pieces(StringView source) throws -> ArrayList<format_piece>
{
  let pieces = ArrayList<format_piece>{heap_allocator()};
  pieces.reserve(source.length / 6 + 1);
  let pending_heredocs = ArrayList<pending_heredoc>{heap_allocator()};
  usize position = 0;
  usize line_start = 0;
  bool has_code_on_line = false;
  bool is_expecting_heredoc_delimiter = false;
  bool should_pending_heredoc_strip_tabs = false;

  while (position < source.length) {
    let const byte = source[position];
    if (byte == '\\' && position + 1 < source.length &&
        source[position + 1] == '\n')
    {
      position += 2;
      while (position < source.length && is_format_blank(source[position]))
        position++;
      continue;
    }
    if (is_format_blank(byte)) {
      position++;
      continue;
    }
    if (byte == '\n') {
      let deferred_heredoc_keyword = Maybe<format_piece>{};
      let deferred_heredoc_comment = Maybe<format_piece>{};
      if (!pending_heredocs.is_empty()) {
        usize keyword_position = pieces.count();
        while (keyword_position > 0 &&
               pieces[keyword_position - 1].kind == format_piece_kind::Comment)
          keyword_position--;

        if (keyword_position > 1) {
          let const keyword_index = keyword_position - 1;
          let &candidate = pieces[keyword_index];
          let const &separator = pieces[keyword_index - 1];
          if (candidate.kind == format_piece_kind::Word &&
              (candidate.text == "then" || candidate.text == "do") &&
              separator.kind == format_piece_kind::Operator &&
              separator.text == ";")
          {
            deferred_heredoc_keyword = candidate;
            pieces.remove(keyword_index);
            if (keyword_index < pieces.count() &&
                pieces[keyword_index].kind == format_piece_kind::Comment)
            {
              deferred_heredoc_comment = pieces[keyword_index];
              pieces.remove(keyword_index);
            }
          }
        }
      }
      pieces.push(format_piece{
          StringView{"\n", 1},
          position, format_piece_kind::Newline,
          !has_code_on_line
      });
      position++;
      has_code_on_line = false;

      for (let const &pending : pending_heredocs) {
        let const body_start = position;
        bool did_find_terminator = false;

        while (position < source.length) {
          let line_end = position;
          while (line_end < source.length && source[line_end] != '\n')
            line_end++;
          let candidate =
              source.substring_of_length(position, line_end - position);
          if (pending.should_strip_tabs) {
            while (!candidate.is_empty() && candidate[0] == '\t')
              candidate = candidate.substring(1);
          }
          position = line_end < source.length ? line_end + 1 : line_end;
          if (candidate == pending.delimiter.view()) {
            did_find_terminator = true;
            break;
          }
        }

        let const body_end = position;
        if (body_end > body_start) {
          pieces.push(format_piece{
              source.substring_of_length(body_start, body_end - body_start),
              body_start, format_piece_kind::Raw, true});
        }
        if (!did_find_terminator) break;
      }
      pending_heredocs.clear();
      if (deferred_heredoc_keyword.has_value()) {
        pieces.push(deferred_heredoc_keyword.take());
        if (deferred_heredoc_comment.has_value())
          pieces.push(deferred_heredoc_comment.take());
        pieces.push(format_piece{
            StringView{"\n", 1},
            position, format_piece_kind::Newline, false
        });
      }
      line_start = position;
      continue;
    }
    if (byte == '#' && !has_code_on_line) {
      let end_position = position;
      while (end_position < source.length && source[end_position] != '\n')
        end_position++;
      pieces.push(format_piece{
          source.substring_of_length(line_start, end_position - line_start),
          line_start, format_piece_kind::Comment, true});
      position = end_position;
      continue;
    }
    if (byte == '#') {
      let end_position = position;
      while (end_position < source.length && source[end_position] != '\n')
        end_position++;
      pieces.push(format_piece{
          source.substring_of_length(position, end_position - position),
          position, format_piece_kind::Comment, false});
      position = end_position;
      continue;
    }
    if ((byte == '<' || byte == '>') && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      let end_position = position + 2;
      scan_balanced_region(source, end_position, ')');
      pieces.push(format_piece{
          source.substring_of_length(position, end_position - position),
          position, format_piece_kind::Word, !has_code_on_line});
      has_code_on_line = true;
      position = end_position;
      continue;
    }
    if (byte == '(' && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      let end_position = position + 1;
      scan_balanced_region(source, end_position, ')');
      pieces.push(format_piece{
          source.substring_of_length(position, end_position - position),
          position, format_piece_kind::Word, !has_code_on_line});
      has_code_on_line = true;
      position = end_position;
      continue;
    }
    if (is_format_operator_at(source, position)) {
      let const end_position = scan_format_operator_end(source, position);
      let const text =
          source.substring_of_length(position, end_position - position);
      if (text == "<<" || text == "<<-") {
        is_expecting_heredoc_delimiter = true;
        should_pending_heredoc_strip_tabs = text == "<<-";
      }
      pieces.push(format_piece{text, position, format_piece_kind::Operator,
                               !has_code_on_line});
      has_code_on_line = true;
      position = end_position;
      continue;
    }

    let end_position = scan_format_word_end(source, position);
    if (end_position == position) {
      position++;
      continue;
    }
    let const initial_word =
        source.substring_of_length(position, end_position - position);
    if (end_position < source.length && source[end_position] == '(' &&
        initial_word.find_character('=').has_value())
    {
      end_position++;
      scan_balanced_region(source, end_position, ')');
    }
    let const text =
        source.substring_of_length(position, end_position - position);
    if (is_expecting_heredoc_delimiter) {
      pending_heredocs.push(pending_heredoc{
          lexer::unquote_heredoc_delimiter(text, heap_allocator()),
          should_pending_heredoc_strip_tabs});
      is_expecting_heredoc_delimiter = false;
    }
    pieces.push(format_piece{text, position, format_piece_kind::Word,
                             !has_code_on_line});
    has_code_on_line = true;
    position = end_position;
  }

  return pieces;
}

pure fn is_redirection_operator(StringView text) wontthrow -> bool
{
  if (text.is_empty()) return false;
  switch (text[0]) {
  case '<':
  case '>': return true;
  case '&': return text.length > 1 && text[1] == '>';
  default: return false;
  }
}

pure fn piece_begins_redirection(const ArrayList<format_piece> &pieces,
                                 usize index) wontthrow -> bool
{
  if (index >= pieces.count()) return false;
  let const &piece = pieces[index];

  if (piece.kind == format_piece_kind::Operator)
    return is_redirection_operator(piece.text);
  if (piece.kind != format_piece_kind::Word) return false;
  if (!piece.text.is_all_decimal_digits()) return false;

  return index + 1 < pieces.count() &&
         pieces[index + 1].kind == format_piece_kind::Operator &&
         is_redirection_operator(pieces[index + 1].text) &&
         piece.source_position + piece.text.count() ==
             pieces[index + 1].source_position;
}

class FormatWriter
{
public:
  static constexpr usize MAX_LINE_WIDTH = 78;

  fn set_indent(usize indent) wontthrow -> void { m_indent = indent; }

  fn append_token(StringView token) throws -> void
  {
    start_line();
    let const separator_length = m_line_has_text ? 1u : 0u;
    let const token_width = toiletline::display_width(token);
    if (m_column + separator_length + token_width > MAX_LINE_WIDTH &&
        m_line_has_text)
    {
      break_with_continuation(m_indent + 2);
    }
    if (m_line_has_text) {
      m_output.push(' ');
      m_column++;
    }
    m_output.append(token);
    let last_newline = Maybe<usize>{};
    for (usize position = token.length; position > 0; position--)
      if (token[position - 1] == '\n') {
        last_newline = position - 1;
        break;
      }
    if (last_newline.has_value())
      m_column = toiletline::display_width(token.substring(*last_newline + 1));
    else
      m_column += token_width;
    m_line_has_text = true;
  }

  fn break_with_continuation(usize continuation_indent) throws -> void
  {
    if (!m_line_has_text) return;
    m_output.append(" \\\n");
    m_line_has_text = false;
    m_column = 0;
    for (usize position = 0; position < continuation_indent; position++)
      m_output.push(' ');
    m_column = continuation_indent;
  }

  fn append_attached(StringView token) throws -> void
  {
    start_line();
    m_output.append(token);
    m_column += toiletline::display_width(token);
    m_line_has_text = true;
  }

  fn append_comment(StringView comment, bool is_at_line_start) throws -> void
  {
    if (is_at_line_start) {
      finish_line();
      usize text_position = 0;
      while (text_position < comment.length &&
             (comment[text_position] == ' ' || comment[text_position] == '\t'))
        text_position++;
      let const text = comment.substring(text_position);
      start_line();
      m_output.append(text);
      m_line_has_text = !text.is_empty();
      m_column += toiletline::display_width(text);
      return;
    }
    if (m_line_has_text) m_output.push(' ');
    m_output.append(comment);
    m_column += toiletline::display_width(comment) + (m_line_has_text ? 1 : 0);
    m_line_has_text = true;
  }

  fn append_raw(StringView raw) throws -> void
  {
    finish_line();
    m_output.append(raw);
    m_line_has_text = !raw.is_empty() && raw[raw.length - 1] != '\n';
    m_column = 0;
  }

  fn finish_line() throws -> void
  {
    if (!m_output.is_empty() && m_output[m_output.count() - 1] != '\n')
      m_output.push('\n');
    m_line_has_text = false;
    m_column = 0;
  }

  fn ensure_blank_line() throws -> void
  {
    finish_line();
    if (!m_output.is_empty() &&
        (m_output.count() < 2 || m_output[m_output.count() - 2] != '\n'))
    {
      m_output.push('\n');
    }
  }

  fn take() throws -> String
  {
    while (!m_output.is_empty() && m_output[m_output.count() - 1] == '\n')
      m_output.pop_back();
    if (!m_output.is_empty()) m_output.push('\n');

    return steal(m_output);
  }

private:
  fn start_line() throws -> void
  {
    if (m_line_has_text || m_column != 0) return;
    for (usize position = 0; position < m_indent; position++)
      m_output.push(' ');
    m_column = m_indent;
  }

  String m_output{heap_allocator()};
  usize m_indent{0};
  usize m_column{0};
  bool m_line_has_text{false};
};

struct option_wrap_position
{
  usize piece_index;
  usize continuation_offset;
};

pure fn word_looks_like_option(StringView word) wontthrow -> bool
{
  return word.length > 1 && word[0] == '-';
}

struct word_layout
{
  usize display_length{0};
  bool has_hard_newline{false};
};

static fn measure_word_layout(StringView word) throws -> word_layout
{
  let measure = word_layout{};
  usize segment_start = 0;
  usize position = 0;

  while (position < word.length) {
    if (word[position] == '\\' && position + 1 < word.length &&
        word[position + 1] == '\n')
    {
      measure.display_length += toiletline::display_width(
          word.substring_of_length(segment_start, position - segment_start));
      measure.display_length++;
      position += 2;
      while (position < word.length && is_format_blank(word[position]))
        position++;
      segment_start = position;
      continue;
    }
    if (word[position] == '\n') measure.has_hard_newline = true;
    position++;
  }
  measure.display_length += toiletline::display_width(
      word.substring_of_length(segment_start, word.length - segment_start));

  return measure;
}

fn collect_option_wrap_positions(const ArrayList<format_piece> &pieces) throws
    -> ArrayList<option_wrap_position>
{
  let positions = ArrayList<option_wrap_position>{heap_allocator()};
  usize segment_start = 0;
  while (segment_start < pieces.count()) {
    usize segment_end = segment_start;
    while (segment_end < pieces.count() &&
           pieces[segment_end].kind == format_piece_kind::Word)
      segment_end++;

    let const has_operands = segment_end - segment_start >= 2;
    let const command =
        has_operands ? pieces[segment_start].text : StringView{};
    let const is_plain_command = has_operands &&
                                 !lexer::word_looks_like_assignment(command) &&
                                 !FORMATTER_KEYWORDS.find(command).has_value();

    if (is_plain_command) {
      usize line_width = 0;
      bool has_multiline_word = false;
      for (usize index = segment_start; index < segment_end; index++) {
        let const measure = measure_word_layout(pieces[index].text);
        line_width += measure.display_length;
        if (index != segment_start) line_width++;
        if (measure.has_hard_newline) has_multiline_word = true;
      }

      if (!has_multiline_word && line_width > FormatWriter::MAX_LINE_WIDTH) {
        let const continuation_offset = toiletline::display_width(command) + 1;
        for (usize index = segment_start + 1; index < segment_end; index++)
          if (word_looks_like_option(pieces[index].text))
            positions.push(option_wrap_position{index, continuation_offset});
      }
    }

    segment_start = segment_end + 1;
  }

  return positions;
}

pure fn has_prior_test_shadow(Maybe<usize> first_shadow_position,
                              usize source_position) wontthrow -> bool
{
  return first_shadow_position.has_value() &&
         *first_shadow_position < source_position;
}

fn first_test_shadow_position(const ArrayList<format_piece> &pieces) throws
    -> Maybe<usize>
{
  for (usize index = 0; index < pieces.count(); index++) {
    let const &piece = pieces[index];
    if (piece.kind != format_piece_kind::Word) continue;
    if (piece.text == "function" && index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Word &&
        pieces[index + 1].text == "test")
    {
      return piece.source_position;
    }
    if (piece.text == "alias" && index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Word &&
        pieces[index + 1].text.starts_with("test="))
    {
      return piece.source_position;
    }
    if (piece.text == "test" && index + 2 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Operator &&
        pieces[index + 1].text == "(" &&
        pieces[index + 2].kind == format_piece_kind::Operator &&
        pieces[index + 2].text == ")")
      return piece.source_position;
  }

  return None;
}

fn render_format_pieces(const ArrayList<format_piece> &pieces,
                        usize initial_indent, usize recursion_depth) throws
    -> String;

fn format_word_substitutions(StringView word, usize indent,
                             usize recursion_depth) throws -> Maybe<String>
{
  static constexpr usize MAX_FORMAT_RECURSION_DEPTH = 64;
  if (recursion_depth >= MAX_FORMAT_RECURSION_DEPTH) return None;
  bool has_formattable_substitution = false;
  for (usize position = 0; position + 1 < word.length; position++) {
    if (((word[position] == '$' || word[position] == '<' ||
          word[position] == '>') &&
         word[position + 1] == '(') ||
        (position + 2 < word.length && word[position] == '$' &&
         word[position + 1] == '{' && is_format_blank(word[position + 2])))
    {
      has_formattable_substitution = true;
      break;
    }
  }
  if (!has_formattable_substitution) return None;

  let output = String{heap_allocator()};
  usize position = 0;

  while (position < word.length) {
    bool is_substitution = false;
    bool is_function_substitution = false;
    usize opener_length = 0;
    char closing = ')';
    if (position + 1 < word.length && word[position] == '$' &&
        word[position + 1] == '(' &&
        (position + 2 >= word.length || word[position + 2] != '('))
    {
      is_substitution = true;
      opener_length = 2;
    } else if (position + 2 < word.length && word[position] == '$' &&
               word[position + 1] == '{' && is_format_blank(word[position + 2]))
    {
      is_substitution = true;
      is_function_substitution = true;
      opener_length = 2;
      closing = '}';
    } else if (position + 1 < word.length &&
               (word[position] == '<' || word[position] == '>') &&
               word[position + 1] == '(')
    {
      is_substitution = true;
      opener_length = 2;
    }
    if (!is_substitution) {
      output.push(word[position++]);
      continue;
    }

    let end = position + opener_length;
    scan_balanced_region(word, end, closing);
    if (end <= position + opener_length || end > word.length ||
        word[end - 1] != closing)
    {
      output.push(word[position++]);
      continue;
    }
    let inner_start = position + opener_length;
    let inner_end = end - 1;
    if (is_function_substitution) {
      while (inner_start < inner_end && is_format_blank(word[inner_start]))
        inner_start++;
      while (inner_end > inner_start && is_format_blank(word[inner_end - 1]))
        inner_end--;
      if (inner_end > inner_start && word[inner_end - 1] == ';') inner_end--;
    }
    let const inner =
        word.substring_of_length(inner_start, inner_end - inner_start);
    let const pieces = scan_format_pieces(inner);
    let const should_close_after_heredoc =
        !pieces.is_empty() && pieces.back().kind == format_piece_kind::Raw &&
        !pieces.back().text.is_empty() &&
        pieces.back().text[pieces.back().text.length - 1] == '\n';
    let formatted =
        render_format_pieces(pieces, indent + 2, recursion_depth + 1);
    while (!formatted.is_empty() && formatted[formatted.count() - 1] == '\n')
      formatted.pop_back();
    usize leading_indent_length = 0;
    while (leading_indent_length < formatted.count() &&
           (formatted[leading_indent_length] == ' ' ||
            formatted[leading_indent_length] == '\t'))
      leading_indent_length++;

    if (is_function_substitution)
      output.append("${ ");
    else
      output.append(word.substring_of_length(position, opener_length));
    output.append(formatted.substring(leading_indent_length));
    if (should_close_after_heredoc) output.push('\n');
    if (is_function_substitution)
      output.append("; }");
    else
      output.push(')');
    position = end;
  }

  return output;
}

fn render_format_pieces(const ArrayList<format_piece> &pieces,
                        usize initial_indent = 0,
                        usize recursion_depth = 0) throws -> String
{
  let writer = FormatWriter{};
  writer.set_indent(initial_indent);
  let const test_shadow_position = first_test_shadow_position(pieces);
  let const option_wrap_positions = collect_option_wrap_positions(pieces);
  usize option_wrap_index = 0;
  bool is_command_start = true;
  bool is_test_command = false;
  bool has_closed_test = false;
  bool is_expecting_case_in = false;
  let case_pattern_states = ArrayList<bool>{heap_allocator()};
  bool should_attach_heredoc_delimiter = false;
  bool should_attach_redirection_operand = false;
  bool should_attach_case_pattern = false;
  bool should_command_start_after_redirection = false;
  bool has_pending_declaration_separator = false;
  bool has_pending_structural_separator = false;
  bool has_completed_structural_statement = false;
  bool has_continued_declaration_statement = false;
  bool is_waiting_for_continued_statement = false;
  bool is_current_statement_declaration = false;
  bool has_classified_current_statement = false;
  usize subshell_depth = 0;
  usize conditional_depth = 0;
  usize indent = initial_indent;

  let const do_close_test = [&]() throws {
    if (is_test_command && !has_closed_test) {
      writer.append_token("]");
      has_closed_test = true;
    }
  };
  let const do_is_declaration_command = [&](usize start_index) throws -> bool {
    bool has_assignment = false;
    bool has_command = false;
    bool is_declaration_builtin = false;
    bool has_operand = false;
    bool has_reporting_option = false;
    bool is_scanning_options = true;
    bool should_skip_redirection_operand = false;

    for (usize index = start_index; index < pieces.count(); index++) {
      let const &piece = pieces[index];
      if (piece.kind == format_piece_kind::Comment) continue;
      if (piece.kind == format_piece_kind::Newline ||
          piece.kind == format_piece_kind::Raw)
      {
        break;
      }
      if (piece.kind == format_piece_kind::Operator) {
        let const operator_kind =
            FORMAT_OPERATORS.find(piece.text).value_or(format_operator::Other);
        if (operator_kind == format_operator::CommandEnd ||
            operator_kind == format_operator::Pipe ||
            operator_kind == format_operator::Continuation)
        {
          break;
        }
        if (is_redirection_operator(piece.text))
          should_skip_redirection_operand = true;
        continue;
      }
      if (should_skip_redirection_operand) {
        should_skip_redirection_operand = false;
        continue;
      }
      if (piece_begins_redirection(pieces, index)) continue;
      if (!has_command && lexer::word_looks_like_assignment(piece.text)) {
        has_assignment = true;
        continue;
      }
      if (!has_command) {
        has_command = true;
        is_declaration_builtin =
            get_analysis_command_info(piece.text)
                .is_in_group(COMMAND_GROUP_DECLARATION_BUILTIN) ||
            piece.text == "export" || piece.text == "readonly";
        if (!is_declaration_builtin) return false;
        continue;
      }
      if (is_scanning_options && piece.text == "--") {
        is_scanning_options = false;
        continue;
      }
      if (is_scanning_options && piece.text.starts_with("-")) {
        if (piece.text.find_character('f').has_value() ||
            piece.text.find_character('F').has_value() ||
            piece.text.find_character('p').has_value())
        {
          has_reporting_option = true;
        }
        continue;
      }
      is_scanning_options = false;
      has_operand = true;
    }

    return (!has_command && has_assignment) ||
           (is_declaration_builtin && has_operand && !has_reporting_option);
  };
  let const do_begin_statement = [&](bool is_declaration, bool is_structural)
                                     throws -> void {
    if (!has_pending_declaration_separator && !has_pending_structural_separator)
    {
      return;
    }

    if (!is_structural && (!is_declaration || has_pending_structural_separator))
    {
      writer.ensure_blank_line();
    }
    has_pending_declaration_separator = false;
    has_pending_structural_separator = false;
  };
  let const do_finish_command = [&]() throws {
    do_close_test();
    is_command_start = true;
    is_test_command = false;
    has_closed_test = false;
    is_current_statement_declaration = false;
    has_classified_current_statement = false;
  };

  for (usize index = 0; index < pieces.count(); index++) {
    let const &piece = pieces[index];
    let const text = piece.text;
    if (piece.kind == format_piece_kind::Raw) {
      writer.append_raw(text);
      continue;
    }
    if (piece.kind == format_piece_kind::Comment) {
      do_close_test();
      if (piece.is_at_line_start && (has_pending_declaration_separator ||
                                     has_pending_structural_separator))
      {
        bool has_following_statement = false;

        for (usize following_index = index + 1;
             following_index < pieces.count(); following_index++)
        {
          let const kind = pieces[following_index].kind;
          if (kind == format_piece_kind::Newline ||
              kind == format_piece_kind::Comment)
          {
            continue;
          }
          if (kind != format_piece_kind::Raw) has_following_statement = true;
          break;
        }

        if (has_following_statement) {
          writer.ensure_blank_line();
          has_pending_declaration_separator = false;
          has_pending_structural_separator = false;
        }
      }
      writer.append_comment(text, piece.is_at_line_start);
      if (piece.source_position == 0 && text.starts_with("#!"))
        writer.ensure_blank_line();
      continue;
    }
    if (piece.kind == format_piece_kind::Newline) {
      if (!is_waiting_for_continued_statement) {
        if (is_current_statement_declaration ||
            has_continued_declaration_statement)
        {
          has_pending_declaration_separator = true;
        }
        if (has_completed_structural_statement)
          has_pending_structural_separator = true;
        has_completed_structural_statement = false;
        has_continued_declaration_statement = false;
      }
      do_finish_command();
      writer.finish_line();
      continue;
    }

    let const next_operator =
        index + 1 < pieces.count() &&
                pieces[index + 1].kind == format_piece_kind::Operator
            ? FORMAT_OPERATORS.find(pieces[index + 1].text)
                  .value_or(format_operator::Other)
            : format_operator::Other;
    let const is_followed_by_continuation =
        next_operator == format_operator::Continuation ||
        next_operator == format_operator::Pipe;
    let const is_followed_by_inline_comment =
        index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Comment &&
        !pieces[index + 1].is_at_line_start;
    let const is_followed_by_redirection =
        piece_begins_redirection(pieces, index + 1);

    if (piece.kind == format_piece_kind::Word) {
      if (option_wrap_index < option_wrap_positions.count() &&
          option_wrap_positions[option_wrap_index].piece_index == index)
      {
        writer.break_with_continuation(
            indent +
            option_wrap_positions[option_wrap_index].continuation_offset);
        option_wrap_index++;
      }
      if (should_attach_heredoc_delimiter || should_attach_redirection_operand)
      {
        let const is_separated_process_substitution_operand =
            should_attach_redirection_operand && index > 0 &&
            pieces[index - 1].source_position + pieces[index - 1].text.count() <
                piece.source_position &&
            (text.starts_with("<(") || text.starts_with(">("));
        if (is_separated_process_substitution_operand)
          writer.append_token(text);
        else
          writer.append_attached(text);
        should_attach_heredoc_delimiter = false;
        should_attach_redirection_operand = false;
        is_command_start = should_command_start_after_redirection;
        should_command_start_after_redirection = false;
        continue;
      }
      let formatted_word =
          format_word_substitutions(text, indent, recursion_depth);
      let const rendered_text =
          formatted_word.has_value() ? formatted_word->view() : text;
      if (is_command_start) is_waiting_for_continued_statement = false;
      if (has_completed_structural_statement && is_command_start &&
          FORMATTER_COMPOUND_OPENERS.contains(rendered_text))
      {
        has_completed_structural_statement = false;
      }
      if (rendered_text == "[[")
        conditional_depth++;
      else if (rendered_text == "]]" && conditional_depth > 0)
        conditional_depth--;
      let const is_case_pattern =
          !case_pattern_states.is_empty() && case_pattern_states.back();
      let const is_case_esac = is_case_pattern && rendered_text == "esac";
      let const is_case_body_esac = !case_pattern_states.is_empty() &&
                                    !case_pattern_states.back() &&
                                    rendered_text == "esac";
      let const is_reserved_position =
          is_command_start && (!is_case_pattern || is_case_esac);
      u8 keyword_flags = 0;
      if (is_reserved_position) {
        if (let const found = FORMATTER_KEYWORDS.find(rendered_text);
            found.has_value())
          keyword_flags = *found;
      }
      let const is_case_in = is_expecting_case_in && rendered_text == "in";
      if ((keyword_flags & formatter_keyword_closes) != 0) {
        do_finish_command();
        writer.finish_line();
        if (is_case_body_esac && indent >= 2) indent -= 2;
        if (indent >= 2) indent -= 2;
        writer.set_indent(indent);
      }
      if ((keyword_flags & formatter_keyword_vertical) != 0 || is_case_in) {
        do_begin_statement(false, true);
        if (!is_case_in) {
          do_finish_command();
          writer.finish_line();
        }
        writer.append_token(rendered_text);
        if (index + 1 < pieces.count() &&
            pieces[index + 1].kind == format_piece_kind::Comment &&
            !pieces[index + 1].is_at_line_start)
        {
          index++;
          writer.append_comment(pieces[index].text, false);
        }
        let const is_compound_terminator =
            (keyword_flags & formatter_keyword_closes) != 0 &&
            (keyword_flags & formatter_keyword_indents) == 0;
        if (is_compound_terminator) has_completed_structural_statement = true;
        if (!is_followed_by_continuation && !is_followed_by_inline_comment &&
            !(is_compound_terminator && is_followed_by_redirection))
        {
          writer.finish_line();
        }

        if ((keyword_flags & formatter_keyword_indents) != 0 || is_case_in)
          indent += 2;
        writer.set_indent(indent);
        is_expecting_case_in = false;
        if (is_case_in) case_pattern_states.push(true);
        if ((keyword_flags & formatter_keyword_esac) != 0 &&
            !case_pattern_states.is_empty())
          case_pattern_states.pop_back();
        continue;
      }
      if ((keyword_flags & formatter_keyword_elif) != 0) {
        do_begin_statement(false, true);
        writer.append_token(rendered_text);
        is_command_start = true;
        continue;
      }
      if ((keyword_flags & formatter_keyword_case) != 0)
        is_expecting_case_in = true;
      let should_rewrite_test =
          is_command_start && !is_case_pattern && rendered_text == "test" &&
          !has_prior_test_shadow(test_shadow_position, piece.source_position);
      if (should_rewrite_test && index + 1 < pieces.count() &&
          pieces[index + 1].text == "(")
        should_rewrite_test = false;
      if (is_command_start && !has_classified_current_statement) {
        is_current_statement_declaration = do_is_declaration_command(index);
        has_classified_current_statement = true;
        do_begin_statement(is_current_statement_declaration, false);
      }
      if (should_rewrite_test) {
        writer.append_token("[");
        is_test_command = true;
        is_command_start = false;
        continue;
      }
      let const is_redirection_descriptor =
          conditional_depth == 0 && index + 1 < pieces.count() &&
          pieces[index + 1].kind == format_piece_kind::Operator &&
          is_redirection_operator(pieces[index + 1].text) &&
          piece.source_position + piece.text.count() ==
              pieces[index + 1].source_position;
      if (is_test_command && !has_closed_test && is_redirection_descriptor)
        do_close_test();

      if (should_attach_case_pattern) {
        writer.append_attached(rendered_text);
        should_attach_case_pattern = false;
      } else {
        writer.append_token(rendered_text);
      }
      if ((keyword_flags & formatter_keyword_prefix) != 0)
        is_command_start = true;
      else if (!is_redirection_descriptor &&
               !lexer::word_looks_like_assignment(rendered_text))
      {
        is_command_start = false;
      }
      continue;
    }

    let const operator_kind =
        FORMAT_OPERATORS.find(text).value_or(format_operator::Other);
    switch (operator_kind) {
    case format_operator::OpenBrace:
      is_waiting_for_continued_statement = false;
      has_completed_structural_statement = false;
      do_begin_statement(false, false);
      writer.finish_line();
      writer.append_token(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      indent += 2;
      writer.set_indent(indent);
      do_finish_command();
      continue;
    case format_operator::OpenParen:
      if (conditional_depth > 0) {
        writer.append_token(text);
        continue;
      }
      if (!case_pattern_states.is_empty() && case_pattern_states.back()) {
        writer.append_token(text);
        should_attach_case_pattern = true;
        continue;
      }
      if (!is_command_start) break;
      is_waiting_for_continued_statement = false;
      has_completed_structural_statement = false;
      do_begin_statement(false, false);
      writer.append_token(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      indent += 2;
      subshell_depth++;
      writer.set_indent(indent);
      do_finish_command();
      continue;
    case format_operator::CloseBrace:
      do_begin_statement(false, true);
      do_finish_command();
      writer.finish_line();
      if (indent >= 2) indent -= 2;
      writer.set_indent(indent);
      writer.append_token(text);
      has_completed_structural_statement = true;
      if (!is_followed_by_continuation && !is_followed_by_inline_comment &&
          !is_followed_by_redirection)
      {
        writer.finish_line();
      }

      continue;
    case format_operator::CaseTerminator:
      do_begin_statement(false, true);
      do_finish_command();
      writer.finish_line();
      if (indent >= 2) indent -= 2;
      writer.set_indent(indent);
      writer.append_token(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      if (!case_pattern_states.is_empty()) case_pattern_states.back() = true;
      continue;
    case format_operator::Pipe:
      if (!case_pattern_states.is_empty() && case_pattern_states.back()) {
        writer.append_token(text);
        continue;
      }
      [[fallthrough]];
    case format_operator::Continuation:
      do_close_test();
      writer.append_token(text);
      if (is_current_statement_declaration)
        has_continued_declaration_statement = true;
      if (has_completed_structural_statement ||
          has_continued_declaration_statement)
      {
        is_waiting_for_continued_statement = true;
      }
      do_finish_command();
      continue;
    case format_operator::CommandEnd:
      do_close_test();
      if (text == "&") writer.append_token(text);
      if (is_current_statement_declaration ||
          has_continued_declaration_statement)
      {
        has_pending_declaration_separator = true;
      }
      if (has_completed_structural_statement)
        has_pending_structural_separator = true;
      has_completed_structural_statement = false;
      has_continued_declaration_statement = false;
      is_waiting_for_continued_statement = false;
      if (!is_followed_by_inline_comment) writer.finish_line();
      do_finish_command();
      continue;
    case format_operator::CloseParen: {
      if (conditional_depth > 0) {
        writer.append_token(text);
        continue;
      }
      let const is_case_pattern =
          !case_pattern_states.is_empty() && case_pattern_states.back();
      if (is_case_pattern) {
        writer.append_attached(text);
        if (!is_followed_by_inline_comment) writer.finish_line();
        indent += 2;
        writer.set_indent(indent);
        case_pattern_states.back() = false;
        should_attach_case_pattern = false;
        is_command_start = true;
      } else if (subshell_depth > 0) {
        do_begin_statement(false, true);
        do_finish_command();
        writer.finish_line();
        if (indent >= 2) indent -= 2;
        subshell_depth--;
        writer.set_indent(indent);
        writer.append_token(text);
        has_completed_structural_statement = true;
      } else {
        writer.append_attached(text);
      }
      continue;
    }
    case format_operator::CloseConditional:
      writer.append_attached(text);
      continue;
    case format_operator::Other: break;
    }

    let const is_redirection =
        conditional_depth == 0 && is_redirection_operator(text);
    if (is_test_command && !has_closed_test && is_redirection) do_close_test();

    if (is_redirection && (text == "<<" || text == "<<-"))
      should_attach_heredoc_delimiter = true;

    if (is_redirection) {
      should_command_start_after_redirection = is_command_start;
      if (index > 0 && pieces[index - 1].kind == format_piece_kind::Word &&
          pieces[index - 1].source_position + pieces[index - 1].text.count() ==
              piece.source_position)
        writer.append_attached(text);
      else
        writer.append_token(text);
      should_attach_redirection_operand = true;
      continue;
    }

    writer.append_token(text);
  }
  do_finish_command();

  return writer.take();
}

fn validate_formatted_source(StringView source, mimic_mood mood,
                             BumpArena &arena, ArrayList<String> &errors) throws
    -> bool
{
  let const mark = arena.mark();
  let const function_mark = FUNCTION_ARENA != nullptr
                                ? Maybe<BumpArena::Mark>{FUNCTION_ARENA->mark()}
                                : None;
  defer
  {
    arena.release(mark);
    if (function_mark.has_value()) FUNCTION_ARENA->release(*function_mark);
  };
  let parser = Parser{
      Lexer{source, arena, false, None, mood}
  };
  unused(parser.construct_ast(errors, nullptr));

  return errors.is_empty();
}

} /* namespace */

fn format_shell_source(StringView source, mimic_mood mood, BumpArena &arena,
                       ArrayList<String> &errors) throws -> Maybe<String>
{
  let normalized = String{heap_allocator()};
  let source_view = source;
  if (source.find_character('\r').has_value()) {
    normalized = String{source};
    normalized.normalize_crlf_line_endings();
    source_view = normalized.view();
  }

  if (!validate_formatted_source(source_view, mood, arena, errors)) return None;
  let const pieces = scan_format_pieces(source_view);
  let formatted = render_format_pieces(pieces);
  let formatted_errors = ArrayList<String>{heap_allocator()};
  if (!validate_formatted_source(formatted.view(), mood, arena,
                                 formatted_errors))
  {
    errors = steal(formatted_errors);
    return None;
  }

  return formatted;
}

fn select_nonconflicting_source_edits(
    ArrayList<const source_edit *> &&candidates) throws
    -> ArrayList<const source_edit *>
{
  candidates.sort([](const source_edit *left, const source_edit *right) {
    if (left->start_position != right->start_position)
      return left->start_position < right->start_position;
    if (left->end_position != right->end_position)
      return left->end_position < right->end_position;
    if (left->expected != right->expected)
      return left->expected < right->expected;
    return left->replacement < right->replacement;
  });

  let unique = ArrayList<const source_edit *>{heap_allocator()};
  for (let const *candidate : candidates) {
    if (!unique.is_empty()) {
      let const *previous = unique.back();
      if (previous->start_position == candidate->start_position &&
          previous->end_position == candidate->end_position &&
          previous->expected == candidate->expected &&
          previous->replacement == candidate->replacement)
        continue;
    }
    unique.push(candidate);
  }
  let nonconflicting = ArrayList<const source_edit *>{heap_allocator()};
  usize candidate_index = 0;

  let const do_starts_conflict_group =
      [](const source_edit &candidate, usize group_start_position,
         usize group_end_position, bool group_starts_with_insertion)
          wontthrow -> bool {
    if (candidate.start_position < group_end_position) return true;
    if (candidate.start_position != group_start_position) return false;
    return group_starts_with_insertion ||
           candidate.start_position == candidate.end_position;
  };

  while (candidate_index < unique.count()) {
    usize group_end_position = candidate_index + 1;
    let const group_start_position = unique[candidate_index]->start_position;
    usize overlap_end_position = unique[candidate_index]->end_position;
    let const group_starts_with_insertion =
        group_start_position == unique[candidate_index]->end_position;
    while (group_end_position < unique.count() &&
           do_starts_conflict_group(*unique[group_end_position],
                                    group_start_position, overlap_end_position,
                                    group_starts_with_insertion))
    {
      if (unique[group_end_position]->end_position > overlap_end_position)
        overlap_end_position = unique[group_end_position]->end_position;
      group_end_position++;
    }
    if (group_end_position == candidate_index + 1)
      nonconflicting.push(unique[candidate_index]);
    candidate_index = group_end_position;
  }

  return nonconflicting;
}

fn apply_source_fixes(StringView source, const ArrayList<source_fix> &fixes,
                      ArrayList<diagnostic_id> *applied_origins) throws
    -> Maybe<String>
{
  let candidates = ArrayList<const source_edit *>{heap_allocator()};
  for (let const &fix : fixes) {
    if (!fix.is_safe_for_fix_all) continue;
    for (let const &edit : fix.edits) {
      if (edit.end_position < edit.start_position ||
          edit.end_position > source.length)
        return None;
      if (source.substring_of_length(edit.start_position,
                                     edit.end_position - edit.start_position) !=
          edit.expected.view())
        return None;
      candidates.push(&edit);
    }
  }
  let const nonconflicting =
      select_nonconflicting_source_edits(steal(candidates));

  if (applied_origins != nullptr) {
    for (let const &fix : fixes) {
      if (!fix.origin.has_value()) continue;
      if (!fix.is_safe_for_fix_all) continue;

      bool was_applied = false;
      for (let const &edit : fix.edits) {
        for (let const *selected_edit : nonconflicting) {
          if (selected_edit != &edit) continue;
          was_applied = true;
          break;
        }
        if (was_applied) break;
      }

      if (was_applied) applied_origins->push(*fix.origin);
    }
  }

  let output = String{heap_allocator()};
  usize output_length = source.length;
  for (let const *selected_edit : nonconflicting) {
    let const &edit = *selected_edit;
    output_length -= edit.end_position - edit.start_position;
    output_length += edit.replacement.count();
  }
  output.reserve(output_length);
  usize source_position = 0;
  for (let const *selected_edit : nonconflicting) {
    let const &edit = *selected_edit;
    output.append(source.substring_of_length(
        source_position, edit.start_position - source_position));
    output.append(edit.replacement.view());
    source_position = edit.end_position;
  }
  output.append(source.substring(source_position));

  return output;
}

fn source_fixes_for_original_line_endings(
    StringView source, const ArrayList<source_fix> &normalized_fixes) throws
    -> ArrayList<source_fix>
{
  let original_positions = ArrayList<usize>{heap_allocator()};
  original_positions.reserve(source.count() + 1);
  original_positions.push(0);
  usize position = 0;

  while (position < source.length) {
    if (source[position] == '\r' && position + 1 < source.length &&
        source[position + 1] == '\n')
      position++;
    position++;
    original_positions.push(position);
  }

  let fixes = ArrayList<source_fix>{heap_allocator()};
  for (let const &fix : normalized_fixes) {
    let edits = ArrayList<source_edit>{heap_allocator()};
    bool is_valid = true;
    for (let const &edit : fix.edits) {
      if (edit.start_position >= original_positions.count() ||
          edit.end_position >= original_positions.count())
      {
        is_valid = false;
        break;
      }
      let const start = original_positions[edit.start_position];
      let const end = original_positions[edit.end_position];
      edits.push(source_edit{
          start, end, String{source.substring_of_length(start, end - start)},
          edit.replacement.clone()});
    }
    if (!is_valid) continue;
    fixes.push(source_fix{fix.title.clone(), steal(edits), fix.is_preferred,
                          fix.is_safe_for_fix_all, fix.origin});
  }

  return fixes;
}

static pure fn skip_source_blanks(StringView source, usize position) wontthrow
    -> usize
{
  while (position < source.length &&
         (source[position] == ' ' || source[position] == '\t'))
  {
    position++;
  }

  return position;
}

static pure fn source_word_end(StringView source, usize position) wontthrow
    -> usize
{
  while (position < source.length) {
    switch (source[position]) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case ';':
    case '&':
    case '|':
    case '(':
    case ')':
    case '<':
    case '>': return position;
    default: position++;
    }
  }

  return position;
}

static pure fn is_plain_test_literal(StringView text) wontthrow -> bool
{
  if (text.is_empty()) return false;

  for (usize position = 0; position < text.length; position++) {
    let const byte = text[position];
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9'))
    {
      continue;
    }

    switch (byte) {
    case '_':
    case '-':
    case '.':
    case '/':
    case ':':
    case ',':
    case '+':
    case '@':
    case '=': continue;
    default: return false;
    }
  }

  return true;
}

static fn test_operand_without_x_prefix(StringView operand) throws
    -> Maybe<String>
{
  if (operand.length < 2 || operand[0] == '\'') return None;

  if (operand[0] == '"' && operand[operand.length - 1] == '"' &&
      operand.length >= 3 && operand[1] == 'x')
  {
    let stripped = String{"\""};
    stripped.append(operand.substring(2));

    return stripped;
  }

  if (operand[0] == 'x' && operand[1] == '"' &&
      operand[operand.length - 1] == '"')
  {
    return String{operand.substring(1)};
  }

  if (operand[0] == 'x' && is_plain_test_literal(operand.substring(1)))
    return String{operand.substring(1)};

  return None;
}

static fn x_prefix_test_fix(StringView source,
                            const SourceLocation &location) throws
    -> Maybe<source_fix>
{
  let const left =
      source.substring_of_length(location.position, location.length);
  if (left.length < 3) return None;

  let left_replacement = test_operand_without_x_prefix(left);
  if (!left_replacement.has_value()) return None;

  let const operator_start =
      skip_source_blanks(source, location.position + location.length);
  if (operator_start == location.position + location.length) return None;

  let const operator_end = source_word_end(source, operator_start);
  let const written_operator =
      source.substring_of_length(operator_start, operator_end - operator_start);
  if (written_operator != "=" && written_operator != "!=") return None;

  let const right_start = skip_source_blanks(source, operator_end);
  if (right_start == operator_end) return None;

  let const right_end = source_word_end(source, right_start);
  let const right =
      source.substring_of_length(right_start, right_end - right_start);
  let right_replacement = test_operand_without_x_prefix(right);
  if (!right_replacement.has_value()) return None;

  let edits = ArrayList<source_edit>{heap_allocator()};
  edits.push(source_edit{location.position, location.position + location.length,
                         String{left}, left_replacement.take()});
  edits.push(source_edit{right_start, right_end, String{right},
                         right_replacement.take()});

  return source_fix{String{"Drop the 'x' prefix from both operands"},
                    steal(edits), true, true, diagnostic_id::sc2268};
}

static fn negated_test_operator_fix(diagnostic_id diagnostic, StringView source,
                                    const SourceLocation &location) throws
    -> Maybe<source_fix>
{
  let const negated = diagnostic == diagnostic_id::sc2236;
  let const written_operator = negated ? StringView{"-z"} : StringView{"-n"};
  let const replacement_operator =
      negated ? StringView{"-n"} : StringView{"-z"};

  if (source.substring_of_length(location.position, location.length) != "!")
    return None;

  let const operator_start =
      skip_source_blanks(source, location.position + location.length);
  if (operator_start == location.position + location.length) return None;

  let const operator_end = source_word_end(source, operator_start);
  if (source.substring_of_length(
          operator_start, operator_end - operator_start) != written_operator)
  {
    return None;
  }

  let edits = ArrayList<source_edit>{heap_allocator()};
  edits.push(
      source_edit{location.position, operator_start,
                  String{source.substring_of_length(
                      location.position, operator_start - location.position)},
                  String{heap_allocator()}});
  edits.push(source_edit{operator_start, operator_end, String{written_operator},
                         String{replacement_operator}});

  let title = String{"Replace the negation with `"};
  title.append(replacement_operator);
  title.push('`');

  return source_fix{steal(title), steal(edits), true, true, diagnostic};
}

fn source_fixes_for_diagnostic(diagnostic_id diagnostic, StringView source,
                               const SourceLocation &location) throws
    -> ArrayList<source_fix>
{
  let fixes = ArrayList<source_fix>{heap_allocator()};
  if (location.position + location.length > source.length) return fixes;
  let replacement = String{heap_allocator()};
  let title = StringView{};
  bool is_fixable = true;
  bool is_safe_for_fix_all = false;
  let const written =
      source.substring_of_length(location.position, location.length);

  switch (diagnostic) {
  case diagnostic_id::sc1082:
    title = "Remove the byte-order mark";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1017:
    title = "Remove the carriage return";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1018:
    title = "Replace the separator with a space";
    replacement = " ";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1100:
    title = "Replace the dash with '-'";
    replacement = "-";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1101:
    title = "Remove whitespace after the continuation";
    replacement = "\\";
    break;
  case diagnostic_id::sc1084:
    title = "Put '#' before '!'";
    replacement = "#!";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1104:
    title = "Add '#' before '!'";
    replacement = "#!";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1113:
    title = "Add '!' after '#'";
    replacement = "#!";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1114:
  case diagnostic_id::sc1115:
    title = "Remove whitespace from the shebang";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc1029: {
    if (written.length < 2 || written[0] != '\\') {
      is_fixable = false;
      break;
    }
    title = "Remove the unnecessary escape";
    replacement = written.substring(1);
    is_safe_for_fix_all = true;
    break;
  }
  case diagnostic_id::sc2108:
    title = "Replace '-a' with '&&'";
    replacement = "&&";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc2110:
    title = "Replace '-o' with '||'";
    replacement = "||";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc3014:
    title = "Replace '==' with '='";
    replacement = "=";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc2196:
    title = "Replace 'egrep' with 'grep -E'";
    replacement = "grep -E";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc2197:
    title = "Replace 'fgrep' with 'grep -F'";
    replacement = "grep -F";
    is_safe_for_fix_all = true;
    break;
  case diagnostic_id::sc2007: {
    if (written.length < 3 || !written.starts_with(StringView{"$["}) ||
        written[written.length - 1] != ']')
    {
      is_fixable = false;
      break;
    }
    let const inner = written.substring_of_length(2, written.length - 3);
    if (inner.find_character('[').has_value() ||
        inner.find_character(']').has_value())
    {
      is_fixable = false;
      break;
    }
    title = "Replace '$[...]' with '$((...))'";
    replacement = "$((";
    replacement.append(inner);
    replacement.append("))");
    is_safe_for_fix_all = true;
    break;
  }
  case diagnostic_id::sc2268: {
    let operand_fix = x_prefix_test_fix(source, location);
    if (operand_fix.has_value()) fixes.push(operand_fix.take());

    return fixes;
  }
  case diagnostic_id::sc2236:
  case diagnostic_id::sc2237: {
    let negation_fix = negated_test_operator_fix(diagnostic, source, location);
    if (negation_fix.has_value()) fixes.push(negation_fix.take());

    return fixes;
  }
  case diagnostic_id::sc2068:
    if (written != "$@") {
      is_fixable = false;
      break;
    }
    title = "Quote '$@'";
    replacement = "\"$@\"";
    break;
  case diagnostic_id::sc2071:
    if (written == ">") {
      title = "Replace '>' with '-gt'";
      replacement = "-gt";
    } else if (written == "<") {
      title = "Replace '<' with '-lt'";
      replacement = "-lt";
    } else {
      is_fixable = false;
    }
    break;
  case diagnostic_id::sc2086_expansion:
  case diagnostic_id::sc2086_test:
    if (written.is_empty()) {
      is_fixable = false;
      break;
    }
    title = "Quote the expansion";
    replacement.push('"');
    replacement.append(written);
    replacement.push('"');
    break;
  default: is_fixable = false; break;
  }
  if (!is_fixable) return fixes;

  let edits = ArrayList<source_edit>{heap_allocator()};
  let const edit_end = location.position + location.length;
  edits.push(source_edit{location.position, edit_end, String{written},
                         steal(replacement)});
  fixes.push(source_fix{String{title}, steal(edits), true, is_safe_for_fix_all,
                        diagnostic});

  return fixes;
}

} /* namespace koshka */
