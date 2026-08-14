#pragma once

/* Timestamp the build. */

#if !defined KOSH_ENVCXXFLAGS
#warning KOSH_ENVCXXFLAGS should be defined. Please use provided makefile \
         for compilation!
#define KOSH_ENVCXXFLAGS "<unknown>"
#endif

#if !defined KOSH_COMPILER_COMMAND
#warning KOSH_COMPILER_COMMAND should be defined. Please use provided makefile \
         for compilation!
#define KOSH_COMPILER_COMMAND "<unknown>"
#endif

#if !defined KOSH_COMMIT_HASH
#warning KOSH_COMMIT_HASH should be defined. Please use provided makefile for \
         compilation!
#define KOSH_COMMIT_HASH "<unknown>"
#endif

#if !defined KOSH_BUILD_MODE
#warning KOSH_BUILD_MODE should be defined. Please use provided makefile for \
         compilation!
#define KOSH_BUILD_MODE "<unset>"
#endif

#if !defined KOSH_OS_INFO
#warning KOSH_OS_INFO should be defined. Please use provided makefile for \
         compilation!
#define KOSH_OS_INFO "<unset>"
#endif

#if !defined KOSH_LIBC
#warning KOSH_LIBC should be defined. Please use provided makefile for \
         compilation!
#define KOSH_LIBC "<unknown libc>"
#endif

#define KOSH_BUILD_DATE (__DATE__ " at " __TIME__)

#define KOSH_COMPILER KOSH_COMPILER_COMMAND " (" __VERSION__ ", " KOSH_LIBC ")"

#define KOSH_VER_MAJOR 0
#define KOSH_VER_MINOR 2
#define KOSH_VER_PATCH 0
#define KOSH_VER_EXTRA "rc1"

#define KOSH_STRINGIFY_INNER(x) #x
#define KOSH_STRINGIFY(x)       KOSH_STRINGIFY_INNER(x)
#define KOSH_VERSION_STRING                                                    \
  KOSH_STRINGIFY(KOSH_VER_MAJOR)                                               \
  "." KOSH_STRINGIFY(KOSH_VER_MINOR) "." KOSH_STRINGIFY(                       \
      KOSH_VER_PATCH) "-" KOSH_VER_EXTRA

#define KOSH_SHORT_LICENSE                                                     \
  "Licensed under the 3-Clause BSD License.\n"                                 \
  "There is NO WARRANTY, to the extent permitted by law."

/* clang-format off */
#include <cctype>
#include <climits>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <clocale>
#include <cmath>
#include <cstring>

#include <type_traits>
#include <initializer_list>
#include <limits>
#include <utility>
#include <new>
#include <exception>

/* clang-format on */

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using u128 = __uint128_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using i128 = __int128_t;

using f64 = double;

using uchar = u8;
using ichar = i8;

using usize = size_t;
using uintptr = uintptr_t;

/* void is ambiguous, this is an alias for clarity. */
using opaque = void;

#if defined __GNUC__ || defined __clang__ || defined __COSMOCC__
#define T__HAS_GCC_EXTENSIONS 1
#define t__used               __attribute__((used))
#define t__pure               __attribute__((pure))
#define t__forceinline        inline __attribute__((always_inline))
#define t__unreachable()      __builtin_unreachable()
#define t__debugtrap()        __builtin_trap()
#else /* __GNUC__ || __clang__ || __COSMOCC__ */
#error Oh no! Segmentation fault. Please download a better compiler that \
       supports GNU extensions!
#define T__HAS_GCC_EXTENSIONS 0
#define t__used               /* None */
#define t__pure               /* None */
#define t__forceinline        /* None */
#define t__unreachable()      abort()
#define t__debugtrap()        abort()
#endif
#define t__concat_literal(x, y) x##y
#define concat_literal(x, y)    t__concat_literal(x, y)

template <typename T>
class t__exit_scope
{
public:
  t__exit_scope(T lambda) : m_lambda(lambda) {}
  ~t__exit_scope() { m_lambda(); }
  t__exit_scope(const t__exit_scope &);

private:
  T m_lambda;
  t__exit_scope &operator=(const t__exit_scope &);
};

class t__exit_scope_help
{
public:
  template <typename T>
  t__exit_scope<T> operator+(T t)
  {
    return t;
  }
};

#define defer                                                                  \
  const auto &concat_literal(defer__, __LINE__) =                              \
      t__exit_scope_help() + [&]() -> void

#define ENUM(e) static_cast<int>(e)

#define sub_sat(a, b) ((a) > (b) ? (a) - (b) : 0)

#define unused(x) ((void) (x))

#define countof(arr) (sizeof(arr) / sizeof(*(arr)))
#define steal        std::move
#define mustuse      [[nodiscard]]

#define fn   auto
#define let  auto
#define loop for (;;)

#define wontthrow noexcept
#define throws    noexcept(false)

#define donteliminate t__used
#define alwaysinline  t__forceinline

#if T__HAS_GCC_EXTENSIONS
#define pure t__pure
#define cold [[gnu::cold]]
#define hot  [[gnu::hot]]
#if defined __clang__
#define flatten [[gnu::flatten]]
#else
#define flatten /* nothing. GNU is too harsh with inlining. */
#endif          /* __clang__ */
#define noinline [[gnu::noinline]]
#else
#define pure
#define cold
#define hot
#define flatten
#define noinline
#endif /* T__HAS_GCC_EXTENSIONS */

namespace koshka {
constexpr const char *EXPRESSION_AST_INDENT = " ";
constexpr const char *EXPRESSION_DOUBLE_AST_INDENT = "  ";
} /* namespace koshka */
