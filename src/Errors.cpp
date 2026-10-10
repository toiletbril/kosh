/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell errors and source-aware reporting. It carries
 * messages, locations, notes, severities, and rendering data across parser
 * and evaluator failures.
 */

#include "Errors.hpp"

#include "CLIColors.hpp"
#include "Completion.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"
#include "base/Debug.hpp"
#include "base/ErrorOr.hpp"
#include "base/Trace.hpp"

namespace koshka {

struct interned_source_name
{
  char *data;
  u32 length;
  source_identity_kind kind;

  interned_source_name(char *data, u32 length, source_identity_kind kind)
      : data{data}, length{length}, kind{kind}
  {}
  interned_source_name(const interned_source_name &) = delete;
  interned_source_name(interned_source_name &&other) noexcept
      : data{other.data}, length{other.length}, kind{other.kind}
  {
    other.data = nullptr;
  }
  ~interned_source_name()
  {
    heap_allocator().free_array(data, static_cast<usize>(length) + 1);
  }
};

static fn get_source_name_table() wontthrow -> ArrayList<interned_source_name> &
{
  static ArrayList<interned_source_name> table{heap_allocator()};

  return table;
}

cold fn intern_source_name(StringView name) throws -> u32
{
  if (name.is_empty()) return 0;

  let const kind = name.data == COMMAND_STRING_SOURCE_NAME.data
                       ? source_identity_kind::CommandString
                       : source_identity_kind::File;
  let &table = get_source_name_table();
  for (usize row_index = 0; row_index < table.count(); row_index++) {
    let const &row = table[row_index];
    if (row.kind == kind && row.length == name.length &&
        std::memcmp(row.data, name.data, name.length) == 0)
    {
      return static_cast<u32>(row_index + 1);
    }
  }

  let const copy = heap_allocator().alloc_array<char>(name.length + 1);
  std::memcpy(copy, name.data, name.length);
  copy[name.length] = '\0';
  table.push(interned_source_name{copy, static_cast<u32>(name.length), kind});

  return static_cast<u32>(table.count());
}

fn source_identity_kind_at(u32 source_name_index) wontthrow
    -> source_identity_kind
{
  let const &table = get_source_name_table();
  if (source_name_index == 0 || source_name_index > table.count())
    return source_identity_kind::File;

  return table[source_name_index - 1].kind;
}

fn source_name_at(u32 source_name_index) wontthrow -> Maybe<StringView>
{
  if (source_name_index == 0) return None;

  let const &table = get_source_name_table();
  if (source_name_index > table.count()) return None;
  let const &row = table[source_name_index - 1];
  if (row.kind == source_identity_kind::CommandString)
    return COMMAND_STRING_SOURCE_NAME;

  return StringView{row.data, row.length};
}

struct diagnostic_color
{
  StringView severity;
  StringView location;
  StringView message;
  StringView caret;

  pure fn get_reset() const wontthrow -> StringView
  {
    return severity.is_empty() ? StringView{} : colors::ansi::RESET;
  }
};

cold static fn diagnostic_colors_for(error_severity severity) throws
    -> diagnostic_color
{
  if (!colors::stderr_wants_color()) return diagnostic_color{};

  switch (severity) {
  case error_severity::error:
    return diagnostic_color{colors::ansi::BOLD_BRIGHT_RED, colors::ansi::BOLD,
                            colors::ansi::BOLD, colors::ansi::BOLD_BRIGHT_RED};
  case error_severity::warning:
    return diagnostic_color{colors::ansi::YELLOW, {}, {}, colors::ansi::YELLOW};
  case error_severity::note:
    return diagnostic_color{
        colors::ansi::CYAN, {}, colors::ansi::CYAN, colors::ansi::CYAN};
  case error_severity::details:
    return diagnostic_color{
        colors::ansi::BLUE, {}, colors::ansi::BLUE, colors::ansi::BLUE};
  case error_severity::trace:
    return diagnostic_color{
        colors::ansi::CYAN, {}, colors::ansi::CYAN, colors::ansi::CYAN};
  }

  unreachable("invalid diagnostic color severity %d", ENUM(severity));
}

pure static fn text_ends_a_sentence(StringView text) wontthrow -> bool
{
  if (text.is_empty()) return false;

  let const last_byte = text[text.length - 1];

  return last_byte == '.' || last_byte == '?' || last_byte == '!' ||
         last_byte == ')';
}

cold static fn append_diagnostic_message(String &out, StringView message,
                                         const diagnostic_color &color) throws
    -> void
{
  let const reset = color.get_reset();
  usize digit_start = message.length;
  if (!reset.is_empty() && message.length > 0 &&
      message[message.length - 1] == ')')
  {
    digit_start = message.length - 1;
    while (digit_start > 0 && message[digit_start - 1] >= '0' &&
           message[digit_start - 1] <= '9')
      digit_start--;
  }

  let const has_code_suffix =
      digit_start + 1 < message.length && digit_start >= 4 &&
      message.substring_of_length(digit_start - 4, 4) == StringView{" (SC"};
  out += color.message;
  if (!has_code_suffix) {
    out += message;
    return;
  }

  out += message.substring_of_length(0, digit_start - 4);
  out += reset;
  out += colors::ansi::DIM;
  out += message.substring(digit_start - 4);
  out += reset;
}

template <class T>
  requires std::is_integral_v<T>
cold static fn number_string_length(T value) wontthrow -> usize
{
  usize digit_count = 1;
  while (value >= 10) {
    digit_count++;
    value /= 10;
  }
  return digit_count;
}

cold static fn reported_line_number(usize rendered_line,
                                    isize line_offset) wontthrow -> usize
{
  return static_cast<usize>(static_cast<isize>(rendered_line + 1) +
                            line_offset);
}

cold static fn get_context_pointing_to(
    StringView source, usize byte_position, usize byte_count,
    const utils::source_line_position &full_line_position, isize line_offset,
    Maybe<StringView> message, const diagnostic_color &color,
    EvalContext *eval_context) throws -> String
{
  let const line_number =
      reported_line_number(full_line_position.line_number, line_offset) - 1;

  static constexpr usize LONG_LINE_CONTEXT_BYTES = 4096;
  let line_position = full_line_position;
  if (line_position.line_end - line_position.line_start >
      2 * LONG_LINE_CONTEXT_BYTES)
  {
    if (byte_position > line_position.line_start + LONG_LINE_CONTEXT_BYTES) {
      line_position.line_start = byte_position - LONG_LINE_CONTEXT_BYTES;
      while (line_position.line_start < byte_position &&
             (static_cast<u8>(source[line_position.line_start]) & 0xC0) == 0x80)
        line_position.line_start++;
    }

    let const caret_end_position =
        byte_count > line_position.line_end - byte_position
            ? line_position.line_end
            : byte_position + byte_count;
    if (line_position.line_end > caret_end_position + LONG_LINE_CONTEXT_BYTES) {
      line_position.line_end = caret_end_position + LONG_LINE_CONTEXT_BYTES;
      while (line_position.line_end > caret_end_position &&
             (static_cast<u8>(source[line_position.line_end]) & 0xC0) == 0x80)
        line_position.line_end--;
    }
  }
  LOG(Debug, "assembling the caret context for line %zu", line_number + 1);

  static constexpr usize LINE_NUMBER_FIELD_WIDTH = 6;
  static constexpr usize BAR_SEPARATOR_WIDTH = 4;
  let const line_number_digit_count = number_string_length(line_number + 1);
  const usize line_number_padding_length =
      sub_sat(LINE_NUMBER_FIELD_WIDTH, line_number_digit_count);
  let const gutter_width = line_number_padding_length +
                           line_number_digit_count + BAR_SEPARATOR_WIDTH;

  let msg = String{heap_allocator()};
  for (usize i = 0; i < line_number_padding_length; i++) {
    msg += ' ';
  }

  msg += String::from(line_number + 1, heap_allocator()) + " |  ";

  const usize line_byte_count =
      line_position.line_end - line_position.line_start;
  let const context =
      source.substring_of_length(line_position.line_start, line_byte_count);
  let const caret_byte_position = byte_position - line_position.line_start;

  ASSERT(!context.find_character('\n').has_value(),
         "'%s', start: %zu, end: %zu", context.data, line_position.line_start,
         line_position.line_end);

  static constexpr usize TAB_WIDTH = 4;
  usize tab_count = 0;
  for (usize i = 0; i < context.count(); i++) {
    if (context[i] != '\t') continue;
    tab_count++;
  }

  let expanded_line = String{heap_allocator()};
  let display_line = context;
  if (tab_count != 0) {
    expanded_line.reserve(context.count() + tab_count * (TAB_WIDTH - 1));
    for (usize i = 0; i < context.count(); i++) {
      if (context[i] == '\t') {
        expanded_line.append_repeated(' ', TAB_WIDTH);
      } else {
        expanded_line += context[i];
      }
    }
    display_line = expanded_line.view();
  }

  usize caret_source_position = 0;
  usize caret_expanded_position = 0;
  let const do_advance_caret_position = [&](usize byte_offset) {
    while (caret_source_position < byte_offset) {
      caret_expanded_position +=
          context[caret_source_position] == '\t' ? TAB_WIDTH : 1;
      caret_source_position++;
    }
    return caret_expanded_position;
  };

  let const caret_limit = line_position.line_end - byte_position;
  const usize caret_byte_count =
      byte_count > caret_limit ? caret_limit : byte_count;
  const usize expanded_caret_byte_position =
      do_advance_caret_position(caret_byte_position);
  const usize expanded_caret_end_byte_position =
      do_advance_caret_position(caret_byte_position + caret_byte_count);

  let const caret_prefix =
      display_line.substring_of_length(0, expanded_caret_byte_position);
  let const caret_text = display_line.substring_of_length(
      expanded_caret_byte_position,
      expanded_caret_end_byte_position - expanded_caret_byte_position);
  let const caret_column = toiletline::get_display_width(caret_prefix);
  let const caret_width = toiletline::get_display_width(caret_text);

  let generated_highlights = ArrayList<highlight_span>{heap_allocator()};
  const ArrayList<highlight_span> *source_highlights = &generated_highlights;
  if (eval_context != nullptr && !color.severity.is_empty()) {
    let *cache = eval_context->get_or_create_diagnostic_highlight_cache();
    source_highlights = cache->spans_for(source, line_position.line_start,
                                         line_position.line_end, *eval_context);
  }
  let expanded_highlights = ArrayList<highlight_span>{heap_allocator()};
  let line_highlights = source_highlights;
  if (tab_count != 0 && !source_highlights->is_empty()) {
    expanded_highlights.reserve(source_highlights->count());
    usize highlight_source_position = 0;
    usize highlight_expanded_position = 0;
    let const do_advance_highlight_position = [&](usize byte_offset) {
      while (highlight_source_position < byte_offset) {
        highlight_expanded_position +=
            context[highlight_source_position] == '\t' ? TAB_WIDTH : 1;
        highlight_source_position++;
      }
      return highlight_expanded_position;
    };
    for (let const &span : *source_highlights) {
      expanded_highlights.push(
          highlight_span{do_advance_highlight_position(span.start),
                         do_advance_highlight_position(span.end), span.role});
    }
    line_highlights = &expanded_highlights;
  }
  let const display_cells = toiletline::get_display_width(display_line);

  usize window_start = 0;
  usize window_end = display_cells;
  bool has_left_ellipsis = false;
  bool has_right_ellipsis = false;

  static constexpr usize UNBOUNDED_STREAM_LINE_WIDTH = 240;
  let const is_stderr_a_terminal = colors::stderr_is_a_terminal();
  let const terminal_dimensions = os::get_terminal_dimensions(KOSH_STDERR);
  let const should_window_for_terminal =
      display_cells > 24 && is_stderr_a_terminal &&
      terminal_dimensions.has_value() &&
      terminal_dimensions->columns > gutter_width + 24 &&
      display_cells > terminal_dimensions->columns - gutter_width;
  let const should_window_for_stream =
      !is_stderr_a_terminal && display_cells > UNBOUNDED_STREAM_LINE_WIDTH;
  if (should_window_for_terminal || should_window_for_stream) {
    let const available_line_width =
        should_window_for_terminal ? terminal_dimensions->columns - gutter_width
                                   : UNBOUNDED_STREAM_LINE_WIDTH;
    let const caret_display_width = caret_width < 1 ? 1 : caret_width;
    let const half_window_width = available_line_width / 2;
    let const caret_center = caret_column + caret_display_width / 2;
    window_start =
        caret_center > half_window_width ? caret_center - half_window_width : 0;
    if (window_start + available_line_width > display_cells)
      window_start = display_cells - available_line_width;
    window_end = window_start + available_line_width;
    has_left_ellipsis = window_start > 0;
    has_right_ellipsis = window_end < display_cells;
    window_end = sub_sat(window_end, (has_left_ellipsis ? 3u : 0u) +
                                         (has_right_ellipsis ? 3u : 0u));
    if (window_start > caret_column) window_start = caret_column;
    if (caret_column + caret_display_width > window_end &&
        window_end < display_cells)
    {
      let const shift = caret_column + caret_display_width - window_end;
      window_start = window_start + shift > caret_column ? caret_column
                                                         : window_start + shift;
      window_end += shift;
    }
    if (window_end > display_cells) window_end = display_cells;
    if (window_end <= window_start) window_end = display_cells;
    has_left_ellipsis = window_start > 0;
    has_right_ellipsis = window_end < display_cells;
  }

  usize window_start_byte = 0;
  usize window_end_byte = display_line.length;
  if (has_left_ellipsis || has_right_ellipsis) {
    window_start_byte = toiletline::get_byte_offset_at_or_before_display_cell(
        display_line, window_start, window_start);
    window_end_byte = toiletline::get_byte_offset_at_or_before_display_cell(
        display_line, window_end, window_end);
  }

  if (has_left_ellipsis) msg += "...";
  completion::append_highlighted_range(msg, display_line, *line_highlights,
                                       window_start_byte, window_end_byte,
                                       colors::DIAGNOSTIC_HIGHLIGHT_THEME);
  if (has_right_ellipsis) msg += "...";

  const usize caret_pad =
      (has_left_ellipsis ? 3u : 0u) + sub_sat(caret_column, window_start);
  let const caret_end = caret_column + caret_width;
  const usize visible_caret =
      sub_sat(caret_end < window_end ? caret_end : window_end, caret_column);

  msg += '\n';
  for (usize i = 0; i + 3 < gutter_width; i++)
    msg += ' ';

  msg += "|  ";

  for (usize i = 0; i < caret_pad; i++)
    msg += ' ';

  msg += color.caret;
  msg += '^';
  if (visible_caret > 1) {
    for (usize i = 0; i < visible_caret - 1; i++)
      msg += '~';
  }

  if (message.has_value()) {
    msg += ' ';
    msg += *message;

    if (!text_ends_a_sentence(*message)) msg += '.';
  }
  msg += color.get_reset();

  return msg;
}

ErrorBase::~ErrorBase() = default;

cold fn ErrorBase::trailing_details_to_string() const throws -> String
{
  let const note = detail_message();
  if (note.is_empty()) return String{heap_allocator()};

  let const severity = error_severity::note;
  let const severity_word = get_error_severity_word(severity);
  let const color = diagnostic_colors_for(severity);

  let const note_period = text_ends_a_sentence(note) ? "" : ".";

  return String{"\n"} + color.severity + severity_word + color.get_reset() +
         ": " + color.message + note + note_period + color.get_reset();
}

cold fn ErrorBase::get_severity() const wontthrow -> error_severity
{
  return error_severity::error;
}

Error::Error(StringView message) : m_message(heap_allocator(), message)
{
  LOG(Debug, "constructing an error with message '%.*s'",
      static_cast<int>(message.length), message.data);
}

ErrorWithDetails::ErrorWithDetails(StringView message, StringView note)
    : Error(message), m_note(note)
{}

cold fn ErrorBase::to_string(StringView source,
                             EvalContext *context) const throws -> String
{
  unused(source);
  unused(context);
  let const severity = get_severity();
  let const severity_word = get_error_severity_word(severity);
  let const color = diagnostic_colors_for(severity);
  let const period = text_ends_a_sentence(message()) ? "" : ".";

  let result = color.severity + severity_word + color.get_reset() + ": ";
  append_diagnostic_message(result, message().view(), color);
  result += period;
  result += color.get_reset();
  result += trailing_details_to_string();
  return result;
}

fn Error::to_string() const throws -> String
{
  return ErrorBase::to_string(StringView{});
}

Warning::Warning(StringView message) : Error(message) {}

WarningWithDetails::WarningWithDetails(StringView message, StringView note)
    : Warning(message), m_note(note)
{}

InterruptErrorWithLocation::InterruptErrorWithLocation(SourceLocation location)
    : ErrorWithLocation(steal(location), "Interrupted")
{
  set_command_status(130);
}

cold fn Warning::get_severity() const wontthrow -> error_severity
{
  return error_severity::warning;
}

Note::Note(StringView message) : Error(message) {}

cold fn Note::get_severity() const wontthrow -> error_severity
{
  return error_severity::note;
}

BrokenPipeExit::BrokenPipeExit() : Error("Broken pipe") {}

TrapAbandonedRedirection::TrapAbandonedRedirection()
    : Error("Redirection abandoned by a trap")
{}

ErrorWithLocation::ErrorWithLocation(SourceLocation location,
                                     StringView message)
    : Error(message), m_location(steal(location))
{
  LOG(Debug, "locating the error at byte %zu spanning %zu bytes",
      m_location.position, m_location.length);
}

cold fn resolve_rendered_site(StringView source, const SourceLocation &location,
                              isize line_offset, bool is_rebased,
                              const EvalContext *context) wontthrow
    -> rendered_site
{
  if (context == nullptr) return rendered_site{source, location, line_offset};

  let mapped_source = source;
  let mapped_location = location;
  if (let const *parent =
          context->map_embedded_site(mapped_source, mapped_location);
      parent != nullptr)
  {
    let const parent_site = context->resolve_render_source(
        mapped_location, parent, static_cast<usize>(-1), 0);
    if (parent_site.is_windowed && parent_site.text != nullptr) {
      return rendered_site{parent_site.text->view(),
                           parent_site.rebase(mapped_location),
                           parent_site.line_offset};
    }

    return rendered_site{mapped_source, mapped_location,
                         parent_site.line_offset};
  }

  if (is_rebased) return rendered_site{source, location, line_offset};

  let const resolved =
      context->resolve_current_function_window(source, location);
  if (!resolved.is_windowed || resolved.text == nullptr)
    return rendered_site{source, location, line_offset};

  return rendered_site{resolved.text->view(), resolved.rebase(location),
                       resolved.line_offset};
}

fn ErrorWithLocation::to_string(StringView source,
                                EvalContext *context) const throws -> String
{
  let const site = resolve_rendered_site(source, m_location, m_line_offset,
                                         m_is_rebased, context);
  source = site.source;
  usize byte_position = site.location.position;
  let const byte_count = site.location.length;

  if (source.data == nullptr || byte_position > source.count())
    return ErrorBase::to_string(source, context);

  LOG_VARS(Debug, byte_position, byte_count);
  let const severity = get_severity();
  let const severity_word = get_error_severity_word(severity);
  LOG(Debug, "formatting located %.*s", static_cast<int>(severity_word.length),
      severity_word.data);

  if (byte_position + 2 < source.count() && source[byte_position] == '\\' &&
      source[byte_position + 1] == '\n')
  {
    byte_position += 2;
  } else if (byte_position + 1 < source.count() &&
             source[byte_position] == '\n')
  {
    byte_position++;
  }

  let const line_position =
      utils::source_line_position_at(source, byte_position);
  const usize line_byte_position =
      toiletline::get_utf8_length(source.data + line_position.line_start,
                                  byte_position - line_position.line_start) +
      1;
  let const color = diagnostic_colors_for(severity);

  let result = String{heap_allocator()};
  result += color.location;
  if (let const name = site.location.get_filename();
      name.has_value() && !m_is_filename_hidden)
  {
    result += *name;
    result += ':';
  }
  result += String::from(
      reported_line_number(line_position.line_number, site.line_offset),
      heap_allocator());
  result += ':';
  result += String::from(line_byte_position, heap_allocator());
  result += ':';
  result += color.get_reset();
  result += ' ';
  result += color.severity;
  result += severity_word;
  result += color.get_reset();
  if (!m_message.is_empty()) {
    result += ": ";
    append_diagnostic_message(result, m_message.view(), color);

    if (!text_ends_a_sentence(m_message.view())) result += '.';

    result += color.get_reset();
  } else {
    result += ':';
  }
  result += '\n';

  result +=
      get_context_pointing_to(source, byte_position, byte_count, line_position,
                              site.line_offset, None, color, context);
  result += trailing_details_to_string();
  return result;
}

CommandResolutionErrorWithLocation::CommandResolutionErrorWithLocation(
    SourceLocation location, StringView message, i64 command_status)
    : ErrorWithLocation(steal(location), message)
{
  set_command_status(command_status);
}

CommandResolutionErrorWithLocationAndDetails::
    CommandResolutionErrorWithLocationAndDetails(SourceLocation location,
                                                 StringView message,
                                                 StringView note,
                                                 i64 command_status)
    : CommandResolutionErrorWithLocation(steal(location), message,
                                         command_status),
      m_note(note)
{}

WarningWithLocation::WarningWithLocation(SourceLocation location,
                                         StringView message)
    : ErrorWithLocation(steal(location), message)
{}

WarningWithLocationAndDetails::WarningWithLocationAndDetails(
    SourceLocation location, StringView message, StringView note)
    : WarningWithLocation(steal(location), message), m_note(note)
{}

cold fn WarningWithLocation::get_severity() const wontthrow -> error_severity
{
  return error_severity::warning;
}

TraceWithLocation::TraceWithLocation(SourceLocation location,
                                     StringView message)
    : ErrorWithLocation(steal(location), message)
{}

cold fn TraceWithLocation::get_severity() const wontthrow -> error_severity
{
  return error_severity::trace;
}

DetailsWithLocation::DetailsWithLocation(SourceLocation location,
                                         StringView message)
    : ErrorWithLocation(steal(location), message)
{}

cold fn DetailsWithLocation::get_severity() const wontthrow -> error_severity
{
  return error_severity::details;
}

ErrorWithLocationAndDetails::ErrorWithLocationAndDetails(
    SourceLocation location, StringView message,
    SourceLocation details_location, StringView details_message,
    StringView note)
    : ErrorWithLocation(steal(location), message),
      m_details_location(steal(details_location)),
      m_details_message(details_message), m_note(note)
{}

ErrorWithLocationAndDetails::ErrorWithLocationAndDetails(
    SourceLocation location, StringView message, StringView note)
    : ErrorWithLocation(steal(location), message),
      m_details_message(heap_allocator()), m_note(note)
{}

cold fn DetailsWithLocation::to_string(StringView source,
                                       EvalContext *context) const throws
    -> String
{
  if (m_message.is_empty()) return String{heap_allocator()};

  let const site = resolve_rendered_site(source, m_location, m_line_offset,
                                         m_is_rebased, context);
  source = site.source;
  usize byte_position = site.location.position;
  let const byte_count = site.location.length;

  if (byte_position > source.count()) return String{heap_allocator()};

  LOG(Debug, "formatting details at byte %zu", byte_position);

  if (byte_position > 0 && byte_position == source.count() &&
      source[byte_position - 1] == '\n')
  {
    byte_position--;
  }

  let const details_line_position =
      utils::source_line_position_at(source, byte_position);
  const usize details_line_byte_position =
      toiletline::get_utf8_length(
          source.data + details_line_position.line_start,
          byte_position - details_line_position.line_start) +
      1;

  let const severity = get_severity();
  let const severity_word = get_error_severity_word(severity);
  let const color = diagnostic_colors_for(severity);

  let result = String{heap_allocator()};
  result += color.location;
  if (let const name = site.location.get_filename();
      name.has_value() && !m_is_filename_hidden)
  {
    result += *name;
    result += ':';
  }
  result += String::from(
      reported_line_number(details_line_position.line_number, site.line_offset),
      heap_allocator());
  result += ':';
  result += String::from(details_line_byte_position, heap_allocator());
  result += ':';
  result += color.get_reset();
  result += ' ';
  result += color.severity;
  result += severity_word;
  result += color.get_reset();
  result += ":\n";

  result += get_context_pointing_to(source, byte_position, byte_count,
                                    details_line_position, site.line_offset,
                                    m_message.view(), color, context);
  return result;
}

cold fn ErrorWithLocationAndDetails::details_to_string(
    StringView source, EvalContext *context) const throws -> String
{
  if (m_details_message.is_empty()) return String{heap_allocator()};

  let const details =
      DetailsWithLocation{m_details_location, m_details_message.view()};
  let result = details.to_string(source, context);
  if (result.is_empty()) return result;

  result += trailing_details_to_string();
  return result;
}

} /* namespace koshka */
