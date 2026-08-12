#include "Diagnostics.hpp"

#include "Lexer.hpp"
#include "Utils.hpp"

namespace koshka {

pure fn get_diagnostic_definition(diagnostic_id id) wontthrow
    -> const diagnostic_definition &
{
  let const index = static_cast<usize>(id);
  ASSERT(index < get_diagnostic_count());
  return DIAGNOSTIC_DEFINITIONS[index];
}

pure fn get_diagnostic_count() wontthrow -> usize
{
  return static_cast<usize>(diagnostic_id::Count);
}

fn format_diagnostic_template(
    const char *text_template,
    std::initializer_list<StringView> arguments) throws -> String
{
  let result = String{heap_allocator()};
  let const text = StringView{text_template};

  for (usize position = 0; position < text.length; position++) {
    if (text[position] != '{' || position + 2 >= text.length ||
        text[position + 2] != '}')
    {
      result += text[position];
      continue;
    }

    usize argument_index = 0;
    switch (text[position + 1]) {
    case '0': argument_index = 0; break;
    case '1': argument_index = 1; break;
    default: result += text[position]; continue;
    }

    if (argument_index >= arguments.size()) return String{heap_allocator()};
    result += *(arguments.begin() + argument_index);
    position += 2;
  }

  return result;
}

fn append_diagnostic_code(String &message, Maybe<u16> shellcheck_code) throws
    -> void
{
  if (!shellcheck_code.has_value()) return;
  if (message.is_empty()) return;

  if (let const last_byte = message.back();
      last_byte != '.' && last_byte != '?' && last_byte != '!')
  {
    message += '.';
  }

  char code_text[32];
  message += " (SC";
  message +=
      utils::int_to_text_into(*shellcheck_code, code_text, sizeof(code_text));
  message += ')';
}

static pure fn parse_diagnostic_code(StringView text) wontthrow -> Maybe<u16>
{
  if (text.length >= 2 && text[0] == 'S' && text[1] == 'C') {
    text = text.substring(2);
  }
  if (!text.is_all_decimal_digits()) return None;

  u32 value = 0;
  for (usize position = 0; position < text.length; position++) {
    value = value * 10 + static_cast<u32>(text[position] - '0');
    if (value > UINT16_MAX) return None;
  }

  return static_cast<u16>(value);
}

pure fn shellcheck_selector_disables(const shellcheck_selector &selector,
                                     StringView source,
                                     diagnostic_id id) wontthrow -> bool
{
  let const &definition = get_diagnostic_definition(id);

  switch (selector.kind) {
  case shellcheck_selector_kind::All: return true;
  case shellcheck_selector_kind::Slug:
    return source.substring_of_length(selector.slug.position,
                                      selector.slug.length) ==
           StringView{definition.slug};
  case shellcheck_selector_kind::Code:
    return definition.shellcheck_code.has_value() &&
           *definition.shellcheck_code == selector.code_start;
  case shellcheck_selector_kind::CodeRange:
    return definition.shellcheck_code.has_value() &&
           *definition.shellcheck_code >= selector.code_start &&
           *definition.shellcheck_code < selector.code_end;
  }

  return false;
}

static fn
push_resolved_selector(StringView source, StringView text,
                       ArrayList<shellcheck_selector> &selectors) throws -> void
{
  if (text == StringView{"all"}) {
    selectors.push(shellcheck_selector{shellcheck_selector_kind::All});
    return;
  }

  if (let const code = parse_diagnostic_code(text); code.has_value()) {
    selectors.push(shellcheck_selector{
        shellcheck_selector_kind::Code, {0, 0},
         *code, 0
    });
    return;
  }

  if (let const separator = text.find_character('-'); separator.has_value()) {
    let const range_start =
        parse_diagnostic_code(text.substring_of_length(0, *separator));
    let const range_end = parse_diagnostic_code(text.substring(*separator + 1));
    if (range_start.has_value() && range_end.has_value()) {
      selectors.push(shellcheck_selector{
          shellcheck_selector_kind::CodeRange,
          {0, 0},
          *range_start,
          *range_end
      });
      return;
    }
  }

  let const slug_position = static_cast<usize>(text.data - source.data);
  selectors.push(shellcheck_selector{
      shellcheck_selector_kind::Slug, {slug_position, text.length},
       0, 0
  });
}

static fn collect_comma_separated_selectors(
    StringView source, StringView value,
    ArrayList<shellcheck_selector> &selectors) throws -> void
{
  usize component_start = 0;
  while (component_start <= value.length) {
    usize component_end = component_start;
    while (component_end < value.length && value[component_end] != ',') {
      component_end++;
    }
    push_resolved_selector(
        source,
        value.substring_of_length(component_start,
                                  component_end - component_start),
        selectors);
    if (component_end == value.length) break;
    component_start = component_end + 1;
  }
}

fn collect_shellcheck_selectors(
    StringView source, shellcheck_directive_span comment_span,
    ArrayList<shellcheck_selector> &selectors) throws -> void
{
  if (comment_span.position + comment_span.length > source.length) return;

  let const comment =
      source.substring_of_length(comment_span.position, comment_span.length);
  usize position = 1;
  while (position < comment.length &&
         (comment[position] == ' ' || comment[position] == '\t'))
  {
    position++;
  }
  let const directive_text = comment.substring(position);
  if (!directive_text.starts_with(StringView{"shellcheck"}) ||
      (directive_text.length > 10 && directive_text[10] != ' ' &&
       directive_text[10] != '\t'))
  {
    return;
  }
  position += 10;

  while (position < comment.length) {
    while (position < comment.length &&
           (comment[position] == ' ' || comment[position] == '\t'))
    {
      position++;
    }
    if (position >= comment.length || comment[position] == '#') {
      break;
    }
    if (!comment.substring(position).starts_with(StringView{"disable="})) {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
      continue;
    }
    position += 8;

    char quote = '\0';
    if (position < comment.length &&
        (comment[position] == '\'' || comment[position] == '"'))
    {
      quote = comment[position++];
    }
    let const value_start = position;
    if (quote != '\0') {
      while (position < comment.length && comment[position] != quote) {
        position++;
      }
      if (position == comment.length) return;
    } else {
      while (position < comment.length && comment[position] != ' ' &&
             comment[position] != '\t' && comment[position] != '#')
      {
        position++;
      }
    }
    collect_comma_separated_selectors(
        source,
        comment.substring_of_length(value_start, position - value_start),
        selectors);
    if (quote != '\0' && position < comment.length) {
      position++;
    }
  }
}

} /* namespace koshka */
