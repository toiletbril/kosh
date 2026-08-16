#include "Builtin.hpp"
#include "Cli.hpp"
#include "Containers.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace utils {

/* Inspiration taken from https://github.com/tsoding/glob.h :3
 * This fragment is under MIT License (c) Alexey Kutepov <reximkut@gmail.com> */
static pure fn is_glob_char_active(const Bitset &glob_active,
                                   usize index) wontthrow -> bool
{
  return index < glob_active.count() && glob_active[index];
}

namespace {

/* One alternative of a bash extended-glob group, a slice of the glob and the
   mask offset that slice begins at. */
struct extglob_alternative
{
  StringView pattern;
  usize mask_offset;
};

hot fn extglob_active(const Bitset &mask, usize index) wontthrow -> bool
{
  return index < mask.count() ? mask[index] : true;
}

/* True when glob at index opens an extended-glob group, one of ?, *, +, @, or !
   immediately followed by (. The caller has opted into extglob, so the group
   structure is read from the text rather than the metacharacter mask, which
   only distinguishes a leaf star or bracket from a quoted literal. */
fn extglob_opens_group(StringView glob, usize index) wontthrow -> bool
{
  if (index + 1 >= glob.count()) return false;
  let const op = glob[index];
  if (op != '?' && op != '*' && op != '+' && op != '@' && op != '!') {
    return false;
  }
  return glob[index + 1] == '(';
}

/* The index of the ) that closes the group whose ( sits at glob[1], tracking
   nested groups by text. Returns glob.count() when the group is unbalanced. */
fn extglob_group_close(StringView glob) wontthrow -> usize
{
  usize depth = 0;
  for (usize i = 1; i < glob.count(); i++) {
    if (glob[i] == '(')
      depth++;
    else if (glob[i] == ')') {
      depth--;
      if (depth == 0) return i;
    }
  }
  return glob.count();
}

fn extglob_full_match(StringView glob, StringView str, const Bitset &mask,
                      usize mask_offset) throws -> bool;

/* Match min_reps or more repetitions of one of the alternatives against the
   front of str, then the suffix against the rest. The min drops to zero after
   the first repetition, so a + needs one and a * needs none. */
fn extglob_match_repetition(const ArrayList<extglob_alternative> &alternatives,
                            StringView suffix, usize suffix_offset,
                            StringView str, const Bitset &mask,
                            usize min_reps) throws -> bool
{
  if (min_reps == 0 && extglob_full_match(suffix, str, mask, suffix_offset)) {
    return true;
  }
  for (let const &alternative : alternatives) {
    for (usize length = 1; length <= str.count(); length++) {
      if (!extglob_full_match(alternative.pattern,
                              str.substring_of_length(0, length), mask,
                              alternative.mask_offset))
        continue;
      const usize next_min = min_reps > 0 ? min_reps - 1 : 0;
      if (extglob_match_repetition(alternatives, suffix, suffix_offset,
                                   str.substring(length), mask, next_min))
        return true;
    }
  }
  return false;
}

fn extglob_full_match(StringView glob, StringView str, const Bitset &mask,
                      usize mask_offset) throws -> bool
{
  if (glob.is_empty()) return str.is_empty();

  let const is_active = extglob_active(mask, mask_offset);
  let const head = glob[0];

  /* An extended-glob group such as @(a|b), *(a|b), or !(a) drives the match
     through the alternatives split on the top-level |. */
  if (extglob_opens_group(glob, 0)) {
    const usize close = extglob_group_close(glob);
    if (close < glob.count()) {
      const StringView content = glob.substring_of_length(2, close - 2);
      const usize content_offset = mask_offset + 2;
      const StringView suffix = glob.substring(close + 1);
      const usize suffix_offset = mask_offset + close + 1;

      let alternatives = ArrayList<extglob_alternative>{heap_allocator()};
      usize depth = 0;
      usize start = 0;
      for (usize i = 0; i <= content.count(); i++) {
        let const is_boundary =
            i == content.count() || (content[i] == '|' && depth == 0);
        if (is_boundary) {
          alternatives.push({content.substring_of_length(start, i - start),
                             content_offset + start});
          start = i + 1;
        } else if (content[i] == '(')
          depth++;
        else if (content[i] == ')')
          depth--;
      }

      switch (head) {
      case '*':
        return extglob_match_repetition(alternatives, suffix, suffix_offset,
                                        str, mask, 0);
      case '+':
        return extglob_match_repetition(alternatives, suffix, suffix_offset,
                                        str, mask, 1);
      case '?':
      case '@':
        for (let const &alternative : alternatives) {
          for (usize length = head == '?' ? 0 : 1; length <= str.count();
               length++)
          {
            if (extglob_full_match(alternative.pattern,
                                   str.substring_of_length(0, length), mask,
                                   alternative.mask_offset) &&
                extglob_full_match(suffix, str.substring(length), mask,
                                   suffix_offset))
              return true;
          }
        }
        /* A ? group also matches zero occurrences, so the suffix may follow
           with nothing consumed. */
        return head == '?' &&
               extglob_full_match(suffix, str, mask, suffix_offset);
      case '!':
        /* A negated group consumes a prefix that none of the alternatives
           match, then the suffix matches the rest. */
        for (usize length = 0; length <= str.count(); length++) {
          bool has_matching_alternative = false;
          for (let const &alternative : alternatives) {
            if (extglob_full_match(alternative.pattern,
                                   str.substring_of_length(0, length), mask,
                                   alternative.mask_offset))
            {
              has_matching_alternative = true;
              break;
            }
          }
          if (!has_matching_alternative &&
              extglob_full_match(suffix, str.substring(length), mask,
                                 suffix_offset))
          {
            return true;
          }
        }
        return false;
      default: break;
      }
    }
  }

  /* A trailing * matches the rest of the string, so it is taken without trying
     every split. */
  if (is_active && head == '*') {
    for (usize eaten = 0; eaten <= str.count(); eaten++) {
      if (extglob_full_match(glob.substring(1), str.substring(eaten), mask,
                             mask_offset + 1))
        return true;
    }
    return false;
  }

  if (str.is_empty()) return false;

  if (is_active && head == '?') {
    return extglob_full_match(glob.substring(1), str.substring(1), mask,
                              mask_offset + 1);
  }

  if (is_active && head == '[') {
    /* Reuse the iterative matcher for a single bracket class by matching one
       character, then continue with the rest of the glob and the string. */
    usize span = 1;
    while (span < glob.count() &&
           !(glob[span] == ']' && extglob_active(mask, mask_offset + span)))
      span++;
    if (span < glob.count()) {
      span++;
      let const did_class_match =
          glob_matches(glob.substring_of_length(0, span),
                       str.substring_of_length(0, 1), mask, mask_offset);
      if (!did_class_match) return false;
      return extglob_full_match(glob.substring(span), str.substring(1), mask,
                                mask_offset + span);
    }
  }

  if (str[0] != head) return false;
  return extglob_full_match(glob.substring(1), str.substring(1), mask,
                            mask_offset + 1);
}

/* The POSIX character classes a bracket accepts as [:name:], each name bound
   to its ctype predicate through a packed-key map so the glob hot path pays
   a word compare rather than a name chain. The wrappers pin the byte through
   unsigned char, the only argument range the ctype functions define. */
using posix_class_test = bool (*)(u8 byte);

constexpr static_string_entry<posix_class_test> POSIX_CLASS_ENTRIES[] = {
    {SSK("alnum"),  [](u8 byte) { return std::isalnum(byte) != 0; } },
    {SSK("alpha"),  [](u8 byte) { return std::isalpha(byte) != 0; } },
    {SSK("blank"),  [](u8 byte) { return std::isblank(byte) != 0; } },
    {SSK("cntrl"),  [](u8 byte) { return std::iscntrl(byte) != 0; } },
    {SSK("digit"),  [](u8 byte) { return std::isdigit(byte) != 0; } },
    {SSK("graph"),  [](u8 byte) { return std::isgraph(byte) != 0; } },
    {SSK("lower"),  [](u8 byte) { return std::islower(byte) != 0; } },
    {SSK("print"),  [](u8 byte) { return std::isprint(byte) != 0; } },
    {SSK("punct"),  [](u8 byte) { return std::ispunct(byte) != 0; } },
    {SSK("space"),  [](u8 byte) { return std::isspace(byte) != 0; } },
    {SSK("upper"),  [](u8 byte) { return std::isupper(byte) != 0; } },
    {SSK("xdigit"), [](u8 byte) { return std::isxdigit(byte) != 0; }},
};

constexpr StaticStringMap POSIX_CLASSES{POSIX_CLASS_ENTRIES};

/* Whether the byte belongs to the named class. An unknown name matches
   nothing, the way bash treats a class it does not know. */
fn byte_is_in_posix_class(StringView class_name, u8 byte) throws -> bool
{
  if (const Maybe<posix_class_test> test = POSIX_CLASSES.find(class_name);
      test.has_value())
    return (*test)(byte);
  return false;
}

} /* namespace */

pure fn token_has_uppercase(StringView token) wontthrow -> bool
{
  for (usize position = 0; position < token.length; position++)
    if (token[position] >= 'A' && token[position] <= 'Z') return true;
  return false;
}

pure fn smart_case_prefix_matches(StringView candidate,
                                  StringView prefix) wontthrow -> bool
{
  if (candidate.starts_with(prefix)) return true;
  if (token_has_uppercase(prefix) || candidate.length < prefix.length) {
    return false;
  }

  for (usize position = 0; position < prefix.length; position++)
    if (ascii_to_lower(candidate[position]) != ascii_to_lower(prefix[position]))
      return false;

  return true;
}

fn glob_matches(StringView glob, StringView str, const Bitset &glob_active,
                usize mask_offset, bool extglob) throws -> bool
{
  /* The extended-glob grammar needs backtracking over alternatives and
     repetition, so it runs in a separate recursive matcher. It is taken only
     when extglob is on and the pattern actually holds a group, so a plain glob
     keeps the iterative matcher below, unchanged, and pays nothing. */
  if (extglob) {
    for (usize i = 0; i + 1 < glob.count(); i++) {
      let const c = glob[i];
      if ((c == '?' || c == '*' || c == '+' || c == '@' || c == '!') &&
          glob[i + 1] == '(')
      {
        return extglob_full_match(glob, str, glob_active, mask_offset);
      }
    }
  }

  usize s = 0;
  usize g = 0;
  usize star_glob_position = static_cast<usize>(-1);
  usize star_string_position = 0;

  while (s < str.count()) {
    if (g >= glob.count()) goto retry_star;
    ASSERT(g < glob.count() && s < str.count());

    if (!is_glob_char_active(glob_active, mask_offset + g)) {
      if (glob[g] != str[s]) goto retry_star;
      g++;
      s++;
      continue;
    }

    switch (glob[g]) {
    case '?': {
      g++;
      s++;
    } break;

    case '*': {
      while (g < glob.count() && glob[g] == '*' &&
             is_glob_char_active(glob_active, mask_offset + g))
        g++;
      if (g >= glob.count()) return true;
      star_glob_position = g;
      star_string_position = s;
    } break;

    case '[': {
      bool is_matched = false;
      bool should_negate = false;

      /* clang-format off */
#define GLOB_GROUP_ERR()                                                       \
  throw ErrorWithLocationAndDetails{                                           \
      {0, 0},                               \
      "Unclosed '[' group",                                                    \
      {0, 1},                                                \
      "expected ] here"                                                        \
  };
      /* clang-format on */

      /* A bracket member, a class terminator ], a negating ! or ^, and a range
         '-' carry their special meaning only when the byte is an active glob
         character. A quoted or escaped ] inside the class is a literal member,
         not the terminator, and a quoted member byte never opens a range, so
         the scan consults the same per-byte mask the rest of the matcher reads.
       */
      let const do_is_active = [&](usize index) wontthrow -> bool {
        return is_glob_char_active(glob_active, mask_offset + index);
      };
      let const do_is_close_at = [&](usize index) wontthrow -> bool {
        return glob[index] == ']' && do_is_active(index);
      };

      /* The unsigned value of a byte, so a high byte at or above 0x80 compares
         as itself rather than as a negative char in the range and equality
         tests. */
      let const do_get_byte_at =
          [](StringView view, usize index)
              wontthrow -> u8 { return static_cast<u8>(view[index]); };

      /* A [:name:] unit inside the bracket is a POSIX character class. The
         index past its closing ":]" comes back when one starts here, so both
         scans treat the unit atomically and its inner ] never closes the
         bracket. */
      let const do_get_class_end_past = [&](usize index)
                                            wontthrow -> Maybe<usize> {
        if (index + 1 >= glob.count() || glob[index] != '[' ||
            glob[index + 1] != ':' || !do_is_active(index))
          return None;
        for (usize scan = index + 2; scan + 1 < glob.count(); scan++) {
          if (glob[scan] == ':' && glob[scan + 1] == ']') return scan + 2;
          /* A ] before any ":]" means the [ was a plain member after all, the
             way [[:a] is a bracket holding [, :, and a. */
          if (glob[scan] == ']' && do_is_active(scan)) return None;
        }
        return None;
      };

      /* A bracket with no closing ] is not a character class, so the [ is a
         literal character, as POSIX specifies. A ] right after [ or [^ is a
         member, so the scan for the closing ] starts past it. */
      usize close_scan = g + 1;
      if (close_scan < glob.count() &&
          (glob[close_scan] == '!' || glob[close_scan] == '^') &&
          do_is_active(close_scan))
      {
        close_scan++;
      }
      if (close_scan < glob.count() && do_is_close_at(close_scan)) {
        close_scan++;
      }
      bool has_closing_bracket = false;
      while (close_scan < glob.count()) {
        if (Maybe<usize> past_class = do_get_class_end_past(close_scan);
            past_class.has_value())
        {
          close_scan = *past_class;
          continue;
        }
        if (do_is_close_at(close_scan)) {
          has_closing_bracket = true;
          break;
        }
        close_scan++;
      }
      if (!has_closing_bracket) {
        if (do_get_byte_at(glob, g) != do_get_byte_at(str, s)) goto retry_star;
        g++;
        s++;
        break;
      }

      g++;
      if (g >= glob.count()) GLOB_GROUP_ERR();

      /* POSIX sh negates a class with a leading '!'. The '^' form is kept as a
         common extension. The negation applies only to an active byte, so a
         quoted ! or ^ at the front is a literal member. */
      if ((glob[g] == '!' || glob[g] == '^') && do_is_active(g)) {
        g++;
        should_negate = true;

        if (g >= glob.count()) GLOB_GROUP_ERR();
      }

      /* The first member bypasses the close check, so a leading ] is a plain
         member. A range is consumed as one atom, so its first endpoint does not
         also match by itself and a later hyphen remains a literal member. */
      bool is_first_member = true;
      while (g < glob.count() && (is_first_member || !do_is_close_at(g))) {
        if (Maybe<usize> past_class = do_get_class_end_past(g);
            past_class.has_value())
        {
          let const class_name =
              glob.substring_of_length(g + 2, *past_class - g - 4);
          is_matched |=
              byte_is_in_posix_class(class_name, do_get_byte_at(str, s));
          g = *past_class;
          is_first_member = false;
          continue;
        }
        if (glob[g] != '-' && g + 2 < glob.count() && glob[g + 1] == '-' &&
            do_is_active(g + 1) && !do_is_close_at(g + 2) &&
            !do_get_class_end_past(g + 2).has_value())
        {
          let const lower = do_get_byte_at(glob, g);
          let const upper = do_get_byte_at(glob, g + 2);
          is_matched |= lower <= do_get_byte_at(str, s) &&
                        do_get_byte_at(str, s) <= upper;
          g += 3;
        } else {
          is_matched |= do_get_byte_at(glob, g) == do_get_byte_at(str, s);
          g++;
        }
        is_first_member = false;
      }

      if (g >= glob.count() || !do_is_close_at(g)) {
        GLOB_GROUP_ERR();
      }
      if (should_negate) is_matched = !is_matched;
      if (!is_matched) goto retry_star;

      g++;
      s++;
    } break;

    default:
      if (glob[g] != str[s]) goto retry_star;
      g++;
      s++;
    }
    continue;

retry_star:
    if (star_glob_position == static_cast<usize>(-1) ||
        star_string_position >= str.count())
      return false;
    star_string_position++;
    s = star_string_position;
    g = star_glob_position;
  }

  if (s >= str.count()) {
    while (g < glob.count() && glob[g] == '*' &&
           is_glob_char_active(glob_active, mask_offset + g))
    {
      g++;
    }

    if (g >= glob.count()) return true;
  }

  return false;
}

} /* namespace utils */

} /* namespace koshka */
