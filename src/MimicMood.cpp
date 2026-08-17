#include "MimicMood.hpp"

#include "PackedStringKey.hpp"

namespace koshka {

pure fn detect_mimic_shell_from_extension(StringView extension) throws
    -> Maybe<mimic_mood>
{
  static constexpr static_string_entry<mimic_mood> EXTENSION_ENTRIES[] = {
      {SSK(".bash"), mimic_mood::Bash   },
      {SSK(".dash"), mimic_mood::Posix  },
      {SSK(".kosh"), mimic_mood::Default},
      {SSK(".sh"),   mimic_mood::Posix  },
  };
  static constexpr StaticStringMap EXTENSIONS{EXTENSION_ENTRIES};
  return EXTENSIONS.find(extension);
}

fn detect_mimic_shell_from_source(StringView source) throws -> Maybe<mimic_mood>
{
  if (!source.starts_with("#!")) return None;

  constexpr usize shebang_byte_limit = 256;
  let const source_limit =
      source.length < shebang_byte_limit ? source.length : shebang_byte_limit;
  usize line_end = 2;
  while (line_end < source_limit && source[line_end] != '\n')
    line_end++;
  if (line_end > 2 && source[line_end - 1] == '\r') line_end--;
  let const line = source.substring_of_length(2, line_end - 2);

  let const do_basename_of = [](StringView token) -> StringView {
    usize last_slash = token.length;
    for (usize position = 0; position < token.length; position++)
      if (token[position] == '/') last_slash = position;
    return last_slash == token.length ? token : token.substring(last_slash + 1);
  };
  usize position = 0;
  let const do_next_token = [&]() -> StringView {
    while (position < line.length &&
           (line[position] == ' ' || line[position] == '\t'))
      position++;
    let const start = position;
    while (position < line.length && line[position] != ' ' &&
           line[position] != '\t')
      position++;
    return line.substring_of_length(start, position - start);
  };

  let shell = do_basename_of(do_next_token());
  if (shell == "env") {
    loop
    {
      let const token = do_next_token();
      if (token.is_empty()) return None;
      if (token[0] == '-') continue;
      shell = do_basename_of(token);
      break;
    }
  }

  static constexpr static_string_entry<mimic_mood> SHELL_ENTRIES[] = {
      {SSK("sh"),   mimic_mood::Posix  },
      {SSK("dash"), mimic_mood::Posix  },
      {SSK("bash"), mimic_mood::Bash   },
      {SSK("kosh"), mimic_mood::Default},
  };
  static constexpr StaticStringMap SHELL_MOODS{SHELL_ENTRIES};
  return SHELL_MOODS.find(shell);
}

} /* namespace koshka */
