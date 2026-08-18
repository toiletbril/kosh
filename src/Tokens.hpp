#pragma once

#include "Common.hpp"
#include "Containers.hpp"
#include "Eval.hpp"
#include "Maybe.hpp"

namespace koshka {

class BumpArena;
class Expression;

struct word_assignment_split;

struct arith_token
{
  enum class kind : u8
  {
    number,
    name,
    op,
    subscript,
  };
  kind k;
  i64 value{0};
  StringView text{};
};

/* The tokenized form of one arithmetic segment. A segment reaches this cache
   only while it is evaluated, so the list is allocated on first evaluation and
   an unevaluated segment carries a null pointer. */
struct arith_token_cache
{
  ArrayList<arith_token> tokens{heap_allocator()};
  bool is_tokenized{false};
  bool is_simple{false};
};

/* The evaluation state of one segment. A literal segment never reaches
   evaluation and never carries this block, so a large script holds one null
   pointer per segment.

   Both caches live in AST_ARENA, and a function-body segment puts them in
   FUNCTION_ARENA, so the generation the cache was filled in is recorded and a
   hit from an earlier generation is treated as stale and refilled. An
   arithmetic segment is never a substitution segment, so the two caches never
   share one segment and one generation stamp answers for both. */
struct segment_eval_cache
{
  const Expression *substitution_ast{nullptr};
  arith_token_cache *arith{nullptr};
  usize arena_generation{0};
  i64 folded_arithmetic_result{0};
  bool has_folded_arithmetic_result{false};
};

class WordSegment
{
public:
  enum class Kind : u8
  {
    LiteralText,
    UnquotedText,
    DoubleQuotedText,
    VariableReference,
    CommandSubstitution,
    ArithmeticExpansion,
    ProcessSubstitution,
    /* The bash 5.3 funsub runs in the current shell, so its assignments and cd
       persist. */
    FunctionSubstitution,
  };

  WordSegment(Kind kind, String text, bool is_in_double_quotes = false,
              bool is_greedy_name = false)
      : kind{kind}, is_in_double_quotes{is_in_double_quotes},
        is_greedy_name{is_greedy_name}, text{steal(text)}
  {}

  ~WordSegment() { release_eval_cache(); }

  cold WordSegment(const WordSegment &other) throws
      : kind{other.kind},
        is_in_double_quotes{other.is_in_double_quotes},
        is_greedy_name{other.is_greedy_name},
        was_ansi_c_quoted{other.was_ansi_c_quoted},
        is_substitution_cache_in_function_arena{
            other.is_substitution_cache_in_function_arena},
        source_position{other.source_position},
        source_length{other.source_length},
        text{other.text}
  {
    if (other.m_eval_cache != nullptr) get_eval_cache() = *other.m_eval_cache;
  }

  WordSegment(WordSegment &&other) wontthrow
      : kind{other.kind},
        is_in_double_quotes{other.is_in_double_quotes},
        is_greedy_name{other.is_greedy_name},
        was_ansi_c_quoted{other.was_ansi_c_quoted},
        is_substitution_cache_in_function_arena{
            other.is_substitution_cache_in_function_arena},
        source_position{other.source_position},
        source_length{other.source_length},
        text{steal(other.text)},
        m_eval_cache{other.m_eval_cache}
  {
    other.m_eval_cache = nullptr;
  }

  cold fn operator=(const WordSegment &other) throws->WordSegment &
  {
    if (this == &other) return *this;

    let copy = WordSegment{other};
    *this = steal(copy);

    return *this;
  }

  fn operator=(WordSegment &&other) wontthrow->WordSegment &
  {
    if (this == &other) return *this;

    release_eval_cache();
    kind = other.kind;
    text = steal(other.text);
    is_in_double_quotes = other.is_in_double_quotes;
    is_greedy_name = other.is_greedy_name;
    was_ansi_c_quoted = other.was_ansi_c_quoted;
    is_substitution_cache_in_function_arena =
        other.is_substitution_cache_in_function_arena;
    source_position = other.source_position;
    source_length = other.source_length;
    m_eval_cache = other.m_eval_cache;
    other.m_eval_cache = nullptr;

    return *this;
  }

  /* The small fields lead so they fill the padding the String would otherwise
     leave. The segment is eighty bytes. */
  Kind kind;
  bool is_in_double_quotes{false};
  bool is_greedy_name{false};
  bool was_ansi_c_quoted{false};
  bool is_substitution_cache_in_function_arena{false};

  mutable u32 source_position{0};
  mutable u32 source_length{0};

  String text;

  fn get_eval_cache() const throws -> segment_eval_cache &
  {
    if (m_eval_cache == nullptr) {
      let const block = heap_allocator().alloc_array<segment_eval_cache>(1);
      m_eval_cache = new (block) segment_eval_cache{};
    }

    return *m_eval_cache;
  }

  pure fn is_split_eligible() const wontthrow -> bool;
  pure fn has_live_glob_chars() const wontthrow -> bool;
  pure fn is_tilde_candidate() const wontthrow -> bool;

  cold fn clone(Allocator allocator) const throws -> WordSegment
  {
    let copy = WordSegment{
        kind, String{allocator, text.view()},
         is_in_double_quotes,
        is_greedy_name
    };
    copy.was_ansi_c_quoted = was_ansi_c_quoted;
    copy.is_substitution_cache_in_function_arena =
        is_substitution_cache_in_function_arena;
    if (source_length > 0) copy.set_source_span(source_position, source_length);
    if (m_eval_cache != nullptr) copy.get_eval_cache() = *m_eval_cache;

    return copy;
  }

  cold fn clone() const throws -> WordSegment
  {
    return clone(text.allocator());
  }

  hot pure fn has_folded_arithmetic_result() const wontthrow -> bool
  {
    return m_eval_cache != nullptr &&
           m_eval_cache->has_folded_arithmetic_result;
  }

  pure fn get_folded_arithmetic_result() const wontthrow -> i64
  {
    ASSERT(has_folded_arithmetic_result());
    return m_eval_cache->folded_arithmetic_result;
  }

  fn set_folded_arithmetic_result(i64 result) const throws -> void
  {
    let &cache = get_eval_cache();
    cache.folded_arithmetic_result = result;
    cache.has_folded_arithmetic_result = true;
  }

  /* A source beyond four gigabytes has no representable span here, so the
     segment reports no location instead of a wrapped one. */
  fn set_source_span(usize position, usize length) wontthrow -> void
  {
    constexpr usize MAXIMUM_SOURCE_OFFSET = ~static_cast<u32>(0);
    if (position > MAXIMUM_SOURCE_OFFSET || length > MAXIMUM_SOURCE_OFFSET) {
      source_position = 0;
      source_length = 0;
      return;
    }

    source_position = static_cast<u32>(position);
    source_length = static_cast<u32>(length);
  }

  pure fn get_source_location(u32 source_name_index) const wontthrow
      -> Maybe<SourceLocation>
  {
    if (source_length == 0) return None;
    return SourceLocation{source_position, source_length, source_name_index};
  }

  pure fn has_glob_metacharacter() const wontthrow -> bool;

private:
  fn release_eval_cache() wontthrow -> void
  {
    if (m_eval_cache == nullptr) return;

    heap_allocator().free_array(m_eval_cache, 1);
    m_eval_cache = nullptr;
  }

  mutable segment_eval_cache *m_eval_cache{nullptr};
};

static_assert(sizeof(usize) != 8 || sizeof(WordSegment) == 80);

class Word
{
public:
  ArrayList<WordSegment> segments{heap_allocator()};

  Word() = default;
  ~Word() { release_constant_value(); }

  /* A copy carries the segments alone. The flattened text is a cache, and it is
     rebuilt on the copy when something asks for it. The copy is held on the
     heap, because the original may sit in an arena the copy has to outlive. */
  cold Word(const Word &other) throws : segments(heap_allocator())
  {
    segments.reserve(other.segments.count());
    for (let const &segment : other.segments)
      segments.push(segment.clone(heap_allocator()));
  }

  Word(Word &&other) wontthrow
      : segments(steal(other.segments)),
        m_constant_value_data(other.m_constant_value_data),
        m_constant_value_length(other.m_constant_value_length),
        m_cached_plain_kind(other.m_cached_plain_kind),
        m_has_cached_plain_kind(other.m_has_cached_plain_kind)
  {
    other.m_constant_value_data = nullptr;
    other.m_constant_value_length = 0;
  }

  cold fn operator=(const Word &other) throws->Word &
  {
    if (this == &other) return *this;

    segments = other.segments;
    release_constant_value();
    m_cached_plain_kind = PlainLiteral::NotPlain;
    m_has_cached_plain_kind = false;

    return *this;
  }

  fn operator=(Word &&other) wontthrow->Word &
  {
    if (this == &other) return *this;

    segments = steal(other.segments);
    release_constant_value();
    m_constant_value_data = other.m_constant_value_data;
    m_constant_value_length = other.m_constant_value_length;
    m_cached_plain_kind = other.m_cached_plain_kind;
    m_has_cached_plain_kind = other.m_has_cached_plain_kind;
    other.m_constant_value_data = nullptr;
    other.m_constant_value_length = 0;

    return *this;
  }

  pure fn is_empty() const wontthrow -> bool;
  fn to_literal_string() const throws -> String;
  fn to_pretty_string() const throws -> String;

  pure fn is_all_ascii_digits() const wontthrow -> bool;

  pure fn fd_allocation_name() const wontthrow -> Maybe<StringView>;

  pure fn runs_substitution() const wontthrow -> bool;

  fn get_assignment_split() const throws -> Maybe<word_assignment_split>;

  /* The split for an operand whose name is quoted, such as "name=value", which
     an assignment builtin still assigns. The name may run across several
     literal segments. Word expansion is unaffected, so this is read by
     analysis alone. */
  cold fn get_quoted_assignment_split() const throws
      -> Maybe<word_assignment_split>;

  enum class PlainLiteral : u8
  {
    NotPlain,
    PlainNoSplit,
    PlainUnquotedOneSegment,
  };

  pure fn plain_literal_kind() const wontthrow -> PlainLiteral;

  fn constant_value() const throws -> StringView;

private:
  fn release_constant_value() wontthrow -> void
  {
    if (m_constant_value_data == nullptr) return;

    heap_allocator().free_array(m_constant_value_data, m_constant_value_length);
    m_constant_value_data = nullptr;
    m_constant_value_length = 0;
  }

  /* Almost every word is one segment and answers from that segment directly, so
     the flattened text is a bare buffer and not a String. */
  mutable char *m_constant_value_data{nullptr};
  mutable u32 m_constant_value_length{0};
  mutable PlainLiteral m_cached_plain_kind{PlainLiteral::NotPlain};
  mutable bool m_has_cached_plain_kind{false};
};

static_assert(sizeof(usize) != 8 || sizeof(Word) == 40);

struct word_assignment_split
{
  String name;
  Word value;
  bool is_append;
};

class Token
{
public:
  enum class Kind : u8
  {
    Invalid,

    /* Significant symbols */
    RightParen,
    LeftParen,
    RightBracket,
    LeftBracket,

    EndOfFile,
    Newline,
    Semicolon,
    DoubleSemicolon,
    SemicolonAmpersand,
    DoubleSemicolonAmpersand,
    Dot,
    Dollar,

    /* Values */
    Word,
    Assignment,

    /* Operators */
    Plus,
    Minus,
    Asterisk,
    Slash,
    Percent,
    Tilde,
    Ampersand,
    DoubleAmpersand,
    AmpersandGreater,
    AmpersandDoubleGreater,
    PipeAmpersand,
    Greater,
    DoubleGreater,
    GreaterEquals,
    Less,
    DoubleLess,
    TripleLess,
    LessEquals,
    Pipe,
    DoublePipe,
    Cap,
    Equals,
    DoubleEquals,
    ExclamationMark,
    ExclamationEquals,

    /* Keywords */
    If,
    Then,
    Else,
    Fi,
    Echo,
    Exit,
    Elif,
    When,
    While,
    Case,
    For,
    Done,
    Esac,
    Until,
    Time,
    Do,
    Function,
  };

  using Flags = u8;

  enum Flag : uint8_t
  {
    /* clang-format off */
    Sentinel       = 0,
    Value          = 1,
    UnaryOperator  = 1 << 1,
    BinaryOperator = 1 << 2,
    Special        = 1 << 3,
    Keyword        = 1 << 4,
    CompoundList   = 1 << 5,
    /* clang-format on */
  };

  Token() = delete;
  virtual ~Token() = default;

  Token(const Token &) = delete;
  Token(Token &&) noexcept = delete;
  Token &operator=(const Token &) = delete;
  Token &operator=(Token &&) noexcept = delete;

  virtual fn kind() const wontthrow -> Kind = 0;
  virtual fn flags() const wontthrow -> Flags = 0;
  virtual fn raw_string() const throws -> String = 0;

  virtual fn raw_view() const wontthrow -> Maybe<StringView>;

  virtual fn to_ast_string() const throws -> String;

  pure fn source_location() const wontthrow -> SourceLocation;

  /* This no-ops for arena storage and frees an ordinary heap token otherwise.
   */
  static fn operator delete(opaque *pointer) wontthrow->void;

protected:
  Token(SourceLocation location);

private:
  SourceLocation m_location;
};

inline constexpr static_string_entry<Token::Kind> KEYWORD_ENTRIES[] = {
    {SSK("if"),       Token::Kind::If      },
    {SSK("then"),     Token::Kind::Then    },
    {SSK("else"),     Token::Kind::Else    },
    {SSK("elif"),     Token::Kind::Elif    },
    {SSK("fi"),       Token::Kind::Fi      },
    {SSK("when"),     Token::Kind::When    },
    {SSK("case"),     Token::Kind::Case    },
    {SSK("esac"),     Token::Kind::Esac    },
    {SSK("while"),    Token::Kind::While   },
    {SSK("for"),      Token::Kind::For     },
    {SSK("done"),     Token::Kind::Done    },
    {SSK("until"),    Token::Kind::Until   },
    {SSK("time"),     Token::Kind::Time    },
    {SSK("do"),       Token::Kind::Do      },
    {SSK("function"), Token::Kind::Function},
};

inline constexpr StaticStringMap KEYWORDS{KEYWORD_ENTRIES};

/* clang-format off */
#define KW_CASE(k)                                                             \
  case Token::Kind::k:                                                         \
    token =                                                                    \
        m_arena->create<tokens::k>(here(actual_cursor_position, byte_count));  \
    break
/* clang-format on */

#define KW_SWITCH_CASES()                                                      \
  KW_CASE(If);                                                                 \
  KW_CASE(Then);                                                               \
  KW_CASE(Else);                                                               \
  KW_CASE(Elif);                                                               \
  KW_CASE(Fi);                                                                 \
  KW_CASE(When);                                                               \
  KW_CASE(Case);                                                               \
  KW_CASE(While);                                                              \
  KW_CASE(Esac);                                                               \
  KW_CASE(For);                                                                \
  KW_CASE(Done);                                                               \
  KW_CASE(Until);                                                              \
  KW_CASE(Time);                                                               \
  KW_CASE(Do);                                                                 \
  KW_CASE(Function);

namespace tokens {

#define TOKEN_STRUCT(t)                                                        \
  class t : public Token                                                       \
  {                                                                            \
  public:                                                                      \
    t(SourceLocation location);                                                \
                                                                               \
    Kind kind() const wontthrow override;                                      \
    Flags flags() const wontthrow override;                                    \
    String raw_string() const throws override;                                 \
    Maybe<StringView> raw_view() const wontthrow override;                     \
  }

TOKEN_STRUCT(If);
TOKEN_STRUCT(Fi);
TOKEN_STRUCT(Else);
TOKEN_STRUCT(Elif);
TOKEN_STRUCT(Then);
TOKEN_STRUCT(Case);
TOKEN_STRUCT(When);
TOKEN_STRUCT(Esac);
TOKEN_STRUCT(For);
TOKEN_STRUCT(While);
TOKEN_STRUCT(Until);
TOKEN_STRUCT(Do);
TOKEN_STRUCT(Done);
TOKEN_STRUCT(Time);
TOKEN_STRUCT(Function);

TOKEN_STRUCT(EndOfFile);

TOKEN_STRUCT(Newline);
TOKEN_STRUCT(Semicolon);
TOKEN_STRUCT(DoubleSemicolon);
TOKEN_STRUCT(SemicolonAmpersand);
TOKEN_STRUCT(DoubleSemicolonAmpersand);
TOKEN_STRUCT(AmpersandGreater);
TOKEN_STRUCT(AmpersandDoubleGreater);
TOKEN_STRUCT(PipeAmpersand);
TOKEN_STRUCT(TripleLess);

TOKEN_STRUCT(Dot);
TOKEN_STRUCT(LeftParen);
TOKEN_STRUCT(RightParen);
TOKEN_STRUCT(LeftBracket);
TOKEN_STRUCT(RightBracket);

class Assignment : public Token
{
public:
  Assignment(SourceLocation location, StringView key, Word value,
             bool is_append);

  fn kind() const wontthrow -> Kind override;
  fn flags() const wontthrow -> Flags override;

  fn raw_string() const throws -> String override;

  pure fn key() const wontthrow -> const String &;
  pure fn value_word() const wontthrow -> const Word &;

  pure fn is_append() const wontthrow -> bool;

protected:
  String m_key;
  Word m_value;
  bool m_is_append;
};

class WordToken : public Token
{
public:
  WordToken(SourceLocation location, Word word);

  fn kind() const wontthrow -> Kind override;
  fn flags() const wontthrow -> Flags override;

  fn raw_string() const throws -> String override;
  fn raw_view() const wontthrow -> Maybe<StringView> override;

  pure fn word() const wontthrow -> const Word &;

protected:
  Word m_word;
};

/* A word carrying a substitution or several segments has no single segment to
   borrow the flattened text from, so the flattened form is owned here. It is
   built on the first read, because analysis walks the segments and most tokens
   are never asked for their flattened text. */
class ExpandedWordToken : public WordToken
{
public:
  ExpandedWordToken(SourceLocation location, Word word);

  fn raw_string() const throws -> String override;
  fn raw_view() const wontthrow -> Maybe<StringView> override;

protected:
  fn fill_literal() const throws -> void;

  mutable String m_literal{heap_allocator()};
};

fn create_word_token(BumpArena &arena, SourceLocation location,
                     Word word) throws -> WordToken *;

class Operator : public Token
{
public:
  Operator(SourceLocation location);

  virtual fn binary_left_associative() const wontthrow -> bool;

  virtual fn left_precedence() const wontthrow -> u8;
  virtual fn construct_binary_expression(const Expression *lhs,
                                         const Expression *rhs) const throws
      -> Expression *;

  virtual fn unary_precedence() const wontthrow -> u8;
  virtual fn construct_unary_expression(const Expression *rhs) const throws
      -> Expression *;
};

#define OPERATOR_TOKEN_STRUCT_COMMON(t)                                        \
public:                                                                        \
  t(SourceLocation location);                                                  \
                                                                               \
  Kind kind() const wontthrow override;                                        \
  Flags flags() const wontthrow override;                                      \
  String raw_string() const throws override;                                   \
  Maybe<StringView> raw_view() const wontthrow override

#define BINARY_OPERATOR_TOKEN_STRUCT_MEMBERS                                   \
  u8 left_precedence() const wontthrow override;                               \
  Expression *construct_binary_expression(                                     \
      const Expression *lhs, const Expression *rhs) const throws override

#define UNARY_OPERATOR_TOKEN_STRUCT_MEMBERS                                    \
  u8 unary_precedence() const wontthrow override;                              \
  Expression *construct_unary_expression(const Expression *rhs)                \
      const throws override

#define UNARY_BINARY_OPERATOR_TOKEN_STRUCT(t)                                  \
  class t : public Operator                                                    \
  {                                                                            \
    OPERATOR_TOKEN_STRUCT_COMMON(t);                                           \
    BINARY_OPERATOR_TOKEN_STRUCT_MEMBERS;                                      \
    UNARY_OPERATOR_TOKEN_STRUCT_MEMBERS;                                       \
  }

UNARY_BINARY_OPERATOR_TOKEN_STRUCT(Plus);
UNARY_BINARY_OPERATOR_TOKEN_STRUCT(Minus);

#define UNARY_OPERATOR_TOKEN_STRUCT(t)                                         \
  class t : public Operator                                                    \
  {                                                                            \
    OPERATOR_TOKEN_STRUCT_COMMON(t);                                           \
    UNARY_OPERATOR_TOKEN_STRUCT_MEMBERS;                                       \
  }

UNARY_OPERATOR_TOKEN_STRUCT(Tilde);
UNARY_OPERATOR_TOKEN_STRUCT(ExclamationMark);

#define BINARY_OPERATOR_TOKEN_STRUCT(t)                                        \
  class t : public Operator                                                    \
  {                                                                            \
    OPERATOR_TOKEN_STRUCT_COMMON(t);                                           \
    BINARY_OPERATOR_TOKEN_STRUCT_MEMBERS;                                      \
  }

BINARY_OPERATOR_TOKEN_STRUCT(Ampersand);
BINARY_OPERATOR_TOKEN_STRUCT(DoubleAmpersand);
BINARY_OPERATOR_TOKEN_STRUCT(DoublePipe);

BINARY_OPERATOR_TOKEN_STRUCT(Slash);
BINARY_OPERATOR_TOKEN_STRUCT(Percent);
BINARY_OPERATOR_TOKEN_STRUCT(Asterisk);
BINARY_OPERATOR_TOKEN_STRUCT(Greater);
BINARY_OPERATOR_TOKEN_STRUCT(DoubleGreater);
BINARY_OPERATOR_TOKEN_STRUCT(GreaterEquals);
BINARY_OPERATOR_TOKEN_STRUCT(Less);
BINARY_OPERATOR_TOKEN_STRUCT(DoubleLess);
BINARY_OPERATOR_TOKEN_STRUCT(LessEquals);
BINARY_OPERATOR_TOKEN_STRUCT(Pipe);
BINARY_OPERATOR_TOKEN_STRUCT(Cap);
BINARY_OPERATOR_TOKEN_STRUCT(Equals);
BINARY_OPERATOR_TOKEN_STRUCT(DoubleEquals);
BINARY_OPERATOR_TOKEN_STRUCT(ExclamationEquals);

} /* namespace tokens */

} /* namespace koshka */
