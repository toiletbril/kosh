#pragma once

#include "Maybe.hpp"
#include "StringMap.hpp"
#include "StringView.hpp"

namespace koshka {

/* A set of byte-string keys over the StringMap open-addressing table. The value
   is None, so a set stores only keys. It owns a copy of every key it holds, so
   a view passed to add need not outlive it. */
class HashSet
{
public:
  explicit HashSet(Allocator allocator) : m_map(allocator) {}

  pure fn allocator() const wontthrow -> Allocator { return m_map.allocator(); }

  hot fn add(StringView key) throws -> bool
  {
    let const previous_count = m_map.count();
    m_map.set(key, Nothing{});
    return m_map.count() != previous_count;
  }

  cold fn remove(StringView key) throws -> void { m_map.erase(key); }

  hot mustuse pure fn contains(StringView key) const wontthrow -> bool
  {
    return m_map.find(key) != nullptr;
  }

  mustuse pure fn count() const wontthrow -> usize { return m_map.count(); }

  mustuse cold fn clone() const throws -> HashSet { return HashSet{*this}; }

  template <class Fn>
  fn for_each(Fn callback) const throws -> void
  {
    m_map.for_each(
        [&callback](StringView key, const Nothing &) { callback(key); });
  }

private:
  StringMap<Nothing> m_map;
};

} /* namespace koshka */
