#include "Builtin.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

cold [[noreturn]] static fn fail_conditional(StringView message,
                                             StringView reason) throws -> void
{
  throw ErrorWithDetails{message, reason};
}

cold [[noreturn]] static fn fail_conditional(StringView reason) throws -> void
{
  fail_conditional("Unable to evaluate the [[ ]]", reason);
}

cold [[noreturn]] static fn fail_conditional_syntax(StringView reason) throws
    -> void
{
  ErrorWithDetails error{"Unable to evaluate the [[ ]]", reason};
  error.set_command_status(2);
  throw error;
}

static fn ascii_lower_copy(Allocator allocator, StringView text) throws
    -> String
{
  return text.to_lower_ascii(allocator);
}

namespace {

struct conditional_evaluator
{
  EvalContext &cxt;
  const ArrayList<conditional_element> &elements;
  usize pos = 0;
  /* A decided && or || branch is parsed to advance past its tokens but not
     evaluated, so a dead-branch glob, regex, or command substitution runs no
     side effect and raises no error, the way bash short-circuits [[ ]]. */
  bool is_skipping = false;

  using Kind = conditional_element::Kind;

  pure fn at_end() const wontthrow -> bool { return pos >= elements.count(); }
  pure fn kind_at(usize i) const wontthrow -> Kind { return elements[i].kind; }

  fn unexpected_token() throws -> String
  {
    if (!at_end()) return operand_literal(elements[pos]);

    return String{heap_allocator()};
  }

  fn operand_literal(const conditional_element &e) throws -> String
  {
    if (e.word != nullptr && e.word->kind() == Token::Kind::Word) {
      return static_cast<const tokens::WordToken *>(e.word)
          ->word()
          .to_literal_string();
    }
    if (e.word != nullptr) return e.word->raw_string();
    return String{heap_allocator()};
  }

  fn operand_value(const conditional_element &e) throws -> String
  {
    if (e.word != nullptr && e.word->kind() == Token::Kind::Word) {
      try {
        return cxt.expand_word_for_assignment(
            static_cast<const tokens::WordToken *>(e.word)->word());
      } catch (const ErrorWithLocation &) {
        throw;
      } catch (const Error &err) {
        relocate_error(err, e.word->source_location());
      }
    }
    if (e.word != nullptr) return e.word->raw_string();
    return String{heap_allocator()};
  }

  /* The mask marks which *, ?, and [ stay active. A quoted or escaped
     metacharacter is masked off and matches literally. */
  fn operand_pattern_masked(const conditional_element &e, Bitset &active) throws
      -> String
  {
    if (e.word != nullptr && e.word->kind() == Token::Kind::Word) {
      try {
        return cxt.expand_case_pattern_masked(
            static_cast<const tokens::WordToken *>(e.word)->word(), active);
      } catch (const ErrorWithLocation &) {
        throw;
      } catch (const Error &err) {
        relocate_error(err, e.word->source_location());
      }
    }
    let raw =
        e.word != nullptr ? e.word->raw_string() : String{heap_allocator()};
    for (usize i = 0; i < raw.count(); i++)
      active.push(true);
    return raw;
  }

  enum class UnaryOperatorKind : uchar
  {
    Empty,
    Nonempty,
    VariableSet,
    PathExists,
    RegularFile,
    Directory,
    Readable,
    Writable,
    Executable,
    NonemptyFile,
    SymbolicLink,
    BlockDevice,
    CharacterDevice,
    Fifo,
    Socket,
    Setgid,
    Setuid,
    Sticky,
    EffectiveUserOwner,
    EffectiveGroupOwner,
    Terminal,
    ShellOption,
  };

  static pure fn unary_operator(StringView text) wontthrow
      -> Maybe<UnaryOperatorKind>
  {
    static constexpr static_string_entry<UnaryOperatorKind> ENTRIES[] = {
        {SSK("-z"), UnaryOperatorKind::Empty              },
        {SSK("-n"), UnaryOperatorKind::Nonempty           },
        {SSK("-v"), UnaryOperatorKind::VariableSet        },
        {SSK("-a"), UnaryOperatorKind::PathExists         },
        {SSK("-e"), UnaryOperatorKind::PathExists         },
        {SSK("-f"), UnaryOperatorKind::RegularFile        },
        {SSK("-d"), UnaryOperatorKind::Directory          },
        {SSK("-r"), UnaryOperatorKind::Readable           },
        {SSK("-w"), UnaryOperatorKind::Writable           },
        {SSK("-x"), UnaryOperatorKind::Executable         },
        {SSK("-s"), UnaryOperatorKind::NonemptyFile       },
        {SSK("-h"), UnaryOperatorKind::SymbolicLink       },
        {SSK("-L"), UnaryOperatorKind::SymbolicLink       },
        {SSK("-b"), UnaryOperatorKind::BlockDevice        },
        {SSK("-c"), UnaryOperatorKind::CharacterDevice    },
        {SSK("-p"), UnaryOperatorKind::Fifo               },
        {SSK("-S"), UnaryOperatorKind::Socket             },
        {SSK("-g"), UnaryOperatorKind::Setgid             },
        {SSK("-u"), UnaryOperatorKind::Setuid             },
        {SSK("-k"), UnaryOperatorKind::Sticky             },
        {SSK("-O"), UnaryOperatorKind::EffectiveUserOwner },
        {SSK("-G"), UnaryOperatorKind::EffectiveGroupOwner},
        {SSK("-t"), UnaryOperatorKind::Terminal           },
        {SSK("-o"), UnaryOperatorKind::ShellOption        },
    };
    static constexpr StaticStringMap OPERATORS{ENTRIES};
    return OPERATORS.find(text);
  }

  enum class BinaryOperatorKind : uchar
  {
    PatternEqual,
    PatternNotEqual,
    Regex,
    ArithmeticEqual,
    ArithmeticNotEqual,
    ArithmeticLess,
    ArithmeticLessEqual,
    ArithmeticGreater,
    ArithmeticGreaterEqual,
    SameFile,
    NewerFile,
    OlderFile,
  };

  static pure fn binary_operator(StringView text) wontthrow
      -> Maybe<BinaryOperatorKind>
  {
    static constexpr static_string_entry<BinaryOperatorKind> ENTRIES[] = {
        {SSK("="),   BinaryOperatorKind::PatternEqual          },
        {SSK("=="),  BinaryOperatorKind::PatternEqual          },
        {SSK("!="),  BinaryOperatorKind::PatternNotEqual       },
        {SSK("=~"),  BinaryOperatorKind::Regex                 },
        {SSK("-eq"), BinaryOperatorKind::ArithmeticEqual       },
        {SSK("-ne"), BinaryOperatorKind::ArithmeticNotEqual    },
        {SSK("-lt"), BinaryOperatorKind::ArithmeticLess        },
        {SSK("-le"), BinaryOperatorKind::ArithmeticLessEqual   },
        {SSK("-gt"), BinaryOperatorKind::ArithmeticGreater     },
        {SSK("-ge"), BinaryOperatorKind::ArithmeticGreaterEqual},
        {SSK("-ef"), BinaryOperatorKind::SameFile              },
        {SSK("-nt"), BinaryOperatorKind::NewerFile             },
        {SSK("-ot"), BinaryOperatorKind::OlderFile             },
    };
    static constexpr StaticStringMap OPERATORS{ENTRIES};
    return OPERATORS.find(text);
  }

  static pure fn is_regex_metacharacter(char c) wontthrow -> bool
  {
    return c == '.' || c == '^' || c == '$' || c == '*' || c == '+' ||
           c == '?' || c == '(' || c == ')' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '|' || c == '\\';
  }

  fn regex_match(StringView value, StringView pattern,
                 const Bitset &active) throws -> bool
  {
    if (!os::HAS_REGEX_ENGINE) {
      fail_conditional("Unable to use =~ in the [[ ]]",
                       "It is not supported on this platform");
    }

    /* An inactive mask byte came from a quoted part of the operand, so a regex
       metacharacter there is backslash-escaped to match itself. */
    let escaped_pattern = String{cxt.scratch_allocator()};
    for (usize i = 0; i < pattern.length; i++) {
      let const is_literal = i < active.count() && !active[i];
      if (is_literal && is_regex_metacharacter(pattern[i])) {
        escaped_pattern += '\\';
      }
      escaped_pattern += pattern[i];
    }

    os::compiled_regex *compiled =
        cxt.cached_compiled_regex(escaped_pattern.view());
    let spans = ArrayList<os::regex_span>{cxt.scratch_allocator()};
    let error_message = String{cxt.scratch_allocator()};
    let const result = os::execute_regex(*compiled, value, spans, error_message,
                                         cxt.scratch_allocator());
    LOG(All, "the =~ regex %s the value",
        result == os::regex_match_result::Matched ? "matched"
                                                  : "did not match");

    if (result == os::regex_match_result::NoMatch) {
      cxt.set_indexed_array("BASH_REMATCH",
                            ArrayList<String>{heap_allocator()});
      return false;
    }
    if (result == os::regex_match_result::Error) {
      /* A genuine engine failure such as REG_ESPACE surfaces with the engine's
         own message instead of reading as false. */
      fail_conditional("Unable to match the =~ pattern", error_message.view());
    }

    let rematch = ArrayList<String>{heap_allocator()};
    rematch.reserve(spans.count());
    for (usize i = 0; i < spans.count(); i++) {
      if (spans[i].start < 0) {
        rematch.push(String{heap_allocator()});
        continue;
      }
      let const start = static_cast<usize>(spans[i].start);
      let const end = static_cast<usize>(spans[i].end);
      rematch.push(String{heap_allocator(),
                          value.substring_of_length(start, end - start)});
    }
    cxt.set_indexed_array("BASH_REMATCH", steal(rematch));
    return true;
  }

  fn eval_unary(UnaryOperatorKind op, StringView operand) throws -> bool
  {
    switch (op) {
    case UnaryOperatorKind::Empty: return operand.is_empty();
    case UnaryOperatorKind::Nonempty: return !operand.is_empty();
    case UnaryOperatorKind::VariableSet: {
      if (let const bracket = operand.find_character('[');
          bracket.has_value() && operand[operand.length - 1] == ']')
      {
        let const name = operand.substring_of_length(0, *bracket);
        let const subscript = operand.substring_of_length(
            *bracket + 1, operand.length - *bracket - 2);
        return cxt.array_element_is_set(name, subscript);
      }
      return cxt.get_variable_value(operand).has_value();
    }
    case UnaryOperatorKind::PathExists: return Path{operand}.exists();
    case UnaryOperatorKind::RegularFile: return Path{operand}.is_regular_file();
    case UnaryOperatorKind::Directory: return Path{operand}.is_directory();
    case UnaryOperatorKind::Readable: return Path{operand}.is_readable();
    case UnaryOperatorKind::Writable: return Path{operand}.is_writable();
    case UnaryOperatorKind::Executable: return Path{operand}.is_executable();
    case UnaryOperatorKind::NonemptyFile: {
      let const path = Path{operand};
      let const size = path.file_size();
      return size.has_value() && size.value() > 0;
    }
    case UnaryOperatorKind::SymbolicLink:
      return Path{operand}.is_symbolic_link();
    case UnaryOperatorKind::BlockDevice: return Path{operand}.is_block_device();
    case UnaryOperatorKind::CharacterDevice:
      return Path{operand}.is_character_device();
    case UnaryOperatorKind::Fifo: return Path{operand}.is_fifo();
    case UnaryOperatorKind::Socket: return Path{operand}.is_socket();
    case UnaryOperatorKind::Setgid: return Path{operand}.has_setgid_bit();
    case UnaryOperatorKind::Setuid: return Path{operand}.has_setuid_bit();
    case UnaryOperatorKind::Sticky: return Path{operand}.has_sticky_bit();
    case UnaryOperatorKind::EffectiveUserOwner:
      return Path{operand}.is_owned_by_effective_user();
    case UnaryOperatorKind::EffectiveGroupOwner:
      return Path{operand}.is_owned_by_effective_group();
    case UnaryOperatorKind::Terminal: {
      if (ErrorOr<i64> descriptor = operand.to<i64>(); !descriptor.is_error())
        return os::is_fd_a_tty(
            os::descriptor_from_fd_number(descriptor.value()));
      /* bash reports a non-integer -t operand with status 2. */
      ErrorWithDetails error{"Unable to test '-t " + operand + "'",
                             "The operand is not an integer"};
      error.set_command_status(2);
      throw error;
    }
    case UnaryOperatorKind::ShellOption:
      return query_shell_option(cxt, operand).value_or(false);
    }

    return false;
  }

  fn eval_binary(StringView left, BinaryOperatorKind op,
                 StringView right) throws -> bool
  {
    switch (op) {
    case BinaryOperatorKind::SameFile:
      return Path{left}.is_same_file_as(Path{right});
    case BinaryOperatorKind::NewerFile:
      return Path{left}.is_newer_than(Path{right});
    case BinaryOperatorKind::OlderFile:
      return Path{left}.is_older_than(Path{right});
    default: break;
    }

    /* The arithmetic comparison operands are full expressions, so 1+1 and a
       bare variable name evaluate. An empty operand reads as zero. */
    let const do_normalize = [&](StringView operand) throws -> StringView {
      for (usize i = 0; i < operand.length; i++) {
        if (operand[i] != ' ' && operand[i] != '\t') return operand;
      }
      return "0";
    };
    let const ordering =
        cxt.compare_arithmetic(do_normalize(left), do_normalize(right));
    switch (op) {
    case BinaryOperatorKind::ArithmeticEqual: return ordering == 0;
    case BinaryOperatorKind::ArithmeticNotEqual: return ordering != 0;
    case BinaryOperatorKind::ArithmeticLess: return ordering < 0;
    case BinaryOperatorKind::ArithmeticLessEqual: return ordering <= 0;
    case BinaryOperatorKind::ArithmeticGreater: return ordering > 0;
    case BinaryOperatorKind::ArithmeticGreaterEqual: return ordering >= 0;
    default: return false;
    }
  }

  fn eval_primary() throws -> bool
  {
    if (at_end())
      fail_conditional(
          "The expression ends unexpectedly",
          "A conditional needs an operator or an operand after this point");
    const conditional_element &first = elements[pos];
    if (first.kind != Kind::Operand)
      fail_conditional("An operator appears where an operand is expected");

    let const first_literal = operand_literal(first);

    let selected_unary_operator = Maybe<UnaryOperatorKind>{};
    if (first.is_bare_unquoted)
      selected_unary_operator = unary_operator(first_literal.view());
    if (selected_unary_operator.has_value()) {
      if (pos + 1 >= elements.count() || kind_at(pos + 1) != Kind::Operand) {
        fail_conditional("The unary operator '" + first_literal +
                         "' is missing its operand");
      }

      pos += 2;
      if (is_skipping) return false;
      /* bash does not nounset the operand of -v, so the unset-variable
         diagnostic stays silent while it expands. The defer restores the prior
         value so a throw cannot strand the suppression on. */
      let const is_existence_test =
          *selected_unary_operator == UnaryOperatorKind::VariableSet;
      let const saved_suppress_unset =
          cxt.is_warning_suppressed(suppressible_warning::UnsetReference);
      let const saved_suppress_test_operand =
          cxt.is_warning_suppressed(suppressible_warning::UnsetTestOperand);
      cxt.set_warning_suppressed(suppressible_warning::UnsetTestOperand, true);
      if (is_existence_test)
        cxt.set_warning_suppressed(suppressible_warning::UnsetReference, true);
      defer
      {
        cxt.set_warning_suppressed(suppressible_warning::UnsetReference,
                                   saved_suppress_unset);
        cxt.set_warning_suppressed(suppressible_warning::UnsetTestOperand,
                                   saved_suppress_test_operand);
      };
      let const operand = operand_value(elements[pos - 1]);
      return eval_unary(*selected_unary_operator, operand.view());
    }

    if (pos + 1 < elements.count()) {
      let const next = kind_at(pos + 1);
      if (next == Kind::Less || next == Kind::Greater) {
        if (pos + 2 >= elements.count() || kind_at(pos + 2) != Kind::Operand) {
          fail_conditional("An operand is missing after a comparison");
        }
        pos += 3;
        if (is_skipping) return false;
        let const left = operand_value(elements[pos - 3]);
        let const right = operand_value(elements[pos - 1]);
        let const order = os::collate_compare(left, right);
        return next == Kind::Less ? order < 0 : order > 0;
      }
      if (next == Kind::Operand && elements[pos + 1].is_bare_unquoted) {
        let const op = operand_literal(elements[pos + 1]);
        let const selected_binary_operator = binary_operator(op.view());
        if (selected_binary_operator.has_value()) {
          if (pos + 2 >= elements.count() || kind_at(pos + 2) != Kind::Operand)
          {
            throw Error{"Expected an operand after '" + op + "'"};
          }
          pos += 3;
          if (is_skipping) return false;

          let const is_test_operand_op = [&]() -> bool {
            switch (*selected_binary_operator) {
            case BinaryOperatorKind::ArithmeticEqual:
            case BinaryOperatorKind::ArithmeticNotEqual:
            case BinaryOperatorKind::ArithmeticLess:
            case BinaryOperatorKind::ArithmeticLessEqual:
            case BinaryOperatorKind::ArithmeticGreater:
            case BinaryOperatorKind::ArithmeticGreaterEqual:
            case BinaryOperatorKind::SameFile:
            case BinaryOperatorKind::NewerFile:
            case BinaryOperatorKind::OlderFile: return true;
            case BinaryOperatorKind::PatternEqual:
            case BinaryOperatorKind::PatternNotEqual:
            case BinaryOperatorKind::Regex: return false;
            }
            return false;
          }();
          let const saved_suppress_test_operand =
              cxt.is_warning_suppressed(suppressible_warning::UnsetTestOperand);
          if (is_test_operand_op)
            cxt.set_warning_suppressed(suppressible_warning::UnsetTestOperand,
                                       true);
          defer
          {
            cxt.set_warning_suppressed(suppressible_warning::UnsetTestOperand,
                                       saved_suppress_test_operand);
          };

          let const left = operand_value(elements[pos - 3]);
          if (*selected_binary_operator == BinaryOperatorKind::PatternEqual ||
              *selected_binary_operator == BinaryOperatorKind::PatternNotEqual)
          {
            let active = Bitset{cxt.scratch_allocator()};
            let const pattern =
                operand_pattern_masked(elements[pos - 1], active);
            let const is_case_insensitive = cxt.is_shopt_enabled("nocasematch");
            if (!is_case_insensitive) {
              let const is_matched =
                  utils::glob_matches(pattern.view(), left.view(), active, 0,
                                      cxt.extglob_enabled());
              return *selected_binary_operator ==
                             BinaryOperatorKind::PatternNotEqual
                         ? !is_matched
                         : is_matched;
            }

            let const match_pattern =
                ascii_lower_copy(cxt.scratch_allocator(), pattern.view());
            let const match_value =
                ascii_lower_copy(cxt.scratch_allocator(), left.view());
            let const is_matched =
                utils::glob_matches(match_pattern.view(), match_value.view(),
                                    active, 0, cxt.extglob_enabled());
            return *selected_binary_operator ==
                           BinaryOperatorKind::PatternNotEqual
                       ? !is_matched
                       : is_matched;
          }
          if (*selected_binary_operator == BinaryOperatorKind::Regex) {
            let active = Bitset{cxt.scratch_allocator()};
            const String pattern =
                operand_pattern_masked(elements[pos - 1], active);
            /* A malformed regex throws without a location, so the caret is
               pointed at the regex operand. */
            try {
              return regex_match(left.view(), pattern.view(), active);
            } catch (const ErrorWithLocation &) {
              throw;
            } catch (const Error &err) {
              const conditional_element &operand = elements[pos - 1];
              if (operand.word != nullptr)
                relocate_error(err, operand.word->source_location());
              throw;
            }
          }
          let const right = operand_value(elements[pos - 1]);
          return eval_binary(left.view(), *selected_binary_operator,
                             right.view());
        }
      }
    }

    pos++;
    if (is_skipping) return false;
    let const value = operand_value(elements[pos - 1]);
    return !value.is_empty();
  }

  fn eval_term() throws -> bool
  {
    if (!at_end() && kind_at(pos) == Kind::Not) {
      pos++;
      return !eval_term();
    }
    if (!at_end() && kind_at(pos) == Kind::OpenParen) {
      pos++;
      let const is_inner_true = eval_or();
      if (at_end() || kind_at(pos) != Kind::CloseParen) {
        throw Error{"Expected ')'"};
      }
      pos++;
      return is_inner_true;
    }
    return eval_primary();
  }

  fn eval_and() throws -> bool
  {
    let and_result = eval_term();
    while (!at_end() && kind_at(pos) == Kind::And) {
      pos++;
      let const was_skipping = is_skipping;
      is_skipping = is_skipping || !and_result;
      let const is_right_hand_side_true = eval_term();
      is_skipping = was_skipping;
      and_result = and_result && is_right_hand_side_true;
    }
    return and_result;
  }

  fn eval_or() throws -> bool
  {
    let or_result = eval_and();
    while (!at_end() && kind_at(pos) == Kind::Or) {
      pos++;
      let const was_skipping = is_skipping;
      is_skipping = is_skipping || or_result;
      let const is_right_hand_side_true = eval_and();
      is_skipping = was_skipping;
      or_result = or_result || is_right_hand_side_true;
    }
    return or_result;
  }
};

} /* namespace */

static constexpr usize REGEX_CACHE_CAP = 128;

fn EvalContext::cached_compiled_regex(StringView pattern) throws
    -> os::compiled_regex *
{
  let const is_case_insensitive = is_shopt_enabled("nocasematch");
  let key = String{scratch_allocator()};
  key.reserve(pattern.length + 1);
  key += is_case_insensitive ? 'i' : 's';
  key += pattern;

  if (CompiledRegex *cached = m_regex_cache.find(key.view()); cached != nullptr)
  {
    LOG(All, "regex cache hit for the pattern '%.*s'",
        static_cast<int>(pattern.length), pattern.data);
    return cached->get();
  }

  if (m_regex_cache.count() >= REGEX_CACHE_CAP) {
    LOG(Debug, "regex cache full, dropping %zu compiled patterns",
        m_regex_cache.count());
    m_regex_cache.clear();
  }

  LOG(Debug, "regex cache miss, compiling the pattern '%.*s'",
      static_cast<int>(pattern.length), pattern.data);
  let const pattern_text = String{scratch_allocator(), pattern};
  os::compiled_regex compiled;
  if (os::compile_regex(pattern_text.view(),
                        is_case_insensitive ? os::case_sensitivity::Insensitive
                                            : os::case_sensitivity::Sensitive,
                        compiled) != os::regex_compile_result::Ok)
  {
    let reason = String{scratch_allocator()};
    reason += "The regular expression '";
    reason += pattern;
    reason += "' is invalid";
    fail_conditional(reason.view(),
                     "The pattern must be a valid extended regular expression");
  }
  return m_regex_cache.set(key.view(), CompiledRegex{compiled})->get();
}

fn EvalContext::evaluate_conditional(
    const ArrayList<conditional_element> &elements) throws -> bool
{
  if (elements.is_empty())
    fail_conditional("The conditional expression is empty");
  LOG(Debug, "evaluating a [[ ]] conditional of %zu elements",
      elements.count());

  let evaluator = conditional_evaluator{*this, elements};
  let const is_conditional_true = evaluator.eval_or();
  if (!evaluator.at_end()) {
    fail_conditional_syntax(
        "The token '" + evaluator.unexpected_token() +
        "' came after a complete conditional, so it may be an "
        "operator the shell does not support or a missing && or || "
        "between two tests");
  }
  return is_conditional_true;
}

} /* namespace koshka */
