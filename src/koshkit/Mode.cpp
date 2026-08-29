#include "Mode.hpp"

namespace koshka::koshkit {

fn parse_file_mode(StringView expression, u32 current_mode, u32 creation_mask,
                   bool is_directory) wontthrow -> Maybe<u32>
{
  if (expression.is_empty()) return None;

  bool is_octal = expression.length <= 4;
  u32 octal_mode = 0;
  for (usize position = 0; position < expression.length; position++) {
    let const byte = expression[position];
    if (byte < '0' || byte > '7') {
      is_octal = false;
      break;
    }
    octal_mode = octal_mode * 8 + static_cast<u32>(byte - '0');
  }
  if (is_octal) return (current_mode & ~07777u) | octal_mode;

  u32 mode = current_mode;
  usize position = 0;
  while (position < expression.length) {
    u32 selected_classes = 0;
    bool has_explicit_class = false;
    while (position < expression.length) {
      let const byte = expression[position];
      if (byte == 'u')
        selected_classes |= 0700;
      else if (byte == 'g')
        selected_classes |= 0070;
      else if (byte == 'o')
        selected_classes |= 0007;
      else if (byte == 'a')
        selected_classes |= 0777;
      else
        break;
      has_explicit_class = true;
      position++;
    }
    if (!has_explicit_class) selected_classes = 0777;
    if (position == expression.length) return None;

    let const operation = expression[position++];
    if (operation != '+' && operation != '-' && operation != '=') return None;

    u32 requested_bits = 0;
    u32 requested_special_bits = 0;
    while (position < expression.length && expression[position] != ',') {
      let const byte = expression[position++];
      switch (byte) {
      case 'r': requested_bits |= 0444; break;
      case 'w': requested_bits |= 0222; break;
      case 'x': requested_bits |= 0111; break;
      case 'X':
        if (is_directory || (mode & 0111) != 0) requested_bits |= 0111;
        break;
      case 's':
        if ((selected_classes & 0700) != 0) requested_special_bits |= 04000;
        if ((selected_classes & 0070) != 0) requested_special_bits |= 02000;
        break;
      case 't': requested_special_bits |= 01000; break;
      case 'u': {
        let const source = (mode >> 6) & 7;
        if ((selected_classes & 0700) != 0) requested_bits |= source << 6;
        if ((selected_classes & 0070) != 0) requested_bits |= source << 3;
        if ((selected_classes & 0007) != 0) requested_bits |= source;
        break;
      }
      case 'g': {
        let const source = (mode >> 3) & 7;
        if ((selected_classes & 0700) != 0) requested_bits |= source << 6;
        if ((selected_classes & 0070) != 0) requested_bits |= source << 3;
        if ((selected_classes & 0007) != 0) requested_bits |= source;
        break;
      }
      case 'o': {
        let const source = mode & 7;
        if ((selected_classes & 0700) != 0) requested_bits |= source << 6;
        if ((selected_classes & 0070) != 0) requested_bits |= source << 3;
        if ((selected_classes & 0007) != 0) requested_bits |= source;
        break;
      }
      default: return None;
      }
    }

    u32 affected_bits = selected_classes;
    if (!has_explicit_class) affected_bits &= ~creation_mask;
    requested_bits &= affected_bits;
    u32 affected_special_bits = 0;
    if ((selected_classes & 0700) != 0) affected_special_bits |= 04000;
    if ((selected_classes & 0070) != 0) affected_special_bits |= 02000;
    if ((selected_classes & 0007) != 0) affected_special_bits |= 01000;

    if (operation == '=') mode &= ~(affected_bits | affected_special_bits);
    if (operation == '-')
      mode &= ~(requested_bits | requested_special_bits);
    else
      mode |= requested_bits | requested_special_bits;

    if (position == expression.length) break;
    position++;
    if (position == expression.length) return None;
  }

  return mode;
}

} // namespace koshka::koshkit
