/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements lexer token, word, segment, span, and cache storage.
 * It owns parsed text transfer, lifetime identities, and runtime copying
 * rules.
 */

#include "Tokens.hpp"

#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Trace.hpp"

namespace koshka {

Token::Token(SourceLocation location, Kind kind)
    : m_location(steal(location)), m_kind(kind)
{}

pure fn Token::source_location() const wontthrow -> SourceLocation
{
  return m_location;
}

fn Token::operator delete(opaque *pointer) wontthrow -> void
{
  if (is_arena_pointer(pointer)) return;
  ::operator delete(pointer);
}

cold fn Token::to_ast_string() const throws -> String { return raw_string(); }

fn Token::raw_view() const wontthrow -> Maybe<StringView> { return None; }

fn keyword_names() throws -> const ArrayList<String> &
{
  static ArrayList<String> names = [] throws {
    let collected = ArrayList<String>{heap_allocator()};
    for (const static_string_entry<Token::Kind> &entry : KEYWORD_ENTRIES)
      collected.push(entry.key.to_string());

    return collected;
  }();

  return names;
}

pure fn WordSegment::is_split_eligible() const wontthrow -> bool
{
  return kind == Kind::UnquotedText ||
         (kind == Kind::VariableReference && !is_in_double_quotes);
}

pure fn WordSegment::has_live_glob_chars() const wontthrow -> bool
{
  return kind == Kind::UnquotedText;
}

pure fn WordSegment::is_tilde_candidate() const wontthrow -> bool
{
  return kind == Kind::UnquotedText;
}

pure fn WordSegment::has_glob_metacharacter() const wontthrow -> bool
{
  return optimizer::word_segment_has_glob_metacharacter(*this);
}

fn WordSegment::get_eval_cache() const throws -> segment_eval_cache &
{
  if (m_eval_cache == nullptr) {
    let const arena =
        is_substitution_cache_in_function_arena ? FUNCTION_ARENA : AST_ARENA;
    if (arena != nullptr) {
      m_eval_cache = arena->create<segment_eval_cache>();
      is_eval_cache_in_arena = true;
    } else {
      let const block = heap_allocator().alloc_array<segment_eval_cache>(1);
      m_eval_cache = new (block) segment_eval_cache{};
    }
  }

  return *m_eval_cache;
}

fn WordSegment::set_exact_constant_arithmetic_text(StringView text) const throws
    -> void
{
  let &cache = get_eval_cache();
  let const arena =
      is_substitution_cache_in_function_arena ? FUNCTION_ARENA : AST_ARENA;
  if (arena == nullptr) return;
  let const allocator = bump_allocator(*arena);
  if (cache.arith == nullptr ||
      !arena->is_lifetime_valid(cache.arithmetic_lifetime))
  {
    cache.arith = arena->create<arith_token_cache>(allocator);
    cache.arithmetic_lifetime = arena->register_lifetime();
  }
  cache.arith->exact_constant_text = String{allocator, text};
  cache.arith->has_exact_constant_text = true;
}

fn WordSegment::release_eval_cache() wontthrow -> void
{
  if (m_eval_cache == nullptr) return;
  if (!is_eval_cache_in_arena) heap_allocator().free_array(m_eval_cache, 1);
  m_eval_cache = nullptr;
  is_eval_cache_in_arena = false;
}

fn WordSegment::move_resources_to_arena(BumpArena &arena) throws -> void
{
  let const allocator = bump_allocator(arena);
  text.move_to_allocator(allocator);
  if (m_eval_cache == nullptr || allocator.owns(m_eval_cache)) return;

  let const cache = arena.create<segment_eval_cache>(*m_eval_cache);
  cache->substitution_ast = nullptr;
  cache->arith = nullptr;
  cache->substitution_lifetime = {};
  cache->arithmetic_lifetime = {};
  if (!is_eval_cache_in_arena) heap_allocator().free_array(m_eval_cache, 1);
  m_eval_cache = cache;
  is_eval_cache_in_arena = true;
}

fn Word::move_resources_to_arena(BumpArena &arena) throws -> void
{
  let const allocator = bump_allocator(arena);

  for (let &segment : segments)
    segment.move_resources_to_arena(arena);

  if (m_constant_value_data != nullptr &&
      !allocator.owns(m_constant_value_data))
  {
    let const moved_value =
        allocator.alloc_array<char>(m_constant_value_length);
    __builtin_memcpy(moved_value, m_constant_value_data,
                     m_constant_value_length);
    segments.allocator().free_array(m_constant_value_data,
                                    m_constant_value_length);
    m_constant_value_data = moved_value;
  }

  segments.move_to_allocator(allocator);
}

pure fn Word::is_empty() const wontthrow -> bool { return segments.is_empty(); }

hot fn Word::to_literal_string() const throws -> String
{
  let result = String{heap_allocator()};
  for (let const &segment : segments) {
    switch (segment.kind) {
    case WordSegment::Kind::CommandSubstitution:
      result += "$(";
      result += segment.text;
      result += ")";
      continue;
    case WordSegment::Kind::FunctionSubstitution:
      result += "${ ";
      result += segment.text;
      result += " }";
      continue;
    case WordSegment::Kind::ArithmeticExpansion:
      result += "$((";
      result += segment.text;
      result += "))";
      continue;
    case WordSegment::Kind::VariableReference: result += '$'; break;
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::ProcessSubstitution: break;
    }
    result += segment.text;
  }
  return result;
}

pure fn Word::plain_literal_kind() const wontthrow -> PlainLiteral
{
  if (!m_has_cached_plain_kind) {
    m_cached_plain_kind = optimizer::classify_plain_literal(*this);
    m_has_cached_plain_kind = true;
  }
  return m_cached_plain_kind;
}

fn Word::constant_value() const throws -> StringView
{
  if (segments.count() == 1) return segments[0].text.view();

  if (m_constant_value_data == nullptr) {
    usize total_length = 0;
    for (let const &segment : segments)
      total_length += segment.text.count();

    if (total_length == 0) return StringView{};
    if (total_length > ~static_cast<u32>(0)) throw std::bad_alloc{};

    let buffer = segments.allocator().alloc_array<char>(total_length);
    usize write_position = 0;
    for (let const &segment : segments) {
      let const view = segment.text.view();
      __builtin_memcpy(buffer + write_position, view.data, view.length);
      write_position += view.length;
    }

    m_constant_value_data = buffer;
    m_constant_value_length = static_cast<u32>(total_length);
  }

  return StringView{m_constant_value_data, m_constant_value_length};
}

pure fn Word::is_all_ascii_digits() const wontthrow -> bool
{
  if (segments.is_empty()) return false;
  bool has_seen_digit = false;
  for (let const &segment : segments) {
    switch (segment.kind) {
    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::ArithmeticExpansion:
    case WordSegment::Kind::FunctionSubstitution: return false;
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::ProcessSubstitution: break;
    }

    for (usize i = 0; i < segment.text.count(); i++) {
      const char c = segment.text[i];
      if (c < '0' || c > '9') return false;
      has_seen_digit = true;
    }
  }
  return has_seen_digit;
}

pure fn Word::fd_allocation_name() const wontthrow -> Maybe<StringView>
{
  if (segments.count() != 1) return None;

  let const &segment = segments[0];
  if (segment.kind != WordSegment::Kind::UnquotedText &&
      segment.kind != WordSegment::Kind::LiteralText)
  {
    return None;
  }

  let const text = segment.text.view();
  if (text.length < 3 || text[0] != '{' || text[text.length - 1] != '}') {
    return None;
  }

  let const name = text.substring_of_length(1, text.length - 2);
  if (!lexer::is_variable_name_start(name[0])) return None;
  for (usize i = 1; i < name.length; i++)
    if (!lexer::is_variable_name(name[i])) return None;

  return name;
}

pure fn Word::runs_substitution() const wontthrow -> bool
{
  for (let const &segment : segments) {
    switch (segment.kind) {
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::FunctionSubstitution: return true;
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::ProcessSubstitution:
    case WordSegment::Kind::ArithmeticExpansion: break;
    }
  }
  return false;
}

cold fn Word::to_pretty_string() const throws -> String
{
  let result = String{"[Word"};
  for (let const &segment : segments) {
    result += "\n  ";
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText: result += "Literal"; break;
    case WordSegment::Kind::UnquotedText: result += "Unquoted"; break;
    case WordSegment::Kind::DoubleQuotedText: result += "DoubleQuoted"; break;
    case WordSegment::Kind::VariableReference: result += "Variable"; break;
    case WordSegment::Kind::CommandSubstitution:
      result += "CommandSubstitution";
      break;
    case WordSegment::Kind::ProcessSubstitution:
      result += "ProcessSubstitution";
      break;
    case WordSegment::Kind::ArithmeticExpansion:
      result += "ArithmeticExpansion";
      break;
    case WordSegment::Kind::FunctionSubstitution:
      result += "FunctionSubstitution";
      break;
    }
    result += " \"";
    result += segment.text;
    result += '"';
  }
  result += "\n]";
  return result;
}

static pure fn segment_holds_literal_text(WordSegment::Kind kind) wontthrow
    -> bool
{
  switch (kind) {
  case WordSegment::Kind::LiteralText:
  case WordSegment::Kind::UnquotedText:
  case WordSegment::Kind::DoubleQuotedText: return true;
  default: return false;
  }
}

static fn append_subscript_segment_source(const WordSegment &segment,
                                          String &out) throws -> bool
{
  switch (segment.kind) {
  case WordSegment::Kind::VariableReference:
    out.append("${");
    out.append(segment.text.view());
    out.push('}');
    return true;
  case WordSegment::Kind::ArithmeticExpansion:
    out.append("$((");
    out.append(segment.text.view());
    out.append("))");
    return true;
  case WordSegment::Kind::CommandSubstitution:
    out.append("$(");
    out.append(segment.text.view());
    out.push(')');
    return true;
  default: return false;
  }
}

/* An array element assignment whose subscript holds an expansion, the $k in
   v[$k]=1, splits across segments since the = lands after the ] in a later
   segment. */
static fn
array_element_assignment_split(const ArrayList<WordSegment> &segments) throws
    -> Maybe<word_assignment_split>
{
  const WordSegment &first = segments[0];
  usize name_end = 1;
  while (name_end < first.text.count() &&
         lexer::is_variable_name(first.text[name_end]))
    name_end++;
  if (name_end >= first.text.count() || first.text[name_end] != '[') {
    return koshka::None;
  }

  let subscript = String{heap_allocator()};
  /* A ] in the rest of segment 0 means the = sits in segment 0 too, already
     ruled out, so this is not an assignment. */
  const StringView head = first.text.substring(name_end + 1);
  if (head.find_character(']').has_value()) return koshka::None;
  subscript.append(head);

  for (usize i = 1; i < segments.count(); i++) {
    let const &segment = segments[i];
    if (!segment_holds_literal_text(segment.kind)) {
      if (!append_subscript_segment_source(segment, subscript))
        return koshka::None;
      continue;
    }

    let const close = segment.text.find_character(']');
    if (!close.has_value()) {
      subscript.append(segment.text.view());
      continue;
    }

    subscript.append(segment.text.substring_of_length(0, *close));
    const StringView after = segment.text.substring(*close + 1);
    bool is_append = false;
    usize value_start = 0;
    if (after.starts_with("+=")) {
      is_append = true;
      value_start = 2;
    } else if (after.starts_with("=")) {
      value_start = 1;
    } else {
      return koshka::None;
    }

    let key = String{first.text.substring_of_length(0, name_end)};
    key.push('[');
    key.append(subscript.view());
    key.push(']');

    LOG(All, "folding the subscript into array element key '%s'", key.c_str());

    let value = Word{};
    value.segments.push(WordSegment{
        WordSegment::Kind::UnquotedText,
        SegmentText{heap_allocator(), after.substring(value_start)},
        false
    });
    for (usize j = i + 1; j < segments.count(); j++)
      value.segments.push(segments[j]);

    return word_assignment_split{steal(key), steal(value), is_append};
  }

  return koshka::None;
}

hot fn Word::get_assignment_split() const throws -> Maybe<word_assignment_split>
{
  if (segments.is_empty()) return koshka::None;

  const WordSegment &first = segments[0];
  if (first.kind != WordSegment::Kind::UnquotedText) return koshka::None;

  if (first.text.is_empty() || !lexer::is_variable_name_start(first.text[0])) {
    return koshka::None;
  }

  let const equals_position = first.text.find_character('=');
  if (!equals_position.has_value()) {
    if (first.text.find_character('[').has_value())
      return array_element_assignment_split(segments);
    return koshka::None;
  }
  if (*equals_position == 0) return koshka::None;

  ASSERT(*equals_position <= first.text.count());

  const bool is_append = first.text[*equals_position - 1] == '+';
  const usize name_length = is_append ? *equals_position - 1 : *equals_position;
  if (name_length == 0) return koshka::None;

  usize name_cursor = 1;
  while (name_cursor < name_length &&
         lexer::is_variable_name(first.text[name_cursor]))
    name_cursor++;
  if (name_cursor < name_length && first.text[name_cursor] == '[') {
    if (first.text[name_length - 1] != ']' || name_length - name_cursor < 3) {
      return koshka::None;
    }
    name_cursor = name_length;
  }
  if (name_cursor != name_length) return koshka::None;

  let const name_view = first.text.substring_of_length(0, name_length);
  let name = String{name_view};

  let value = Word{};
  /* The value always begins with an unquoted segment, even when empty, so that
     FOO= produces one empty field rather than no field at all. */
  value.segments.push(WordSegment{
      WordSegment::Kind::UnquotedText,
      SegmentText{heap_allocator(), first.text.substring(*equals_position + 1)},
      false
  });
  for (usize i = 1; i < segments.count(); i++)
    value.segments.push(segments[i]);

  return word_assignment_split{steal(name), steal(value), is_append};
}

/* The name characters ahead of the first literal =, gathered across the
   literal segments the quoting split the operand into. */
static fn quoted_assignment_name_prefix(const ArrayList<WordSegment> &segments,
                                        usize &equals_segment,
                                        usize &equals_position) throws -> String
{
  let prefix = String{heap_allocator()};

  for (usize i = 0; i < segments.count(); i++) {
    if (!segment_holds_literal_text(segments[i].kind)) break;

    let const text = segments[i].text.view();
    let const found = text.find_character('=');
    if (!found.has_value()) {
      prefix.append(text);
      continue;
    }

    prefix.append(text.substring_of_length(0, *found));
    equals_segment = i;
    equals_position = *found;
    break;
  }

  return prefix;
}

cold fn Word::get_quoted_assignment_split() const throws
    -> Maybe<word_assignment_split>
{
  let equals_segment = segments.count();
  let equals_position = usize{0};
  let const prefix =
      quoted_assignment_name_prefix(segments, equals_segment, equals_position);
  if (equals_segment == segments.count()) return koshka::None;

  let const prefix_view = prefix.view();
  let const is_append =
      !prefix_view.is_empty() && prefix_view[prefix_view.length - 1] == '+';
  let const name_length =
      is_append ? prefix_view.length - 1 : prefix_view.length;
  if (name_length == 0) return koshka::None;

  if (!lexer::is_variable_name_start(prefix_view[0])) return koshka::None;

  usize name_cursor = 1;
  while (name_cursor < name_length &&
         lexer::is_variable_name(prefix_view[name_cursor]))
    name_cursor++;

  if (name_cursor < name_length && prefix_view[name_cursor] == '[') {
    if (prefix_view[name_length - 1] != ']' || name_length - name_cursor < 3) {
      return koshka::None;
    }
    name_cursor = name_length;
  }

  if (name_cursor != name_length) return koshka::None;

  const WordSegment &carrier = segments[equals_segment];

  let value = Word{};
  value.segments.push(WordSegment{
      carrier.kind,
      SegmentText{heap_allocator(),
                  carrier.text.substring(equals_position + 1)},
      carrier.is_in_double_quotes != 0
  });
  for (usize i = equals_segment + 1; i < segments.count(); i++)
    value.segments.push(segments[i]);

  return word_assignment_split{
      String{prefix_view.substring_of_length(0, name_length)}, steal(value),
      is_append};
}

cold fn SegmentText::grow_owned(usize needed) throws -> void
{
  usize new_capacity =
      m_capacity == 0 ? needed : static_cast<usize>(m_capacity);
  while (new_capacity < needed) {
    if (new_capacity > MAXIMUM_TEXT_LENGTH / 2) {
      new_capacity = needed;
      break;
    }
    new_capacity *= 2;
  }

  let const fresh = heap_allocator().alloc_array<char>(new_capacity);
  if (m_length > 0) std::memcpy(fresh, m_data, m_length);
  if (m_capacity != 0) {
    heap_allocator().free_array(const_cast<char *>(m_data), m_capacity);
  }

  m_data = fresh;
  m_capacity = static_cast<u32>(new_capacity);
}

fn SegmentText::append(StringView other) throws -> void
{
  if (other.length == 0) return;
  if (other.length > MAXIMUM_TEXT_LENGTH - m_length) [[unlikely]]
    throw std::bad_alloc{};

  let const needed = static_cast<usize>(m_length) + other.length;
  if (needed > m_capacity) {
    /* The source may live inside this text, so its offset is kept and the view
       is rebound after the buffer moves. */
    let const source_address = reinterpret_cast<uintptr>(other.data);
    let const storage_address = reinterpret_cast<uintptr>(m_data);
    let const is_aliased = m_data != nullptr &&
                           source_address >= storage_address &&
                           source_address - storage_address < m_length;
    let const source_offset =
        is_aliased ? source_address - storage_address : usize{0};

    grow_owned(needed);

    if (is_aliased) other.data = m_data + source_offset;
  }

  std::memcpy(const_cast<char *>(m_data) + m_length, other.data, other.length);
  m_length = static_cast<u32>(needed);
}

namespace tokens {

#define TOKEN_DECLS(t, s)                                                      \
  t::t(SourceLocation location) : Token(steal(location), Token::Kind::t) {}    \
  String t::raw_string() const throws { return s; }                            \
  Maybe<StringView> t::raw_view() const wontthrow { return StringView{s}; }

TOKEN_DECLS(If, "if");
TOKEN_DECLS(Then, "then");
TOKEN_DECLS(Else, "else");
TOKEN_DECLS(Elif, "elif");
TOKEN_DECLS(Fi, "fi");
TOKEN_DECLS(For, "for");
TOKEN_DECLS(While, "while");
TOKEN_DECLS(Until, "until");
TOKEN_DECLS(Do, "do");
TOKEN_DECLS(Done, "done");
TOKEN_DECLS(Case, "case");
TOKEN_DECLS(When, "when");
TOKEN_DECLS(Esac, "esac");
TOKEN_DECLS(Time, "time");
TOKEN_DECLS(Function, "function");
TOKEN_DECLS(Newline, "newline");
TOKEN_DECLS(Semicolon, ";");
TOKEN_DECLS(EndOfFile, "end of input");
TOKEN_DECLS(DoubleSemicolon, ";;");
TOKEN_DECLS(SemicolonAmpersand, ";&");
TOKEN_DECLS(DoubleSemicolonAmpersand, ";;&");
TOKEN_DECLS(AmpersandGreater, "&>");
TOKEN_DECLS(AmpersandDoubleGreater, "&>>");
TOKEN_DECLS(PipeAmpersand, "|&");
TOKEN_DECLS(TripleLess, "<<<");
TOKEN_DECLS(Dot, ".");
TOKEN_DECLS(LeftParen, "(");
TOKEN_DECLS(RightParen, ")");
TOKEN_DECLS(RightBracket, "}");

Assignment::Assignment(SourceLocation location, String key, Word value,
                       bool is_append)
    : Token(steal(location), Token::Kind::Assignment), m_key(steal(key)),
      m_value(steal(value)), m_is_append(is_append)
{}

fn Assignment::raw_string() const throws -> String
{
  let result = m_key.clone();
  result += m_is_append ? "+=" : "=";
  result += m_value.to_literal_string();
  return result;
}

pure fn Assignment::key() const wontthrow -> const String & { return m_key; }

pure fn Assignment::is_append() const wontthrow -> bool { return m_is_append; }

pure fn Assignment::value_word() const wontthrow -> const Word &
{
  return m_value;
}

namespace {

/* A word whose flattened text is already spelled out by one of its segments,
   so nothing has to be built and owned to answer for it. */
pure fn borrowed_word_literal(const Word &word) wontthrow -> Maybe<StringView>
{
  if (word.segments.is_empty()) return StringView{};
  if (word.segments.count() != 1) return None;

  switch (word.segments.front().kind) {
  case WordSegment::Kind::LiteralText:
  case WordSegment::Kind::UnquotedText:
  case WordSegment::Kind::DoubleQuotedText:
  case WordSegment::Kind::ProcessSubstitution:
    return word.segments.front().text.view();
  default: return None;
  }
}

} /* namespace */

WordToken::WordToken(SourceLocation location, Word word)
    : Token(steal(location), Token::Kind::Word), m_word(steal(word))
{}

fn WordToken::raw_view() const wontthrow -> Maybe<StringView>
{
  return borrowed_word_literal(m_word);
}

fn WordToken::raw_string() const throws -> String
{
  if (let const borrowed = borrowed_word_literal(m_word); borrowed.has_value())
  {
    return String{heap_allocator(), *borrowed};
  }

  return m_word.to_literal_string();
}

pure fn WordToken::word() const wontthrow -> const Word & { return m_word; }

ExpandedWordToken::ExpandedWordToken(SourceLocation location, Word word)
    : WordToken(steal(location), steal(word))
{}

ExpandedWordToken::~ExpandedWordToken()
{
  if (m_literal_data == nullptr || is_arena_pointer(m_literal_data)) return;

  heap_allocator().free_array(m_literal_data, m_literal_length);
}

/* The flattened text of such a word is empty only when every segment is empty,
   and rebuilding an empty result costs nothing, so the null buffer doubles as
   the not-yet-built mark. */
fn ExpandedWordToken::fill_literal() const throws -> void
{
  if (m_literal_data != nullptr) return;

  let const built = m_word.to_literal_string();
  let const view = built.view();

  if (view.length == 0) return;
  if (view.length > ~static_cast<u32>(0)) throw std::bad_alloc{};

  let buffer = m_word.segments.allocator().alloc_array<char>(view.length);
  __builtin_memcpy(buffer, view.data, view.length);

  m_literal_data = buffer;
  m_literal_length = static_cast<u32>(view.length);
}

fn ExpandedWordToken::raw_view() const wontthrow -> Maybe<StringView>
{
  /* The override is noexcept, so a failed build answers as an unavailable view
     the same way a word with no borrowable segment does. */
  try {
    fill_literal();
  } catch (...) {
    return None;
  }

  if (m_literal_data == nullptr) return StringView{};

  return StringView{m_literal_data, m_literal_length};
}

fn ExpandedWordToken::raw_string() const throws -> String
{
  fill_literal();

  if (m_literal_data == nullptr) return String{heap_allocator()};

  return String{
      StringView{m_literal_data, m_literal_length}
  };
}

fn create_word_token(BumpArena &arena, SourceLocation location,
                     Word word) throws -> WordToken *
{
  word.move_resources_to_arena(arena);

  if (borrowed_word_literal(word).has_value())
    return arena.create<WordToken>(steal(location), steal(word));

  return arena.create<ExpandedWordToken>(steal(location), steal(word));
}

TOKEN_DECLS(Plus, "+");
TOKEN_DECLS(Minus, "-");
TOKEN_DECLS(DoublePipe, "||");
TOKEN_DECLS(Ampersand, "&");
TOKEN_DECLS(DoubleAmpersand, "&&");
TOKEN_DECLS(Slash, "/");
TOKEN_DECLS(Asterisk, "*");
TOKEN_DECLS(Percent, "%");
TOKEN_DECLS(Greater, ">");
TOKEN_DECLS(DoubleGreater, ">>");
TOKEN_DECLS(GreaterEquals, ">=");
TOKEN_DECLS(Less, "<");
TOKEN_DECLS(DoubleLess, "<<");
TOKEN_DECLS(LessEquals, "<=");
TOKEN_DECLS(Pipe, "|");
TOKEN_DECLS(Cap, "^");
TOKEN_DECLS(Equals, "=");
TOKEN_DECLS(DoubleEquals, "==");
TOKEN_DECLS(ExclamationEquals, "!=");
TOKEN_DECLS(ExclamationMark, "!");
TOKEN_DECLS(Tilde, "~");

} /* namespace tokens */

} /* namespace koshka */
