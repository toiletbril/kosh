/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements parameter expansion operators for default values,
 * assignments, errors, lengths, substrings, trimming, replacement, quoting,
 * and case changes. The same operators apply to scalar and array values. The
 * split keeps parameter syntax and transformations outside the word-expansion
 * coordinator.
 */

#include "Arena.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

fn compute_substring_bounds(i64 value_count, i64 offset, Maybe<i64> length,
                            substring_subject subject) throws
    -> substring_bounds
{
  i64 start = offset < 0 && offset < -value_count ? -1
              : offset < 0                        ? value_count + offset
                                                  : offset;
  if (start < 0) {
    if (subject == substring_subject::Scalar) return substring_bounds{0, 0};
    start = 0;
  }
  if (start > value_count) start = value_count;

  i64 end = value_count;
  if (length.has_value()) {
    if (*length < 0) {
      if (subject == substring_subject::List)
        throw Error{"Unable to take the substring because the length names "
                    "a point before the offset"};
      end = *length < -value_count ? -1 : value_count + *length;
    } else {
      let const clamped_length = *length > value_count ? value_count : *length;
      end = clamped_length > value_count - start ? value_count
                                                 : start + clamped_length;
    }
  }
  if (end > value_count) end = value_count;
  if (end < start)
    throw Error{"Unable to take the substring because the length names "
                "a point before the offset"};

  return substring_bounds{start, end};
}

namespace {

static fn source_location_for_subview(
    const SourceLocation *source_location, StringView source, StringView part,
    SourceLocation &storage, usize source_location_offset = 0) wontthrow
    -> const SourceLocation *
{
  if (source_location == nullptr) return nullptr;
  return source_location->subspan_for_view(source, part, storage,
                                           source_location_offset);
}

enum class trim_end : u8
{
  Prefix,
  Suffix,
};

/* The active mask marks which pattern bytes may act as glob metacharacters, so
   a quoted or escaped * or ? matches itself. */
fn trim_matching(Allocator result_allocator, StringView value,
                 StringView pattern, const Bitset &active, trim_end end,
                 bool longest, bool extglob_enabled) throws -> String
{
  ASSERT(active.count() == pattern.length);

  if (end == trim_end::Prefix) {
    if (longest) {
      for (usize length = value.length;; length--) {
        if (utils::glob_matches(pattern, value.substring_of_length(0, length),
                                active, 0, extglob_enabled))
          return String{result_allocator, value.substring(length)};
        if (length == 0) break;
      }
    } else {
      for (usize length = 0; length <= value.length; length++) {
        if (utils::glob_matches(pattern, value.substring_of_length(0, length),
                                active, 0, extglob_enabled))
          return String{result_allocator, value.substring(length)};
      }
    }

  } else {
    if (longest) {
      for (usize start = 0; start <= value.length; start++) {
        if (utils::glob_matches(pattern, value.substring(start), active, 0,
                                extglob_enabled))
          return String{result_allocator, value.substring_of_length(0, start)};
      }
    } else {
      for (usize start = value.length;; start--) {
        if (utils::glob_matches(pattern, value.substring(start), active, 0,
                                extglob_enabled))
          return String{result_allocator, value.substring_of_length(0, start)};
        if (start == 0) break;
      }
    }
  }
  return String{result_allocator, value};
}

static fn
trim_value_with_modifier(EvalContext &cxt, StringView value, StringView word,
                         trim_end end, bool longest,
                         const SourceLocation *source_location = nullptr) throws
    -> String
{
  LOG(All, "trimming a value of %zu bytes with the pattern word '%.*s'",
      value.length, static_cast<int>(word.length), word.data);
  let active = Bitset{cxt.scratch_allocator()};
  let const pattern =
      cxt.expand_modifier_word_masked(word, active, true, source_location);
  return trim_matching(cxt.scratch_allocator(), value, pattern.view(), active,
                       end, longest, cxt.extglob_enabled());
}

} /* namespace */

fn EvalContext::expand_modifier_word(
    StringView word, bool remove_quotes, bool strip_escaped_literals,
    const SourceLocation *source_location) throws -> String
{
  return expand_modifier_word_worker(word, nullptr, remove_quotes, false,
                                     strip_escaped_literals, source_location);
}

fn EvalContext::expand_modifier_word_masked(
    StringView word, Bitset &active_out, bool remove_quotes,
    const SourceLocation *source_location) throws -> String
{
  return expand_modifier_word_worker(word, &active_out, remove_quotes, true,
                                     false, source_location);
}

fn EvalContext::expand_modifier_word_worker(
    StringView word, Bitset *active_out, bool remove_quotes,
    bool is_pattern_word, bool strip_escaped_literals,
    const SourceLocation *source_location) throws -> String
{
  LOG(All, "expanding a modifier word of %zu bytes", word.length);
  let out = String{scratch_allocator()};

  let const do_emit_byte = [&](char byte, bool is_active) {
    out += byte;
    if (active_out != nullptr) active_out->push(is_active);
  };

  let const do_emit_run = [&](StringView bytes, bool is_active) {
    out.append(bytes);
    if (active_out != nullptr) {
      for (usize k = 0; k < bytes.length; k++)
        active_out->push(is_active);
    }
  };

  let is_in_single_quote = false;
  let is_in_double_quote = false;
  for (usize i = 0; i < word.length; i++) {
    /* A heredoc body passes remove_quotes as false, so its quotes stay. */
    if (remove_quotes && !is_in_single_quote && word[i] == '"') {
      is_in_double_quote = !is_in_double_quote;
      continue;
    }
    if (remove_quotes && !is_in_double_quote && word[i] == '\'') {
      is_in_single_quote = !is_in_single_quote;
      continue;
    }
    if (is_in_single_quote) {
      do_emit_byte(word[i], false);
      continue;
    }

    if (word[i] == '\\') {
      /* In a # or % pattern word a backslash quotes the next byte so a quoted
         glob character such as \* matches itself. */
      if (is_pattern_word && i + 1 < word.length) {
        do_emit_byte(word[i + 1], false);
        i++;
        continue;
      }
      if (i + 1 < word.length) {
        let const next = word[i + 1];
        if (next == '\n') {
          i++;
          continue;
        }
        if (strip_escaped_literals && remove_quotes && !is_in_double_quote) {
          do_emit_byte(next, false);
          i++;
          continue;
        }
        if (next == '$' || next == '`' || next == '\\' ||
            (remove_quotes && next == '"'))
        {
          do_emit_byte(next, false);
          i++;
          continue;
        }
      }
      do_emit_byte('\\', false);
      continue;
    }

    if (word[i] == '`') {
      /* The POSIX backquote unescaping strips a backslash before a backtick, a
         dollar sign, or another backslash. */
      let inner = String{scratch_allocator()};
      usize j = i + 1;
      for (; j < word.length; j++) {
        if (word[j] == '\\' && j + 1 < word.length &&
            (word[j + 1] == '`' || word[j + 1] == '$' || word[j + 1] == '\\'))
        {
          inner += word[j + 1];
          j++;
          continue;
        }
        if (word[j] == '`') break;
        inner += word[j];
      }
      let call_site = SourceLocation{};
      const SourceLocation *call_site_pointer = nullptr;
      if (source_location != nullptr) {
        call_site = source_location->subspan(i, j - i + (j < word.length));
        call_site_pointer = &call_site;
      }
      do_emit_run(capture_command_substitution(inner, None, call_site_pointer),
                  !is_in_double_quote);
      i = j;
      continue;
    }

    if (word[i] != '$') {
      do_emit_byte(word[i], !is_in_double_quote);
      continue;
    }
    if (i + 1 >= word.length) {
      do_emit_byte('$', !is_in_double_quote);
      break;
    }

    let const next = word[i + 1];
    if (next == '\'' && remove_quotes && !is_in_double_quote &&
        !is_posix_mode())
    {
      let body = String{scratch_allocator()};
      usize j = i + 2;
      while (j < word.length && word[j] != '\'') {
        body.push(word[j]);
        if (word[j] == '\\' && j + 1 < word.length) {
          body.push(word[j + 1]);
          j++;
        }
        j++;
      }
      let decoded = String{scratch_allocator()};
      utils::decode_ansi_c_escapes(decoded, body.view());
      do_emit_run(decoded.view(), false);
      i = j;
      continue;
    }
    if (next == '{') {
      /* Scan the ${...} body to the matching } at brace depth one. A quote run
         or a backslash escape keeps its bytes literal so a } inside is never
         counted. */
      let inner = String{scratch_allocator()};
      usize j = i + 2;
      i32 depth = 1;
      char quote = 0;
      while (j < word.length) {
        let const ch = word[j];
        if (quote != 0) {
          inner += ch;
          if (quote == '"' && ch == '\\' && j + 1 < word.length) {
            inner += word[++j];
            j++;
            continue;
          }
          if (ch == quote) quote = 0;
          j++;
          continue;
        }
        if (ch == '\\' && j + 1 < word.length) {
          inner += ch;
          inner += word[++j];
          j++;
          continue;
        }
        if (ch == '\'' || ch == '"') {
          quote = ch;
          inner += ch;
          j++;
          continue;
        }
        if (ch == '`') {
          inner += ch;
          j++;
          while (j < word.length) {
            let const b = word[j];
            inner += b;
            j++;
            if (b == '\\' && j < word.length) {
              inner += word[j++];
              continue;
            }
            if (b == '`') break;
          }
          continue;
        }
        if (ch == '$' && j + 1 < word.length && word[j + 1] == '(') {
          inner += ch;
          inner += word[++j];
          j++;
          usize paren_depth = 1;
          char nested_quote = 0;
          while (j < word.length) {
            let const p = word[j];
            inner += p;
            j++;
            if (nested_quote != 0) {
              if (nested_quote == '"' && p == '\\' && j < word.length) {
                inner += word[j++];
                continue;
              }
              if (p == nested_quote) nested_quote = 0;
              continue;
            }
            if (p == '\\' && j < word.length) {
              inner += word[j++];
              continue;
            }
            if (p == '\'' || p == '"') {
              nested_quote = p;
            } else if (p == '(') {
              paren_depth++;
            } else if (p == ')') {
              paren_depth--;
              if (paren_depth == 0) break;
            }
          }
          continue;
        }
        if (ch == '$' && j + 1 < word.length && word[j + 1] == '{') {
          depth++;
          inner += ch;
          inner += word[++j];
          j++;
          continue;
        }
        if (ch == '}') {
          depth--;
          if (depth == 0) break;
          inner += ch;
          j++;
          continue;
        }
        inner += ch;
        j++;
      }
      let inner_location = SourceLocation{};
      const SourceLocation *inner_location_pointer = nullptr;
      if (source_location != nullptr) {
        inner_location = source_location->subspan(i + 2, inner.count());
        inner_location_pointer = &inner_location;
      }
      do_emit_run(apply_parameter_expansion(inner, inner_location_pointer),
                  !is_in_double_quote);
      i = j;
    } else if (lexer::is_variable_name_start(next)) {
      let name = String{scratch_allocator()};
      usize j = i + 1;
      while (j < word.length && lexer::is_variable_name(word[j]))
        name += word[j++];
      /* A nested reference obeys set -u the way a top level reference does. A
         stored name resolves without the extra dynamic-value lookup and copy.
       */
      let const *stored = lookup_shell_variable(name);
      if (stored != nullptr) {
        do_emit_run(stored->view(), !is_in_double_quote);
      } else {
        let value = get_variable_value(name);
        if (!value.has_value()) report_unset_reference(name);
        do_emit_run(value.has_value() ? value->view() : StringView{},
                    !is_in_double_quote);
      }
      i = j - 1;
    } else if (next == '(' && i + 2 < word.length && word[i + 2] == '(') {
      /* Arithmetic $((...)), scanned to the matching )). A quote run keeps its
         bytes literal so a ) inside a string does not count. */
      let inner = String{scratch_allocator()};
      usize j = i + 3;
      usize depth = 0;
      char quote = 0;
      for (; j < word.length; j++) {
        let const ch = word[j];
        if (quote != 0) {
          inner += ch;
          if (quote == '"' && ch == '\\' && j + 1 < word.length) {
            inner += word[++j];
            continue;
          }
          if (ch == quote) quote = 0;
          continue;
        }
        if (ch == '\\' && j + 1 < word.length) {
          inner += ch;
          inner += word[++j];
          continue;
        }
        if (ch == '\'' || ch == '"') {
          quote = ch;
        } else if (ch == '(') {
          depth++;
        } else if (ch == ')' && depth > 0) {
          depth--;
        } else if (ch == ')' && j + 1 < word.length && word[j + 1] == ')') {
          j += 2;
          break;
        }
        inner += ch;
      }
      let inner_location = SourceLocation{};
      let const inner_source = word.substring_of_length(i + 3, inner.count());
      do_emit_run(
          evaluate_arithmetic_text(
              inner, source_location_for_subview(source_location, word,
                                                 inner_source, inner_location)),
          false);
      i = j - 1;
    } else if (next == '(') {
      /* Command substitution $(...), scanned to the matching ). A quote run
         keeps its bytes literal so a ) inside a string does not close early. */
      let inner = String{scratch_allocator()};
      usize j = i + 2;
      usize depth = 1;
      char quote = 0;
      for (; j < word.length; j++) {
        let const ch = word[j];
        if (quote != 0) {
          inner += ch;
          if (quote == '"' && ch == '\\' && j + 1 < word.length) {
            inner += word[++j];
            continue;
          }
          if (ch == quote) quote = 0;
          continue;
        }
        if (ch == '\\' && j + 1 < word.length) {
          inner += ch;
          inner += word[++j];
          continue;
        }
        if (ch == '\'' || ch == '"') {
          quote = ch;
        } else if (ch == '(') {
          depth++;
        } else if (ch == ')') {
          depth--;
          if (depth == 0) break;
        }
        inner += ch;
      }
      let call_site = SourceLocation{};
      const SourceLocation *call_site_pointer = nullptr;
      if (source_location != nullptr) {
        call_site = source_location->subspan(i, j - i + (j < word.length));
        call_site_pointer = &call_site;
      }
      do_emit_run(capture_command_substitution(inner, None, call_site_pointer),
                  !is_in_double_quote);
      i = j;
    } else if (next == '?' || next == '@' || next == '*' || next == '#' ||
               next == '$' || next == '!' || next == '-' ||
               lexer::is_number(next))
    {
      let const special_name = StringView{&next, 1};
      if (!get_variable_value(special_name).has_value())
        report_unset_reference(special_name);
      do_emit_run(expand_variable(special_name), !is_in_double_quote);
      i++;
    } else {
      do_emit_byte('$', !is_in_double_quote);
    }
  }
  return out;
}

hot fn EvalContext::apply_parameter_expansion(
    StringView spec, const SourceLocation *source_location,
    usize source_location_offset) throws -> String
{
  LOG(All, "applying the parameter expansion '${%.*s}'",
      static_cast<int>(spec.length), spec.data);

  /* A nested ${name:-${...}} default re-enters this dispatch at each level, so
     the depth is capped before the native stack is exhausted. */
  enter_parameter_expansion();
  defer { leave_parameter_expansion(); };

  let const do_source_location_for =
      [&](StringView part, SourceLocation &storage) -> const SourceLocation * {
    return source_location_for_subview(source_location, spec, part, storage,
                                       source_location_offset);
  };
  let const do_expand_modifier_word = [&](StringView word) throws -> String {
    let word_location = SourceLocation{};
    return expand_modifier_word(word, true, true,
                                do_source_location_for(word, word_location));
  };

  if (spec.is_empty()) return String{scratch_allocator()};

  /* ${!name} indirection, or a prefix listing when it ends with * or @. */
  if (spec.length > 1 && spec[0] == '!') {
    let const body = spec.substring(1);
    /* A modifier after the name applies to the indirected value, the bare
       trailing * and @ stay with the body as the prefix-listing forms. */
    usize name_end = 0;
    while (name_end < body.length && lexer::is_variable_name(body[name_end]))
      name_end++;
    if (name_end > 0 && name_end < body.length && body[name_end] == '[') {
      if (let const close = body.substring(name_end).find_character(']'))
        name_end += *close + 1;
    }
    if (name_end > 0 && name_end < body.length &&
        !(name_end == body.length - 1 &&
          (body[name_end] == '*' || body[name_end] == '@')))
    {
      let const name = body.substring_of_length(0, name_end);
      let const target = get_variable_value(name);
      let const target_name = target.has_value() ? target->view() : name;
      let const suffix = body.substring(name_end);
      let rewritten = String{scratch_allocator()};
      rewritten.reserve(target_name.length + suffix.length);
      /* An unset indirection name stands in for the target so the modifier sees
         the unset state, a fatal error would be harsher than bash. */
      rewritten.append(target_name);
      rewritten.append(suffix);
      let suffix_location = SourceLocation{};
      let const *suffix_location_pointer =
          do_source_location_for(suffix, suffix_location);
      return apply_parameter_expansion(
          rewritten.view(), suffix_location_pointer, target_name.length);
    }
    return apply_indirect_or_name_listing(body);
  }

  if (spec.length > 1 && spec[0] == '#') {
    let const name = spec.substring(1);
    if (name == "@" || name == "*") {
      return String::from(m_positional_params.count(), scratch_allocator());
    }

    /* ${#a[@]} is the element count, ${#a[i]} the length of one element. */
    if (let const bracket = name.find_character('[');
        bracket.has_value() && *bracket > 0 && name[name.length - 1] == ']' &&
        lexer::is_variable_name_start(name[0]))
    {
      let const array_name = name.substring_of_length(0, *bracket);
      let const subscript =
          name.substring_of_length(*bracket + 1, name.length - *bracket - 2);
      if (subscript == "@" || subscript == "*") {
        return String::from(array_element_count(array_name),
                            scratch_allocator());
      }
      let subscript_location = SourceLocation{};
      return String::from(
          apply_array_subscript(
              array_name, subscript,
              do_source_location_for(subscript, subscript_location))
              .length(),
          scratch_allocator());
    }

    if (let const *stored = lookup_shell_variable(name); stored != nullptr)
      return String::from(stored->count(), scratch_allocator());
    let const value = get_variable_value(name);
    if (!value.has_value()) report_unset_reference(name);
    return String::from(value.has_value() ? value->length() : 0,
                        scratch_allocator());
  }

  ASSERT(!spec.is_empty());
  usize name_end = 0;
  if (lexer::is_variable_name_start(spec[0])) {
#pragma clang loop unroll_count(4)
    while (name_end < spec.length && lexer::is_variable_name(spec[name_end]))
      name_end++;
  } else if (lexer::is_number(spec[0])) {
#pragma clang loop unroll_count(4)
    while (name_end < spec.length && lexer::is_number(spec[name_end]))
      name_end++;
  } else {
    name_end = 1;
  }

  let const name = spec.substring_of_length(0, name_end);
  let const rest = spec.substring(name_end);

  if (!rest.is_empty() && rest[0] == '[' && !name.is_empty() &&
      lexer::is_variable_name_start(name[0]))
  {
    if (let const close = rest.find_character(']'); close.has_value()) {
      let const subscript = rest.substring_of_length(1, *close - 1);
      let subscript_location = SourceLocation{};
      let const *subscript_location_pointer =
          do_source_location_for(subscript, subscript_location);
      if (*close + 1 == rest.length)
        return apply_array_subscript(name, subscript,
                                     subscript_location_pointer);
      /* The / # % ^ , modifiers after the ] modify the one element, a different
         modifier such as :- falls through to the general path. */
      let const modifier = rest.substring(*close + 1);
      let modifier_location = SourceLocation{};
      let const *modifier_location_pointer =
          do_source_location_for(modifier, modifier_location);
      let const modifier_op = modifier.is_empty() ? '\0' : modifier[0];
      if (subscript != "@" && subscript != "*" &&
          (modifier_op == '/' || modifier_op == '#' || modifier_op == '%' ||
           modifier_op == '^' || modifier_op == ','))
      {
        return apply_value_modifier(
            apply_array_subscript(name, subscript, subscript_location_pointer)
                .view(),
            modifier, modifier_location_pointer);
      }
      if (subscript != "@" && subscript != "*" && !modifier.is_empty()) {
        let const is_colon = modifier_op == ':';
        let const after =
            is_colon && modifier.length > 1 ? modifier[1] : modifier_op;
        let const is_test_form = is_colon_modifier_operator(after);
        if (is_colon && !is_test_form) {
          let const substring_body = modifier.substring(1);
          let substring_location = SourceLocation{};
          return apply_substring_to_value(
              apply_array_subscript(name, subscript, subscript_location_pointer)
                  .view(),
              substring_body,
              do_source_location_for(substring_body, substring_location));
        }
        if (is_test_form) {
          let const element_is_set = array_element_is_set(name, subscript);
          let value = element_is_set
                          ? apply_array_subscript(name, subscript,
                                                  subscript_location_pointer)
                          : String{scratch_allocator()};
          let const treat_as_unset =
              is_colon ? value.is_empty() : !element_is_set;
          let const word = modifier.substring(is_colon ? 2 : 1);
          switch (after) {
          case '-':
            if (treat_as_unset) return do_expand_modifier_word(word);
            return value;
          case '+':
            if (treat_as_unset) return String{scratch_allocator()};
            return do_expand_modifier_word(word);
          case '=': {
            if (!treat_as_unset) return value;
            let assigned = do_expand_modifier_word(word);
            assign_array_element(name, subscript, assigned.view(), false);
            return assigned;
          }
          case '?':
            if (treat_as_unset) {
              if (word.is_empty())
                throw_script_fatal(
                    "Unable to expand '" + name + "[" + subscript +
                    "]' because the element is not set or is empty");
              throw_script_fatal(do_expand_modifier_word(word));
            }
            return value;
          default: break;
          }
        }
      }
    }
  }

  if (rest.is_empty()) {
    /* A plain reference reports under set -u, a modifier form such as ${x:-w}
       handles the unset case itself. */
    if (let const *stored = lookup_shell_variable(name); stored != nullptr)
      return String{scratch_allocator(), stored->view()};
    let value = get_variable_value(name);
    if (!value.has_value()) report_unset_reference(name);
    if (value.has_value()) return String{scratch_allocator(), value->view()};

    return String{scratch_allocator()};
  }

  /* A leading colon makes the test forms treat an empty value as unset. */
  let const is_colon_form = rest[0] == ':';
  const usize op_index = is_colon_form ? 1 : 0;
  if (op_index >= rest.length) return expand_variable(name);

  if (is_colon_form) {
    let const after_colon = rest[op_index];
    if (!is_colon_modifier_operator(after_colon) && name != "@" && name != "*")
    {
      let const substring_body = rest.substring(1);
      let substring_location = SourceLocation{};
      return apply_substring_expansion(
          name, substring_body,
          do_source_location_for(substring_body, substring_location));
    }
  }

  if (!is_colon_form && rest[0] == '/' && name != "@" && name != "*") {
    let rest_location = SourceLocation{};
    return apply_pattern_replacement(
        name, rest, do_source_location_for(rest, rest_location));
  }

  if (!is_colon_form && (rest[0] == '^' || rest[0] == ',' || rest[0] == '~') &&
      name != "@" && name != "*")
  {
    let rest_location = SourceLocation{};
    return apply_case_modification(name, rest,
                                   do_source_location_for(rest, rest_location));
  }

  if (!is_colon_form && rest[0] == '@' && rest.length >= 2 &&
      mood() != mimic_mood::Posix && name != "@" && name != "*")
  {
    return apply_parameter_transform(name, rest[1]);
  }

  let const op = rest[op_index];
  let const is_doubled = (op_index + 1 < rest.length &&
                          rest[op_index + 1] == op && (op == '#' || op == '%'));
  let const word = rest.substring(op_index + (is_doubled ? 2 : 1));

  let current = get_variable_value(name);
  let const is_set = current.has_value();
  let const is_empty = !is_set || current->is_empty();
  let const treat_as_unset = is_colon_form ? is_empty : !is_set;

  switch (op) {
  case '-':
    if (treat_as_unset) return do_expand_modifier_word(word);
    ASSERT(current.has_value());
    return steal(*current);
  case '=':
    if (treat_as_unset) {
      let const assigned = do_expand_modifier_word(word);
      set_shell_variable(name, assigned);
      return assigned;
    }
    ASSERT(current.has_value());
    return steal(*current);
  case '+':
    if (treat_as_unset) return String{scratch_allocator()};
    return do_expand_modifier_word(word);
  case '?':
    if (treat_as_unset) {
      if (word.is_empty())
        throw_script_fatal("Unable to expand '" + name +
                           "' because the parameter is not set or is empty");
      throw_script_fatal(do_expand_modifier_word(word));
    }
    ASSERT(current.has_value());
    return steal(*current);

  case '#': {
    let word_location = SourceLocation{};
    let const current_view =
        current.has_value() ? current->view() : StringView{};
    return trim_value_with_modifier(
        *this, current_view, word, trim_end::Prefix, is_doubled,
        do_source_location_for(word, word_location));
  }

  case '%': {
    let word_location = SourceLocation{};
    let const current_view =
        current.has_value() ? current->view() : StringView{};
    return trim_value_with_modifier(
        *this, current_view, word, trim_end::Suffix, is_doubled,
        do_source_location_for(word, word_location));
  }

  default: return expand_variable(name);
  }
}

/* The index of the colon that separates the offset from the length, or the body
   length when there is none. Parentheses and a ternary inside the offset are
   tracked so a colon belonging to a ternary is not mistaken for it. */
fn find_substring_length_separator(StringView body) wontthrow -> usize
{
  usize paren_depth = 0;
  usize question_depth = 0;
  for (usize i = 0; i < body.length; i++) {
    let const character = body[i];
    if (character == '(') {
      paren_depth++;
    } else if (character == ')') {
      if (paren_depth > 0) paren_depth--;
    } else if (character == '?' && paren_depth == 0) {
      question_depth++;
    } else if (character == ':' && paren_depth == 0) {
      if (question_depth > 0)
        question_depth--;
      else
        return i;
    }
  }
  return body.length;
}

fn EvalContext::get_variable_value_checked(StringView name) const throws
    -> Maybe<String>
{
  let current = get_variable_value(name);
  if (error_unset() && !current.has_value())
    throw_script_fatal("Unable to expand '" + name +
                       "' because the parameter is not set");
  return current;
}

fn EvalContext::apply_substring_expansion(
    StringView name, StringView body,
    const SourceLocation *source_location) throws -> String
{
  let const current = get_variable_value_checked(name);
  let const current_view = current.has_value() ? current->view() : StringView{};
  return apply_substring_to_value(current_view, body, source_location);
}

fn EvalContext::apply_substring_to_value(
    StringView value, StringView body,
    const SourceLocation *source_location) throws -> String
{
  LOG(All, "taking the substring '%.*s' of a value of %zu bytes",
      static_cast<int>(body.length), body.data, value.length);
  let const value_length = static_cast<i64>(value.length);

  let const separator = find_substring_length_separator(body);
  let const offset_text = body.substring_of_length(0, separator);
  let offset_location = SourceLocation{};
  const i64 offset =
      offset_text.is_empty()
          ? 0
          : evaluate_arithmetic(offset_text, source_location_for_subview(
                                                 source_location, body,
                                                 offset_text, offset_location));

  Maybe<i64> requested_length = None;
  if (separator < body.length) {
    let const length_text = body.substring(separator + 1);
    let length_location = SourceLocation{};
    requested_length =
        length_text.is_empty()
            ? 0
            : evaluate_arithmetic(length_text,
                                  source_location_for_subview(source_location,
                                                              body, length_text,
                                                              length_location));
  }
  let const bounds = compute_substring_bounds(
      value_length, offset, requested_length, substring_subject::Scalar);

  return String{
      scratch_allocator(),
      value.substring_of_length(static_cast<usize>(bounds.start),
                                static_cast<usize>(bounds.end - bounds.start))};
}

/* A slash inside a quote run or behind a backslash belongs to the pattern, the
   way bash reads ${var/#"a/b"/c}, so the scan tracks the quote state. */
static fn find_replacement_separator(StringView body) wontthrow -> usize
{
  char quote = 0;
  for (usize i = 0; i < body.length; i++) {
    let const character = body[i];
    if (quote == '\'') {
      if (character == '\'') quote = 0;
      continue;
    }
    if (character == '\\') {
      i++;
      continue;
    }
    if (quote == '"') {
      if (character == '"') quote = 0;
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = character;
      continue;
    }
    if (character == '/') return i;
  }
  return body.length;
}

static fn longest_pattern_match_at(StringView pattern,
                                   const Bitset &pattern_active,
                                   StringView value, usize start,
                                   bool extglob) throws -> Maybe<usize>
{
  for (usize end = value.length; end >= start; end--) {
    if (utils::glob_matches(pattern,
                            value.substring_of_length(start, end - start),
                            pattern_active, 0, extglob))
      return end - start;
    if (end == start) break;
  }
  return None;
}

fn EvalContext::apply_pattern_replacement(
    StringView name, StringView spec,
    const SourceLocation *source_location) throws -> String
{
  let const current = get_variable_value_checked(name);
  let const current_view = current.has_value() ? current->view() : StringView{};
  return pattern_replace_value(current_view, spec, source_location);
}

/* & reads as the matched span, \& is a literal &, and a backslash before any
   other byte is dropped. */
static fn append_pattern_replacement(String &out, StringView replacement,
                                     StringView matched) throws -> void
{
  for (usize i = 0; i < replacement.length; i++) {
    if (replacement[i] == '\\' && i + 1 < replacement.length) {
      out.push(replacement[i + 1]);
      i++;
    } else if (replacement[i] == '&') {
      out.append(matched);
    } else {
      out.push(replacement[i]);
    }
  }
}

fn EvalContext::pattern_replace_value(
    StringView value, StringView spec,
    const SourceLocation *source_location) throws -> String
{
  LOG(All, "applying the pattern replacement '%.*s' to a value of %zu bytes",
      static_cast<int>(spec.length), spec.data, value.length);
  StringView remainder = spec.substring(1);
  bool should_replace_all = false;
  bool is_anchored_at_start = false;
  bool is_anchored_at_end = false;
  if (!remainder.is_empty() && remainder[0] == '/') {
    should_replace_all = true;
    remainder = remainder.substring(1);
  } else if (!remainder.is_empty() && remainder[0] == '#') {
    is_anchored_at_start = true;
    remainder = remainder.substring(1);
  } else if (!remainder.is_empty() && remainder[0] == '%') {
    is_anchored_at_end = true;
    remainder = remainder.substring(1);
  }

  let const separator = find_replacement_separator(remainder);
  let const pattern_word = remainder.substring_of_length(0, separator);
  let pattern_location = SourceLocation{};
  let pattern_active = Bitset{scratch_allocator()};
  let const pattern = expand_modifier_word_masked(
      pattern_word, pattern_active, true,
      source_location_for_subview(source_location, spec, pattern_word,
                                  pattern_location));
  let replacement_location = SourceLocation{};
  let const replacement =
      separator < remainder.length
          ? expand_modifier_word(
                remainder.substring(separator + 1), true, false,
                source_location_for_subview(source_location, spec,
                                            remainder.substring(separator + 1),
                                            replacement_location))
          : String{heap_allocator()};

  /* An empty unanchored pattern matches nothing in bash, so the value is
     returned unchanged. The anchored forms still splice at the start or the
     end. */
  if (pattern.is_empty() && !is_anchored_at_start && !is_anchored_at_end)
    return String{scratch_allocator(), value};

  let out = String{scratch_allocator()};

  if (is_anchored_at_start) {
    if (let const matched = longest_pattern_match_at(
            pattern.view(), pattern_active, value, 0, extglob_enabled()))
    {
      append_pattern_replacement(out, replacement.view(),
                                 value.substring_of_length(0, *matched));
      out.append(value.substring(*matched));
    } else {
      out.append(value);
    }
    return out;
  }

  if (is_anchored_at_end) {
    for (usize start = 0; start <= value.length; start++) {
      if (utils::glob_matches(pattern.view(), value.substring(start),
                              pattern_active, 0, extglob_enabled()))
      {
        out.append(value.substring_of_length(0, start));
        append_pattern_replacement(out, replacement.view(),
                                   value.substring(start));
        return out;
      }
    }
    out.append(value);
    return out;
  }

  /* A zero-length match advances one byte so the scan cannot loop. */
  bool has_replaced = false;
  usize i = 0;
  while (i < value.length) {
    Maybe<usize> matched;
    if (!has_replaced || should_replace_all) {
      matched = longest_pattern_match_at(pattern.view(), pattern_active, value,
                                         i, extglob_enabled());
    }
    if (matched.has_value()) {
      append_pattern_replacement(out, replacement.view(),
                                 value.substring_of_length(i, *matched));
      has_replaced = true;
      if (*matched == 0) {
        out.push(value[i]);
        i++;
      } else {
        i += *matched;
      }
      if (!should_replace_all) {
        out.append(value.substring(i));
        return out;
      }
    } else {
      out.push(value[i]);
      i++;
    }
  }
  return out;
}

static fn append_shell_quoted(String &out, StringView arg) throws -> void
{
  if (utils::append_ansi_c_quote_if_needed(out, arg)) return;

  out.push('\'');
  for (usize i = 0; i < arg.length; i++) {
    if (arg[i] == '\'')
      out += "'\\''";
    else
      out.push(arg[i]);
  }
  out.push('\'');
}

/* Q quotes for reuse, U u L change the case, E expands backslash escapes, A
   prints a recreating assignment, and a lists the attribute letters. */
fn EvalContext::apply_parameter_transform(StringView name, char op) throws
    -> String
{
  let const value = get_variable_value_checked(name);

  if (!value.has_value()) return String{scratch_allocator()};

  return apply_parameter_transform_to_value(value->view(), op, name);
}

fn EvalContext::apply_parameter_transform_to_value(StringView text, char op,
                                                   StringView name) throws
    -> String
{
  let out = String{scratch_allocator()};
  out.reserve(text.length);
  switch (op) {
  case 'U':
    for (usize i = 0; i < text.length; i++)
      out.push(
          static_cast<char>(std::toupper(static_cast<unsigned char>(text[i]))));
    return out;
  case 'L':
    for (usize i = 0; i < text.length; i++)
      out.push(
          static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
    return out;
  case 'u':
    for (usize i = 0; i < text.length; i++) {
      char character = text[i];
      if (i == 0)
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
      out.push(character);
    }
    return out;
  case 'Q':
  case 'K':
  case 'k':
    /* On a bare name K and k quote the value the way Q does, the key-and-value
       listing is the ${a[@]@K} array-field form on the element path. */
    append_shell_quoted(out, text);
    return out;
  case 'P': return toiletline::expand_prompt_template(text, *this);
  case 'A':
    out.append(name);
    out += '=';
    append_shell_quoted(out, text);
    return out;
  case 'E':
    for (usize i = 0; i < text.length; i++) {
      if (text[i] != '\\' || i + 1 >= text.length) {
        out.push(text[i]);
        continue;
      }
      i++;
      switch (text[i]) {
      case 'n': out.push('\n'); break;
      case 't': out.push('\t'); break;
      case 'r': out.push('\r'); break;
      case 'a': out.push('\a'); break;
      case 'b': out.push('\b'); break;
      case 'f': out.push('\f'); break;
      case 'v': out.push('\v'); break;
      case 'e': out.push('\x1b'); break;
      case '\\': out.push('\\'); break;
      case '\'': out.push('\''); break;
      case '"': out.push('"'); break;
      default:
        out.push('\\');
        out.push(text[i]);
        break;
      }
    }
    return out;
  case 'a':
    if (lookup_indexed_array(name) != nullptr) out.push('a');
    if (is_associative_array(name)) out.push('A');
    if (is_integer_variable(name)) out.push('i');
    if (is_readonly(name)) out.push('r');
    if (is_exported(name)) out.push('x');
    return out;
  default: return expand_variable(name);
  }
}

fn EvalContext::apply_case_modification(
    StringView name, StringView spec,
    const SourceLocation *source_location) throws -> String
{
  let const current = get_variable_value_checked(name);
  let const current_view = current.has_value() ? current->view() : StringView{};
  return apply_case_modification_to_value(current_view, spec, source_location);
}

fn EvalContext::apply_case_modification_to_value(
    StringView value, StringView spec,
    const SourceLocation *source_location) throws -> String
{
  LOG(All, "applying the case modification '%.*s' to a value of %zu bytes",
      static_cast<int>(spec.length), spec.data, value.length);
  let const op = spec[0];
  let const should_modify_all = spec.length > 1 && spec[1] == op;
  let const pattern_word = spec.substring(should_modify_all ? 2 : 1);

  let pattern_active = Bitset{scratch_allocator()};
  String pattern{scratch_allocator()};
  if (pattern_word.is_empty()) {
    pattern = String{scratch_allocator(), "?"};
    pattern_active.push(true);
  } else {
    let pattern_location = SourceLocation{};
    pattern = expand_modifier_word_masked(
        pattern_word, pattern_active, true,
        source_location_for_subview(source_location, spec, pattern_word,
                                    pattern_location));
  }

  let const pattern_matches_any = pattern_word.is_empty();
  let out = String{scratch_allocator()};
  out.reserve(value.length);
  for (usize i = 0; i < value.length; i++) {
    char character = value[i];
    let const is_affected = should_modify_all || i == 0;
    if (is_affected &&
        (pattern_matches_any ||
         utils::glob_matches(pattern.view(), value.substring_of_length(i, 1),
                             pattern_active, 0, extglob_enabled())))
    {
      const unsigned char byte = static_cast<unsigned char>(character);
      if (op == '^') {
        character = static_cast<char>(std::toupper(byte));
      } else if (op == ',') {
        character = static_cast<char>(std::tolower(byte));
      } else {
        if (std::islower(byte) != 0)
          character = static_cast<char>(std::toupper(byte));
        else if (std::isupper(byte) != 0)
          character = static_cast<char>(std::tolower(byte));
      }
    }
    out.push(character);
  }
  return out;
}

fn EvalContext::apply_value_modifier(
    StringView value, StringView modifier,
    const SourceLocation *source_location) throws -> String
{
  if (modifier.is_empty()) return String{scratch_allocator(), value};
  let const op = modifier[0];
  if (op == '/') return pattern_replace_value(value, modifier, source_location);
  if (op == '^' || op == ',')
    return apply_case_modification_to_value(value, modifier, source_location);
  if (op == '#' || op == '%') {
    let const is_doubled = modifier.length > 1 && modifier[1] == op;
    let const pattern_word = modifier.substring(is_doubled ? 2 : 1);
    let pattern_location = SourceLocation{};
    return trim_value_with_modifier(
        *this, value, pattern_word,
        op == '#' ? trim_end::Prefix : trim_end::Suffix, is_doubled,
        source_location_for_subview(source_location, modifier, pattern_word,
                                    pattern_location));
  }
  return String{scratch_allocator(), value};
}

} /* namespace koshka */
