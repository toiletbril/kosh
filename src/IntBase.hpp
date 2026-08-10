#pragma once

#include "Common.hpp"

namespace koshka {

enum class int_base : u8
{
  binary = 2,
  octal = 8,
  decimal = 10,
  hex = 16
};

template <int_base TagBase, class T>
struct tagged_int
{
  using underlying = T;
  static constexpr int_base base = TagBase;

  T value{0};

  constexpr tagged_int() wontthrow = default;
  constexpr tagged_int(T initial) wontthrow : value(initial) {}
  constexpr operator T() const wontthrow { return value; }
};

template <class T>
struct is_tagged_int : std::false_type
{};
template <int_base TagBase, class T>
struct is_tagged_int<tagged_int<TagBase, T>> : std::true_type
{};
template <class T>
inline constexpr bool is_tagged_int_v = is_tagged_int<T>::value;

using bi16 = tagged_int<int_base::binary, i16>;
using bi32 = tagged_int<int_base::binary, i32>;
using bi64 = tagged_int<int_base::binary, i64>;
using bu16 = tagged_int<int_base::binary, u16>;
using bu32 = tagged_int<int_base::binary, u32>;
using bu64 = tagged_int<int_base::binary, u64>;

using oi16 = tagged_int<int_base::octal, i16>;
using oi32 = tagged_int<int_base::octal, i32>;
using oi64 = tagged_int<int_base::octal, i64>;
using ou16 = tagged_int<int_base::octal, u16>;
using ou32 = tagged_int<int_base::octal, u32>;
using ou64 = tagged_int<int_base::octal, u64>;

using hi16 = tagged_int<int_base::hex, i16>;
using hi32 = tagged_int<int_base::hex, i32>;
using hi64 = tagged_int<int_base::hex, i64>;
using hu16 = tagged_int<int_base::hex, u16>;
using hu32 = tagged_int<int_base::hex, u32>;
using hu64 = tagged_int<int_base::hex, u64>;

} // namespace koshka
