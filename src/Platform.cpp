#include "Platform.hpp"

namespace koshka {
namespace os {

static fn is_trappable_signal(i32 signal_number) wontthrow -> bool;

} /* namespace os */
} /* namespace koshka */

#if defined __x86_64__ && !defined __COSMOPOLITAN__
#include <immintrin.h>
#elif defined __aarch64__ || defined __arm64__ || defined _M_ARM64
#include <arm_acle.h>
#if defined __linux__
#include <sys/auxv.h>
#endif
#endif

#if KOSH_PLATFORM_IS KOSH_PLATFORM_POSIX
/* clang-format off */
#include "PlatformPosixExtra.cpp"
#include "PlatformPosix.cpp"
#include "PlatformPosixFilesystem.cpp"
#include "PlatformPosixProcess.cpp"
/* clang-format on */
#elif KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
#include "PlatformWin32.cpp"
#include "PlatformWin32Filesystem.cpp"
#include "PlatformWin32Process.cpp"
#else
#error Unsupported platform
#endif

namespace koshka {
namespace os {

static fn is_trappable_signal(i32 signal_number) wontthrow -> bool
{
  return signal_number > 0 && signal_number < SIGNAL_FLAG_COUNT;
}

fn take_pending_signal() wontthrow -> i32
{
  for (i32 number = 1; number < SIGNAL_FLAG_COUNT; number++) {
    if (PENDING_SIGNAL_FLAGS[number] != 0) {
      PENDING_SIGNAL_FLAGS[number] = 0;
      return number;
    }
  }
  return 0;
}

fn get_shell_process_id() wontthrow -> i64
{
  return static_cast<i64>(PARENT_SHELL_PID);
}

fn get_file_creation_mask() wontthrow -> u32
{
  let const previous_mask = KOSH_UMASK(0);
  KOSH_UMASK(previous_mask);

  return static_cast<u32>(previous_mask);
}

fn set_file_creation_mask(u32 mask) wontthrow -> void { KOSH_UMASK(mask); }

fn descriptor_is_shell_fd(os::descriptor fd, i32 shell_fd) wontthrow -> bool
{
  return fd == descriptor_for_shell_fd(shell_fd);
}

fn compile_regex(StringView pattern, case_sensitivity sensitivity,
                 compiled_regex &out) throws -> regex_compile_result
{
  let const is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  let const pattern_text = String{heap_allocator(), pattern};
  int compile_flags = REG_EXTENDED;
  if (is_case_insensitive) compile_flags |= REG_ICASE;

  if (regcomp(&out.re, pattern_text.c_str(), compile_flags) != 0)
    return regex_compile_result::Invalid;

  return regex_compile_result::Ok;
}

fn compile_basic_regex(StringView pattern, case_sensitivity sensitivity,
                       compiled_regex &out) throws -> regex_compile_result
{
  let const is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  let const pattern_text = String{heap_allocator(), pattern};
  int compile_flags = 0;
  if (is_case_insensitive) compile_flags |= REG_ICASE;

  if (regcomp(&out.re, pattern_text.c_str(), compile_flags) != 0)
    return regex_compile_result::Invalid;

  return regex_compile_result::Ok;
}

fn execute_regex(compiled_regex &compiled, StringView subject,
                 ArrayList<regex_span> &spans, String &error_message,
                 Allocator scratch) throws -> regex_match_result
{
  let const subject_text = String{scratch, subject};
  let const group_count = compiled.re.re_nsub + 1;
  let matches = ArrayList<regmatch_t>{scratch};
  matches.reserve(group_count);
  for (usize i = 0; i < group_count; i++)
    matches.push(regmatch_t{});

  const int match_result = regexec(&compiled.re, subject_text.c_str(),
                                   group_count, matches.begin(), 0);

  if (match_result == REG_NOMATCH) return regex_match_result::NoMatch;

  if (match_result != 0) {
    char error_text[256];
    regerror(match_result, &compiled.re, error_text, sizeof(error_text));
    error_message = String{heap_allocator(), StringView{error_text}};
    return regex_match_result::Error;
  }

  spans.reserve(group_count);
  for (usize i = 0; i < group_count; i++) {
    spans.push(regex_span{static_cast<i64>(matches[i].rm_so),
                          static_cast<i64>(matches[i].rm_eo)});
  }

  return regex_match_result::Matched;
}

fn free_regex(compiled_regex &compiled) wontthrow -> void
{
  regfree(&compiled.re);
}

fn compile_search_regex(StringView pattern, case_sensitivity sensitivity,
                        compiled_regex &out) throws -> regex_compile_result
{
  let const is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  const String pattern_text{heap_allocator(), pattern};
  int compile_flags = REG_NOSUB;
  if (is_case_insensitive) compile_flags |= REG_ICASE;

  if (regcomp(&out.re, pattern_text.c_str(), compile_flags) != 0)
    return regex_compile_result::Invalid;

  return regex_compile_result::Ok;
}

fn regex_matches(compiled_regex &compiled, StringView subject) throws -> bool
{
#if defined REG_STARTEND
  regmatch_t bounds[1];
  bounds[0].rm_so = 0;
  bounds[0].rm_eo = static_cast<regoff_t>(subject.length);
  return regexec(&compiled.re, subject.data, 1, bounds, REG_STARTEND) == 0;
#else
  const String null_terminated{heap_allocator(), subject};
  return regexec(&compiled.re, null_terminated.c_str(), 0, nullptr, 0) == 0;
#endif
}

fn regex_matches_null_terminated(compiled_regex &compiled,
                                 StringView subject) throws -> bool
{
#if defined REG_STARTEND
  return regex_matches(compiled, subject);
#else
  return regexec(&compiled.re, subject.data, 0, nullptr, 0) == 0;
#endif
}

static u64 DESCRIPTOR_EPOCH = 0;

pure fn get_descriptor_epoch() wontthrow -> u64 { return DESCRIPTOR_EPOCH; }

fn note_descriptor_rebound() wontthrow -> void { DESCRIPTOR_EPOCH++; }

fn read_fd_to_string(os::descriptor fd, Allocator allocator) throws
    -> Maybe<String>
{
  let contents = String{allocator};
  char buffer[16384];
  loop
  {
    let const read_count = read_fd(fd, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) return Maybe<String>{steal(contents)};
    contents.append(StringView{buffer, *read_count});
  }
}

} /* namespace os */
} /* namespace koshka */

namespace koshka {
namespace os {

namespace {

#if defined __x86_64__ && !defined __COSMOPOLITAN__
#if defined __clang__
[[gnu::target("crc32")]]
#else
[[gnu::target("sse4.2")]]
#endif
pure fn crc32c_update_sse42(u32 crc, const u8 *data, usize length) wontthrow
    -> u32
{
  while (length >= 8) {
    u64 word;
    __builtin_memcpy(&word, data, 8);
    crc = static_cast<u32>(_mm_crc32_u64(crc, word));
    data += 8;
    length -= 8;
  }
  while (length-- > 0)
    crc = _mm_crc32_u8(crc, *data++);
  return crc;
}

fn is_x86_sse42_available() wontthrow -> bool
{
  u32 eax = 1;
  u32 ebx;
  u32 ecx;
  u32 edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  unused(ebx);
  unused(edx);
  return (ecx & (1u << 20)) != 0;
}
#endif

#if defined __aarch64__ || defined __arm64__ || defined _M_ARM64
[[gnu::target("+crc")]] pure fn crc32c_update_acle(u32 crc, const u8 *data,
                                                   usize length) wontthrow
    -> u32
{
  while (length >= 8) {
    u64 word;
    __builtin_memcpy(&word, data, 8);
    crc = __crc32cd(crc, word);
    data += 8;
    length -= 8;
  }
  while (length-- > 0)
    crc = __crc32cb(crc, *data++);
  return crc;
}

fn is_aarch64_crc32c_available() wontthrow -> bool
{
#if defined __linux__
  let const crc32c_hardware_capability = 1UL << 7;
  return (getauxval(AT_HWCAP) & crc32c_hardware_capability) != 0;
#elif defined __APPLE__
  int is_available = 0;
  usize value_length = sizeof(is_available);
  if (sysctlbyname("hw.optional.armv8_crc32", &is_available, &value_length,
                   nullptr, 0) != 0)
    return false;
  return is_available != 0;
#elif defined _WIN32 && defined PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
  return IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) !=
         FALSE;
#else
  return false;
#endif
}
#endif

pure alwaysinline fn crc32c_update_software(u32 crc, const u8 *data,
                                            usize length) wontthrow -> u32
{
  for (usize position = 0; position < length; position++) {
    crc ^= data[position];
    for (int bit = 0; bit < 8; bit++) {
      let const mix =
          static_cast<u32>(-static_cast<i32>(crc & 1)) & 0x82f63b78u;
      crc = (crc >> 1) ^ mix;
    }
  }
  return crc;
}

} /* namespace */

fn crc32c_update(u32 crc, const void *data, usize length) wontthrow -> u32
{
  let const bytes = static_cast<const u8 *>(data);

#if defined __x86_64__ && !defined __COSMOPOLITAN__
  static let const has_sse42 = is_x86_sse42_available();
  if (has_sse42) return crc32c_update_sse42(crc, bytes, length);
#elif defined __aarch64__ || defined __arm64__ || defined _M_ARM64
  static let const has_crc32c = is_aarch64_crc32c_available();
  if (has_crc32c) return crc32c_update_acle(crc, bytes, length);
#endif

  return crc32c_update_software(crc, bytes, length);
}

} /* namespace os */
} /* namespace koshka */
