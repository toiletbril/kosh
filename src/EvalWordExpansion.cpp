#include "Arena.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

struct modifier_array_word
{
  StringView array_name;
  bool is_star;
  bool is_quoted;
};

static fn parse_modifier_array_word(StringView word) wontthrow
    -> Maybe<modifier_array_word>
{
  let inner_word = word;
  let const is_quoted = inner_word.length >= 2 && inner_word[0] == '"' &&
                        inner_word[inner_word.length - 1] == '"';
  if (is_quoted)
    inner_word = inner_word.substring_of_length(1, inner_word.length - 2);
  if (inner_word.length < 6 || inner_word[0] != '$' || inner_word[1] != '{' ||
      inner_word[inner_word.length - 1] != '}')
  {
    return None;
  }
  let const inner = inner_word.substring_of_length(2, inner_word.length - 3);
  usize name_end = 0;
  while (name_end < inner.length && lexer::is_variable_name(inner[name_end]))
    name_end++;
  if (name_end == 0 || name_end + 3 != inner.length || inner[name_end] != '[' ||
      (inner[name_end + 1] != '@' && inner[name_end + 1] != '*') ||
      inner[name_end + 2] != ']')
  {
    return None;
  }
  return modifier_array_word{inner.substring_of_length(0, name_end),
                             inner[name_end + 1] == '*', is_quoted};
}

hot fn EvalContext::expand_word(const Word &word) throws
    -> ArrayList<glob_field>
{
  LOG(All, "expanding a word of %zu segments into fields",
      word.segments.count());
  let const scratch = scratch_allocator();

  let const *segments = &word.segments;
  let tilde_expanded_segments = ArrayList<WordSegment>{scratch};
  if (!word.segments.is_empty() && word.segments.front().is_tilde_candidate() &&
      !word.segments.front().text.is_empty() &&
      word.segments.front().text.first_character() == '~')
  {
    tilde_expanded_segments = word.segments;
    expand_tilde(tilde_expanded_segments.front(),
                 tilde_expanded_segments.count() > 1, !is_posix_mode());
    segments = &tilde_expanded_segments;
  }

  let fields = ArrayList<glob_field>{scratch};
  let current = glob_field{scratch};
  let has_current = false;

  let const do_flush = [&]() {
    if (has_current) {
      fields.push(steal(current));
      current = glob_field{scratch};
      has_current = false;
    }
  };

  /* An empty glob mask reads as all-false, so the first active run
     materializes it and back-fills false for the bytes already appended. */
  let const do_append_run = [&](StringView text, bool glob_active) {
    let const text_count_before = current.text.count();
    current.text.append(text);

    if (glob_active || !current.glob_active.is_empty()) {
      current.glob_active.reserve(current.text.count());
      while (current.glob_active.count() < text_count_before)
        current.glob_active.push(false);
      for (usize k = 0; k < text.length; k++)
        current.glob_active.push(glob_active);
    }

    has_current = true;
  };

  let const do_emit_empty_field = [&]() { fields.push(glob_field{scratch}); };

  /* IFS whitespace folds and a non-whitespace IFS byte delimits one field each.
     A run of k delimiters ends the field and emits k minus one empty fields. */
  let const do_append_split_run = [&](StringView text, bool glob_active) {
    usize i = 0;
    while (i < text.length) {
      let const byte = text.data[i];
      if (!is_field_separator(byte)) {
        usize start = i;
#pragma clang loop unroll_count(4)
        while (i < text.length && !is_field_separator(text.data[i]))
          i++;
        do_append_run(StringView{text.data + start, i - start}, glob_active);
        continue;
      }

      let const was_field_started = has_current;
      usize delimiter_count = 0;
#pragma clang loop unroll_count(4)
      while (i < text.length && is_field_separator(text.data[i])) {
        let const separator = text.data[i];
        if (separator != ' ' && separator != '\t' && separator != '\n') {
          delimiter_count++;
        }
        i++;
      }

      do_flush();
      if (delimiter_count == 0) continue;

      if (!was_field_started) do_emit_empty_field();
      for (usize k = 1; k < delimiter_count; k++)
        do_emit_empty_field();
    }
  };

  let const do_emit_elements = [&](const ArrayList<String> &values, bool quoted,
                                   bool star) throws {
    if (quoted && star) {
      let const ifs = m_field_separators.view();
      let joined = String{scratch_allocator()};
      for (usize i = 0; i < values.count(); i++) {
        if (i > 0 && !ifs.is_empty()) {
          joined.push(ifs[0]);
        }
        joined.append(values[i].view());
      }
      do_append_run(joined, false);
      return;
    }
    for (usize i = 0; i < values.count(); i++) {
      if (i > 0) do_flush();
      if (quoted)
        do_append_run(values[i].view(), false);
      else
        do_append_split_run(values[i].view(), true);
    }
  };

  for (const WordSegment &segment : *segments) {
    let const segment_text =
        StringView{segment.text.data(), segment.text.count()};
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::DoubleQuotedText:
      do_append_run(segment_text, false);
      break;
    case WordSegment::Kind::UnquotedText:
      do_append_run(segment_text, true);
      break;
    case WordSegment::Kind::VariableReference: {
      if (segment.text == "@" && segment.is_in_double_quotes) {
        for (usize i = 0; i < m_positional_params.count(); i++) {
          if (i > 0) do_flush();
          do_append_run(StringView{m_positional_params[i].data(),
                                   m_positional_params[i].count()},
                        false);
        }
        break;
      }
      if ((segment.text == "@" || segment.text == "*") &&
          !segment.is_in_double_quotes)
      {
        for (usize i = 0; i < m_positional_params.count(); i++) {
          if (i > 0) do_flush();
          do_append_split_run(StringView{m_positional_params[i].data(),
                                         m_positional_params[i].count()},
                              true);
        }
        break;
      }
      if (segment_text.length >= 2 && segment_text[0] == '!' &&
          (segment_text[segment_text.length - 1] == '@' ||
           segment_text[segment_text.length - 1] == '*'))
      {
        const StringView prefix =
            segment_text.substring_of_length(1, segment_text.length - 2);
        let const is_star = segment_text[segment_text.length - 1] == '*';
        let const names = matching_prefix_names(prefix);
        do_emit_elements(names, segment.is_in_double_quotes, is_star);
        break;
      }
      if (segment_text.length >= 5 && segment_text[0] == '!' &&
          segment_text[segment_text.length - 1] == ']' &&
          segment_text[segment_text.length - 3] == '[' &&
          (segment_text[segment_text.length - 2] == '@' ||
           segment_text[segment_text.length - 2] == '*') &&
          lexer::is_variable_name_start(segment_text[1]))
      {
        const StringView array_name =
            segment_text.substring_of_length(1, segment_text.length - 4);
        let const is_star = segment_text[segment_text.length - 2] == '*';
        let const subscripts = collect_array_subscripts(array_name);
        do_emit_elements(subscripts, segment.is_in_double_quotes, is_star);
        break;
      }
      if (segment_text.length >= 4 &&
          segment_text[segment_text.length - 1] == ']' &&
          segment_text[segment_text.length - 3] == '[' &&
          (segment_text[segment_text.length - 2] == '@' ||
           segment_text[segment_text.length - 2] == '*') &&
          lexer::is_variable_name_start(segment_text[0]))
      {
        let const array_name =
            segment_text.substring_of_length(0, segment_text.length - 3);
        let is_plain_array_name = true;
        for (usize i = 0; i < array_name.length; i++)
          if (!lexer::is_variable_name(array_name[i])) {
            is_plain_array_name = false;
            break;
          }
        if (is_plain_array_name) {
          let const is_star = segment_text[segment_text.length - 2] == '*';
          let const elements = collect_array_elements(array_name);
          do_emit_elements(elements, segment.is_in_double_quotes, is_star);
          break;
        }
      }
      let const segment_source_location =
          segment.get_source_location(m_current_location.filename);
      let const do_source_location_for =
          [&](StringView part,
              SourceLocation &storage) -> const SourceLocation * {
        if (!segment_source_location.has_value()) return nullptr;
        return segment_source_location->subspan_for_view(segment_text, part,
                                                         storage);
      };
      let const is_positional_word =
          !segment_text.is_empty() &&
          (segment_text[0] == '@' || segment_text[0] == '*');
      let const positional_test_has_colon = is_positional_word &&
                                            segment_text.length > 1 &&
                                            segment_text[1] == ':';
      const usize positional_test_op_position =
          positional_test_has_colon ? 2 : 1;
      if (is_positional_word &&
          segment_text.length > positional_test_op_position &&
          is_colon_modifier_operator(segment_text[positional_test_op_position]))
      {
        let const is_star = segment_text[0] == '*';
        let const op = segment_text[positional_test_op_position];
        let const word =
            segment_text.substring(positional_test_op_position + 1);
        let const param_count = m_positional_params.count();
        let const positional_is_null =
            param_count == 0 ||
            (param_count == 1 && m_positional_params[0].view().is_empty());
        let const treat_as_unset =
            positional_test_has_colon ? positional_is_null : param_count == 0;

        let const do_emit_positional = [&]() throws {
          do_emit_elements(m_positional_params, segment.is_in_double_quotes,
                           is_star);
        };
        let const do_emit_word = [&]() throws {
          if (let const array_word = parse_modifier_array_word(word);
              array_word.has_value())
          {
            do_emit_elements(collect_array_elements(array_word->array_name),
                             array_word->is_quoted ||
                                 segment.is_in_double_quotes,
                             array_word->is_star);
            return;
          }
          let word_location = SourceLocation{};
          let const expanded = expand_modifier_word(
              word, true, true, do_source_location_for(word, word_location));
          if (segment.is_in_double_quotes)
            do_append_run(expanded.view(), false);
          else
            do_append_split_run(expanded.view(), true);
        };

        switch (op) {
        case '-':
          if (treat_as_unset)
            do_emit_word();
          else
            do_emit_positional();
          break;
        case '+':
          if (!treat_as_unset) do_emit_word();
          break;
        case '=':
          if (treat_as_unset)
            throw_script_fatal(
                "Unable to assign to the positional parameters this way");
          do_emit_positional();
          break;
        case '?':
          if (treat_as_unset) {
            if (word.is_empty())
              throw_script_fatal("Unable to expand the positional parameters "
                                 "because they are not set or are empty");
            let word_location = SourceLocation{};
            throw_script_fatal(String{expand_modifier_word(
                word, true, true,
                do_source_location_for(word, word_location))});
          }
          do_emit_positional();
          break;
        default: break;
        }
        break;
      }
      /* Index zero names the shell itself, the way bash counts $0 into the
         positional slice. */
      if (!segment_text.is_empty() &&
          (segment_text[0] == '@' || segment_text[0] == '*') &&
          segment_text.length > 1 && segment_text[1] == ':')
      {
        let const is_star = segment_text[0] == '*';
        let const slice = segment_text.substring(2);
        let const param_count = m_positional_params.count();
        let const total = static_cast<i64>(param_count) + 1;
        let const do_positional_at = [&](i64 index) wontthrow -> StringView {
          return index == 0 ? m_shell_name.view()
                            : m_positional_params[static_cast<usize>(index - 1)]
                                  .view();
        };

        let const sep = find_substring_length_separator(slice);
        let const offset_text = slice.substring_of_length(0, sep);
        let offset_location = SourceLocation{};
        const i64 offset =
            offset_text.is_empty()
                ? 0
                : evaluate_arithmetic(
                      offset_text,
                      do_source_location_for(offset_text, offset_location));
        i64 start = offset < 0 ? total + offset : offset;
        if (start < 0) start = 0;
        if (start > total) start = total;
        i64 end = total;
        if (sep < slice.length) {
          let const length_text = slice.substring(sep + 1);
          let length_location = SourceLocation{};
          i64 length =
              length_text.is_empty()
                  ? 0
                  : evaluate_arithmetic(
                        length_text,
                        do_source_location_for(length_text, length_location));
          if (length < 0)
            throw Error{"Unable to take the substring because the length names "
                        "a point before the offset"};
          if (length > total) length = total;
          end = start + length;
        }
        if (end > total) end = total;
        if (end < start) end = start;

        if (segment.is_in_double_quotes && is_star) {
          let const ifs = m_field_separators.view();
          let joined = String{scratch_allocator()};
          for (i64 j = start; j < end; j++) {
            if (j > start && !ifs.is_empty()) {
              joined.push(ifs[0]);
            }
            joined.append(do_positional_at(j));
          }
          do_append_run(joined, false);
        } else if (segment.is_in_double_quotes) {
          for (i64 j = start; j < end; j++) {
            if (j > start) do_flush();
            do_append_run(do_positional_at(j), false);
          }
        } else {
          for (i64 j = start; j < end; j++) {
            if (j > start) do_flush();
            do_append_split_run(do_positional_at(j), true);
          }
        }
        break;
      }
      const char positional_at_op =
          segment_text.length > 2 && segment_text[1] == '@' &&
                  (segment_text[2] == 'Q' || segment_text[2] == 'E' ||
                   segment_text[2] == 'U' || segment_text[2] == 'L' ||
                   segment_text[2] == 'u' || segment_text[2] == 'P')
              ? segment_text[2]
              : '\0';
      if (!segment_text.is_empty() &&
          (segment_text[0] == '@' || segment_text[0] == '*') &&
          segment_text.length > 1 &&
          (segment_text[1] == '/' || segment_text[1] == '#' ||
           segment_text[1] == '%' || segment_text[1] == '^' ||
           segment_text[1] == ',' || positional_at_op != '\0'))
      {
        let const is_star = segment_text[0] == '*';
        let const modifier = segment_text.substring(1);
        let modifier_location = SourceLocation{};
        let const *modifier_location_pointer =
            do_source_location_for(modifier, modifier_location);
        let const do_transform = [&](StringView value) -> String {
          if (positional_at_op != '\0')
            return apply_parameter_transform_to_value(value, positional_at_op,
                                                      StringView{});
          return apply_value_modifier(value, modifier,
                                      modifier_location_pointer);
        };
        if (segment.is_in_double_quotes && is_star) {
          let const ifs = m_field_separators.view();
          let joined = String{scratch_allocator()};
          for (usize i = 0; i < m_positional_params.count(); i++) {
            if (i > 0 && !ifs.is_empty()) {
              joined.push(ifs[0]);
            }
            joined.append(do_transform(m_positional_params[i].view()).view());
          }
          do_append_run(joined, false);
        } else {
          for (usize i = 0; i < m_positional_params.count(); i++) {
            if (i > 0) do_flush();
            let const modified = do_transform(m_positional_params[i].view());
            if (segment.is_in_double_quotes)
              do_append_run(modified.view(), false);
            else
              do_append_split_run(modified.view(), true);
          }
        }
        break;
      }
      if (lexer::is_variable_name_start(segment_text[0])) {
        usize name_end = 1;
        while (name_end < segment_text.length &&
               lexer::is_variable_name(segment_text[name_end]))
          name_end++;
        let const after_array_colon = name_end + 4 < segment_text.length
                                          ? segment_text[name_end + 4]
                                          : '\0';
        if (name_end + 4 <= segment_text.length &&
            segment_text[name_end] == '[' &&
            (segment_text[name_end + 1] == '@' ||
             segment_text[name_end + 1] == '*') &&
            segment_text[name_end + 2] == ']' &&
            segment_text[name_end + 3] == ':' &&
            !is_colon_modifier_operator(after_array_colon))
        {
          let const array_name = segment_text.substring_of_length(0, name_end);
          let const is_star = segment_text[name_end + 1] == '*';
          let const slice = segment_text.substring(name_end + 4);
          let const elements = collect_array_elements(array_name);
          let const total = static_cast<i64>(elements.count());

          let const sep = find_substring_length_separator(slice);
          let const offset_text = slice.substring_of_length(0, sep);
          let offset_location = SourceLocation{};
          const i64 offset =
              offset_text.is_empty()
                  ? 0
                  : evaluate_arithmetic(
                        offset_text,
                        do_source_location_for(offset_text, offset_location));
          i64 start = offset < 0 ? total + offset : offset;
          if (start < 0) start = 0;
          if (start > total) start = total;
          i64 end = total;
          if (sep < slice.length) {
            let const length_text = slice.substring(sep + 1);
            let length_location = SourceLocation{};
            i64 length =
                length_text.is_empty()
                    ? 0
                    : evaluate_arithmetic(
                          length_text,
                          do_source_location_for(length_text, length_location));
            if (length < 0)
              throw Error{
                  "Unable to take the substring because the length names "
                  "a point before the offset"};
            if (length > total) length = total;
            end = start + length;
          }
          if (end > total) end = total;
          if (end < start) end = start;

          if (segment.is_in_double_quotes && is_star) {
            let const ifs = m_field_separators.view();
            let joined = String{scratch_allocator()};
            for (i64 j = start; j < end; j++) {
              if (j > start && !ifs.is_empty()) {
                joined.push(ifs[0]);
              }
              joined.append(elements[static_cast<usize>(j)].view());
            }
            do_append_run(joined, false);
          } else if (segment.is_in_double_quotes) {
            for (i64 j = start; j < end; j++) {
              if (j > start) do_flush();
              do_append_run(elements[static_cast<usize>(j)].view(), false);
            }
          } else {
            for (i64 j = start; j < end; j++) {
              if (j > start) do_flush();
              do_append_split_run(elements[static_cast<usize>(j)].view(), true);
            }
          }
          break;
        }
        let const field_modifier_op = name_end + 3 < segment_text.length
                                          ? segment_text[name_end + 3]
                                          : '\0';
        const char at_transform_op =
            field_modifier_op == '@' && name_end + 4 < segment_text.length
                ? segment_text[name_end + 4]
                : '\0';
        const bool is_mapped_at_op =
            at_transform_op == 'Q' || at_transform_op == 'E' ||
            at_transform_op == 'U' || at_transform_op == 'L' ||
            at_transform_op == 'u' || at_transform_op == 'P' ||
            at_transform_op == 'a';
        if (name_end + 3 < segment_text.length &&
            segment_text[name_end] == '[' &&
            (segment_text[name_end + 1] == '@' ||
             segment_text[name_end + 1] == '*') &&
            segment_text[name_end + 2] == ']' &&
            (field_modifier_op == '/' || field_modifier_op == '#' ||
             field_modifier_op == '%' || field_modifier_op == '^' ||
             field_modifier_op == ',' || is_mapped_at_op))
        {
          let const array_name = segment_text.substring_of_length(0, name_end);
          let const modifier = segment_text.substring(name_end + 3);
          let modifier_location = SourceLocation{};
          let const *modifier_location_pointer =
              do_source_location_for(modifier, modifier_location);
          let const is_star = segment_text[name_end + 1] == '*';
          let const elements = collect_array_elements(array_name);
          let const do_transform = [&](StringView element_value) -> String {
            if (is_mapped_at_op)
              return apply_parameter_transform_to_value(
                  element_value, at_transform_op, array_name);
            return apply_value_modifier(element_value, modifier,
                                        modifier_location_pointer);
          };
          if (segment.is_in_double_quotes && is_star) {
            let const ifs = m_field_separators.view();
            let joined = String{scratch_allocator()};
            for (usize i = 0; i < elements.count(); i++) {
              if (i > 0 && !ifs.is_empty()) {
                joined.push(ifs[0]);
              }
              joined.append(do_transform(elements[i].view()).view());
            }
            do_append_run(joined, false);
          } else {
            for (usize i = 0; i < elements.count(); i++) {
              if (i > 0) do_flush();
              let const modified = do_transform(elements[i].view());
              if (segment.is_in_double_quotes)
                do_append_run(modified.view(), false);
              else
                do_append_split_run(modified.view(), true);
            }
          }
          break;
        }
        if (name_end + 3 < segment_text.length &&
            segment_text[name_end] == '[' &&
            (segment_text[name_end + 1] == '@' ||
             segment_text[name_end + 1] == '*') &&
            segment_text[name_end + 2] == ']')
        {
          let const rest = segment_text.substring(name_end + 3);
          let const is_colon_form = !rest.is_empty() && rest[0] == ':';
          let const op_index = is_colon_form ? usize{1} : usize{0};
          if (op_index < rest.length &&
              (rest[op_index] == '+' || rest[op_index] == '-'))
          {
            let const array_name =
                segment_text.substring_of_length(0, name_end);
            let const modifier_op = rest[op_index];
            let const modifier_word = rest.substring(op_index + 1);
            let const is_star = segment_text[name_end + 1] == '*';
            let const elements = collect_array_elements(array_name);
            let is_every_element_empty = true;
            for (const String &element : elements)
              if (!element.is_empty()) {
                is_every_element_empty = false;
                break;
              }
            let const treat_as_unset =
                is_colon_form ? is_every_element_empty : elements.is_empty();
            let const should_expand_word =
                modifier_op == '+' ? !treat_as_unset : treat_as_unset;

            if (!should_expand_word) {
              if (modifier_op == '-')
                do_emit_elements(elements, segment.is_in_double_quotes,
                                 is_star);
              break;
            }

            /* The inner word's own quoting governs the split, so a quoted
               "${arr[@]}" keeps each element whole even though the outer
               modifier here is unquoted. */
            if (let const array_word = parse_modifier_array_word(modifier_word);
                array_word.has_value())
            {
              do_emit_elements(collect_array_elements(array_word->array_name),
                               array_word->is_quoted ||
                                   segment.is_in_double_quotes,
                               array_word->is_star);
              break;
            }
            let modifier_word_location = SourceLocation{};
            let const value = expand_modifier_word(
                modifier_word, true, true,
                do_source_location_for(modifier_word, modifier_word_location));
            if (segment.is_in_double_quotes)
              do_append_run(value, false);
            else
              do_append_split_run(value, true);
            break;
          }
        }
        let const rest = segment_text.substring(name_end);
        let const is_colon_form = !rest.is_empty() && rest[0] == ':';
        let const op_index = is_colon_form ? usize{1} : usize{0};
        if (op_index < rest.length &&
            (rest[op_index] == '+' || rest[op_index] == '-'))
        {
          if (let const array_word =
                  parse_modifier_array_word(rest.substring(op_index + 1));
              array_word.has_value())
          {
            let const subject_elements = collect_array_elements(
                segment_text.substring_of_length(0, name_end));
            let const treat_as_unset = is_colon_form
                                           ? (subject_elements.is_empty() ||
                                              subject_elements[0].is_empty())
                                           : subject_elements.is_empty();
            let const modifier_op = rest[op_index];
            let const should_expand_word =
                modifier_op == '+' ? !treat_as_unset : treat_as_unset;
            if (should_expand_word) {
              let const values = collect_array_elements(array_word->array_name);
              let const emit_quoted =
                  array_word->is_quoted || segment.is_in_double_quotes;
              do_emit_elements(values, emit_quoted, array_word->is_star);
              break;
            }
            if (modifier_op == '+') break;
          }
        }
      }
      if (!segment_text.is_empty() &&
          lexer::is_variable_name_start(segment_text[0]))
      {
        let is_plain_name = true;
        for (usize i = 1; i < segment_text.length; i++)
          if (!lexer::is_variable_name(segment_text[i])) {
            is_plain_name = false;
            break;
          }
        if (is_plain_name)
          if (let const *stored = lookup_shell_variable(segment_text);
              stored != nullptr)
          {
            if (segment.is_in_double_quotes)
              do_append_run(stored->view(), false);
            else
              do_append_split_run(stored->view(), true);
            break;
          }
      }
      let const source_location =
          segment.get_source_location(m_current_location.filename);
      let const value = apply_parameter_expansion(
          segment.text.view(),
          source_location.has_value() ? &*source_location : nullptr);
      if (segment.is_in_double_quotes)
        do_append_run(value, false);
      else
        do_append_split_run(value, true);
    } break;

    case WordSegment::Kind::CommandSubstitution: {
      let const output = capture_command_substitution(segment);
      if (segment.is_in_double_quotes)
        do_append_run(output, false);
      else
        do_append_split_run(output, true);
    } break;

    case WordSegment::Kind::FunctionSubstitution: {
      let const output = capture_function_substitution(segment);
      if (segment.is_in_double_quotes)
        do_append_run(output, false);
      else
        do_append_split_run(output, true);
    } break;

    case WordSegment::Kind::ProcessSubstitution: {
      let const path = setup_process_substitution(segment);
      do_append_run(path, false);
    } break;

    case WordSegment::Kind::ArithmeticExpansion: {
      let const can_use_folded_result = segment.has_folded_arithmetic_result &&
                                        !has_debug_trap() &&
                                        !should_echo_expanded();
      let const result = can_use_folded_result
                             ? segment.get_folded_arithmetic_result()
                             : evaluate_arithmetic_cached(segment);
      char buffer[24];
      let const value = utils::int_to_text_into(result, buffer, sizeof(buffer));
      if (segment.is_in_double_quotes)
        do_append_run(value, false);
      else
        do_append_split_run(value, false);
    } break;
    }
  }

  do_flush();

  return fields;
}

hot fn EvalContext::expand_word_for_assignment(const Word &word) throws
    -> String
{
  LOG(All, "expanding an assignment word of %zu segments",
      word.segments.count());
  /* An assignment expands a tilde after an unquoted colon too, the rule bash
     applies to PATH=~/bin:~/tmp. */
  let const *segments = &word.segments;
  let tilde_expanded_segments = ArrayList<WordSegment>{scratch_allocator()};
  let const has_leading_tilde =
      !word.segments.is_empty() && word.segments.front().is_tilde_candidate() &&
      !word.segments.front().text.is_empty() &&
      word.segments.front().text.first_character() == '~';
  let has_colon_tilde = false;
  for (const WordSegment &segment : word.segments) {
    if (!segment.is_tilde_candidate()) continue;
    if (segment.text.find_substring(":~").has_value()) {
      has_colon_tilde = true;
      break;
    }
  }
  if (has_leading_tilde || has_colon_tilde) {
    tilde_expanded_segments = word.segments;
    if (has_leading_tilde)
      expand_tilde(tilde_expanded_segments.front(),
                   tilde_expanded_segments.count() > 1, true);
    if (has_colon_tilde)
      for (usize i = 0; i < tilde_expanded_segments.count(); i++)
        expand_colon_tildes(tilde_expanded_segments[i],
                            i + 1 < tilde_expanded_segments.count());
    segments = &tilde_expanded_segments;
  }

  let result = String{scratch_allocator()};
  for (const WordSegment &segment : *segments) {
    let const segment_text = segment.text.view();
    switch (segment.kind) {
    case WordSegment::Kind::VariableReference: {
      let const source_location =
          segment.get_source_location(m_current_location.filename);
      result += apply_parameter_expansion(
          segment_text,
          source_location.has_value() ? &*source_location : nullptr);
    } break;
    case WordSegment::Kind::CommandSubstitution:
      result += capture_command_substitution(segment);
      break;
    case WordSegment::Kind::FunctionSubstitution:
      result += capture_function_substitution(segment);
      break;
    case WordSegment::Kind::ArithmeticExpansion: {
      let const can_use_folded_result = segment.has_folded_arithmetic_result &&
                                        !has_debug_trap() &&
                                        !should_echo_expanded();
      let const number = can_use_folded_result
                             ? segment.get_folded_arithmetic_result()
                             : evaluate_arithmetic_cached(segment);
      char buffer[24];
      result += utils::int_to_text_into(number, buffer, sizeof(buffer));
    } break;
    default: result += segment_text; break;
    }
  }
  return result;
}

fn EvalContext::expand_case_pattern_masked(const Word &word,
                                           Bitset &active_out) throws -> String
{
  let const *segments = &word.segments;
  let tilde_expanded_segments = ArrayList<WordSegment>{scratch_allocator()};
  if (!word.segments.is_empty() && word.segments.front().is_tilde_candidate() &&
      !word.segments.front().text.is_empty() &&
      word.segments.front().text.first_character() == '~')
  {
    tilde_expanded_segments = word.segments;
    expand_tilde(tilde_expanded_segments.front(),
                 tilde_expanded_segments.count() > 1, !is_posix_mode());
    segments = &tilde_expanded_segments;
  }

  let result = String{scratch_allocator()};

  let const do_emit_run = [&](StringView bytes, bool is_active) {
    result.append(bytes);
    for (usize k = 0; k < bytes.length; k++)
      active_out.push(is_active);
  };

  for (const WordSegment &segment : *segments) {
    let const segment_text = segment.text.view();
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::DoubleQuotedText:
      do_emit_run(segment_text, false);
      break;
    case WordSegment::Kind::UnquotedText:
      do_emit_run(segment_text, true);
      break;
    case WordSegment::Kind::VariableReference: {
      let const source_location =
          segment.get_source_location(m_current_location.filename);
      let const value = apply_parameter_expansion(
          segment_text,
          source_location.has_value() ? &*source_location : nullptr);
      do_emit_run(value.view(), !segment.is_in_double_quotes);
    } break;
    case WordSegment::Kind::CommandSubstitution: {
      let const output = capture_command_substitution(segment);
      do_emit_run(output.view(), !segment.is_in_double_quotes);
    } break;
    case WordSegment::Kind::FunctionSubstitution: {
      let const output = capture_function_substitution(segment);
      do_emit_run(output.view(), !segment.is_in_double_quotes);
    } break;
    case WordSegment::Kind::ProcessSubstitution: {
      let const path = setup_process_substitution(segment);
      do_emit_run(path.view(), false);
    } break;
    case WordSegment::Kind::ArithmeticExpansion: {
      let const can_use_folded_result = segment.has_folded_arithmetic_result &&
                                        !has_debug_trap() &&
                                        !should_echo_expanded();
      let const number = can_use_folded_result
                             ? segment.get_folded_arithmetic_result()
                             : evaluate_arithmetic_cached(segment);
      char buffer[24];
      do_emit_run(utils::int_to_text_into(number, buffer, sizeof(buffer)),
                  false);
    } break;
    }
  }
  return result;
}

fn EvalContext::expand_wordlist_to_fields(StringView wordlist,
                                          bool allow_expansion) throws
    -> ArrayList<String>
{
  let const do_split_plain = [&]() throws -> ArrayList<String> {
    let words = ArrayList<String>{heap_allocator()};
    usize start = 0;
    for (usize i = 0; i <= wordlist.length; i++) {
      let const character = i < wordlist.length ? wordlist[i] : ' ';
      if (character == ' ' || character == '\t' || character == '\n') {
        if (i > start)
          words.push(String{wordlist.substring_of_length(start, i - start)});
        start = i + 1;
      }
    }
    return words;
  };

  if (!allow_expansion) return do_split_plain();

  let has_expandable_byte = false;
  for (usize i = 0; i < wordlist.length && !has_expandable_byte; i++) {
    let const character = wordlist[i];
    has_expandable_byte = character == '$' || character == '`' ||
                          character == '"' || character == '\'' ||
                          character == '\\' || character == '~' ||
                          character == '{';
  }
  if (!has_expandable_byte) return do_split_plain();

  /* The list expands wrapped in an array literal, so a top-level structural
     byte that closes the literal early and runs the tail as a command is a
     break-out. Such a list degrades to the plain split. */
  let const do_array_literal_is_safe = [&]() wontthrow -> bool {
    char quote = 0;
    usize paren_depth = 0;
    usize brace_depth = 0;
    let is_in_backtick = false;
    let is_at_word_start = true;
    for (usize i = 0; i < wordlist.length; i++) {
      let const character = wordlist[i];
      if (quote != 0) {
        if (character == quote) quote = 0;
        is_at_word_start = false;
        continue;
      }
      if (character == '\\') {
        i++;
        is_at_word_start = false;
        continue;
      }
      if (character == '\'' || character == '"') {
        quote = character;
      } else if (character == '`') {
        is_in_backtick = !is_in_backtick;
      } else if (character == '$' && i + 1 < wordlist.length &&
                 wordlist[i + 1] == '(')
      {
        if (i + 2 < wordlist.length && wordlist[i + 2] == '(') {
          paren_depth += 2;
          i += 2;
        } else {
          paren_depth++;
          i++;
        }
      } else if (character == '$' && i + 1 < wordlist.length &&
                 wordlist[i + 1] == '{')
      {
        brace_depth++;
        i++;
      } else if (character == ')' && paren_depth > 0) {
        paren_depth--;
      } else if (character == '}' && brace_depth > 0) {
        brace_depth--;
      } else if (!is_in_backtick && paren_depth == 0 && brace_depth == 0) {
        if (character == ')' || character == '(' || character == ';' ||
            character == '|' || character == '&' || character == '<' ||
            character == '>' || character == '\n')
        {
          return false;
        }
        if (character == '#' && is_at_word_start) {
          return false;
        }
      }
      is_at_word_start = character == ' ' || character == '\t';
    }
    return quote == 0 && !is_in_backtick && paren_depth == 0 &&
           brace_depth == 0;
  };
  if (!do_array_literal_is_safe()) {
    LOG(Debug, "-W list is not array-literal safe, splitting plain");
    return do_split_plain();
  }

  defer
  {
    m_indexed_arrays.erase("t__wordlist_fields");
    force_unset_shell_variable("t__wordlist_fields");
  };
  let fields = ArrayList<String>{heap_allocator()};
  try {
    let expansion_source = String{"t__wordlist_fields=("};
    expansion_source.append(wordlist);
    expansion_source.push(')');
    run_source(expansion_source.view(), "a -W word list",
               return_handling::Propagate);
    if (const ArrayList<String> *expanded =
            lookup_indexed_array("t__wordlist_fields");
        expanded != nullptr)
    {
      fields.reserve(expanded->count());
      for (const String &word : *expanded)
        fields.push_managed(word.view());
    }
  } catch (const ErrorBase &error) {
    LOG(Debug, "-W expansion failed, splitting plain: %s",
        error.message().c_str());
    return do_split_plain();
  }
  return fields;
}

} /* namespace koshka */
