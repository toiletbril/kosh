#pragma once

#include "Common.hpp"

namespace koshka {
class String;
}

#if !defined NDEBUG
#define TRACE(...)                                                             \
  do {                                                                         \
    unused(std::fprintf(stderr, "[TRACE] " __FILE__ ":%d: ", __LINE__));       \
    unused(std::fprintf(stderr, __VA_ARGS__));                                 \
    unused(fflush(stderr));                                                    \
  } while (0)
#define TRACELN(...)                                                           \
  do {                                                                         \
    unused(std::fprintf(stderr, "[TRACE] " __FILE__ ":%d: ", __LINE__));       \
    unused(std::fprintf(stderr, __VA_ARGS__));                                 \
    unused(fputc('\n', stderr));                                               \
    unused(fflush(stderr));                                                    \
  } while (0)
#if defined __clang__
/* The string parameter is a template so the body sees a complete
   ::koshka::String only at the call site. Debug.hpp can not include String.hpp,
   since String.hpp includes Debug.hpp, so naming String here would close an
   include cycle. */
namespace koshka {

template <class P>
constexpr bool t__is_dumpable_record_pointer()
{
  using Pointer = __remove_cvref(P);

  if constexpr (!__is_pointer(Pointer)) {
    return false;
  } else {
    using Record = __remove_cv(__remove_pointer(Pointer));

    return requires { sizeof(Record); } &&
           (__is_class(Record) || __is_union(Record));
  }
}

template <class StringT>
struct t__dump_state
{
  StringT output;
  usize base_indent{0};
  bool is_at_line_start{true};
};

template <class StringT>
fn t__append_dump_text(t__dump_state<StringT> &state, const char *text) throws
    -> void
{
  for (; *text != '\0'; text++) {
    if (state.is_at_line_start && *text != '\n') {
      for (usize indent = 0; indent < state.base_indent; indent++)
        state.output.push(' ');

      state.is_at_line_start = false;
    }

    state.output.push(*text);
    if (*text == '\n') state.is_at_line_start = true;
  }
}

template <class Last>
fn t__last_dump_argument(Last &last) wontthrow -> Last &
{
  return last;
}

template <class First, class... Rest>
  requires(sizeof...(Rest) > 0)
fn t__last_dump_argument(First &, Rest &...rest) wontthrow -> decltype(auto)
{
  return t__last_dump_argument(rest...);
}

template <class StringT, class... Args>
fn t__strprintf(t__dump_state<StringT> &state, const char *format,
                Args &&...args) throws -> void
{
  const int written = ::snprintf(nullptr, 0, format, args...);
  if (written < 0) return;

  const usize formatted_length = static_cast<usize>(written);
  let allocator = state.output.allocator();
  char *rendered = allocator.template alloc_array<char>(formatted_length + 1);
  defer { allocator.free_array(rendered, formatted_length + 1); };

  unused(::snprintf(rendered, formatted_length + 1, format, args...));

  usize clang_indent = 0;
  while (rendered[clang_indent] == ' ')
    clang_indent++;

  t__append_dump_text(state, rendered);

  if constexpr (sizeof...(Args) > 0) {
    let pointer = t__last_dump_argument(args...);
    using Last = decltype(pointer);

    if constexpr (t__is_dumpable_record_pointer<Last>()) {
      const bool is_pointer_value = ::strstr(format, "%p") != nullptr;
      const bool is_unknown_field = ::strstr(format, "*%p") != nullptr;

      if (is_pointer_value && !is_unknown_field && pointer != nullptr) {
        if (!state.is_at_line_start) t__append_dump_text(state, "\n");

        const usize previous_indent = state.base_indent;
        state.base_indent += clang_indent + 2;
        __builtin_dump_struct(pointer, t__strprintf<StringT>, state);
        state.base_indent = previous_indent;
      }
    }
  }
}

template <class StringT, class T, class AllocatorT>
fn t__string_from_struct(const T &value, AllocatorT allocator) throws -> StringT
{
  let state = t__dump_state<StringT>{StringT{allocator}, 0, true};
  __builtin_dump_struct(&value, t__strprintf<StringT>, state);
  return static_cast<StringT &&>(state.output);
}

} // namespace koshka

#define STRUCT_STRING(x)                                                       \
  ::koshka::t__string_from_struct<::koshka::String>(                           \
      x, ::koshka::heap_allocator())
#endif
#else /* !NDEBUG */
#define STRUCT_STRING(...) ::koshka::String{"<optimized out>"}
#define TRACE(...)         /* None */
#define TRACELN(...)       /* None */
#endif

#if !defined STRUCT_STRING
#define STRUCT_STRING(...) ::koshka::String{"<not supported>"}
#endif

#define t__va_are_empty(...) (sizeof((char[]) {#__VA_ARGS__}) == 1)

#define VA_ARE_EMPTY(...) t__va_are_empty(__VA_ARGS__)

#if !defined NDEBUG
#define TRAP(...)                                                              \
  do {                                                                         \
    TRACELN("Encountered a debug trap");                                       \
    if (!VA_ARE_EMPTY(__VA_ARGS__)) {                                          \
      TRACELN("Details: " __VA_ARGS__);                                        \
    }                                                                          \
    t__debugtrap();                                                            \
  } while (0)
#else
#define TRAP(...) abort()
#endif

#if !defined NDEBUG
#define unreachable(...)                                                       \
  do {                                                                         \
    TRACELN("Reached an unreachable statement");                               \
    if (!VA_ARE_EMPTY(__VA_ARGS__)) {                                          \
      TRACELN("Details: " __VA_ARGS__);                                        \
    }                                                                          \
    t__unreachable();                                                          \
  } while (0)
#else
#define unreachable(...) t__unreachable()
#endif

#if !defined NDEBUG
#define ASSERT(x, ...)                                                         \
  do {                                                                         \
    if (!(x)) [[unlikely]] {                                                   \
      TRACELN("'ASSERT(" #x ")' fail in %s().", __func__);                     \
      if (!VA_ARE_EMPTY(__VA_ARGS__)) {                                        \
        TRACELN("Details: " __VA_ARGS__);                                      \
      }                                                                        \
      TRAP();                                                                  \
    }                                                                          \
  } while (0)
#else
#define ASSERT(...) /* None */
#endif
