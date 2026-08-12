#pragma once

#include "ExpressionsInternal.hpp"

namespace koshka::expressions {

enum class bracketed_constant_kind : u8
{
  False,
  Zero,
  True,
  One,
};

cold fn negated_test_operator(StringView op) wontthrow -> Maybe<StringView>;
cold fn is_test_binary_operator_word(StringView op) wontthrow -> bool;
cold fn is_test_file_comparison_word(StringView op) wontthrow -> bool;
cold fn is_test_numeric_operator_word(StringView op) wontthrow -> bool;
cold fn is_known_test_operator_word(StringView op) wontthrow -> bool;
cold fn is_test_path_unary_operator_word(StringView op) wontthrow -> bool;
cold fn is_test_condition_opener_word(StringView word) wontthrow -> bool;
cold fn get_bracketed_constant_kind(StringView word) wontthrow
    -> Maybe<bracketed_constant_kind>;
cold fn test_inequality_left_operand(const ArrayList<const Token *> &args,
                                     usize operator_index,
                                     usize operand_end) wontthrow
    -> Maybe<StringView>;
cold fn view_looks_like_test_operator(StringView view) wontthrow -> bool;
cold fn view_has_decimal_fraction(StringView view) wontthrow -> bool;
cold fn view_has_arithmetic_operator(StringView view) wontthrow -> bool;
cold fn view_repeats_a_letter(StringView view) wontthrow -> bool;
cold fn view_is_glob_shaped_pattern(StringView view) wontthrow -> bool;
cold fn view_is_plain_substitution_script(StringView view) wontthrow -> bool;
cold fn find_echo_escape_sequence(StringView view) wontthrow -> StringView;
cold fn view_settles_echo_escapes(StringView view) wontthrow -> bool;
cold fn substitution_runs_pattern_matcher(StringView body) throws -> bool;
cold fn word_is_fully_literal(const Word &word) wontthrow -> bool;
cold fn token_has_command_substitution(const Token *token) wontthrow -> bool;
cold fn token_has_ansi_c_quote(const Token *token) wontthrow -> bool;
cold fn printf_consumed_argument_count(StringView format,
                                       bool &has_quote_conversion) wontthrow
    -> usize;
cold pure fn view_is_integer_literal(StringView view) wontthrow -> bool;
cold fn args_have_short_flag(const ArrayList<const Token *> &args,
                             char letter) throws -> bool;
cold fn single_literal_file_operand(const ArrayList<const Token *> &args) throws
    -> Maybe<const Token *>;
pure fn ssh_option_takes_value(char letter) wontthrow -> bool;
pure fn leading_command_word(StringView text) wontthrow -> StringView;
pure fn is_system_directory(StringView path) wontthrow -> bool;
pure fn is_find_action(StringView word) wontthrow -> bool;
pure fn is_find_leading_option(StringView word) wontthrow -> bool;
pure fn is_shell_only_builtin(StringView name) wontthrow -> bool;
fn check_posix_parameter_expansion(AnalysisContext &actx,
                                   const WordSegment &segment, StringView text,
                                   SourceLocation fallback_location) throws
    -> void;
pure fn arithmetic_assignment_target(StringView expression,
                                     usize equals_position) wontthrow
    -> StringView;

} /* namespace koshka::expressions */
