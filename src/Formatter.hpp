/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell source formatting and syntax-tree output. It
 * walks parsed expressions, preserves source meaning, and produces
 * normalized text or tree representations.
 */

#pragma once

#include "Arena.hpp"
#include "Common.hpp"
#include "Diagnostics.hpp"
#include "MimicMood.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

struct source_edit
{
  usize start_position;
  usize end_position;
  String expected;
  String replacement;
};

struct source_fix
{
  String title;
  ArrayList<source_edit> edits;
  bool is_preferred{true};
  bool is_safe_for_fix_all{false};
  Maybe<diagnostic_id> origin{None};
};

fn format_shell_source(StringView source, mimic_mood mood, BumpArena &arena,
                       ArrayList<String> &errors,
                       String *ast_output = nullptr) throws -> Maybe<String>;

fn apply_source_fixes(StringView source, const ArrayList<source_fix> &fixes,
                      ArrayList<diagnostic_id> *applied_origins =
                          nullptr) throws -> Maybe<String>;

fn select_nonconflicting_source_edits(
    ArrayList<const source_edit *> &&candidates) throws
    -> ArrayList<const source_edit *>;

fn source_fixes_for_original_line_endings(
    StringView source, const ArrayList<source_fix> &normalized_fixes) throws
    -> ArrayList<source_fix>;

fn source_fixes_for_diagnostic(diagnostic_id diagnostic, StringView source,
                               const SourceLocation &location) throws
    -> ArrayList<source_fix>;

} /* namespace koshka */
