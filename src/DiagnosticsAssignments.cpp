#include "DiagnosticsChecksInternal.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

namespace {

fn check_posix_redirection_portability(AnalysisContext &actx,
                                       const Redirection &redirection,
                                       SourceLocation fallback_location) throws
    -> void
{
  let const do_get_location = [&]() -> SourceLocation {
    return redirection.target != nullptr ? redirection.target->source_location()
                                         : fallback_location;
  };

  if (redirection.fd_allocation_name_token != nullptr) {
    let const name = redirection.fd_allocation_name_token->raw_view();
    actx.report_diagnostic(
        diagnostic_id::sc3022,
        redirection.fd_allocation_name_token->source_location(),
        {name.value_or(StringView{})});
  }

  let const descriptor =
      redirection.fd > redirection.dup_fd ? redirection.fd : redirection.dup_fd;
  if (descriptor > 9) {
    let const descriptor_text = String::from(descriptor, heap_allocator());
    actx.report_diagnostic(diagnostic_id::sc3023, do_get_location(),
                           {descriptor_text.view()});
  }

  if (redirection.is_both_streams_spelling)
    actx.report_diagnostic(diagnostic_id::sc3020, do_get_location());

  if (redirection.target == nullptr) return;

  let const target_text = redirection.target->raw_view();
  if (!target_text.has_value()) return;

  let const view = *target_text;
  if (redirection.kind == Redirection::Kind::DuplicateOutput &&
      redirection.can_dup_be_filename)
  {
    actx.report_diagnostic(diagnostic_id::sc3021, do_get_location(), {view});
  }

  if (view.starts_with(StringView{"/dev/tcp/"}) ||
      view.starts_with(StringView{"/dev/udp/"}))
  {
    actx.report_diagnostic(diagnostic_id::sc3025, do_get_location(), {view});
  }
}

/* A bare word naming one of these programs is almost always a missing command
   substitution or a missing pipe, shellcheck SC2209 and SC2238. Words that read
   naturally as data, such as test, id, set, true, and echo, are left out. */
constexpr PackedStringKey COMMAND_NAME_VALUE_KEYS[] = {
    SSK("awk"),   SSK("cat"),    SSK("chmod"), SSK("chown"), SSK("cp"),
    SSK("curl"),  SSK("docker"), SSK("git"),   SSK("grep"),  SSK("hostname"),
    SSK("ln"),    SSK("ls"),     SSK("mkdir"), SSK("mv"),    SSK("printf"),
    SSK("pwd"),   SSK("rm"),     SSK("rmdir"), SSK("sed"),   SSK("ssh"),
    SSK("sudo"),  SSK("touch"),  SSK("tr"),    SSK("uname"), SSK("whoami"),
    SSK("xargs"),
};
constexpr StaticStringSet COMMAND_NAME_VALUES{COMMAND_NAME_VALUE_KEYS};

cold fn plain_output_redirection_spelling(Redirection::Kind kind) wontthrow
    -> Maybe<StringView>
{
  switch (kind) {
  case Redirection::Kind::TruncateOutput: return StringView{">"};
  case Redirection::Kind::TruncateOutputOverride: return StringView{">|"};
  case Redirection::Kind::AppendOutput: return StringView{">>"};
  default: return None;
  }
}

} /* namespace */

/* The redirection lints. 2>&1 before the stdout file redirect is SC2069,
   reading and truncating the same file is SC2094, an input redirect into a
   non-stdin command is SC2217. */
fn check_redirection_lints(AnalysisContext &actx,
                           const command_lint_input &input) throws -> void
{
  let saw_stderr_to_stdout = false;
  /* An owned String, since the view of a to_literal_string() temporary would
     dangle past the statement. */
  String read_target{heap_allocator()};
  const Token *read_token = nullptr;
  let const is_test_command =
      input.is_in_group(COMMAND_GROUP_TEST) && !input.command_is_shadowed;
  /* Descriptors 0 through 9 are the ones a script writes, and the location of
     the first claim is kept so the second claim can point back at it. */
  SourceLocation claimed_fd_locations[10]{};
  u16 claimed_fd_mask = 0;

  for (let const &redirection : input.redirections) {
    if (redirection.kind == Redirection::Kind::DuplicateOutput &&
        redirection.fd == 2 && redirection.dup_fd == 1)
    {
      saw_stderr_to_stdout = true;
      continue;
    }

    let const is_posix = actx.is_posix_sh_shebang;
    if (is_posix) {
      check_posix_redirection_portability(actx, redirection,
                                          input.command_location());
    }

    /* A descriptor points at one file, so a second claim silently wins and the
       first is lost, shellcheck SC2261. */
    if (redirection.claims_descriptor() && redirection.fd >= 0 &&
        redirection.fd < 10 && redirection.target != nullptr)
    {
      let const fd_bit = static_cast<u16>(1u << redirection.fd);
      if ((claimed_fd_mask & fd_bit) != 0) {
        actx.report_diagnostic(
            diagnostic_id::sc2261, redirection.target->source_location(),
            {redirection.target->raw_view().value_or(StringView{})},
            claimed_fd_locations[redirection.fd]);
      } else {
        claimed_fd_mask |= fd_bit;
        claimed_fd_locations[redirection.fd] =
            redirection.target->source_location();
      }
    }

    /* The local shell expands an unquoted here document body before ssh sends
       it, so the remote host receives values from this host, shellcheck
       SC2087. */
    if (redirection.kind == Redirection::Kind::Heredoc &&
        redirection.should_expand_heredoc &&
        input.command_id() == command_name_id::Ssh &&
        !input.command_is_shadowed)
    {
      actx.report_diagnostic(diagnostic_id::sc2087,
                             redirection.target != nullptr
                                 ? redirection.target->source_location()
                                 : input.command_location());
    }

    let const is_file_output =
        redirection.kind == Redirection::Kind::TruncateOutput ||
        redirection.kind == Redirection::Kind::TruncateOutputOverride;
    if (is_file_output && redirection.fd == 1 && saw_stderr_to_stdout &&
        redirection.target != nullptr)
    {
      actx.report_diagnostic(diagnostic_id::sc2069,
                             redirection.target->source_location());
    }

    if (redirection.target != nullptr) {
      let const output_spelling =
          plain_output_redirection_spelling(redirection.kind);
      let const is_input_redirection =
          redirection.kind == Redirection::Kind::ReadInput;

      if (is_test_command) {
        if (output_spelling.has_value()) {
          actx.report_diagnostic(diagnostic_id::sc2065,
                                 redirection.target->source_location(),
                                 {*output_spelling});
        }
        if (is_input_redirection) {
          actx.report_diagnostic(diagnostic_id::sc2073,
                                 redirection.target->source_location());
        }
      } else if (output_spelling.has_value() || is_input_redirection) {
        let const digits = redirection.target->raw_view();
        if (digits.has_value() && !digits->is_empty() &&
            digits->is_all_decimal_digits())
        {
          actx.report_diagnostic(diagnostic_id::sc2210,
                                 redirection.target->source_location(),
                                 {*digits});
        }

        /* Quoting the name states that a file is meant, and the word literal
           drops the quotes, so the source text decides, shellcheck SC2238. */
        if (digits.has_value() && COMMAND_NAME_VALUES.contains(*digits)) {
          let const target_source =
              redirection.target->source_location().get_source_text(
                  actx.source);
          if (target_source.has_value() && !target_source->is_empty() &&
              (*target_source)[0] != '"' && (*target_source)[0] != '\'')
          {
            actx.report_diagnostic(diagnostic_id::sc2238,
                                   redirection.target->source_location(),
                                   {*digits});
          }
        }
      }
    }

    if (redirection.target != nullptr &&
        redirection.target->kind() == Token::Kind::Word)
    {
      let const &target_word =
          static_cast<const tokens::WordToken *>(redirection.target)->word();
      let has_glob_target = false;

      for (let const &segment : target_word.segments) {
        if (is_posix && !has_glob_target) {
          has_glob_target =
              segment.has_live_glob_chars() && segment.has_glob_metacharacter();
        }

        if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
            (view_contains(segment.text.view(), StringView{"++"}) ||
             view_contains(segment.text.view(), StringView{"--"}) ||
             segment.text.view().find_character('=').has_value()))
        {
          actx.report_diagnostic(diagnostic_id::sc2257,
                                 redirection.target->source_location());
          break;
        }
      }

      if (has_glob_target) {
        actx.report_diagnostic(
            diagnostic_id::sc3031, redirection.target->source_location(),
            {redirection.target->raw_view().value_or(StringView{})});
      }
    }
    if (redirection.kind == Redirection::Kind::ReadInput &&
        redirection.target != nullptr &&
        redirection.target->kind() == Token::Kind::Word)
    {
      read_target = static_cast<const tokens::WordToken *>(redirection.target)
                        ->word()
                        .to_literal_string();
      read_token = redirection.target;
    }
    if (is_file_output && redirection.target != nullptr &&
        redirection.target->kind() == Token::Kind::Word &&
        read_token != nullptr)
    {
      let const write_target =
          static_cast<const tokens::WordToken *>(redirection.target)
              ->word()
              .to_literal_string();
      if (!read_target.is_empty() && write_target.view() == read_target.view())
      {
        actx.report_diagnostic(
            diagnostic_id::sc2094, redirection.target->source_location(),
            {read_target.view()}, read_token->source_location());
      }
    }
  }

  if (input.redirections.is_empty() || input.command_is_shadowed) {
    return;
  }
  if (!input.is_in_group(COMMAND_GROUP_NON_STDIN_READER)) return;
  if (args_have_stdin_operand(input.args)) return;

  for (let const &redirection : input.redirections)
    if (redirection.kind == Redirection::Kind::ReadInput ||
        redirection.kind == Redirection::Kind::Heredoc ||
        redirection.kind == Redirection::Kind::HereString)
    {
      actx.report_diagnostic(diagnostic_id::sc2217, input.command_location(),
                             {input.command_literal});
      break;
    }
}

fn check_test_operand_lints(AnalysisContext &actx,
                            const command_lint_input &input) throws -> void
{
  if (!input.is_in_group(COMMAND_GROUP_TEST) || input.command_is_shadowed)
    return;

  let const &args = input.args;
  let const is_posix = actx.is_posix_sh_shebang;

  /* The operand range excludes the closing bracket, so the operator loop and
     the operand loop share one bound. */
  usize operand_end = args.count();
  bool is_bracket_form_closed = true;
  if (input.command_id() == command_name_id::SingleBracket ||
      input.command_id() == command_name_id::DoubleBracket)
  {
    is_bracket_form_closed =
        args.count() >= 2 &&
        args[args.count() - 1]->kind() == Token::Kind::Word &&
        static_cast<const tokens::WordToken *>(args[args.count() - 1])
                ->word()
                .to_literal_string()
                .view() ==
            (input.command_id() == command_name_id::SingleBracket ? "]" : "]]");
    if (is_bracket_form_closed) operand_end = args.count() - 1;
  }

  /* Obsolescent or redundant test forms. -a or -o joining two conditions is
     SC2166, warned only past the first operand and not after a !. A negated -z
     or -n is SC2236 and SC2237. */
  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const literal = static_cast<const tokens::WordToken *>(args[i])
                            ->word()
                            .to_literal_string();
    let const view = literal.view();
    /* The literal of the previous word, empty for a non-word predecessor. */
    let const previous_literal =
        args[i - 1]->kind() == Token::Kind::Word
            ? static_cast<const tokens::WordToken *>(args[i - 1])
                  ->word()
                  .to_literal_string()
            : String{heap_allocator()};
    /* == is a bashism in test, shellcheck SC3014, warned only when == sits in
       the operator slot so [ x = == ] comparing the literal == is left
       alone. */
    if (view == "==" && i >= 2 &&
        !is_test_binary_operator_word(previous_literal.view()))
    {
      actx.report_diagnostic(diagnostic_id::sc3014, args[i]->source_location());
    }
    let const previous_is_bang = previous_literal.view() == "!";

    /* A dash-led word that names no operator, shellcheck SC2057 and SC2058.
       The word after a condition opener sits in the unary slot, the word after
       a plain operand sits in the binary slot, and the word after a known
       operator is an operand. */
    if (view_looks_like_test_operator(view) && view[1] != '-' &&
        !is_known_test_operator_word(view))
    {
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const is_unary_slot =
          i == 1 || is_test_condition_opener_word(previous_literal.view());
      let is_string_operand = false;
      if (is_unary_slot && i + 1 < args.count() &&
          args[i + 1]->kind() == Token::Kind::Word)
      {
        /* A three-word test compares strings when the middle word is a binary
           operator, so [ -verbose = "$1" ] holds an operand. */
        let const next = static_cast<const tokens::WordToken *>(args[i + 1])
                             ->word()
                             .to_literal_string();
        is_string_operand = is_test_binary_operator_word(next.view());
      }

      if (!is_string_operand && word_is_fully_literal(word)) {
        if (is_unary_slot) {
          actx.report_diagnostic(diagnostic_id::sc2058,
                                 args[i]->source_location(), {view});
        } else if (!is_known_test_operator_word(previous_literal.view())) {
          actx.report_diagnostic(diagnostic_id::sc2057,
                                 args[i]->source_location(), {view});
        }
      }
    }

    if (view == ">=" || view == "<=") {
      actx.report_diagnostic(diagnostic_id::sc2122, args[i]->source_location(),
                             {view});
    }

    /* Both sides of a comparison are written out, so the answer is fixed,
       shellcheck SC2050. The file comparisons read the filesystem and are left
       alone. */
    if (i >= 2 && i + 1 < operand_end && is_test_binary_operator_word(view) &&
        !is_test_file_comparison_word(view) &&
        !is_test_binary_operator_word(previous_literal.view()) &&
        args[i - 1]->kind() == Token::Kind::Word &&
        args[i + 1]->kind() == Token::Kind::Word)
    {
      let const &left =
          static_cast<const tokens::WordToken *>(args[i - 1])->word();
      let const &right =
          static_cast<const tokens::WordToken *>(args[i + 1])->word();
      if (word_is_fully_literal(left) && word_is_fully_literal(right)) {
        /* A bracket test receives its operands already expanded, so a glob or a
           brace list on either side is not the text that is compared. */
        let const left_shape = classify_test_operand(left);
        let const right_shape = classify_test_operand(right);
        let const is_expanded_before_test =
            left_shape.has_unquoted_glob || right_shape.has_unquoted_glob ||
            left_shape.has_brace_expansion || right_shape.has_brace_expansion;

        if (!is_expanded_before_test) {
          actx.report_diagnostic(
              diagnostic_id::sc2050,
              location_spanning(args[i - 1]->source_location(),
                                args[i + 1]->source_location()),
              {view});
        }
      }
    }

    /* The bracket test takes one word per operand, so an operand that expands
       to several words leaves the test with a stray argument. The shape is read
       once from the segments the word already holds. */
    if (i < operand_end) {
      let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
      let const shape = classify_test_operand(word);
      if (shape.has_positional_reference) actx.mark_positional_reference();

      let const written =
          analysis_source_text(actx, args[i]->source_location());
      let const previous = previous_literal.view();

      if (shape.has_array_spread) {
        actx.report_diagnostic(diagnostic_id::sc2198,
                               args[i]->source_location(), {written});
      }

      if (shape.has_brace_expansion) {
        actx.report_diagnostic(diagnostic_id::sc2200,
                               args[i]->source_location(), {written});
      }

      if (shape.has_unquoted_glob) {
        if (previous == "-v") {
          actx.report_diagnostic(diagnostic_id::sc2208,
                                 args[i]->source_location(), {written});
        } else if (is_test_path_unary_operator_word(previous)) {
          actx.report_diagnostic(diagnostic_id::sc2245,
                                 args[i]->source_location(),
                                 {previous, written});
        } else if (!is_test_binary_operator_word(previous)) {
          actx.report_diagnostic(diagnostic_id::sc2202,
                                 args[i]->source_location(), {written});
        }
      }

      if (shape.has_unquoted_expansion && previous == "-n") {
        actx.report_diagnostic(diagnostic_id::sc2070,
                               args[i]->source_location());
      }
    }

    if (is_posix) {
      let const is_operator_slot =
          i >= 2 && !is_test_binary_operator_word(previous_literal.view());
      if ((view == "<" || view == ">") && is_operator_slot) {
        actx.report_diagnostic(diagnostic_id::sc3012,
                               args[i]->source_location(), {view});
      } else if (is_test_file_comparison_word(view) && is_operator_slot) {
        actx.report_diagnostic(diagnostic_id::sc3013,
                               args[i]->source_location(), {view});
      } else if (view == "-a" && (i == 1 || previous_is_bang)) {
        actx.report_diagnostic(diagnostic_id::sc3017,
                               args[i]->source_location());
      }
    }

    /* Two inequalities on the same operand joined by -o hold for every value,
       shellcheck SC2056. */
    if (view == "-o" && i >= 3) {
      let const before = test_inequality_left_operand(args, i - 2, operand_end);
      let const after = test_inequality_left_operand(args, i + 2, operand_end);
      if (before.has_value() && after.has_value() && *before == *after) {
        actx.report_diagnostic(diagnostic_id::sc2056,
                               args[i]->source_location(), {*before});
      }
    }

    if (i >= 2 && !previous_is_bang && (view == "-a" || view == "-o")) {
      actx.report_diagnostic(diagnostic_id::sc2166, args[i]->source_location());
    } else if (view == "!" && i + 1 < args.count() &&
               args[i + 1]->kind() == Token::Kind::Word)
    {
      let const next = static_cast<const tokens::WordToken *>(args[i + 1])
                           ->word()
                           .to_literal_string();
      if (next.view() == "-z") {
        actx.report_diagnostic(diagnostic_id::sc2236,
                               args[i]->source_location());
      } else if (next.view() == "-n") {
        actx.report_diagnostic(diagnostic_id::sc2237,
                               args[i]->source_location());
      } else if (i + 2 < args.count() &&
                 args[i + 2]->kind() == Token::Kind::Word)
      {
        /* The ! X OP Y shape where OP has a direct negated form, shellcheck
           SC2335. */
        let const op = static_cast<const tokens::WordToken *>(args[i + 2])
                           ->word()
                           .to_literal_string();
        let const inverse = negated_test_operator(op.view());
        if (inverse.has_value()) {
          actx.report_diagnostic(diagnostic_id::sc2335,
                                 args[i]->source_location(),
                                 {op.view(), inverse.value()});
        }
      }
    }
  }

  /* A test with no operand always fails, shellcheck SC2212. */
  if (is_bracket_form_closed && operand_end == 1)
    actx.report_diagnostic(diagnostic_id::sc2212, input.command_location());

  /* A single-operand test with no operator is the nonempty-string test. A
     bracketed true, false, 0 or 1 reads as the builtin and is SC2158 through
     SC2161, another literal is the constant condition SC2078, command output is
     SC2243, and a variable is SC2244. A flag-shaped operand is left alone so
     [ -n ] is not told to use -n. */
  if (is_bracket_form_closed && operand_end == 2 &&
      args[1]->kind() == Token::Kind::Word)
  {
    let const &word = static_cast<const tokens::WordToken *>(args[1])->word();
    let const operand = word.to_literal_string();
    let const view = operand.view();
    let const location = args[1]->source_location();
    let const is_literal = word_is_fully_literal(word);
    let const constant = is_literal ? get_bracketed_constant_kind(view) : None;

    if (constant.has_value()) {
      switch (*constant) {
      case bracketed_constant_kind::False:
        actx.report_diagnostic(diagnostic_id::sc2158, location);
        break;
      case bracketed_constant_kind::Zero:
        actx.report_diagnostic(diagnostic_id::sc2159, location);
        break;
      case bracketed_constant_kind::True:
        actx.report_diagnostic(diagnostic_id::sc2160, location);
        break;
      case bracketed_constant_kind::One:
        actx.report_diagnostic(diagnostic_id::sc2161, location);
        break;
      }
    } else if (view.length == 0 || view[0] != '-') {
      if (is_literal) {
        actx.report_diagnostic(diagnostic_id::sc2078, location, {view});
      } else if (word.segments.count() == 1 &&
                 word.segments[0].kind ==
                     WordSegment::Kind::CommandSubstitution)
      {
        actx.report_diagnostic(diagnostic_id::sc2243, location);
      } else {
        actx.report_diagnostic(diagnostic_id::sc2244, location);
      }
    }
  }

  /* The operand-shape lints over the closed operand range. A -z or -n on a
     literal operand is SC2157, the same test on collected matcher output is
     SC2143, a numeric comparison against a non-numeric literal is SC2170, and a
     = or == against a glob literal is SC2081. */
  for (usize i = 1; i < operand_end; i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
    let const literal = word.to_literal_string();
    let const view = literal.view();

    if ((view == "-z" || view == "-n") && i + 1 < operand_end &&
        args[i + 1]->kind() == Token::Kind::Word)
    {
      let const &next =
          static_cast<const tokens::WordToken *>(args[i + 1])->word();
      if (word_is_fully_literal(next)) {
        actx.report_diagnostic(diagnostic_id::sc2157,
                               args[i + 1]->source_location(), {view});
      } else if (next.segments.count() == 1 &&
                 next.segments[0].kind ==
                     WordSegment::Kind::CommandSubstitution &&
                 substitution_runs_pattern_matcher(
                     next.segments[0].text.view()))
      {
        actx.report_diagnostic(diagnostic_id::sc2143,
                               args[i + 1]->source_location());
      }
    }

    if (is_test_numeric_operator_word(view)) {
      for (usize side = i - 1; side <= i + 1; side += 2) {
        /* Index zero is the command word, never an operand. */
        if (side == 0 || side >= operand_end ||
            args[side]->kind() != Token::Kind::Word)
          continue;
        check_numeric_comparison_operand(actx, view, args[side], false);
      }
    }

    if (input.command_id() != command_name_id::DoubleBracket &&
        (view == "=" || view == "==") && i + 1 < operand_end &&
        args[i + 1]->kind() == Token::Kind::Word)
    {
      let const &right =
          static_cast<const tokens::WordToken *>(args[i + 1])->word();
      if (word_is_fully_literal(right)) {
        let const right_literal = right.to_literal_string();
        if (right_literal.view().find_character('*').has_value() ||
            right_literal.view().find_character('?').has_value())
        {
          actx.report_diagnostic(diagnostic_id::sc2081,
                                 args[i + 1]->source_location());
        } else if (i >= 2 && args[i - 1]->kind() == Token::Kind::Word) {
          /* Two differing literals never compare equal, shellcheck SC2193. */
          let const &left =
              static_cast<const tokens::WordToken *>(args[i - 1])->word();
          let const left_literal = left.to_literal_string();
          if (word_is_fully_literal(left) &&
              left_literal.view() != right_literal.view())
          {
            actx.report_diagnostic(diagnostic_id::sc2193,
                                   args[i]->source_location(),
                                   {left_literal.view(), right_literal.view()});
          }
        }
      }
    }

    /* A test against $? checks the exit status indirectly, shellcheck
       SC2181. */
    if (word.segments.count() == 1 &&
        word.segments[0].kind == WordSegment::Kind::VariableReference &&
        word.segments[0].text.view() == "?")
    {
      actx.report_diagnostic(diagnostic_id::sc2181, args[i]->source_location());
    }
  }
}

namespace {

/* The variables whose value is a command name by convention, so an ordinary
   prefix assignment to one of them is not a swallowed command. */
constexpr PackedStringKey COMMAND_VALUED_VARIABLE_KEYS[] = {
    SSK("BROWSER"), SSK("CC"),     SSK("CXX"),      SSK("DIFFPROG"),
    SSK("EDITOR"),  SSK("FCEDIT"), SSK("MANPAGER"), SSK("PAGER"),
    SSK("SHELL"),   SSK("VISUAL"),
};
constexpr StaticStringSet COMMAND_VALUED_VARIABLES{
    COMMAND_VALUED_VARIABLE_KEYS};

/* A prefix such as `BIN="$BIN"` hands the outer value to the command, so a
   reference to the name among the operands reads those same bytes. */
pure fn prefix_value_is_own_name(const prefix_assignment &var) wontthrow -> bool
{
  /* A quoted value carries an empty text segment for the quote itself, which
     contributes no bytes to the value. */
  let has_matching_reference = false;
  for (let const &segment : var.value.segments) {
    if (segment.kind == WordSegment::Kind::VariableReference) {
      if (has_matching_reference) return false;
      if (segment.text.view() != var.name.view()) return false;

      has_matching_reference = true;
      continue;
    }

    if (!segment.text.is_empty()) return false;
  }

  return has_matching_reference;
}

} /* namespace */

/* A prefix assignment does not affect the expansion on the same command, so a
   reference to one of its names reads the old value. */
fn check_prefix_assignment_reads(AnalysisContext &actx,
                                 const command_lint_input &input) throws -> void
{
  if (input.local_vars.is_empty()) return;

  let const &args = input.args;

  /* One prefix assignment whose value names a command leaves the next word as
     the command name, shellcheck SC2037. A variable that holds a command name
     by convention keeps its ordinary use. */
  if (input.local_vars.count() == 1 && !input.command_literal.is_empty()) {
    let const &value = input.local_vars[0].value;
    let const value_is_bare_word =
        value.segments.count() == 1 &&
        (value.segments[0].kind == WordSegment::Kind::UnquotedText ||
         value.segments[0].kind == WordSegment::Kind::LiteralText);
    if (value_is_bare_word) {
      let const assigned = value.segments[0].text.view();
      let const name = input.local_vars[0].name.view();
      let const value_names_a_command =
          !COMMAND_VALUED_VARIABLES.contains(name) &&
          get_analysis_command_info(assigned).id != command_name_id::Unknown &&
          input.command_id() == command_name_id::Unknown;
      if (input.command_literal[0] == '-' || value_names_a_command) {
        actx.report_diagnostic(diagnostic_id::sc2037,
                               input.local_vars[0].location,
                               {name, input.command_literal});
      }
    }
  }

  for (usize i = 1; i < args.count(); i++) {
    if (args[i]->kind() != Token::Kind::Word) continue;
    let const &word = static_cast<const tokens::WordToken *>(args[i])->word();
    for (let const &segment : word.segments) {
      if (segment.kind != WordSegment::Kind::VariableReference) continue;
      const StringView referenced{segment.text.data(), segment.text.count()};
      bool has_name_prefix = false;
      for (let const &var : input.local_vars) {
        if (var.name.view() != referenced) continue;

        has_name_prefix = !prefix_value_is_own_name(var);
        break;
      }
      if (has_name_prefix) {
        actx.report_diagnostic(diagnostic_id::assignment_prefix_read,
                               args[i]->source_location(),
                               {segment.text.view()});
        break;
      }
    }
  }
}

namespace {

/* An arithmetic value assigns text unless it is wrapped, so the operand shape
   is read from the raw assignment. */
pure fn value_is_self_arithmetic(StringView name, StringView value) wontthrow
    -> bool
{
  usize position = 0;
  if (position < value.length && value[position] == '$') {
    position++;
  }

  if (position + name.length > value.length) return false;
  if (value.substring_of_length(position, name.length) != name) return false;
  position += name.length;

  if (position >= value.length) return false;

  switch (value[position]) {
  case '+':
  case '-':
  case '*':
  case '/': position++; break;

  default: return false;
  }

  if (position >= value.length) return false;

  return value.substring(position).is_all_decimal_digits();
}

/* The value keeps its quote bytes, so the surrounding pair is dropped before
   the name is compared. */
pure fn assignment_value_is_own_name(StringView name,
                                     StringView value) wontthrow -> bool
{
  StringView inner = value;
  if (inner.length >= 2 && inner[0] == '"' && inner[inner.length - 1] == '"')
    inner = inner.substring_of_length(1, inner.length - 2);

  if (inner.length < 2 || inner[0] != '$') {
    return false;
  }

  if (inner.length >= 4 && inner[1] == '{' && inner[inner.length - 1] == '}')
    return inner.substring_of_length(2, inner.length - 3) == name;

  return inner.substring(1) == name;
}

pure fn value_has_written_escape(StringView value) wontthrow -> bool
{
  for (usize i = 0; i + 1 < value.length; i += 1) {
    if (value[i] != '\\') continue;

    switch (value[i + 1]) {
    case 'n':
    case 'r':
    case 't': return true;

    default: break;
    }
  }

  return false;
}

} /* namespace */

/* A brace expansion needs a comma between the braces, so a lone brace stays
   data. A question mark is common inside a plain URL, so only the star counts
   as a glob here. */
static pure fn segment_holds_literal_pattern(StringView text) wontthrow -> bool
{
  if (text.find_character('*').has_value()) return true;

  let const open = text.find_character('{');
  if (!open.has_value()) return false;

  let const comma = text.substring(*open).find_character(',');
  if (!comma.has_value()) return false;

  return text.substring(*open + *comma).find_character('}').has_value();
}

fn scan_assignment_value(AnalysisContext &actx, const Word &value_word,
                         SourceLocation location) throws
    -> assignment_value_shape
{
  assignment_value_shape shape{};

  for (let const &segment : value_word.segments) {
    if (segment.kind != WordSegment::Kind::UnquotedText)
      shape.has_bare_literal_value = false;

    if (actx.is_posix_sh_shebang)
      check_posix_word_portability(actx, segment, location);

    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::DoubleQuotedText:
      if (segment.text.view().find_character('"').has_value())
        shape.has_quoted_literal_value = true;
      break;

    case WordSegment::Kind::UnquotedText: break;

    default: shape.has_only_literal_segments = false; break;
    }

    if (segment.kind == WordSegment::Kind::UnquotedText &&
        segment_holds_literal_pattern(segment.text.view()))
    {
      shape.has_unquoted_pattern = true;
    }

    if (segment.kind == WordSegment::Kind::VariableReference &&
        segment.text.view() == "@")
    {
      actx.report_diagnostic(diagnostic_id::sc2124, location);
      break;
    }

    if (segment.kind == WordSegment::Kind::ArithmeticExpansion &&
        segment.text.view().find_character('$').has_value())
    {
      actx.report_diagnostic(diagnostic_id::sc2004, location);
    }
  }

  return shape;
}

fn check_assignment_value_shape(AnalysisContext &actx,
                                const assignment_lint_input &input) throws
    -> void
{
  let const equals = input.raw_assignment.find_character('=');
  if (!equals.has_value()) return;

  let const value = input.raw_assignment.substring(*equals + 1);

  if (input.shape.has_unquoted_pattern) {
    actx.report_diagnostic(diagnostic_id::sc2125, input.location, {input.name});
  }

  /* PATH without a separator and without its own value replaces the search
     path, shellcheck SC2123. An expanded value may already hold a path list. */
  if (input.name == "PATH" && !input.is_append && !value.is_empty() &&
      input.shape.has_only_literal_segments &&
      !value.find_character(':').has_value() &&
      !view_contains(value, StringView{"PATH"}))
  {
    actx.report_diagnostic(diagnostic_id::sc2123, input.location, {value});
  }

  if (value_is_self_arithmetic(input.name, value)) {
    let const id =
        value[0] == '$' ? diagnostic_id::sc2099 : diagnostic_id::sc2100;
    actx.report_diagnostic(id, input.location, {input.name});
  }

  /* A prefix repeating the value the name already holds exports it into the
     environment of the command, which an ordinary assignment does not do. */
  if (!input.is_append && !input.is_command_prefix &&
      assignment_value_is_own_name(input.name, value))
  {
    actx.report_diagnostic(diagnostic_id::sc2269, input.location, {input.name});
  }

  /* A separator written as two text bytes never becomes the control byte,
     shellcheck SC2141. */
  if (input.name == "IFS" && input.shape.has_only_literal_segments &&
      value_has_written_escape(value))
  {
    actx.report_diagnostic(diagnostic_id::sc2141, input.location, {input.name});
  }

  /* A prefix naming the program another tool is meant to start, such as
     `PAGER=cat cmd`, hands the name to that tool and is deliberate. */
  let const is_deliberate_command_prefix =
      input.is_command_prefix && COMMAND_VALUED_VARIABLES.contains(input.name);

  if (!input.is_append && !is_deliberate_command_prefix &&
      input.shape.has_bare_literal_value &&
      COMMAND_NAME_VALUES.contains(value) &&
      actx.should_report(diagnostic_id::sc2209))
  {
    actx.command_name_assignments.push(command_name_assignment_record{
        String{input.name}, String{value}, input.location});
  }

  let const first_bracket = input.name.find_character('[');
  if (input.shape.has_quoted_literal_value &&
      input.shape.has_only_literal_segments && !first_bracket.has_value())
  {
    actx.quoted_literal_assignments.set(input.name, input.location);
  }

  if (!first_bracket.has_value()) {
    /* A scalar assignment to an array name touches the first element alone,
       shellcheck SC2178 and SC2179. */
    if (actx.array_valued_names.count() != 0 &&
        actx.array_valued_names.contains(input.name))
    {
      let const id =
          input.is_append ? diagnostic_id::sc2179 : diagnostic_id::sc2178;
      actx.report_diagnostic(id, input.location, {input.name});
    }

    return;
  }

  /* A second subscript makes the name a multidimensional array, which the shell
     does not have, shellcheck SC2180. */
  let const after_first = input.name.substring(*first_bracket + 1);
  let const closer = after_first.find_character(']');
  if (closer.has_value() &&
      after_first.substring(*closer + 1).find_character('[').has_value())
  {
    actx.report_diagnostic(diagnostic_id::sc2180, input.location, {input.name});
  }
}

namespace {

pure fn option_letter_index(char letter) wontthrow -> u32
{
  if (letter >= 'a' && letter <= 'z') {
    return static_cast<u32>(letter - 'a');
  }
  if (letter >= 'A' && letter <= 'Z')
    return static_cast<u32>(letter - 'A') + 26;

  return 64;
}

pure fn segment_is_literal(const WordSegment &segment) wontthrow -> bool
{
  return segment.kind == WordSegment::Kind::LiteralText ||
         segment.kind == WordSegment::Kind::UnquotedText ||
         segment.kind == WordSegment::Kind::DoubleQuotedText;
}

pure fn literal_run_length(const Word &case_word, usize start,
                           usize end) wontthrow -> usize
{
  usize length = 0;
  for (usize i = start; i < end; i++)
    length += case_word.segments[i].text.view().length;

  return length;
}

pure fn literal_run_matches_at(const Word &case_word, usize start, usize end,
                               StringView pattern, usize position) wontthrow
    -> bool
{
  for (usize i = start; i < end; i++) {
    let const text = case_word.segments[i].text.view();
    if (position + text.length > pattern.length) return false;
    if (pattern.substring_of_length(position, text.length) != text)
      return false;
    position += text.length;
  }

  return true;
}

/* Whether the literal chunks of the case word leave room for the pattern. An
   expansion matches anything, so only a literal chunk can refute a match, and
   an unproven case answers true. The leading run is held to the start of the
   pattern and the trailing run to its end, since an expansion cannot move
   either. */
pure fn case_pattern_can_match_word(const Word &case_word,
                                    StringView pattern) wontthrow -> bool
{
  usize first = 0;
  usize last = case_word.segments.count();
  usize front = 0;
  usize back = pattern.length;

  while (first < last && segment_is_literal(case_word.segments[first])) {
    let const text = case_word.segments[first].text.view();
    if (front + text.length > back) return false;
    if (pattern.substring_of_length(front, text.length) != text) return false;
    front += text.length;
    first++;
  }

  if (first == last) return front == back;

  while (last > first && segment_is_literal(case_word.segments[last - 1])) {
    let const text = case_word.segments[last - 1].text.view();
    if (back < front + text.length) return false;
    if (pattern.substring_of_length(back - text.length, text.length) != text)
      return false;
    back -= text.length;
    last--;
  }

  usize at = first;
  while (at < last) {
    if (!segment_is_literal(case_word.segments[at])) {
      at++;
      continue;
    }

    usize run_end = at;
    while (run_end < last && segment_is_literal(case_word.segments[run_end]))
      run_end++;

    let const run_length = literal_run_length(case_word, at, run_end);
    let is_found = false;
    for (usize position = front; position + run_length <= back; position++) {
      if (!literal_run_matches_at(case_word, at, run_end, pattern, position))
        continue;
      front = position + run_length;
      is_found = true;
      break;
    }

    if (!is_found) return false;

    at = run_end;
  }

  return true;
}

} /* namespace */

fn check_case_word_shape(AnalysisContext &actx,
                         const case_lint_input &input) throws -> void
{
  ASSERT(input.case_word != nullptr);

  if (word_is_fully_literal(*input.case_word) &&
      !input.case_word_source.is_empty())
  {
    actx.report_diagnostic(diagnostic_id::sc2194, input.case_location,
                           {input.case_word_source});
  }
}

fn check_case_pattern_shape(AnalysisContext &actx, const case_lint_input &input,
                            const Word &pattern_word,
                            StringView pattern_literal,
                            StringView pattern_source,
                            SourceLocation pattern_location,
                            case_arm_tally &tally) throws -> void
{
  let const is_bare_pattern =
      pattern_word.segments.count() == 1 &&
      pattern_word.segments[0].kind == WordSegment::Kind::UnquotedText;

  if (is_bare_pattern && pattern_word.segments[0].text.view() == "*") {
    tally.has_default_arm = true;
    return;
  }

  if (pattern_literal == "?") tally.has_question_arm = true;

  let has_glob_metacharacter = false;
  let is_literal_pattern = true;
  let has_unquoted_expansion = false;

  for (let const &segment : pattern_word.segments) {
    switch (segment.kind) {
    case WordSegment::Kind::LiteralText:
    case WordSegment::Kind::UnquotedText:
    case WordSegment::Kind::DoubleQuotedText:
      if (segment.has_live_glob_chars() && segment.has_glob_metacharacter())
        has_glob_metacharacter = true;
      break;

    case WordSegment::Kind::VariableReference:
    case WordSegment::Kind::CommandSubstitution:
    case WordSegment::Kind::ArithmeticExpansion:
      is_literal_pattern = false;
      if (!segment.is_in_double_quotes) has_unquoted_expansion = true;
      break;

    default: is_literal_pattern = false; break;
    }
  }

  /* An expanded pattern is matched as a glob, so its bytes never compare
     literally, shellcheck SC2254. */
  if (has_unquoted_expansion && !pattern_source.is_empty()) {
    actx.report_diagnostic(diagnostic_id::sc2254, pattern_location,
                           {pattern_source});
  }

  /* A literal pattern with no metacharacter matches one string, so the literal
     chunks of the case word decide it, shellcheck SC2195. */
  if (input.case_word != nullptr && is_literal_pattern &&
      !has_glob_metacharacter && !pattern_source.is_empty() &&
      !input.case_word_source.is_empty() &&
      input.case_word->segments.count() != 0 &&
      !case_pattern_can_match_word(*input.case_word, pattern_literal))
  {
    actx.report_diagnostic(diagnostic_id::sc2195, pattern_location,
                           {pattern_source, input.case_word_source},
                           input.case_location);
  }

  if (!input.is_getopts_case) return;
  if (pattern_literal.length != 1) return;

  let const letter_index = option_letter_index(pattern_literal[0]);
  if (letter_index == 64) return;

  tally.handled_option_letters |= u64{1} << letter_index;

  if (!input.getopts_optstring.find_character(pattern_literal[0]).has_value()) {
    actx.report_diagnostic(diagnostic_id::sc2214, pattern_location,
                           {pattern_literal}, input.getopts_location);
  }
}

fn check_case_option_coverage(AnalysisContext &actx,
                              const case_lint_input &input,
                              const case_arm_tally &tally) throws -> void
{
  if (!input.is_getopts_case) return;

  for (usize i = 0; i < input.getopts_optstring.length; i++) {
    let const letter = input.getopts_optstring[i];
    let const letter_index = option_letter_index(letter);
    if (letter_index == 64) continue;
    if ((tally.handled_option_letters & (u64{1} << letter_index)) != 0)
      continue;

    actx.report_diagnostic(diagnostic_id::sc2213, input.case_location,
                           {input.getopts_optstring.substring_of_length(i, 1)},
                           input.getopts_location);
  }

  /* getopts stores a question mark for an unknown option, so a case without a
     catch-all silently skips it, shellcheck SC2220. */
  if (!tally.has_default_arm && !tally.has_question_arm) {
    actx.report_diagnostic(diagnostic_id::sc2220, input.case_location, {},
                           input.getopts_location);
  }
}

} /* namespace expressions */

} /* namespace koshka */
