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
  format_piece_kind kind;
  String text;
  usize source_position;
  bool starts_line{false};
};

struct pending_heredoc
{
  String delimiter;
  bool strips_tabs{false};
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
};

constexpr static_string_entry<u8> FORMATTER_KEYWORD_ENTRIES[] = {
    {SSK("!"),     formatter_keyword_prefix                              },
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
    {SSK("then"),  formatter_keyword_vertical | formatter_keyword_indents},
    {SSK("time"),  formatter_keyword_prefix                              },
    {SSK("until"), formatter_keyword_prefix                              },
    {SSK("while"), formatter_keyword_prefix                              },
};
constexpr StaticStringMap FORMATTER_KEYWORDS{FORMATTER_KEYWORD_ENTRIES};

constexpr PackedStringKey LINE_OPERATOR_KEYS[] = {
    SSK(";"), SSK("&&"), SSK("||"), SSK("|"), SSK("|&"), SSK("&"),
};
constexpr StaticStringSet LINE_OPERATORS{LINE_OPERATOR_KEYS};

constexpr PackedStringKey CASE_TERMINATOR_KEYS[] = {
    SSK(";;"),
    SSK(";&"),
    SSK(";;&"),
};
constexpr StaticStringSet CASE_TERMINATORS{CASE_TERMINATOR_KEYS};

pure fn is_format_blank(char byte) wontthrow -> bool
{
  return byte == ' ' || byte == '\t' || byte == '\r';
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

fn unquoted_heredoc_delimiter(StringView word) throws -> String
{
  let delimiter = String{heap_allocator()};
  bool is_single_quoted = false;
  bool is_double_quoted = false;

  for (usize position = 0; position < word.length; position++) {
    let const byte = word[position];
    if (byte == '\\' && !is_single_quoted && position + 1 < word.length) {
      delimiter.push(word[++position]);
      continue;
    }
    if (byte == '\'' && !is_double_quoted) {
      is_single_quoted = !is_single_quoted;
      continue;
    }
    if (byte == '"' && !is_single_quoted) {
      is_double_quoted = !is_double_quoted;
      continue;
    }
    delimiter.push(byte);
  }

  return delimiter;
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

fn scan_balanced_region(StringView source, usize &position,
                        char closing) wontthrow -> void
{
  usize depth = 1;

  while (position < source.length && depth > 0) {
    let const byte = source[position++];
    if (byte == '\\' && position < source.length) {
      position++;
      continue;
    }
    if (byte == '\'' || byte == '"' || byte == '`') {
      scan_quoted_region(source, position, byte);
      continue;
    }
    if (byte == closing) {
      depth--;
      continue;
    }
    if ((closing == ')' && byte == '(') || (closing == '}' && byte == '{'))
      depth++;
  }
}

fn scan_format_word_end(StringView source, usize position) wontthrow -> usize
{
  while (position < source.length) {
    let const byte = source[position];
    if (byte == '\\' && position + 1 < source.length) {
      if (source[position + 1] == '\n') break;
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
  static const StringView OPERATORS[] = {
      ";;&", "&>>", "<<<", ";;", ";&", "&&", "||", "|&", "<<-", "<<",
      ">>",  "&>",  ">&",  "<&", "<>", ">|", "((", "))", "[[",  "]]",
      "(",   ")",   "{",   "}",  ";",  "&",  "|",  "<",  ">",
  };

  for (let const candidate : OPERATORS) {
    if (position + candidate.length <= source.length &&
        source.substring_of_length(position, candidate.length) == candidate)
      return position + candidate.length;
  }

  return position + 1;
}

fn scan_format_pieces(StringView source) throws -> ArrayList<format_piece>
{
  let pieces = ArrayList<format_piece>{heap_allocator()};
  let pending_heredocs = ArrayList<pending_heredoc>{heap_allocator()};
  usize position = 0;
  usize line_start = 0;
  bool line_has_code = false;
  bool expects_heredoc_delimiter = false;
  bool pending_heredoc_strips_tabs = false;

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
            deferred_heredoc_keyword = steal(candidate);
            pieces.remove(keyword_index);
            if (keyword_index < pieces.count() &&
                pieces[keyword_index].kind == format_piece_kind::Comment)
            {
              deferred_heredoc_comment = steal(pieces[keyword_index]);
              pieces.remove(keyword_index);
            }
          }
        }
      }
      pieces.push(format_piece{format_piece_kind::Newline, String{"\n"},
                               position, !line_has_code});
      position++;
      line_start = position;
      line_has_code = false;

      for (let const &pending : pending_heredocs) {
        let const body_start = position;
        bool found_terminator = false;

        while (position < source.length) {
          let line_end = position;
          while (line_end < source.length && source[line_end] != '\n')
            line_end++;
          let candidate =
              source.substring_of_length(position, line_end - position);
          if (pending.strips_tabs) {
            while (!candidate.is_empty() && candidate[0] == '\t')
              candidate = candidate.substring(1);
          }
          position = line_end < source.length ? line_end + 1 : line_end;
          if (candidate == pending.delimiter.view()) {
            found_terminator = true;
            break;
          }
        }

        let const body_end = position;
        if (body_end > body_start) {
          pieces.push(format_piece{format_piece_kind::Raw,
                                   String{source.substring_of_length(
                                       body_start, body_end - body_start)},
                                   body_start, true});
        }
        if (!found_terminator) break;
      }
      pending_heredocs.clear();
      if (deferred_heredoc_keyword.has_value()) {
        pieces.push(deferred_heredoc_keyword.take());
        if (deferred_heredoc_comment.has_value())
          pieces.push(deferred_heredoc_comment.take());
        pieces.push(format_piece{format_piece_kind::Newline, String{"\n"},
                                 position, false});
      }
      line_start = position;
      continue;
    }
    if (byte == '#' && !line_has_code) {
      let end = position;
      while (end < source.length && source[end] != '\n')
        end++;
      pieces.push(format_piece{
          format_piece_kind::Comment,
          String{source.substring_of_length(line_start, end - line_start)},
          line_start, true});
      position = end;
      continue;
    }
    if (byte == '#') {
      let end = position;
      while (end < source.length && source[end] != '\n')
        end++;
      pieces.push(format_piece{
          format_piece_kind::Comment,
          String{source.substring_of_length(position, end - position)},
          position, false});
      position = end;
      continue;
    }
    if ((byte == '<' || byte == '>') && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      let end = position + 2;
      scan_balanced_region(source, end, ')');
      pieces.push(format_piece{
          format_piece_kind::Word,
          String{source.substring_of_length(position, end - position)},
          position, !line_has_code});
      line_has_code = true;
      position = end;
      continue;
    }
    if (byte == '(' && position + 1 < source.length &&
        source[position + 1] == '(')
    {
      let end = position + 1;
      scan_balanced_region(source, end, ')');
      pieces.push(format_piece{
          format_piece_kind::Word,
          String{source.substring_of_length(position, end - position)},
          position, !line_has_code});
      line_has_code = true;
      position = end;
      continue;
    }
    if (is_format_operator_at(source, position)) {
      let const end = scan_format_operator_end(source, position);
      let text = String{source.substring_of_length(position, end - position)};
      if (text == "<<" || text == "<<-") {
        expects_heredoc_delimiter = true;
        pending_heredoc_strips_tabs = text == "<<-";
      }
      pieces.push(format_piece{format_piece_kind::Operator, steal(text),
                               position, !line_has_code});
      line_has_code = true;
      position = end;
      continue;
    }

    let end = scan_format_word_end(source, position);
    if (end == position) {
      position++;
      continue;
    }
    let const initial_word =
        source.substring_of_length(position, end - position);
    if (end < source.length && source[end] == '(' &&
        initial_word.find_character('=').has_value())
    {
      end++;
      scan_balanced_region(source, end, ')');
    }
    let text = String{source.substring_of_length(position, end - position)};
    if (expects_heredoc_delimiter) {
      pending_heredocs.push(
          pending_heredoc{unquoted_heredoc_delimiter(text.view()),
                          pending_heredoc_strips_tabs});
      expects_heredoc_delimiter = false;
    }
    pieces.push(format_piece{format_piece_kind::Word, steal(text), position,
                             !line_has_code});
    line_has_code = true;
    position = end;
  }

  return pieces;
}

pure fn is_assignment_word(StringView word) wontthrow -> bool
{
  let const equals = word.find_character('=');
  if (!equals.has_value() || *equals == 0) return false;

  for (usize position = 0; position < *equals; position++) {
    let const byte = word[position];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          byte == '_' || (position > 0 && byte >= '0' && byte <= '9')))
      return false;
  }

  return true;
}

pure fn is_line_operator(StringView text) wontthrow -> bool
{
  return LINE_OPERATORS.contains(text);
}

pure fn is_case_terminator(StringView text) wontthrow -> bool
{
  return CASE_TERMINATORS.contains(text);
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

class format_writer
{
public:
  fn set_indent(usize indent) wontthrow -> void { m_indent = indent; }
  pure fn indent() const wontthrow -> usize { return m_indent; }

  fn append_token(StringView token) throws -> void
  {
    start_line();
    let const separator_length = m_line_has_text ? 1u : 0u;
    let const token_width = toiletline::display_width(token);
    if (m_column + separator_length + token_width > 78 && m_line_has_text) {
      m_output.append(" \\\n");
      m_line_has_text = false;
      m_column = 0;
      let const saved_indent = m_indent;
      m_indent += 2;
      start_line();
      m_indent = saved_indent;
    }
    if (m_line_has_text) {
      m_output.push(' ');
      m_column++;
    }
    m_output.append(token);
    let last_newline = Maybe<usize>{};
    for (usize position = 0; position < token.length; position++)
      if (token[position] == '\n') last_newline = position;
    if (last_newline.has_value())
      m_column = toiletline::display_width(token.substring(*last_newline + 1));
    else
      m_column += token_width;
    m_line_has_text = true;
  }

  fn append_attached(StringView token) throws -> void
  {
    start_line();
    m_output.append(token);
    m_column += toiletline::display_width(token);
    m_line_has_text = true;
  }

  fn append_comment(StringView comment, bool starts_line) throws -> void
  {
    if (starts_line) {
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
    if (m_line_has_text) return;
    for (usize position = 0; position < m_indent; position++)
      m_output.push(' ');
    m_column = m_indent;
  }

  String m_output{heap_allocator()};
  usize m_indent{0};
  usize m_column{0};
  bool m_line_has_text{false};
};

pure fn has_prior_test_shadow(const ArrayList<usize> &positions,
                              usize source_position) wontthrow -> bool
{
  return !positions.is_empty() && positions.front() < source_position;
}

fn collect_test_shadow_positions(const ArrayList<format_piece> &pieces) throws
    -> ArrayList<usize>
{
  let positions = ArrayList<usize>{heap_allocator()};
  for (usize index = 0; index < pieces.count(); index++) {
    let const &piece = pieces[index];
    if (piece.kind != format_piece_kind::Word) continue;
    if (piece.text == "function" && index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Word &&
        pieces[index + 1].text == "test")
    {
      positions.push(piece.source_position);
      continue;
    }
    if (piece.text == "alias" && index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Word &&
        pieces[index + 1].text.view().starts_with("test="))
    {
      positions.push(piece.source_position);
      continue;
    }
    if (piece.text == "test" && index + 2 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Operator &&
        pieces[index + 1].text == "(" &&
        pieces[index + 2].kind == format_piece_kind::Operator &&
        pieces[index + 2].text == ")")
      positions.push(piece.source_position);
  }
  positions.sort();

  return positions;
}

fn render_format_pieces(const ArrayList<format_piece> &pieces,
                        usize initial_indent, usize recursion_depth) throws
    -> String;

fn format_word_substitutions(StringView word, usize indent,
                             usize recursion_depth) throws -> String
{
  if (recursion_depth >= 64) return String{word};
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
    let formatted =
        render_format_pieces(pieces, indent + 2, recursion_depth + 1);
    while (!formatted.is_empty() && formatted[formatted.count() - 1] == '\n')
      formatted.pop_back();
    usize leading_indent = 0;
    while (
        leading_indent < formatted.count() &&
        (formatted[leading_indent] == ' ' || formatted[leading_indent] == '\t'))
      leading_indent++;

    if (is_function_substitution)
      output.append("${ ");
    else
      output.append(word.substring_of_length(position, opener_length));
    output.append(formatted.substring(leading_indent));
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
  let writer = format_writer{};
  writer.set_indent(initial_indent);
  let const test_shadow_positions = collect_test_shadow_positions(pieces);
  bool is_command_start = true;
  bool is_test_command = false;
  bool has_closed_test = false;
  bool expects_case_in = false;
  let case_pattern_states = ArrayList<bool>{heap_allocator()};
  bool should_attach_heredoc_delimiter = false;
  bool should_attach_redirection_operand = false;
  bool command_starts_after_redirection = false;
  usize subshell_depth = 0;
  usize conditional_depth = 0;
  usize indent = initial_indent;

  let const do_close_test = [&]() throws {
    if (is_test_command && !has_closed_test) {
      writer.append_token("]");
      has_closed_test = true;
    }
  };
  let const do_finish_command = [&]() throws {
    do_close_test();
    is_command_start = true;
    is_test_command = false;
    has_closed_test = false;
  };

  for (usize index = 0; index < pieces.count(); index++) {
    let const &piece = pieces[index];
    let const text = piece.text.view();
    let const is_followed_by_continuation =
        index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Operator &&
        (pieces[index + 1].text == "&&" || pieces[index + 1].text == "||" ||
         pieces[index + 1].text == "|" || pieces[index + 1].text == "|&");
    let const is_followed_by_inline_comment =
        index + 1 < pieces.count() &&
        pieces[index + 1].kind == format_piece_kind::Comment &&
        !pieces[index + 1].starts_line;
    if (piece.kind == format_piece_kind::Raw) {
      writer.append_raw(text);
      continue;
    }
    if (piece.kind == format_piece_kind::Comment) {
      do_close_test();
      writer.append_comment(text, piece.starts_line);
      continue;
    }
    if (piece.kind == format_piece_kind::Newline) {
      do_finish_command();
      writer.finish_line();
      continue;
    }

    if (piece.kind == format_piece_kind::Word) {
      if (should_attach_heredoc_delimiter || should_attach_redirection_operand)
      {
        writer.append_attached(text);
        should_attach_heredoc_delimiter = false;
        should_attach_redirection_operand = false;
        is_command_start = command_starts_after_redirection;
        command_starts_after_redirection = false;
        continue;
      }
      let formatted_word =
          format_word_substitutions(text, indent, recursion_depth);
      let const rendered_text = formatted_word.view();
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
      let const is_case_in = expects_case_in && rendered_text == "in";
      if ((keyword_flags & formatter_keyword_closes) != 0) {
        do_finish_command();
        writer.finish_line();
        if (is_case_body_esac && indent >= 2) indent -= 2;
        if (indent >= 2) indent -= 2;
        writer.set_indent(indent);
      }
      if ((keyword_flags & formatter_keyword_vertical) != 0 || is_case_in) {
        do_finish_command();
        writer.finish_line();
        writer.append_token(rendered_text);
        if (index + 1 < pieces.count() &&
            pieces[index + 1].kind == format_piece_kind::Comment &&
            !pieces[index + 1].starts_line)
        {
          index++;
          writer.append_comment(pieces[index].text.view(), false);
        }
        if (!is_followed_by_continuation && !is_followed_by_inline_comment)
          writer.finish_line();
        if ((keyword_flags & formatter_keyword_indents) != 0 || is_case_in)
          indent += 2;
        writer.set_indent(indent);
        expects_case_in = false;
        if (is_case_in) case_pattern_states.push(true);
        if ((keyword_flags & formatter_keyword_esac) != 0 &&
            !case_pattern_states.is_empty())
          case_pattern_states.pop_back();
        continue;
      }
      if ((keyword_flags & formatter_keyword_elif) != 0) {
        writer.append_token(rendered_text);
        is_command_start = true;
        continue;
      }
      if ((keyword_flags & formatter_keyword_case) != 0) expects_case_in = true;
      let should_rewrite_test =
          is_command_start && !is_case_pattern && rendered_text == "test" &&
          !has_prior_test_shadow(test_shadow_positions, piece.source_position);
      if (should_rewrite_test && index + 1 < pieces.count() &&
          pieces[index + 1].text == "(")
        should_rewrite_test = false;
      if (should_rewrite_test) {
        writer.append_token("[");
        is_test_command = true;
        is_command_start = false;
        continue;
      }
      let const is_redirection_descriptor =
          conditional_depth == 0 && index + 1 < pieces.count() &&
          pieces[index + 1].kind == format_piece_kind::Operator &&
          is_redirection_operator(pieces[index + 1].text.view()) &&
          piece.source_position + piece.text.count() ==
              pieces[index + 1].source_position;
      if (is_test_command && !has_closed_test && is_redirection_descriptor)
        do_close_test();

      writer.append_token(rendered_text);
      if ((keyword_flags & formatter_keyword_prefix) != 0)
        is_command_start = true;
      else if (!is_redirection_descriptor && !is_assignment_word(rendered_text))
      {
        is_command_start = false;
      }
      continue;
    }

    if (text == "{") {
      writer.finish_line();
      writer.append_token(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      indent += 2;
      writer.set_indent(indent);
      do_finish_command();
      continue;
    }
    if (text == "(" && is_command_start) {
      writer.append_token(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      indent += 2;
      subshell_depth++;
      writer.set_indent(indent);
      do_finish_command();
      continue;
    }
    if (text == "}") {
      do_finish_command();
      writer.finish_line();
      if (indent >= 2) indent -= 2;
      writer.set_indent(indent);
      writer.append_token(text);
      if (!is_followed_by_continuation && !is_followed_by_inline_comment)
        writer.finish_line();
      continue;
    }
    if (is_case_terminator(text)) {
      do_finish_command();
      writer.finish_line();
      if (indent >= 2) indent -= 2;
      writer.set_indent(indent);
      writer.append_token(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      if (!case_pattern_states.is_empty()) case_pattern_states.back() = true;
      continue;
    }
    let const is_case_pattern =
        !case_pattern_states.is_empty() && case_pattern_states.back();
    if (text == "|" && is_case_pattern) {
      writer.append_token(text);
      continue;
    }
    if (is_line_operator(text)) {
      do_close_test();
      if (text == ";" || text == "&") {
        if (text == "&") writer.append_token(text);
        if (!is_followed_by_inline_comment) writer.finish_line();
        do_finish_command();
      } else {
        writer.append_token(text);
        do_finish_command();
      }
      continue;
    }
    let const is_redirection =
        conditional_depth == 0 && is_redirection_operator(text);
    if (is_test_command && !has_closed_test && is_redirection) do_close_test();

    if (is_redirection && (text == "<<" || text == "<<-"))
      should_attach_heredoc_delimiter = true;

    if (is_redirection) {
      command_starts_after_redirection = is_command_start;
      if (index > 0 && pieces[index - 1].kind == format_piece_kind::Word &&
          pieces[index - 1].source_position + pieces[index - 1].text.count() ==
              piece.source_position)
        writer.append_attached(text);
      else
        writer.append_token(text);
      should_attach_redirection_operand = true;
      continue;
    }

    if (text == ")" && is_case_pattern) {
      writer.append_attached(text);
      if (!is_followed_by_inline_comment) writer.finish_line();
      indent += 2;
      writer.set_indent(indent);
      case_pattern_states.back() = false;
      is_command_start = true;
    } else if (text == ")" && subshell_depth > 0) {
      do_finish_command();
      writer.finish_line();
      if (indent >= 2) indent -= 2;
      subshell_depth--;
      writer.set_indent(indent);
      writer.append_token(text);
    } else if (text == ")" || text == "]]")
      writer.append_attached(text);
    else
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
  defer { arena.release(mark); };
  let parser = Parser{
      Lexer{String{source}, arena, false, None, mood}
  };
  unused(parser.construct_ast(errors, nullptr));

  return errors.is_empty();
}

} /* namespace */

fn format_shell_source(StringView source, mimic_mood mood, BumpArena &arena,
                       ArrayList<String> &errors) throws -> Maybe<String>
{
  let normalized = String{source};
  normalized.normalize_crlf_line_endings();
  if (!validate_formatted_source(normalized.view(), mood, arena, errors))
    return None;
  let const pieces = scan_format_pieces(normalized.view());
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
    if (left->start != right->start) return left->start < right->start;
    if (left->end != right->end) return left->end < right->end;
    if (left->expected != right->expected)
      return left->expected < right->expected;
    return left->replacement < right->replacement;
  });

  let unique = ArrayList<const source_edit *>{heap_allocator()};
  for (let const *candidate : candidates) {
    if (!unique.is_empty()) {
      let const *previous = unique.back();
      if (previous->start == candidate->start &&
          previous->end == candidate->end &&
          previous->expected == candidate->expected &&
          previous->replacement == candidate->replacement)
        continue;
    }
    unique.push(candidate);
  }
  let nonconflicting = ArrayList<const source_edit *>{heap_allocator()};
  usize candidate_index = 0;

  while (candidate_index < unique.count()) {
    usize group_end = candidate_index + 1;
    usize overlap_end = unique[candidate_index]->end;
    while (group_end < unique.count() &&
           (unique[group_end]->start < overlap_end ||
            (unique[candidate_index]->start == overlap_end &&
             unique[candidate_index]->start == unique[candidate_index]->end &&
             unique[group_end]->start == unique[candidate_index]->start)))
    {
      if (unique[group_end]->end > overlap_end)
        overlap_end = unique[group_end]->end;
      group_end++;
    }
    if (group_end == candidate_index + 1)
      nonconflicting.push(unique[candidate_index]);
    candidate_index = group_end;
  }

  return nonconflicting;
}

fn apply_source_fixes(StringView source, const ArrayList<source_fix> &fixes,
                      bool safe_only) throws -> Maybe<String>
{
  let candidates = ArrayList<const source_edit *>{heap_allocator()};
  for (let const &fix : fixes) {
    if (safe_only && !fix.is_safe_for_fix_all) continue;
    for (let const &edit : fix.edits) {
      if (edit.end < edit.start || edit.end > source.length) return None;
      if (source.substring_of_length(edit.start, edit.end - edit.start) !=
          edit.expected.view())
        return None;
      candidates.push(&edit);
    }
  }
  let const nonconflicting =
      select_nonconflicting_source_edits(steal(candidates));

  let output = String{heap_allocator()};
  usize output_length = source.length;
  for (let const *selected_edit : nonconflicting) {
    let const &edit = *selected_edit;
    output_length -= edit.end - edit.start;
    output_length += edit.replacement.count();
  }
  output.reserve(output_length);
  usize source_position = 0;
  for (let const *selected_edit : nonconflicting) {
    let const &edit = *selected_edit;
    output.append(source.substring_of_length(source_position,
                                             edit.start - source_position));
    output.append(edit.replacement.view());
    source_position = edit.end;
  }
  output.append(source.substring(source_position));

  return output;
}

fn source_fixes_for_original_line_endings(
    StringView source, const ArrayList<source_fix> &normalized_fixes) throws
    -> ArrayList<source_fix>
{
  let original_positions = ArrayList<usize>{heap_allocator()};
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
      if (edit.start >= original_positions.count() ||
          edit.end >= original_positions.count())
      {
        is_valid = false;
        break;
      }
      let const start = original_positions[edit.start];
      let const end = original_positions[edit.end];
      edits.push(source_edit{
          start, end, String{source.substring_of_length(start, end - start)},
          edit.replacement.clone()});
    }
    if (!is_valid) continue;
    fixes.push(source_fix{fix.title.clone(), steal(edits), fix.is_preferred,
                          fix.is_safe_for_fix_all});
  }

  return fixes;
}

fn source_fixes_for_diagnostic(diagnostic_id id, StringView source,
                               SourceLocation location) throws
    -> ArrayList<source_fix>
{
  let fixes = ArrayList<source_fix>{heap_allocator()};
  if (location.position + location.length > source.length) return fixes;
  let replacement = String{heap_allocator()};
  let title = String{heap_allocator()};
  bool is_fixable = true;
  let const written =
      source.substring_of_length(location.position, location.length);

  switch (id) {
  case diagnostic_id::sc1082: title = "Remove the byte-order mark"; break;
  case diagnostic_id::sc1017: title = "Remove the carriage return"; break;
  case diagnostic_id::sc1018:
    title = "Replace the separator with a space";
    replacement = " ";
    break;
  case diagnostic_id::sc1100:
    title = "Replace the dash with '-'";
    replacement = "-";
    break;
  case diagnostic_id::sc1101:
    title = "Remove whitespace after the continuation";
    replacement = "\\";
    break;
  case diagnostic_id::sc1084:
    title = "Put '#' before '!'";
    replacement = "#!";
    break;
  case diagnostic_id::sc1104:
    title = "Add '#' before '!'";
    replacement = "#!";
    break;
  case diagnostic_id::sc1113:
    title = "Add '!' after '#'";
    replacement = "#!";
    break;
  case diagnostic_id::sc1114:
  case diagnostic_id::sc1115:
    title = "Remove whitespace from the shebang";
    break;
  case diagnostic_id::sc1029: {
    if (written.length < 2 || written[0] != '\\') {
      is_fixable = false;
      break;
    }
    title = "Remove the unnecessary escape";
    replacement = written.substring(1);
    break;
  }
  case diagnostic_id::sc2108:
    title = "Replace '-a' with '&&'";
    replacement = "&&";
    break;
  case diagnostic_id::sc2110:
    title = "Replace '-o' with '||'";
    replacement = "||";
    break;
  case diagnostic_id::sc3014:
    title = "Replace '==' with '='";
    replacement = "=";
    break;
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
  fixes.push(source_fix{steal(title), steal(edits), true, true});

  return fixes;
}

} /* namespace koshka */
