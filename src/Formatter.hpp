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
  usize start;
  usize end;
  String expected;
  String replacement;
};

struct source_fix
{
  String title;
  ArrayList<source_edit> edits;
  bool is_preferred{true};
  bool is_safe_for_fix_all{true};
};

fn format_shell_source(StringView source, mimic_mood mood, BumpArena &arena,
                       ArrayList<String> &errors) throws -> Maybe<String>;

fn apply_source_fixes(StringView source, const ArrayList<source_fix> &fixes,
                      bool safe_only = true) throws -> Maybe<String>;

fn select_nonconflicting_source_edits(
    ArrayList<const source_edit *> &&candidates) throws
    -> ArrayList<const source_edit *>;

fn source_fixes_for_original_line_endings(
    StringView source, const ArrayList<source_fix> &normalized_fixes) throws
    -> ArrayList<source_fix>;

fn source_fixes_for_diagnostic(diagnostic_id id, StringView source,
                               SourceLocation location) throws
    -> ArrayList<source_fix>;

} /* namespace koshka */
