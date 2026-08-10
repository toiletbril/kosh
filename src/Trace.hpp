#pragma once

#include "Common.hpp"
#include "Containers.hpp"

namespace koshka {

enum class verbosity : u8
{
  Nothing = 0,
  Info,
  Debug,
  All,
};

/* A message prints when its level is at or below this. */
inline verbosity LOGGER_VERBOSITY = verbosity::Nothing;

inline std::FILE *LOGGER_OUTPUT = nullptr;

inline fn log_output_stream() -> std::FILE *
{
  return LOGGER_OUTPUT != nullptr ? LOGGER_OUTPUT : stderr;
}

constexpr fn verbosity_to_string(verbosity verbosity) -> const char *
{
  switch (verbosity) {
  case verbosity::Nothing: return "OFF";
  case verbosity::Info: return "INF";
  case verbosity::Debug: return "DBG";
  case verbosity::All: return "ALL";
  }
  return "???";
}

namespace log_detail {

inline fn value_to_log_string(StringView value) -> String
{
  return String{value};
}

inline fn value_to_log_string(const char *value) -> String
{
  return value != nullptr ? String{value} : String{"(null)"};
}

inline fn value_to_log_string(bool value) -> String
{
  return value ? String{"true"} : String{"false"};
}

inline fn value_to_log_string(char value) -> String
{
  String out{heap_allocator()};
  out.push(value);
  return out;
}

template <class T>
  requires std::is_integral_v<T>
fn value_to_log_string(T value) -> String
{
  char buffer[32];
  if constexpr (std::is_signed_v<T>) {
    std::snprintf(buffer, sizeof(buffer), "%lld",
                  static_cast<long long>(value));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
  }
  return String{buffer};
}

template <class T>
  requires std::is_floating_point_v<T>
fn value_to_log_string(T value) -> String
{
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
  return String{buffer};
}

template <class T>
  requires std::is_pointer_v<T>
fn value_to_log_string(T value) -> String
{
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%p",
                static_cast<const opaque *>(value));
  return String{buffer};
}

template <class... Args>
fn format_named_values(StringView names, Args &&...args) -> String
{
  String out{heap_allocator()};
  usize index = 0;
  const usize value_count = sizeof...(Args);

  auto do_append_one = [&](auto &&value) {
    StringView name = names;
    Maybe<usize> comma_position = names.find_character(',');
    if (comma_position.has_value()) {
      name = names.substring_of_length(0, comma_position.value());
      names = names.substring(comma_position.value() + 1);
    } else {
      names = StringView{};
    }
    usize start = 0;
    while (start < name.length &&
           (name.data[start] == ' ' || name.data[start] == '\t'))
      start++;
    usize stop = name.length;
    while (stop > start &&
           (name.data[stop - 1] == ' ' || name.data[stop - 1] == '\t'))
      stop--;
    name = name.substring_of_length(start, stop - start);

    out.append(name);
    out.append(StringView{" = "});
    out.append(value_to_log_string(value).view());
    if (++index < value_count) out.append(StringView{", "});
  };

  (do_append_one(std::forward<Args>(args)), ...);
  return out;
}

} /* namespace log_detail */

} /* namespace koshka */

#define T__LOG_STRINGIZE2(x) #x
#define T__LOG_STRINGIZE(x)  T__LOG_STRINGIZE2(x)

#if defined NDEBUG

#define LOG(level, ...)                                                        \
  do {                                                                         \
  } while (0)
#define LOG_VARS(level, ...)                                                   \
  do {                                                                         \
  } while (0)

#else /* NDEBUG */

/* The level is named unqualified, such as Debug, and the macro prepends
   ::koshka::verbosity the way the FLAG macro prepends the section, so a call
   site spells neither the namespace nor the enum. */
#define LOG(level, ...)                                                        \
  do {                                                                         \
    constexpr ::koshka::verbosity t__log_level = ::koshka::verbosity::level;   \
    if (t__log_level <= ::koshka::LOGGER_VERBOSITY) [[unlikely]] {             \
      std::FILE *t__log_stream = ::koshka::log_output_stream();                \
      unused(std::fprintf(t__log_stream, "[%s] %32s %32s(): ",                 \
                          ::koshka::verbosity_to_string(t__log_level),         \
                          __FILE__ ":" T__LOG_STRINGIZE(__LINE__), __func__)); \
      unused(std::fprintf(t__log_stream, __VA_ARGS__));                        \
      unused(std::fputc('\n', t__log_stream));                                 \
      unused(std::fflush(t__log_stream));                                      \
    }                                                                          \
  } while (0)

#define LOG_VARS(level, ...)                                                   \
  do {                                                                         \
    constexpr ::koshka::verbosity t__log_level = ::koshka::verbosity::level;   \
    if (t__log_level <= ::koshka::LOGGER_VERBOSITY) [[unlikely]] {             \
      ::koshka::String t__vars = ::koshka::log_detail::format_named_values(    \
          #__VA_ARGS__, __VA_ARGS__);                                          \
      std::FILE *t__log_stream = ::koshka::log_output_stream();                \
      unused(std::fprintf(t__log_stream, "[%s] %32s %32s(): %s\n",             \
                          ::koshka::verbosity_to_string(t__log_level),         \
                          __FILE__ ":" T__LOG_STRINGIZE(__LINE__), __func__,   \
                          t__vars.c_str()));                                   \
      unused(std::fflush(t__log_stream));                                      \
    }                                                                          \
  } while (0)

#endif /* !NDEBUG */
