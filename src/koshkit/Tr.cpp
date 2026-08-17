#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-d] set1 [set2]");

HELP_DESCRIPTION_DECL(
    "The tr utility translates the bytes in set1 to the matching bytes in "
    "set2.");

FLAG(TR_DELETE, Bool, 'd', "", "Delete the bytes in set1.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tr);

namespace koshka {

namespace koshkit {

using posix_class_test = bool (*)(u8 byte);

constexpr static_string_entry<posix_class_test> TR_POSIX_CLASS_ENTRIES[] = {
    {SSK("alnum"),  [](u8 byte) { return std::isalnum(byte) != 0; } },
    {SSK("alpha"),  [](u8 byte) { return std::isalpha(byte) != 0; } },
    {SSK("blank"),  [](u8 byte) { return std::isblank(byte) != 0; } },
    {SSK("cntrl"),  [](u8 byte) { return std::iscntrl(byte) != 0; } },
    {SSK("digit"),  [](u8 byte) { return std::isdigit(byte) != 0; } },
    {SSK("graph"),  [](u8 byte) { return std::isgraph(byte) != 0; } },
    {SSK("lower"),  [](u8 byte) { return std::islower(byte) != 0; } },
    {SSK("print"),  [](u8 byte) { return std::isprint(byte) != 0; } },
    {SSK("punct"),  [](u8 byte) { return std::ispunct(byte) != 0; } },
    {SSK("space"),  [](u8 byte) { return std::isspace(byte) != 0; } },
    {SSK("upper"),  [](u8 byte) { return std::isupper(byte) != 0; } },
    {SSK("xdigit"), [](u8 byte) { return std::isxdigit(byte) != 0; }},
};

constexpr StaticStringMap TR_POSIX_CLASSES{TR_POSIX_CLASS_ENTRIES};

struct decoded_char
{
  unsigned char byte;
  usize width_count;
};

static fn decode_escaped_char(StringView set, usize position) wontthrow
    -> decoded_char
{
  if (set[position] != '\\' || position + 1 >= set.length) {
    return {static_cast<unsigned char>(set[position]), 1};
  }

  let const escaped = set[position + 1];
  switch (escaped) {
  case 'a': return {'\a', 2};
  case 'b': return {'\b', 2};
  case 'f': return {'\f', 2};
  case 'n': return {'\n', 2};
  case 'r': return {'\r', 2};
  case 't': return {'\t', 2};
  case 'v': return {'\v', 2};
  case '\\': return {'\\', 2};
  }

  if (escaped >= '0' && escaped <= '7') {
    int value = 0;
    usize digit_count = 0;
    while (digit_count < 3 && position + 1 + digit_count < set.length) {
      let const digit = set[position + 1 + digit_count];
      if (digit < '0' || digit > '7') {
        break;
      }
      value = value * 8 + (digit - '0');
      digit_count++;
    }
    return {static_cast<unsigned char>(value), 1 + digit_count};
  }

  return {static_cast<unsigned char>(escaped), 2};
}

static fn expand_posix_class(StringView set, usize position,
                             String &expanded) throws -> usize
{
  if (set[position] != '[' || position + 1 >= set.length ||
      set[position + 1] != ':')
  {
    return 0;
  }

  usize scan = position + 2;
  while (scan + 1 < set.length && !(set[scan] == ':' && set[scan + 1] == ']'))
    scan++;

  if (scan + 1 >= set.length || set[scan] != ':' || set[scan + 1] != ']') {
    return 0;
  }

  let const name_start = position + 2;
  let const class_name = set.substring_of_length(name_start, scan - name_start);
  let const test = TR_POSIX_CLASSES.find(class_name);
  if (!test.has_value()) return 0;

  for (int byte = 0; byte < 256; byte++) {
    if ((*test)(static_cast<u8>(byte))) expanded.push(static_cast<char>(byte));
  }

  return scan + 2 - position;
}

static fn expand_set(StringView set, Allocator allocator) throws -> String
{
  String expanded{allocator};
  usize i = 0;
  while (i < set.length) {
    if (let const class_width = expand_posix_class(set, i, expanded);
        class_width > 0)
    {
      i += class_width;
      continue;
    }

    let const first = decode_escaped_char(set, i);
    let const after = i + first.width_count;

    if (after < set.length && set[after] == '-' && after + 1 < set.length) {
      let const second = decode_escaped_char(set, after + 1);
      if (first.byte > second.byte) {
        throw ErrorWithDetails{
            "tr rejects a reverse range in a set",
            "Order the range endpoints so the low byte precedes the high byte"};
      }

      for (int c = first.byte; c <= second.byte; c++)
        expanded.push(static_cast<char>(c));

      i = after + 1 + second.width_count;
      continue;
    }

    expanded.push(static_cast<char>(first.byte));
    i = after;
  }
  return expanded;
}

Tr::Tr() = default;

pure fn Tr::kind() const wontthrow -> Utility::Kind { return Kind::Tr; }

fn Tr::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const is_deleting = FLAG_TR_DELETE.is_enabled();
  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  if (!is_deleting && operands.count() < 2) {
    throw ErrorWithDetails{"tr expects two sets unless -d is given",
                           "Supply SET1 and SET2, or use `-d` with one set"};
  }

  let const set1 = expand_set(operands[0].view(), cxt.scratch_allocator());
  let const set2 =
      is_deleting ? String{cxt.scratch_allocator()}
                  : expand_set(operands[1].view(), cxt.scratch_allocator());

  static constexpr usize BYTE_VALUE_COUNT = 256;
  bool is_in_set1[BYTE_VALUE_COUNT] = {};
  unsigned char translation[BYTE_VALUE_COUNT];
  for (usize i = 0; i < BYTE_VALUE_COUNT; i++)
    translation[i] = static_cast<unsigned char>(i);

  for (usize i = 0; i < set1.count(); i++) {
    let const from = static_cast<unsigned char>(set1.view()[i]);
    is_in_set1[from] = true;
    if (!is_deleting && set2.count() > 0) {
      let const index = i < set2.count() ? i : set2.count() - 1;
      translation[from] = static_cast<unsigned char>(set2.view()[index]);
    }
  }

  char input[65536];
  char output[sizeof(input)];
  loop
  {
    let const read_count =
        os::read_fd(ec.in_fd.value_or(KOSH_STDIN), input, sizeof(input));
    if (!read_count.has_value()) {
      if (os::INTERRUPT_REQUESTED) return 130;
      report_soft_koshkit_error(
          ec, cxt, "tr: read failed: " + os::last_system_error_message());
      return 1;
    }
    if (*read_count == 0) break;
    usize output_count = 0;
    for (usize i = 0; i < *read_count; i++) {
      let const byte = static_cast<unsigned char>(input[i]);
      if (!is_in_set1[byte]) {
        output[output_count++] = static_cast<char>(byte);
      } else if (!is_deleting) {
        output[output_count++] = static_cast<char>(translation[byte]);
      }
    }
    if (output_count > 0) ec.print_to_stdout(StringView{output, output_count});
  }

  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
