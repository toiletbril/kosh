/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements mknod. It accepts POSIX positional node types and
 * modern named type and device-number options, while keeping node creation
 * behind the platform filesystem boundary.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"
#include "Mode.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-m mode] [--type type --major N --minor N] name [type [major minor]]");
HELP_DESCRIPTION_DECL(
    "The mknod utility creates FIFO, character, and block special files.\n"
    "Examples: mknod pipe p; mknod --fifo pipe; "
    "mknod --character --major 1 --minor 3 device.");

FLAG(MKNOD_MODE, String, 'm', "mode", "Set the node permission mode.");
FLAG(MKNOD_TYPE, String, '\0', "type",
     "Choose fifo, character, or block node type.");
FLAG(MKNOD_FIFO, Bool, '\0', "fifo", "Create a FIFO node.");
FLAG(MKNOD_CHARACTER, Bool, '\0', "character", "Create a character node.");
FLAG(MKNOD_BLOCK, Bool, '\0', "block", "Create a block node.");
FLAG(MKNOD_MAJOR, String, '\0', "major", "Set the device major number.");
FLAG(MKNOD_MINOR, String, '\0', "minor", "Set the device minor number.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Mknod);

namespace koshka::koshkit {

namespace {

static constexpr u32 FIFO_TYPE = 0010000;
static constexpr u32 CHARACTER_TYPE = 0020000;
static constexpr u32 BLOCK_TYPE = 0060000;

fn parse_device_number(StringView text) throws -> Maybe<u64>
{
  let const parsed = utils::parse_decimal_u64(text);
  if (parsed.is_error()) return None;
  return parsed.value();
}

pure fn node_type(StringView text) wontthrow -> Maybe<u32>
{
  if (text == "p" || text == "fifo") return FIFO_TYPE;
  if (text == "c" || text == "char" || text == "character")
    return CHARACTER_TYPE;
  if (text == "b" || text == "block") return BLOCK_TYPE;
  return None;
}

} /* namespace */

Mknod::Mknod() = default;

pure fn Mknod::kind() const wontthrow -> Utility::Kind { return Kind::Mknod; }

fn Mknod::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const [operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const allocator = cxt.scratch_allocator();
  let type_text = StringView{};
  let type_location = operand_locations[0];
  let const named_type_count =
      static_cast<usize>(FLAG_MKNOD_TYPE.is_set()) +
      static_cast<usize>(FLAG_MKNOD_FIFO.is_enabled()) +
      static_cast<usize>(FLAG_MKNOD_CHARACTER.is_enabled()) +
      static_cast<usize>(FLAG_MKNOD_BLOCK.is_enabled());
  if (named_type_count > 1) {
    KOSHKIT_REPORT_ERROR_AT(FLAG_MKNOD_TYPE.is_set()
                                ? FLAG_MKNOD_TYPE.value_location()
                                : FLAG_MKNOD_FIFO.is_enabled()
                                      ? FLAG_MKNOD_FIFO.value_location()
                                      : FLAG_MKNOD_CHARACTER.is_enabled()
                                            ? FLAG_MKNOD_CHARACTER.value_location()
                                            : FLAG_MKNOD_BLOCK.value_location(),
                            "Conflicting node types",
                            "choose one of --type, --fifo, --character, or --block");
    return 1;
  }
  if (FLAG_MKNOD_TYPE.is_set()) {
    type_text = FLAG_MKNOD_TYPE.value();
    type_location = FLAG_MKNOD_TYPE.value_location();
  } else if (FLAG_MKNOD_FIFO.is_enabled()) {
    type_text = "fifo";
    type_location = FLAG_MKNOD_FIFO.value_location();
  } else if (FLAG_MKNOD_CHARACTER.is_enabled()) {
    type_text = "character";
    type_location = FLAG_MKNOD_CHARACTER.value_location();
  } else if (FLAG_MKNOD_BLOCK.is_enabled()) {
    type_text = "block";
    type_location = FLAG_MKNOD_BLOCK.value_location();
  } else if (operands.count() >= 2) {
    type_text = operands[1].view();
    type_location = operand_locations[1];
  } else {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0], "Missing node type",
                            "use p, c, b, fifo, character, or block");
    return 1;
  }

  let const type = node_type(type_text);
  if (!type.has_value()) {
    KOSHKIT_REPORT_ERROR_AT(type_location,
                            "Invalid node type '" + String{allocator, type_text} +
                                "'",
                            "use p, c, b, fifo, character, or block");
    return 1;
  }

  let const is_fifo = *type == FIFO_TYPE;
  if (is_fifo && (FLAG_MKNOD_MAJOR.is_set() || FLAG_MKNOD_MINOR.is_set())) {
    KOSHKIT_REPORT_ERROR_AT(
        FLAG_MKNOD_MAJOR.is_set() ? FLAG_MKNOD_MAJOR.value_location()
                                  : FLAG_MKNOD_MINOR.value_location(),
        "Device numbers require a character or block node",
        "remove --major and --minor or choose a device node type");
    return 1;
  }

  let const has_named_type = named_type_count != 0;
  usize next_operand_index = has_named_type ? 1 : 2;
  StringView major_text{};
  StringView minor_text{};
  SourceLocation major_location = type_location;
  SourceLocation minor_location = type_location;
  bool has_major_text = false;
  bool has_minor_text = false;
  Maybe<u64> major_number;
  Maybe<u64> minor_number;
  if (FLAG_MKNOD_MAJOR.is_set()) {
    major_text = FLAG_MKNOD_MAJOR.value();
    major_location = FLAG_MKNOD_MAJOR.value_location();
    has_major_text = true;
  }
  if (FLAG_MKNOD_MINOR.is_set()) {
    minor_text = FLAG_MKNOD_MINOR.value();
    minor_location = FLAG_MKNOD_MINOR.value_location();
    has_minor_text = true;
  }
  if (!is_fifo) {
    if (!has_major_text && operands.count() > next_operand_index) {
      major_text = operands[next_operand_index].view();
      major_location = operand_locations[next_operand_index++];
      has_major_text = true;
    }
    if (!has_major_text) {
      KOSHKIT_REPORT_ERROR_AT(type_location, "Missing major device number",
                              "provide a major device number");
      return 1;
    }
    major_number = parse_device_number(major_text);
    if (!major_number.has_value()) {
      KOSHKIT_REPORT_ERROR_AT(major_location,
                              "Invalid major device number '" +
                                  String{allocator, major_text} + "'",
                              "use an unsigned decimal integer");
      return 1;
    }
    if (!has_minor_text && operands.count() > next_operand_index) {
      minor_text = operands[next_operand_index].view();
      minor_location = operand_locations[next_operand_index++];
      has_minor_text = true;
    }
    if (!has_minor_text) {
      KOSHKIT_REPORT_ERROR_AT(type_location, "Missing minor device number",
                              "provide a minor device number");
      return 1;
    }
    minor_number = parse_device_number(minor_text);
    if (!minor_number.has_value()) {
      KOSHKIT_REPORT_ERROR_AT(minor_location,
                              "Invalid minor device number '" +
                                  String{allocator, minor_text} + "'",
                              "use an unsigned decimal integer");
      return 1;
    }
    if (*major_number > 0xfffu) {
      KOSHKIT_REPORT_ERROR_AT(major_location,
                              "Major device number '" +
                                  String{allocator, major_text} +
                                  "' is out of range",
                              "use a major device number that fits 12 bits");
      return 1;
    }
    if (*minor_number > 0xfffffu) {
      KOSHKIT_REPORT_ERROR_AT(minor_location,
                              "Minor device number '" +
                                  String{allocator, minor_text} +
                                  "' is out of range",
                              "use a minor device number that fits 20 bits");
      return 1;
    }
  }

  if (operands.count() > next_operand_index) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[next_operand_index],
                            "Too many operands",
                            "provide one name and one node specification");
    return 1;
  }
  u32 mode = 0666;
  if (FLAG_MKNOD_MODE.is_set()) {
    let const parsed = parse_file_mode(FLAG_MKNOD_MODE.value(), mode, 0, false);
    if (!parsed.has_value())
    {
      KOSHKIT_REPORT_ERROR_AT(FLAG_MKNOD_MODE.value_location(),
                              "Invalid mode",
                              "use an octal or symbolic permission mode");
      return 1;
    }
    mode = *parsed;
  }

  i32 status = 0;
  let const &name = operands[0];
  let const did_create =
      is_fifo ? os::make_fifo(name.view(), mode)
              : os::make_device_node(name.view(), mode | *type,
                                     static_cast<u32>(*major_number),
                                     static_cast<u32>(*minor_number));
  if (!did_create) {
    report_soft_koshkit_error(
        ec, cxt, "mknod: Cannot create '" + name +
                       "': " + os::last_system_error_message());
    status = 1;
  }
  return status;
}

} /* namespace koshka::koshkit */
