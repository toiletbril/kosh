/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file expands parsed word segments into scalar values and command
 * fields. It coordinates parameter, command, arithmetic, tilde, pathname,
 * brace, quote, assignment, case-pattern, and word-list expansion while
 * preserving segment masks and cache ownership. The split provides one
 * coordinator above the specialized expansion sources.
 */

#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Debug.hpp"
#include "base/Path.hpp"
#include "base/Trace.hpp"

namespace koshka {

static fn clone_word_segments(const Word &word, Allocator allocator) throws
    -> ArrayList<WordSegment>
{
  let segments = ArrayList<WordSegment>{allocator};
  segments.reserve(word.segments.count());
  for (let const &segment : word.segments)
    segments.push(segment.clone(allocator));

  return segments;
}

static fn is_field_sensitive_word(StringView word) wontthrow -> bool
{
  for (usize i = 0; i < word.length; i++) {
    let const byte = word[i];
    if (byte == '"' || byte == '\'' || byte == '\\' || byte == '@') {
      return true;
    }
    if (byte == '*' && i > 0 &&
        (word[i - 1] == '{' || word[i - 1] == '[' || word[i - 1] == '$'))
    {
      return true;
    }
  }

  return false;
}

static fn get_segment_quoting(const WordSegment &segment) wontthrow
    -> parameter_word_quoting
{
  return segment.is_in_double_quotes ? parameter_word_quoting::DoubleQuoted
                                     : parameter_word_quoting::Unquoted;
}

hot fn EvalContext::expand_word(const Word &word) throws
    -> ArrayList<glob_field>
{
  LOG(All, "expanding a word of %zu segments into fields",
      word.segments.count());
  let const scratch = scratch_allocator();
  let const was_expanding_assignment_value =
      expansion_store().is_expanding_assignment_value();
  let const was_expanding_single_string =
      expansion_store().is_expanding_single_string();
  expansion_store().is_expanding_assignment_value() = false;
  expansion_store().is_expanding_single_string() = false;
  defer
  {
    expansion_store().is_expanding_assignment_value() =
        was_expanding_assignment_value;
    expansion_store().is_expanding_single_string() =
        was_expanding_single_string;
  };

  let const *segments = &word.segments;
  let tilde_expanded_segments = ArrayList<WordSegment>{scratch};
  if (!word.segments.is_empty() && word.segments.front().is_tilde_candidate() &&
      !word.segments.front().text.is_empty() &&
      word.segments.front().text.first_character() == '~')
  {
    tilde_expanded_segments = clone_word_segments(word, scratch);
    expand_tilde(tilde_expanded_segments.front(),
                 tilde_expanded_segments.count() > 1,
                 !runtime_state().is_posix_mode());
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

  let const do_append_split_run = [&](StringView text, bool glob_active) {
    let const separators = variable_store().field_separators();
    let const is_multibyte =
        variable_store().has_non_ascii_field_separators() &&
        get_glob_charset_for(separators) == glob_charset::Utf8 &&
        get_glob_charset_for(text) == glob_charset::Utf8;
    let const do_separator_length = [&](usize at) -> usize {
      let const byte = text.data[at];
      if (!is_multibyte || static_cast<u8>(byte) < 0x80)
        return variable_store().is_field_separator(byte) ? 1 : 0;

      let const length =
          utils::charset_character_length(text, at, glob_charset::Utf8);
      let const character = text.substring_of_length(at, length);
      for (usize position = 0; position < separators.length;) {
        let const separator_length = utils::charset_character_length(
            separators, position, glob_charset::Utf8);
        if (separators.substring_of_length(position, separator_length) ==
            character)
          return length;
        position += separator_length;
      }
      return 0;
    };
    let const do_step = [&](usize at) -> usize {
      return is_multibyte
                 ? utils::charset_character_length(text, at, glob_charset::Utf8)
                 : 1;
    };

    usize i = 0;
    while (i < text.length) {
      if (do_separator_length(i) == 0) {
        usize start = i;
#pragma clang loop unroll_count(4)
        while (i < text.length && do_separator_length(i) == 0)
          i += do_step(i);
        do_append_run(StringView{text.data + start, i - start}, glob_active);
        continue;
      }

      let const was_field_started = has_current;
      usize delimiter_count = 0;
      while (i < text.length) {
        let const separator_length = do_separator_length(i);
        if (separator_length == 0) break;

        let const separator = text[i];
        if (separator_length > 1 ||
            (separator != ' ' && separator != '\t' && separator != '\n'))
        {
          delimiter_count++;
        }
        i += separator_length;
      }

      do_flush();
      if (delimiter_count == 0) continue;

      if (!was_field_started) do_emit_empty_field();
      for (usize k = 1; k < delimiter_count; k++)
        do_emit_empty_field();
    }
  };

  let const do_begin_list_element = [&](usize index) throws {
    if (index == 0) return;

    let const separators = variable_store().field_separators();
    if (separators.is_empty() || separators[0] == ' ' ||
        separators[0] == '\t' || separators[0] == '\n' ||
        runtime_state().is_posix_mode())
    {
      do_flush();
      return;
    }

    do_append_split_run(
        separators.substring_of_length(
            0, utils::charset_character_length(
                   separators, 0, get_glob_charset_for(separators))),
        true);
  };

  let const do_emit_elements = [&](const ArrayList<String> &values, bool quoted,
                                   bool star) throws {
    if (quoted && star) {
      let const ifs = variable_store().field_separators();
      let joined = String{scratch_allocator()};
      let const separator = first_field_separator();
      for (usize i = 0; i < values.count(); i++) {
        if (i > 0 && !ifs.is_empty()) {
          joined.append(separator);
        }
        joined.append(values[i].view());
      }
      do_append_run(joined, false);
      return;
    }
    for (usize i = 0; i < values.count(); i++) {
      if (quoted) {
        if (i > 0) do_flush();
        do_append_run(values[i].view(), false);
      } else {
        do_begin_list_element(i);
        do_append_split_run(values[i].view(), true);
      }
    }
  };

  let const do_emit_modifier_word = [&](StringView word,
                                        const SourceLocation *word_location,
                                        bool is_quoted) throws {
    if (!is_field_sensitive_word(word)) {
      let const expanded =
          expand_modifier_word(word, true, true, word_location, !is_quoted,
                               is_quoted ? parameter_word_quoting::DoubleQuoted
                                         : parameter_word_quoting::Unquoted);
      if (is_quoted)
        do_append_run(expanded.view(), false);
      else
        do_append_split_run(expanded.view(), true);
      return;
    }

    let active = Bitset{scratch};
    let break_offsets = ArrayList<usize>{scratch};
    let marks = ArrayList<quoted_empty_mark>{scratch};
    let const text = expand_modifier_word_fields(
        word, is_quoted, active, break_offsets, marks, word_location);
    usize piece_start = 0;
    usize mark_index = 0;
    for (usize piece = 0; piece <= break_offsets.count(); piece++) {
      let const piece_end =
          piece < break_offsets.count() ? break_offsets[piece] : text.count();
      if (piece > 0) do_flush();
      if (is_quoted) do_append_run(StringView{}, false);

      usize position = piece_start;
      for (;;) {
        while (mark_index < marks.count() && marks[mark_index].piece == piece &&
               marks[mark_index].offset == position)
        {
          do_append_run(StringView{}, false);
          mark_index++;
        }
        if (position >= piece_end) break;

        let const has_mark =
            mark_index < marks.count() && marks[mark_index].piece == piece;
        let const run_limit = has_mark ? marks[mark_index].offset : piece_end;
        usize run_start = position;
        while (run_start < run_limit) {
          let const is_active = active[run_start] && !is_quoted;
          usize run_end = run_start + 1;
          while (run_end < run_limit &&
                 (active[run_end] && !is_quoted) == is_active)
          {
            run_end++;
          }

          let const run =
              StringView{text.data() + run_start, run_end - run_start};
          if (is_active)
            do_append_split_run(run, true);
          else
            do_append_run(run, false);
          run_start = run_end;
        }
        position = run_limit;
      }
      piece_start = piece_end;
    }
  };

  for (let const &segment : *segments) {
    let segment_text = StringView{segment.text.data(), segment.text.count()};
    let resolved_text = Maybe<String>{};
    if (segment.kind == WordSegment::Kind::VariableReference &&
        variable_store().attributes().has_namerefs())
      rarely
      {
        resolved_text = resolve_nameref_parameter(segment_text);
        if (resolved_text.has_value()) segment_text = resolved_text->view();
      }
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::DoubleQuotedText:
      do_append_run(segment_text, false);
      break;
    case WordSegment::Kind::UnquotedText:
      do_append_run(segment_text, true);
      if (segment.has_glob_metacharacter()) current.has_literal_glob = true;
      break;
    case WordSegment::Kind::VariableReference: {
      if (segment.text == "@" && segment.is_in_double_quotes) {
        for (usize i = 0; i < variable_store().positional_params().count(); i++)
        {
          if (i > 0) do_flush();
          do_append_run(
              StringView{variable_store().positional_params()[i].data(),
                         variable_store().positional_params()[i].count()},
              false);
        }
        break;
      }
      if ((segment.text == "@" || segment.text == "*") &&
          !segment.is_in_double_quotes)
      {
        for (usize i = 0; i < variable_store().positional_params().count(); i++)
        {
          do_begin_list_element(i);
          do_append_split_run(variable_store().positional_params()[i].view(),
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
      let const segment_source_location = segment.get_source_location(
          source_store().current_location().source_name_index);
      let const do_source_location_for =
          [&](StringView part,
              SourceLocation &storage) -> const SourceLocation * {
        if (!segment_source_location.has_value() || resolved_text.has_value())
          return nullptr;
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
        let const param_count = variable_store().positional_params().count();
        let const positional_is_null =
            param_count == 0 ||
            (param_count == 1 &&
             variable_store().positional_params()[0].view().is_empty());
        let const is_posix = runtime_state().is_posix_mode();
        let const treat_as_unset =
            is_posix ? is_posix_positional_test_null(
                           positional_test_has_colon, op,
                           segment.is_in_double_quotes && is_star)
            : positional_test_has_colon ? positional_is_null
                                        : param_count == 0;
        let const is_quoted_list = is_posix && segment.is_in_double_quotes;

        let const do_emit_positional = [&]() throws {
          if (is_quoted_list && param_count == 0)
            do_append_run(StringView{}, false);
          do_emit_elements(variable_store().positional_params(),
                           segment.is_in_double_quotes, is_star);
        };
        let const do_emit_word = [&]() throws {
          let word_location = SourceLocation{};
          do_emit_modifier_word(word,
                                do_source_location_for(word, word_location),
                                segment.is_in_double_quotes);
        };

        switch (op) {
        case '-':
          if (treat_as_unset)
            do_emit_word();
          else
            do_emit_positional();
          break;
        case '+':
          if (!treat_as_unset)
            do_emit_word();
          else if (is_quoted_list)
            do_append_run(StringView{}, false);
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
      if (!segment_text.is_empty() &&
          (segment_text[0] == '@' || segment_text[0] == '*') &&
          segment_text.length > 1 && segment_text[1] == ':')
      {
        let const is_star = segment_text[0] == '*';
        let const slice = segment_text.substring(2);
        let const param_count = variable_store().positional_params().count();
        let const total = static_cast<i64>(param_count) + 1;
        let const do_positional_at = [&](i64 index) wontthrow -> StringView {
          return index == 0
                     ? execution_store().get_shell_name()
                     : variable_store()
                           .positional_params()[static_cast<usize>(index - 1)]
                           .view();
        };

        let slice_location = SourceLocation{};
        let const bounds = compute_list_slice_bounds(
            slice, total, do_source_location_for(slice, slice_location));
        let const start = bounds.start;
        let const end = bounds.end;

        if (segment.is_in_double_quotes && is_star) {
          do_append_run(
              join_list_slice(bounds, variable_store().positional_params(),
                              execution_store().get_shell_name(), true),
              false);
        } else if (segment.is_in_double_quotes) {
          for (i64 j = start; j < end; j++) {
            if (j > start) do_flush();
            do_append_run(do_positional_at(j), false);
          }
        } else {
          for (i64 j = start; j < end; j++) {
            do_begin_list_element(static_cast<usize>(j - start));
            do_append_split_run(do_positional_at(j), true);
          }
        }
        break;
      }
      if (segment.is_in_double_quotes && segment_text == "@@A") {
        let const fields = get_declaration_fields("@");
        if (fields.count() > 1) {
          for (usize i = 0; i < fields.count(); i++) {
            if (i > 0) do_flush();
            do_append_run(fields[i].view(), false);
          }
          break;
        }
      }
      const char positional_at_op =
          segment_text.length > 2 && segment_text[1] == '@' &&
                  (segment_text[2] == 'Q' || segment_text[2] == 'E' ||
                   segment_text[2] == 'U' || segment_text[2] == 'L' ||
                   segment_text[2] == 'u' || segment_text[2] == 'P' ||
                   segment_text[2] == 'K' || segment_text[2] == 'k' ||
                   segment_text[2] == 'a')
              ? segment_text[2]
              : '\0';
      if (!segment_text.is_empty() &&
          (segment_text[0] == '@' || segment_text[0] == '*') &&
          segment_text.length > 1 &&
          (segment_text[1] == '/' || segment_text[1] == '#' ||
           segment_text[1] == '%' || segment_text[1] == '^' ||
           segment_text[1] == ',' || segment_text[1] == '~' ||
           positional_at_op != '\0'))
      {
        let const is_star = segment_text[0] == '*';
        let const modifier = segment_text.substring(1);
        let modifier_location = SourceLocation{};
        let const *modifier_location_pointer =
            do_source_location_for(modifier, modifier_location);
        if (runtime_state().is_posix_mode() &&
            (segment_text[1] == '#' || segment_text[1] == '%'))
        {
          let const trimmed =
              trim_positional_fields(is_star, segment.is_in_double_quotes,
                                     modifier, modifier_location_pointer);
          if (segment.is_in_double_quotes && trimmed.is_empty())
            do_append_run(StringView{}, false);
          for (usize i = 0; i < trimmed.count(); i++) {
            if (i > 0) do_flush();
            if (segment.is_in_double_quotes)
              do_append_run(trimmed[i].view(), false);
            else
              do_append_split_run(trimmed[i].view(), true);
          }
          break;
        }
        let const do_transform = [&](StringView value) -> String {
          if (positional_at_op != '\0')
            return apply_parameter_transform_to_value(value, positional_at_op,
                                                      StringView{});
          return apply_value_modifier(value, modifier,
                                      modifier_location_pointer);
        };
        if (segment.is_in_double_quotes && is_star) {
          let const ifs = variable_store().field_separators();
          let joined = String{scratch_allocator()};
          for (usize i = 0; i < variable_store().positional_params().count();
               i++)
          {
            if (i > 0 && !ifs.is_empty()) {
              joined.append(first_field_separator());
            }
            joined.append(
                do_transform(variable_store().positional_params()[i].view())
                    .view());
          }
          do_append_run(joined, false);
        } else {
          for (usize i = 0; i < variable_store().positional_params().count();
               i++)
          {
            let const modified =
                do_transform(variable_store().positional_params()[i].view());
            if (segment.is_in_double_quotes) {
              if (i > 0) do_flush();
              do_append_run(modified.view(), false);
            } else {
              do_begin_list_element(i);
              do_append_split_run(modified.view(), true);
            }
          }
        }
        break;
      }
      if (!segment_text.is_empty() &&
          lexer::is_variable_name_start(segment_text[0]))
      {
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

          let slice_location = SourceLocation{};
          let const bounds = compute_array_slice_bounds(
              array_name, slice, total,
              do_source_location_for(slice, slice_location));
          let const start = bounds.start;
          let const end = bounds.end;

          if (segment.is_in_double_quotes && is_star) {
            do_append_run(join_list_slice(bounds, elements, None, true), false);
          } else if (segment.is_in_double_quotes) {
            for (i64 j = start; j < end; j++) {
              if (j > start) do_flush();
              do_append_run(elements[static_cast<usize>(j)].view(), false);
            }
          } else {
            for (i64 j = start; j < end; j++) {
              do_begin_list_element(static_cast<usize>(j - start));
              do_append_split_run(elements[static_cast<usize>(j)].view(), true);
            }
          }
          break;
        }
        if (segment.is_in_double_quotes &&
            name_end + 5 == segment_text.length &&
            segment_text[name_end] == '[' &&
            segment_text[name_end + 1] == '@' &&
            segment_text[name_end + 2] == ']' &&
            segment_text[name_end + 3] == '@' &&
            segment_text[name_end + 4] == 'A')
        {
          let const fields = get_declaration_fields(
              segment_text.substring_of_length(0, name_end));
          if (fields.count() > 1) {
            for (usize i = 0; i < fields.count(); i++) {
              if (i > 0) do_flush();
              do_append_run(fields[i].view(), false);
            }
            break;
          }
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
            at_transform_op == 'a' || at_transform_op == 'k';
        if (name_end + 3 < segment_text.length &&
            segment_text[name_end] == '[' &&
            (segment_text[name_end + 1] == '@' ||
             segment_text[name_end + 1] == '*') &&
            segment_text[name_end + 2] == ']' &&
            (field_modifier_op == '/' || field_modifier_op == '#' ||
             field_modifier_op == '%' || field_modifier_op == '^' ||
             field_modifier_op == ',' || field_modifier_op == '~' ||
             is_mapped_at_op))
        {
          let const array_name = segment_text.substring_of_length(0, name_end);
          let const modifier = segment_text.substring(name_end + 3);
          let modifier_location = SourceLocation{};
          let const *modifier_location_pointer =
              do_source_location_for(modifier, modifier_location);
          let const is_star = segment_text[name_end + 1] == '*';
          let elements = collect_array_elements(array_name);
          if (at_transform_op == 'a' && elements.is_empty() &&
              is_valueless_array(array_name))
          {
            elements.push(String{heap_allocator()});
          }
          if (at_transform_op == 'k') {
            let const keys = collect_array_subscripts(array_name);
            let pairs = ArrayList<String>{heap_allocator()};
            pairs.reserve(keys.count() * 2);
            for (usize i = 0; i < keys.count() && i < elements.count(); i++) {
              pairs.push(String{heap_allocator(), keys[i].view()});
              pairs.push(String{heap_allocator(), elements[i].view()});
            }
            do_emit_elements(pairs, segment.is_in_double_quotes, is_star);
            break;
          }
          let const do_transform = [&](StringView element_value) -> String {
            if (is_mapped_at_op)
              return apply_parameter_transform_to_value(
                  element_value, at_transform_op, array_name);
            return apply_value_modifier(element_value, modifier,
                                        modifier_location_pointer);
          };
          if (segment.is_in_double_quotes && is_star) {
            let const ifs = variable_store().field_separators();
            let joined = String{scratch_allocator()};
            for (usize i = 0; i < elements.count(); i++) {
              if (i > 0 && !ifs.is_empty()) {
                joined.append(first_field_separator());
              }
              joined.append(do_transform(elements[i].view()).view());
            }
            do_append_run(joined, false);
          } else if (!segment.is_in_double_quotes &&
                     (field_modifier_op == '#' || field_modifier_op == '%') &&
                     variable_store().field_separators().is_empty())
          {
            let joined = String{scratch_allocator()};
            for (usize i = 0; i < elements.count(); i++) {
              if (i > 0) joined.push(' ');
              joined.append(do_transform(elements[i].view()).view());
            }
            if (!joined.is_empty()) do_append_run(joined, true);
          } else {
            for (usize i = 0; i < elements.count(); i++) {
              let const modified = do_transform(elements[i].view());
              if (segment.is_in_double_quotes) {
                if (i > 0) do_flush();
                do_append_run(modified.view(), false);
              } else {
                do_begin_list_element(i);
                do_append_split_run(modified.view(), true);
              }
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
          let const array_test_op =
              op_index < rest.length ? rest[op_index] : '\0';
          if (is_colon_modifier_operator(array_test_op)) {
            let const array_name =
                segment_text.substring_of_length(0, name_end);
            let const modifier_op = array_test_op;
            let const modifier_word = rest.substring(op_index + 1);
            let const is_star = segment_text[name_end + 1] == '*';
            let const elements = collect_array_elements(array_name);
            let is_every_element_empty = true;
            for (let const &element : elements)
              if (!element.is_empty()) {
                is_every_element_empty = false;
                break;
              }
            let const is_joined_value_empty =
                is_every_element_empty &&
                (elements.count() <= 1 ||
                 (is_star && segment.is_in_double_quotes &&
                  variable_store().field_separators().is_empty()));
            let const treat_as_unset =
                is_colon_form ? is_joined_value_empty : elements.is_empty();
            if (!treat_as_unset && modifier_op != '+') {
              do_emit_elements(elements, segment.is_in_double_quotes, is_star);
              break;
            }
            if (modifier_op == '+' || modifier_op == '-') {
              let const should_expand_word =
                  modifier_op == '+' ? !treat_as_unset : treat_as_unset;

              if (!should_expand_word) break;

              let modifier_word_location = SourceLocation{};
              do_emit_modifier_word(
                  modifier_word,
                  do_source_location_for(modifier_word, modifier_word_location),
                  segment.is_in_double_quotes);
              break;
            }
          }
        }
      }
      usize alternate_name_end = 0;
      if (!segment_text.is_empty() &&
          (segment_text[0] == '#' || segment_text[0] == '?' ||
           segment_text[0] == '-' || segment_text[0] == '$'))
      {
        alternate_name_end = 1;
      } else {
        while (alternate_name_end < segment_text.length &&
               lexer::is_variable_name(segment_text[alternate_name_end]))
        {
          alternate_name_end++;
        }
      }
      if (alternate_name_end > 0 && alternate_name_end < segment_text.length) {
        let const rest = segment_text.substring(alternate_name_end);
        let const is_colon_form = rest[0] == ':';
        let const op_index = is_colon_form ? usize{1} : usize{0};
        if (op_index < rest.length &&
            (rest[op_index] == '+' || rest[op_index] == '-') &&
            is_field_sensitive_word(rest.substring(op_index + 1)))
        {
          let const subject = get_variable_value(
              segment_text.substring_of_length(0, alternate_name_end));
          let const is_unset =
              !subject.has_value() || (is_colon_form && subject->is_empty());
          let const should_expand_word =
              rest[op_index] == '+' ? !is_unset : is_unset;
          if (should_expand_word) {
            let const modifier_word = rest.substring(op_index + 1);
            let modifier_word_location = SourceLocation{};
            do_emit_modifier_word(
                modifier_word,
                do_source_location_for(modifier_word, modifier_word_location),
                segment.is_in_double_quotes);
          } else if (rest[op_index] == '-') {
            if (segment.is_in_double_quotes)
              do_append_run(subject->view(), false);
            else
              do_append_split_run(subject->view(), true);
          } else if (segment.is_in_double_quotes) {
            do_append_run(StringView{}, false);
          }
          break;
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
          if (let const stored =
                  variable_store().find_plain_scalar(segment_text);
              stored.has_value())
          {
            if (segment.is_in_double_quotes)
              do_append_run(stored->view(), false);
            else
              do_append_split_run(stored->view(), true);
            break;
          }
      }
      let const source_location = segment.get_source_location(
          source_store().current_location().source_name_index);
      let const value = apply_parameter_expansion(
          segment.text.view(),
          source_location.has_value() ? &*source_location : nullptr, 0,
          !segment.is_in_double_quotes, get_segment_quoting(segment));
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
      let const value = evaluate_arithmetic_cached_text(segment);
      if (segment.is_in_double_quotes)
        do_append_run(value.view(), false);
      else
        do_append_split_run(value.view(), false);
    } break;
    }
  }

  do_flush();

  return fields;
}

hot fn EvalContext::expand_word_for_assignment(const Word &word,
                                               bool is_assignment_value) throws
    -> String
{
  LOG(All, "expanding an assignment word of %zu segments",
      word.segments.count());
  let const was_expanding_assignment_value =
      expansion_store().is_expanding_assignment_value();
  let const was_expanding_single_string =
      expansion_store().is_expanding_single_string();
  expansion_store().is_expanding_assignment_value() = is_assignment_value;
  expansion_store().is_expanding_single_string() = true;
  defer
  {
    expansion_store().is_expanding_assignment_value() =
        was_expanding_assignment_value;
    expansion_store().is_expanding_single_string() =
        was_expanding_single_string;
  };
  let const *segments = &word.segments;
  let tilde_expanded_segments = ArrayList<WordSegment>{scratch_allocator()};
  let const has_leading_tilde =
      !word.segments.is_empty() && word.segments.front().is_tilde_candidate() &&
      !word.segments.front().text.is_empty() &&
      word.segments.front().text.first_character() == '~';
  let has_colon_tilde = false;
  for (let const &segment : word.segments) {
    if (!segment.is_tilde_candidate()) continue;
    if (segment.text.find_substring(":~").has_value()) {
      has_colon_tilde = true;
      break;
    }
  }
  if (has_leading_tilde || has_colon_tilde) {
    tilde_expanded_segments = clone_word_segments(word, scratch_allocator());
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
  for (let const &segment : *segments) {
    let const segment_text = segment.text.view();
    switch (segment.kind) {
    case WordSegment::Kind::VariableReference: {
      let const source_location = segment.get_source_location(
          source_store().current_location().source_name_index);
      result += apply_parameter_expansion(
          segment_text,
          source_location.has_value() ? &*source_location : nullptr, 0,
          !segment.is_in_double_quotes, get_segment_quoting(segment));
    } break;
    case WordSegment::Kind::CommandSubstitution: {
      result += capture_command_substitution(segment);
    } break;
    case WordSegment::Kind::FunctionSubstitution: {
      result += capture_function_substitution(segment);
    } break;
    case WordSegment::Kind::ArithmeticExpansion: {
      result += evaluate_arithmetic_cached_text(segment).view();
    } break;
    case WordSegment::Kind::ProcessSubstitution: {
      result += setup_process_substitution(segment);
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
    tilde_expanded_segments = clone_word_segments(word, scratch_allocator());
    expand_tilde(tilde_expanded_segments.front(),
                 tilde_expanded_segments.count() > 1,
                 !runtime_state().is_posix_mode());
    segments = &tilde_expanded_segments;
  }

  let result = String{scratch_allocator()};

  let const do_emit_run = [&](StringView bytes, bool is_active) {
    result.append(bytes);
    for (usize k = 0; k < bytes.length; k++)
      active_out.push(is_active);
  };

  let const do_emit_expansion_run = [&](StringView bytes, bool is_active) {
    if (!is_active) {
      do_emit_run(bytes, false);
      return;
    }

    for (usize k = 0; k < bytes.length; k++) {
      let const is_escape = bytes[k] == '\\' && k + 1 < bytes.length;
      if (is_escape) k++;
      result.push(bytes[k]);
      active_out.push(!is_escape);
    }
  };

  for (let const &segment : *segments) {
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
      let const source_location = segment.get_source_location(
          source_store().current_location().source_name_index);
      let const value = apply_parameter_expansion(
          segment_text,
          source_location.has_value() ? &*source_location : nullptr, 0,
          !segment.is_in_double_quotes, get_segment_quoting(segment));
      do_emit_expansion_run(value.view(), !segment.is_in_double_quotes);
    } break;
    case WordSegment::Kind::CommandSubstitution: {
      let const output = capture_command_substitution(segment);
      do_emit_expansion_run(output.view(), !segment.is_in_double_quotes);
    } break;
    case WordSegment::Kind::FunctionSubstitution: {
      let const output = capture_function_substitution(segment);
      do_emit_expansion_run(output.view(), !segment.is_in_double_quotes);
    } break;
    case WordSegment::Kind::ProcessSubstitution: {
      let const path = setup_process_substitution(segment);
      do_emit_run(path.view(), false);
    } break;
    case WordSegment::Kind::ArithmeticExpansion: {
      let const number = evaluate_arithmetic_cached_text(segment);
      do_emit_run(number.view(), false);
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
    variable_store().indexed_arrays().erase("t__wordlist_fields");
    force_unset_shell_variable("t__wordlist_fields");
  };
  let fields = ArrayList<String>{heap_allocator()};
  try {
    let expansion_source = String{"t__wordlist_fields=("};
    expansion_source.append(wordlist);
    expansion_source.push(')');
    run_source(expansion_source.view(), "a -W word list", None, None, nullptr,
               nullptr, return_handling::Propagate);
    if (let const expanded =
            variable_store().indexed_arrays().find("t__wordlist_fields");
        expanded.has_value())
    {
      fields.reserve(expanded->count());
      for (let const &word : *expanded.value())
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
