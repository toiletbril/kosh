#pragma once

#include "Common.hpp"
#include "Debug.hpp"

namespace koshka {

class Nothing
{};

inline constexpr Nothing None{};

/* A value or None. There is no failure reason inside, so the caller handles the
   empty case itself. Reading the value out of an empty Maybe traps in the debug
   build. The value is stored inline, so no allocation happens. */
template <class T>
class mustuse Maybe
{
public:
  static_assert(std::is_nothrow_destructible_v<T>);

  Maybe() noexcept : m_has_value(false), m_storage{} {}
  Maybe(Nothing) noexcept : m_has_value(false), m_storage{} {}
  Maybe(T value) : m_has_value(true) { new (&m_storage) T(steal(value)); }

  Maybe(const Maybe &other) : m_has_value(other.m_has_value)
  {
    if (m_has_value) new (&m_storage) T(other.reference());
  }
  Maybe(Maybe &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
      : m_has_value(other.m_has_value)
  {
    if (m_has_value) new (&m_storage) T(steal(other.reference()));
  }

  mustuse fn clone() const throws -> Maybe { return Maybe{*this}; }

  fn operator=(const Maybe &other) throws->Maybe &
  {
    if (this != &other) {
      reset();
      if (other.m_has_value) {
        new (&m_storage) T(other.reference());
        m_has_value = true;
      }
    }
    return *this;
  }
  fn operator=(Maybe &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
      ->Maybe &
  {
    if (this != &other) {
      reset();
      if (other.m_has_value) {
        new (&m_storage) T(steal(other.reference()));
        m_has_value = true;
      }
    }
    return *this;
  }

  ~Maybe() { reset(); }

  hot mustuse pure fn has_value() const wontthrow -> bool
  {
    return m_has_value;
  }
  hot mustuse pure explicit operator bool() const wontthrow
  {
    return m_has_value;
  }

  hot mustuse pure fn value() wontthrow -> T &
  {
    ASSERT(m_has_value);
    return reference();
  }
  hot mustuse pure fn value() const wontthrow -> const T &
  {
    ASSERT(m_has_value);
    return reference();
  }
  hot flatten mustuse pure fn operator*() wontthrow->T & { return value(); }
  hot flatten mustuse pure fn operator*() const wontthrow->const T &
  {
    return value();
  }
  hot flatten mustuse pure fn operator->() wontthrow->T * { return &value(); }
  hot flatten mustuse pure fn operator->() const wontthrow->const T *
  {
    return &value();
  }

  mustuse fn take() throws -> T
  {
    ASSERT(m_has_value);
    let taken_value = steal(reference());
    reset();
    return taken_value;
  }

  mustuse fn value_or(T fallback) const throws -> T
  {
    return m_has_value ? value() : steal(fallback);
  }

  mustuse fn operator==(const T &other) const throws->bool
  {
    return m_has_value && reference() == other;
  }
  mustuse fn operator!=(const T &other) const throws->bool
  {
    return !(*this == other);
  }

  fn reset() wontthrow -> void
  {
    if (m_has_value) {
      reference().~T();
      m_has_value = false;
    }
  }

private:
  fn reference() wontthrow -> T & { return *reinterpret_cast<T *>(&m_storage); }
  fn reference() const wontthrow -> const T &
  {
    return *reinterpret_cast<const T *>(&m_storage);
  }

  bool m_has_value;
  alignas(T) unsigned char m_storage[sizeof(T)];
};

/* Evaluate a Maybe expression, return None from the enclosing function when
   it is empty, otherwise yield the value. The enclosing function must itself
   return a Maybe. */
#define UNWRAP(maybe_expr)                                                     \
  ({                                                                           \
    auto t__result = (maybe_expr);                                             \
    if (!t__result) return ::koshka::None;                                     \
    t__result.take();                                                          \
  })

} /* namespace koshka */
