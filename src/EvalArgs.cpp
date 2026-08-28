#include "Arena.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ResolvedCommand.hpp"
#include "StaticStringMap.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace {

/* The stand-in byte for an opaque segment in the brace template. A doubled
   marker is literal, while marker-O-decimal-semicolon names a segment. */
constexpr char BRACE_OPAQUE_MARKER = '\x01';

pure fn word_has_brace_candidate(const Word &word) wontthrow -> bool
{
  for (const WordSegment &segment : word.segments) {
    if (segment.kind != WordSegment::Kind::UnquotedText) continue;
    if (segment.text.find_character('{').has_value()) return true;
  }
  return false;
}

struct sequence_integer
{
  i64 value;
  usize field_width;
  bool has_leading_zero;
};

fn parse_sequence_integer(StringView text) wontthrow -> Maybe<sequence_integer>
{
  if (text.is_empty()) return None;
  usize i = 0;
  if (text[0] == '-' || text[0] == '+') {
    i++;
  }
  let const digit_start = i;
  if (!text.substring(digit_start).is_all_decimal_digits()) return None;

  u64 magnitude = 0;
  let const maximum_magnitude = text[0] == '-' ? static_cast<u64>(INT64_MAX) + 1
                                               : static_cast<u64>(INT64_MAX);
  for (usize j = digit_start; j < text.length; j++) {
    let const digit = static_cast<u64>(text[j] - '0');
    if (magnitude > (maximum_magnitude - digit) / 10) return None;
    magnitude = magnitude * 10 + digit;
  }
  i64 value;
  if (text[0] == '-' && magnitude == static_cast<u64>(INT64_MAX) + 1)
    value = INT64_MIN;
  else
    value = text[0] == '-' ? -static_cast<i64>(magnitude)
                           : static_cast<i64>(magnitude);
  let const digit_width = text.length - digit_start;
  let const leading_zero = digit_width > 1 && text[digit_start] == '0';
  return sequence_integer{value, text.length, leading_zero};
}

fn split_sequence_parts(StringView content, Allocator alloc) throws
    -> ArrayList<StringView>
{
  let parts = ArrayList<StringView>{alloc};
  usize start = 0;
  usize i = 0;
  while (i + 1 < content.length) {
    if (content[i] == '.' && content[i + 1] == '.') {
      parts.push(content.substring_of_length(start, i - start));
      i += 2;
      start = i;
      continue;
    }
    i++;
  }
  parts.push(content.substring(start));
  return parts;
}

fn parse_brace_sequence(StringView content, Allocator alloc) throws
    -> Maybe<ArrayList<String>>
{
  let const parts = split_sequence_parts(content, alloc);
  if (parts.count() != 2 && parts.count() != 3) {
    return None;
  }

  i64 step = 1;
  if (parts.count() == 3) {
    let const parsed_step = parse_sequence_integer(parts[2]);
    if (!parsed_step.has_value()) return None;
    step = parsed_step->value;
  }
  if (step == 0) step = 1;
  const u64 magnitude =
      step < 0 ? static_cast<u64>(-(step + 1)) + 1 : static_cast<u64>(step);

  let const start_int = parse_sequence_integer(parts[0]);
  let const end_int = parse_sequence_integer(parts[1]);
  if (start_int.has_value() && end_int.has_value()) {
    let const from = start_int->value;
    let const to = end_int->value;
    let const increment = from <= to ? static_cast<i128>(magnitude)
                                     : -static_cast<i128>(magnitude);
    const bool should_pad =
        start_int->has_leading_zero || end_int->has_leading_zero;
    let const width = should_pad
                          ? (start_int->field_width > end_int->field_width
                                 ? start_int->field_width
                                 : end_int->field_width)
                          : 0;
    let elements = ArrayList<String>{alloc};
    let const distance = from <= to
                             ? static_cast<u128>(static_cast<i128>(to) - from)
                             : static_cast<u128>(static_cast<i128>(from) - to);
    let const element_count = distance / magnitude + 1;
    if (element_count > static_cast<u128>(static_cast<usize>(-1))) return None;
    elements.reserve(static_cast<usize>(element_count));
    for (i128 current = from; increment > 0 ? current <= to : current >= to;
         current += increment)
    {
      String number = String::from(static_cast<i64>(current), heap_allocator());
      if (should_pad) {
        const bool is_negative = !number.is_empty() && number.view()[0] == '-';
        let const digits = number.view().substring(is_negative ? 1 : 0);
        const usize sign_length = is_negative ? 1 : 0;
        let const digit_width = width > sign_length ? width - sign_length : 0;
        if (digits.length < digit_width) {
          let padded = String{alloc};
          if (is_negative) padded.push('-');
          for (usize z = digits.length; z < digit_width; z++)
            padded.push('0');
          padded.append(digits);
          number = steal(padded);
        }
      }
      elements.push(steal(number));
    }
    return elements;
  }

  if (parts[0].length == 1 && parts[1].length == 1) {
    let const from = parts[0][0];
    let const to = parts[1][0];
    const bool is_from_alpha =
        (from >= 'a' && from <= 'z') || (from >= 'A' && from <= 'Z');
    const bool is_to_alpha =
        (to >= 'a' && to <= 'z') || (to >= 'A' && to <= 'Z');
    if (is_from_alpha && is_to_alpha) {
      let const increment = from <= to ? static_cast<i128>(magnitude)
                                       : -static_cast<i128>(magnitude);
      let elements = ArrayList<String>{alloc};
      for (i128 c = from; increment > 0 ? c <= to : c >= to; c += increment) {
        let element = String{alloc};
        element.push(static_cast<char>(c));
        elements.push(steal(element));
      }
      return elements;
    }
  }
  return None;
}

fn brace_group_alternatives(StringView content, Allocator alloc) throws
    -> Maybe<ArrayList<String>>
{
  usize depth = 0;
  let comma_positions = ArrayList<usize>{alloc};
  for (usize i = 0; i < content.length; i++) {
    let const c = content[i];
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      if (depth > 0) depth--;
    } else if (c == ',' && depth == 0) {
      comma_positions.push(i);
    }
  }

  if (!comma_positions.is_empty()) {
    let alternatives = ArrayList<String>{alloc};
    alternatives.reserve(comma_positions.count() + 1);
    usize start = 0;
    for (const usize comma : comma_positions) {
      alternatives.push(
          String{alloc, content.substring_of_length(start, comma - start)});
      start = comma + 1;
    }
    alternatives.push_managed(content.substring(start));
    return alternatives;
  }

  return parse_brace_sequence(content, alloc);
}

struct brace_group
{
  usize open;
  usize close;
  ArrayList<String> alternatives{heap_allocator()};
};

fn find_brace_group(StringView text, Allocator alloc) throws
    -> Maybe<brace_group>
{
  for (usize open = 0; open < text.length; open++) {
    if (text[open] != '{') continue;
    usize depth = 0;
    for (usize j = open; j < text.length; j++) {
      let const c = text[j];
      if (c == '{') {
        depth++;
      } else if (c == '}') {
        depth--;
        if (depth == 0) {
          let alternatives = brace_group_alternatives(
              text.substring_of_length(open + 1, j - open - 1), alloc);
          if (alternatives.has_value()) {
            let group = brace_group{open, j, ArrayList<String>{alloc}};
            group.alternatives = steal(*alternatives);
            return group;
          }
          break;
        }
      }
    }
  }
  return None;
}

/* The recursion cap keeps a pathological {a,{b,{c,...}}} off the native
   stack. */
constexpr usize MAX_BRACE_DEPTH = 256;

fn brace_expand_text(StringView text, Allocator alloc, usize depth = 0) throws
    -> ArrayList<String>
{
  let results = ArrayList<String>{alloc};
  let const group = find_brace_group(text, alloc);
  if (!group.has_value() || depth >= MAX_BRACE_DEPTH) {
    results.push_managed(text);
    return results;
  }

  let const preamble = text.substring_of_length(0, group->open);
  let const postamble = text.substring(group->close + 1);
  let const post_expansions = brace_expand_text(postamble, alloc, depth + 1);

  for (const String &alternative : group->alternatives) {
    for (const String &expanded_alt :
         brace_expand_text(alternative.view(), alloc, depth + 1))
    {
      for (const String &expanded_post : post_expansions) {
        let combined = String{alloc, preamble};
        combined.append(expanded_alt.view());
        combined.append(expanded_post.view());
        results.push(steal(combined));
      }
    }
  }
  return results;
}

fn expand_braces(const Word &word, Allocator alloc) throws -> ArrayList<Word>
{
  let opaque_segments = ArrayList<const WordSegment *>{alloc};
  let word_template = String{alloc};
  for (const WordSegment &segment : word.segments) {
    if (segment.kind == WordSegment::Kind::UnquotedText) {
      for (usize byte_index = 0; byte_index < segment.text.count();
           byte_index++)
      {
        let const byte = segment.text[byte_index];
        word_template.push(byte);
        if (byte == BRACE_OPAQUE_MARKER)
          word_template.push(BRACE_OPAQUE_MARKER);
      }
    } else {
      word_template.push(BRACE_OPAQUE_MARKER);
      word_template.push('O');
      word_template += String::from(opaque_segments.count(), alloc);
      word_template.push(';');
      opaque_segments.push(&segment);
    }
  }

  let const expanded = brace_expand_text(word_template.view(), alloc);

  let words = ArrayList<Word>{alloc};
  words.reserve(expanded.count());
  for (const String &produced : expanded) {
    let out = Word{};
    let run = String{alloc};
    for (usize i = 0; i < produced.count(); i++) {
      let const c = produced[i];
      if (c == BRACE_OPAQUE_MARKER && i + 1 < produced.count() &&
          produced[i + 1] == BRACE_OPAQUE_MARKER)
      {
        run.push(BRACE_OPAQUE_MARKER);
        i++;
        continue;
      }

      usize marker_end = i + 2;
      usize opaque_index = 0;
      bool is_opaque_marker = c == BRACE_OPAQUE_MARKER &&
                              i + 2 < produced.count() &&
                              produced[i + 1] == 'O';
      if (is_opaque_marker) {
        bool has_digit = false;
        while (marker_end < produced.count() && produced[marker_end] >= '0' &&
               produced[marker_end] <= '9')
        {
          has_digit = true;
          let const digit = static_cast<usize>(produced[marker_end] - '0');
          if (opaque_index > (static_cast<usize>(-1) - digit) / 10) {
            is_opaque_marker = false;
            break;
          }
          opaque_index = opaque_index * 10 + digit;
          marker_end++;
        }
        is_opaque_marker &= has_digit && marker_end < produced.count() &&
                            produced[marker_end] == ';' &&
                            opaque_index < opaque_segments.count();
      }

      if (is_opaque_marker) {
        if (!run.is_empty()) {
          out.segments.push(WordSegment{
              WordSegment::Kind::UnquotedText, SegmentText{alloc, run.view()},
              false
          });
          run = String{alloc};
        }
        out.segments.push(*opaque_segments[opaque_index]);
        i = marker_end;
      } else {
        run.push(c);
      }
    }
    if (!run.is_empty()) {
      out.segments.push(WordSegment{
          WordSegment::Kind::UnquotedText, SegmentText{alloc, run.view()},
          false
      });
    }

    for (usize s = 0; s + 1 < out.segments.count(); s++) {
      WordSegment &reference = out.segments[s];
      if (reference.kind != WordSegment::Kind::VariableReference ||
          !reference.is_greedy_name)
        continue;

      WordSegment &following = out.segments[s + 1];
      if (following.kind != WordSegment::Kind::UnquotedText) continue;

      usize taken = 0;
      while (taken < following.text.count() &&
             lexer::is_variable_name(following.text.view()[taken]))
        taken++;
      if (taken == 0) continue;

      let joined = String{alloc, reference.text.view()};
      joined.append(following.text.view().substring_of_length(0, taken));
      reference.text.assign_copy(alloc, joined.view());

      if (taken == following.text.count()) {
        out.segments.remove(s + 1);
      } else {
        following.text.assign_copy(alloc,
                                   following.text.view().substring(taken));
      }
    }

    words.push(steal(out));
  }
  LOG(Debug, "brace expansion produced %zu words", words.count());
  return words;
}

} /* namespace */

static pure fn segment_is_literal(const WordSegment &segment) wontthrow -> bool
{
  return segment.kind == WordSegment::Kind::LiteralText ||
         segment.kind == WordSegment::Kind::UnquotedText;
}

static fn word_starts_array_subscript(const Word &word) wontthrow -> bool
{
  if (word.segments.is_empty()) return false;

  const WordSegment &first = word.segments[0];
  if (!segment_is_literal(first) || first.text.is_empty() ||
      first.text.view()[0] != '[')
    return false;

  for (usize segment_position = 0; segment_position < word.segments.count();
       segment_position++)
  {
    const WordSegment &segment = word.segments[segment_position];
    if (!segment_is_literal(segment)) continue;

    let const text = segment.text.view();
    for (usize i = 0; i < text.length; i++) {
      if (text[i] != ']') continue;

      if (i + 1 < text.length) return text[i + 1] == '=';

      for (usize next = segment_position + 1; next < word.segments.count();
           next++)
      {
        if (!segment_is_literal(word.segments[next])) return false;
        let const next_text = word.segments[next].text.view();
        if (next_text.is_empty()) continue;
        return next_text[0] == '=';
      }
      return false;
    }
  }

  return false;
}

enum declaration_command_flag : u8
{
  declaration_flag_local = 1 << 0,
  declaration_flag_declare = 1 << 1,
  declaration_flag_declaration = 1 << 2,
  declaration_flag_test = 1 << 3,
};

constexpr static_string_entry<u8> DECLARATION_COMMAND_ENTRIES[] = {
    {SSK("local"),    declaration_flag_local | declaration_flag_declaration  },
    {SSK("declare"),  declaration_flag_declare | declaration_flag_declaration},
    {SSK("typeset"),  declaration_flag_declare | declaration_flag_declaration},
    {SSK("export"),   declaration_flag_declaration                           },
    {SSK("readonly"), declaration_flag_declaration                           },
    {SSK("unset"),    declaration_flag_declaration                           },
    {SSK("test"),     declaration_flag_test                                  },
};
constexpr StaticStringMap DECLARATION_COMMANDS{DECLARATION_COMMAND_ENTRIES};

hot fn EvalContext::process_args(
    const ArrayList<const Token *> &args, argument_lifetime lifetime,
    argument_context context,
    ArrayList<SourceLocation> *expanded_locations) throws -> ArrayList<String>
{
  let const args_are_transient = lifetime == argument_lifetime::Transient;
  let const is_array_literal = context == argument_context::ArrayLiteral;
  LOG(Debug, "expanding %zu argument tokens", args.count());
  /* A transient request lives on the caller's scratch region and leaves its
     fields for the caller, so only the heap form releases them on return. */
  let expanded_args = args_are_transient
                          ? ArrayList<String>{scratch_allocator()}
                          : ArrayList<String>{heap_allocator()};
  expanded_args.reserve(args.count());

  /* The location list mirrors the string list one field per entry, so a builtin
     can caret the specific token a field came from. It lives on the same
     allocator as the strings. */
  if (expanded_locations != nullptr) {
    *expanded_locations = args_are_transient
                              ? ArrayList<SourceLocation>{scratch_allocator()}
                              : ArrayList<SourceLocation>{heap_allocator()};
    expanded_locations->reserve(args.count());
  }
  let const do_record_location = [expanded_locations](SourceLocation loc)
                                     wontthrow -> void {
    if (expanded_locations != nullptr) expanded_locations->push(steal(loc));
  };

  let const fields_mark = m_scratch_arena.mark();
  defer
  {
    if (!args_are_transient) m_scratch_arena.release(fields_mark);
  };

  /* A declaration builtin treats a name=value argument as an assignment whose
     value expands with no field splitting or globbing. The plain literal
     command word is decided before any expansion, the way bash does. */
  let is_declaration_command = false;
  let is_local_command = false;
  let is_declare_command = false;
  let is_test_command = false;
  if (!args.is_empty() && args[0]->kind() == Token::Kind::Word) {
    const Word &command_word =
        static_cast<const tokens::WordToken *>(args[0])->word();
    if (command_word.plain_literal_kind() != Word::PlainLiteral::NotPlain) {
      let const flags =
          DECLARATION_COMMANDS.find(command_word.constant_value()).value_or(0);
      is_local_command = (flags & declaration_flag_local) != 0;
      is_declare_command = (flags & declaration_flag_declare) != 0;
      is_declaration_command = (flags & declaration_flag_declaration) != 0;
      is_test_command = (flags & declaration_flag_test) != 0;
    }
    /* The lone bracket is the test builtin and earns the same glob exemption,
       though it never classifies as a plain literal above. */
    else if (command_word.segments.count() == 1 &&
             command_word.segments[0].kind == WordSegment::Kind::UnquotedText &&
             command_word.segments[0].text.view() == "[")
    {
      is_test_command = true;
    }
  }

  /* A test or [ command probes the filesystem, so an unmatched glob there stays
     literal and the probe returns false rather than tripping failglob. A
     declaration command such as unset names a variable, so its operand stays
     literal too. */
  let const previous_glob_exempt = m_glob_exempt_for_test;
  m_glob_exempt_for_test = is_test_command || is_declaration_command;
  defer { m_glob_exempt_for_test = previous_glob_exempt; };

  /* An unset variable in a test operand is the question the command asks, so
     the advisory warning is suppressed. An explicit set -u still aborts. */
  let const previous_suppress_test_warning =
      is_warning_suppressed(suppressible_warning::UnsetTestOperand);
  if (is_test_command)
    set_warning_suppressed(suppressible_warning::UnsetTestOperand, true);
  defer
  {
    set_warning_suppressed(suppressible_warning::UnsetTestOperand,
                           previous_suppress_test_warning);
  };

  for (const Token *token : args) {
    let const location = token->source_location();
    try {
      let fallback_word = Maybe<Word>{};
      const Word *word = nullptr;
      if (token->kind() == Token::Kind::Word) {
        word = &static_cast<const tokens::WordToken *>(token)->word();
      } else if (token->kind() == Token::Kind::Assignment) {
        let const assignment_token =
            static_cast<const tokens::Assignment *>(token);
        ASSERT(assignment_token != nullptr);
        if (is_declaration_command) {
          let assignment = String{expanded_args.allocator()};
          assignment.append(assignment_token->key().view());
          if (assignment_token->is_append() &&
              (is_local_command || is_declare_command))
          {
            /* local shadows an outer name and declare may apply -i on the same
               command, so name+=value passes through literally and the builtin
               computes the append after its own effects exist. */
            assignment += '+';
            assignment += '=';
            assignment.append(
                expand_word_for_assignment(assignment_token->value_word())
                    .view());
          } else {
            assignment += '=';
            if (assignment_token->is_append()) {
              let const existing = get_variable_value(assignment_token->key());
              if (existing.has_value()) assignment.append(existing->view());
            }
            let const expanded_value =
                expand_word_for_assignment(assignment_token->value_word());
            /* An integer name adds rather than concatenates. */
            if (assignment_token->is_append() &&
                is_integer_variable(assignment_token->key()))
            {
              append_integer_expression(assignment, expanded_value.view());
            } else {
              assignment.append(expanded_value.view());
            }
          }
          expanded_args.push(steal(assignment));
          do_record_location(location);
          continue;
        }
        /* An assignment as an argument, like echo k=$v, is an ordinary word. */
        let key_literal = String{assignment_token->key().view()};
        if (assignment_token->is_append()) key_literal += "+";
        key_literal += "=";
        fallback_word = Word{};
        fallback_word->segments = ArrayList<WordSegment>{scratch_allocator()};
        fallback_word->segments.push(WordSegment{
            WordSegment::Kind::LiteralText,
            SegmentText{scratch_allocator(), key_literal.view()},
            false
        });
        let const &value = assignment_token->value_word();
        for (const WordSegment &value_segment : value.segments)
          fallback_word->segments.push(value_segment);
        word = &*fallback_word;
      } else {
        fallback_word = Word{};
        fallback_word->segments = ArrayList<WordSegment>{scratch_allocator()};
        fallback_word->segments.push(WordSegment{
            WordSegment::Kind::UnquotedText,
            SegmentText{scratch_allocator(), token->raw_string().view()},
            false
        });
        word = &*fallback_word;
      }

      if (is_array_literal && word != nullptr &&
          word_starts_array_subscript(*word))
      {
        expanded_args.push(String{expanded_args.allocator(),
                                  expand_word_for_assignment(*word).view()});
        do_record_location(location);
        continue;
      }

      let const do_expand_one_word = [&](const Word &expandable)
                                         throws -> void {
        let const plain_kind = expandable.plain_literal_kind();
        let did_take_fast_path = false;
        if (plain_kind == Word::PlainLiteral::PlainNoSplit) {
          expanded_args.push(
              String{expanded_args.allocator(), expandable.constant_value()});
          do_record_location(location);
          did_take_fast_path = true;
        } else if (plain_kind == Word::PlainLiteral::PlainUnquotedOneSegment) {
          /* A single unquoted segment still needs the IFS check, since an IFS
             byte in its text splits it into more than one field. */
          let literal = String{expanded_args.allocator(),
                               expandable.segments[0].text.view()};

          let should_split = false;
          for (usize i = 0; i < literal.count(); i++)
            if (is_field_separator(literal[i])) {
              should_split = true;
              break;
            }

          if (!should_split) {
            expanded_args.push(steal(literal));
            do_record_location(location);
            did_take_fast_path = true;
          }
        }

        /* A lone "$@" copies each positional parameter straight into the
           vector, the hot path of a set -- "$@" extra growth loop. */
        if (!did_take_fast_path && expandable.segments.count() == 1) {
          const WordSegment &only = expandable.segments[0];
          if (only.kind == WordSegment::Kind::VariableReference &&
              only.is_in_double_quotes && only.text.view() == "@")
          {
            for (const String &param : m_positional_params) {
              expanded_args.push(
                  String{expanded_args.allocator(), param.view()});
              do_record_location(location);
            }
            did_take_fast_path = true;
          }
        }

        /* The single-field fast path covers a word that can neither split nor
           glob, expanding straight into one argument. A positional or array
           reference, an unquoted segment, a substitution, and a leading tilde
           fall through to the full machine. */
        if (!did_take_fast_path) {
          let is_single_field = !expandable.segments.is_empty();
          for (const WordSegment &segment : expandable.segments) {
            if (!is_single_field) break;

            if (segment.is_tilde_candidate() && !segment.text.is_empty() &&
                segment.text.first_character() == '~')
            {
              is_single_field = false;
              break;
            }

            switch (segment.kind) {
            case WordSegment::Kind::LiteralText:
            case WordSegment::Kind::DoubleQuotedText: break;
            case WordSegment::Kind::ArithmeticExpansion:
              if (!segment.is_in_double_quotes) is_single_field = false;
              break;
            case WordSegment::Kind::VariableReference: {
              /* The '@', '*', and '[' bytes mark a reference that expands to
                 many fields, so their absence leaves a scalar of one field. */
              let const spec = segment.text.view();
              let has_multi_field_marker = !segment.is_in_double_quotes;
#pragma clang loop unroll_count(4)
              for (usize i = 0; !has_multi_field_marker && i < spec.length; i++)
              {
                let const byte = spec[i];
                if (byte == '@' || byte == '*' || byte == '[') {
                  has_multi_field_marker = true;
                }
              }
              if (has_multi_field_marker) is_single_field = false;
            } break;
            default: is_single_field = false; break;
            }
          }

          if (is_single_field) {
            let value = String{expanded_args.allocator()};
            for (const WordSegment &segment : expandable.segments) {
              switch (segment.kind) {
              case WordSegment::Kind::VariableReference: {
                let const spec = segment.text.view();
                let is_plain_name =
                    !spec.is_empty() && lexer::is_variable_name_start(spec[0]);
#pragma clang loop unroll_count(4)
                for (usize i = 1; is_plain_name && i < spec.length; i++)
                  if (!lexer::is_variable_name(spec[i])) is_plain_name = false;
                if (is_plain_name)
                  if (let const *stored = lookup_shell_variable(spec);
                      stored != nullptr)
                  {
                    value += stored->view();
                    break;
                  }
                let const source_location = segment.get_source_location(
                    m_current_location.source_name_index);
                value += apply_parameter_expansion(
                    spec,
                    source_location.has_value() ? &*source_location : nullptr);
              } break;
              case WordSegment::Kind::ArithmeticExpansion: {
                value += evaluate_arithmetic_cached_text(segment).view();
              } break;
              default: value += segment.text.view(); break;
              }
            }
            expanded_args.push(steal(value));
            do_record_location(location);
            did_take_fast_path = true;
          }
        }

        if (!did_take_fast_path) {
          for (glob_field &field : expand_word(expandable)) {
            /* A field with no active glob is its own single result, pushed
               straight in without a directory scan. */
            if (no_glob() ||
                !first_active_glob(field.text.view(), field.glob_active,
                                   extglob_enabled())
                     .has_value())
            {
              expanded_args.push_managed(field.text.view());
              do_record_location(location);
              continue;
            }
            for (String &g : expand_path(steal(field), location)) {
              expanded_args.push_managed(StringView{g.c_str(), g.count()});
              do_record_location(location);
            }
          }
        }
      };

      if (bash_additions_enabled() &&
          shell_option_state(shell_option_id::Braceexpand) &&
          word_has_brace_candidate(*word))
      {
        for (const Word &brace_word : expand_braces(*word, scratch_allocator()))
          do_expand_one_word(brace_word);
      } else {
        do_expand_one_word(*word);
      }
    } catch (const ErrorWithLocation &) {
      throw;
    } catch (const Error &e) {
      relocate_error(e, location);
    }
  }

  /* The first character of PS4 is repeated once per enclosing subshell before
     the whole prefix, so the default '+ ' shows '++ ' in a substitution.
     BASH_XTRACEFD steers the trace to a descriptor, falling back to standard
     error when unset or unparsable. */
  if (should_echo_expanded()) {
    let trace = String{scratch_allocator()};
    let const ps4 = get_variable_value("PS4").value_or(String{"+ "});
    if (!ps4.is_empty()) {
      for (usize i = 0; i < m_subshell_depth; i++)
        trace.push(ps4[0]);
      trace.append(ps4.view());
    }
    trace.append(utils::merge_args_to_string(expanded_args));
    trace.push('\n');

    Maybe<i64> xtrace_fd;
    if (Maybe<String> xtrace_fd_value = get_variable_value("BASH_XTRACEFD");
        xtrace_fd_value.has_value())
    {
      let const parsed = xtrace_fd_value->view().to<i64>();
      if (!parsed.is_error() && parsed.value() >= 0) {
        xtrace_fd = parsed.value();
      }
    }

    if (xtrace_fd.has_value())
      (void) os::write_to_numbered_fd(*xtrace_fd, trace.data(), trace.length());
    else
      koshka::print_error(trace);
  }

  return expanded_args;
}

} /* namespace koshka */
