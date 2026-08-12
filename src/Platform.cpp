#include "Platform.hpp"

#if defined __x86_64__ && !defined __COSMOPOLITAN__
#include <immintrin.h>
#elif defined __aarch64__ || defined __arm64__ || defined _M_ARM64
#include <arm_acle.h>
#if defined __linux__
#include <sys/auxv.h>
#endif
#endif

#if KOSH_PLATFORM_IS POSIX
/* clang-format off */
#include "PlatformPosixExtra.cpp"
#include "PlatformPosix.cpp"
/* clang-format on */
#elif KOSH_PLATFORM_IS WIN32
#include "PlatformWin32.cpp"
#else
#error Unsupported platform
#endif

namespace koshka {
namespace os {

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
    if (*read_count == 0) return contents;
    contents.append(StringView{buffer, *read_count});
  }
}

} /* namespace os */
} /* namespace koshka */

namespace koshka {
namespace os {

namespace {

#if defined __x86_64__ && !defined __COSMOPOLITAN__
[[gnu::target("sse4.2")]] pure fn crc32c_update_sse42(u32 crc, const u8 *data,
                                                      usize length) wontthrow
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
  static let const has_sse42 = []() -> bool {
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.2");
  }();
  if (has_sse42) return crc32c_update_sse42(crc, bytes, length);
#elif defined __aarch64__ || defined __arm64__ || defined _M_ARM64
  static let const has_crc32c = is_aarch64_crc32c_available();
  if (has_crc32c) return crc32c_update_acle(crc, bytes, length);
#endif

  return crc32c_update_software(crc, bytes, length);
}

} /* namespace os */
} /* namespace koshka */
