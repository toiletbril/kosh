#pragma once

#include "Allocator.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Maybe.hpp"

namespace koshka {

template <class T>
class ArrayList
{
public:
  static_assert(std::is_nothrow_destructible_v<T>);

  explicit ArrayList(Allocator allocator) : m_allocator(allocator) {}

  ArrayList(std::initializer_list<T> elements) : ArrayList(heap_allocator())
  {
    reserve(elements.size());
    for (const T &element : elements)
      push(element);
  }

  cold ArrayList(const ArrayList &other) : ArrayList(other.m_allocator)
  {
    reserve(other.m_length);
    for (usize i = 0; i < other.m_length; i++) {
      new (&m_data[m_length]) T(other.m_data[i]);
      m_length++;
    }
  }

  mustuse cold fn clone() const throws -> ArrayList { return ArrayList{*this}; }

  ArrayList(ArrayList &&other) noexcept
      : m_allocator(other.m_allocator), m_data(other.m_data),
        m_length(other.m_length), m_capacity(other.m_capacity)
  {
    other.m_data = nullptr;
    other.m_length = 0;
    other.m_capacity = 0;
  }

  fn operator=(ArrayList &&other) wontthrow->ArrayList &
  {
    if (this != &other) {
      destroy_all();
      m_allocator = other.m_allocator;
      m_data = other.m_data;
      m_length = other.m_length;
      m_capacity = other.m_capacity;
      other.m_data = nullptr;
      other.m_length = 0;
      other.m_capacity = 0;
    }
    return *this;
  }
  cold fn operator=(const ArrayList &other) throws->ArrayList &
  {
    if (this != &other) {
      ArrayList copy{other};
      *this = steal(copy);
    }
    return *this;
  }

  ~ArrayList() { destroy_all(); }

  hot mustuse pure fn count() const wontthrow -> usize { return m_length; }
  mustuse pure fn capacity() const wontthrow -> usize { return m_capacity; }
  mustuse pure fn is_empty() const wontthrow -> bool { return m_length == 0; }
  hot mustuse pure fn operator[](usize i) wontthrow->T &
  {
    ASSERT(i < m_length, "array index is past the end");
    return m_data[i];
  }
  hot mustuse pure fn operator[](usize i) const wontthrow->const T &
  {
    ASSERT(i < m_length, "array index is past the end");
    return m_data[i];
  }

  hot flatten mustuse pure fn
  operator==(const ArrayList &other) const throws->bool
  {
    if (m_length != other.m_length) return false;
    if constexpr (std::is_trivially_copyable_v<T> &&
                  std::has_unique_object_representations_v<T> &&
                  !std::is_pointer_v<T>)
    {
      return m_length == 0 ||
             __builtin_memcmp(m_data, other.m_data, m_length * sizeof(T)) == 0;
    } else {
      for (usize i = 0; i < m_length; i++)
        if (!(m_data[i] == other.m_data[i])) return false;
      return true;
    }
  }
  hot flatten mustuse pure fn
  operator!=(const ArrayList &other) const throws->bool
  {
    return !(*this == other);
  }

  hot mustuse pure fn begin() wontthrow -> T * { return m_data; }
  hot mustuse pure fn end() wontthrow -> T *
  {
    return m_data == nullptr ? nullptr : m_data + m_length;
  }
  hot mustuse pure fn begin() const wontthrow -> const T * { return m_data; }
  hot mustuse pure fn end() const wontthrow -> const T *
  {
    return m_data == nullptr ? nullptr : m_data + m_length;
  }

  template <class Wanted>
  hot mustuse pure fn find(const Wanted &wanted) const throws -> Maybe<usize>
  {
    for (usize element_index = 0; element_index < m_length; element_index++)
      if (m_data[element_index] == wanted) return element_index;
    return None;
  }

  hot fn push(T value) throws -> void
  {
    if (m_length == m_capacity) [[unlikely]]
      reserve(m_length + 1);
    new (&m_data[m_length]) T(steal(value));
    m_length++;
  }

  pure fn allocator() const wontthrow -> Allocator { return m_allocator; }

  template <typename... Args>
  hot fn push_managed(Args &&...args) throws -> void
  {
    push(T{m_allocator, static_cast<Args &&>(args)...});
  }

  /* The caller guarantees the list is not empty. */
  fn pop_back() wontthrow -> void
  {
    ASSERT(m_length > 0, "pop_back on an empty list");
    m_length--;
    m_data[m_length].~T();
  }

  /* The caller guarantees index is in range. */
  fn remove(usize index) throws -> void
  {
    ASSERT(index < m_length, "remove past the end of the list");
    for (usize i = index; i + 1 < m_length; i++)
      m_data[i] = steal(m_data[i + 1]);
    m_length--;
    m_data[m_length].~T();
  }

  fn clear() wontthrow -> void
  {
    for (usize i = 0; i < m_length; i++)
      m_data[i].~T();
    m_length = 0;
  }

  /* Destroy every element and give the storage back, where clear keeps the
     capacity for the next fill. */
  fn release() wontthrow -> void { destroy_all(); }

  hot mustuse pure fn back() wontthrow -> T &
  {
    ASSERT(m_length > 0, "back() on an empty list");
    return m_data[m_length - 1];
  }
  hot mustuse pure fn back() const wontthrow -> const T &
  {
    ASSERT(m_length > 0, "back() on an empty list");
    return m_data[m_length - 1];
  }
  mustuse pure fn front() wontthrow -> T &
  {
    ASSERT(m_length > 0, "front() on an empty list");
    return m_data[0];
  }
  mustuse pure fn front() const wontthrow -> const T &
  {
    ASSERT(m_length > 0, "front() on an empty list");
    return m_data[0];
  }

  cold fn reserve(usize needed) throws -> void
  {
    if (needed <= m_capacity) return;
    constexpr usize INITIAL_ALLOCATION_BYTES = 256;
    constexpr usize MAXIMUM_INITIAL_ELEMENT_COUNT = 16;
    constexpr usize INITIAL_ELEMENT_COUNT =
        sizeof(T) >= INITIAL_ALLOCATION_BYTES
            ? 1
            : (INITIAL_ALLOCATION_BYTES / sizeof(T) <
                       MAXIMUM_INITIAL_ELEMENT_COUNT
                   ? INITIAL_ALLOCATION_BYTES / sizeof(T)
                   : MAXIMUM_INITIAL_ELEMENT_COUNT);
    usize new_capacity = INITIAL_ELEMENT_COUNT;
    if (m_capacity != 0) {
      let const growth =
          m_capacity < 64 ? static_cast<usize>(4) : static_cast<usize>(2);
      new_capacity =
          m_capacity > SIZE_MAX / growth ? needed : m_capacity * growth;
    }
    while (new_capacity < needed) {
      if (new_capacity > SIZE_MAX / 2) {
        new_capacity = needed;
        break;
      }
      new_capacity *= 2;
    }
    let const fresh = m_allocator.alloc_array<T>(new_capacity);
    try {
      relocate_to(fresh);
    } catch (...) {
      m_allocator.free_array(fresh, new_capacity);
      throw;
    }
    if (m_data != nullptr) m_allocator.free_array(m_data, m_capacity);
    m_data = fresh;
    m_capacity = new_capacity;
  }

  /* The exact-size move to another allocator. A list is built on the heap where
     growth is cheap and is then parked in an arena without its growth slack. */
  cold fn move_to_allocator(Allocator allocator) throws -> void
  {
    if (m_length == 0) {
      if (m_data != nullptr) m_allocator.free_array(m_data, m_capacity);
      m_data = nullptr;
      m_capacity = 0;
      m_allocator = allocator;
      return;
    }

    let const fresh = allocator.alloc_array<T>(m_length);
    try {
      relocate_to(fresh);
    } catch (...) {
      allocator.free_array(fresh, m_length);
      throw;
    }

    m_allocator.free_array(m_data, m_capacity);
    m_allocator = allocator;
    m_data = fresh;
    m_capacity = m_length;
  }

  cold fn shrink_to_fit() throws -> void
  {
    if (m_length == m_capacity) return;
    if (m_length == 0) {
      if (m_data != nullptr) m_allocator.free_array(m_data, m_capacity);
      m_data = nullptr;
      m_capacity = 0;
      return;
    }
    let const fresh = m_allocator.alloc_array<T>(m_length);
    try {
      relocate_to(fresh);
    } catch (...) {
      m_allocator.free_array(fresh, m_length);
      throw;
    }
    m_allocator.free_array(m_data, m_capacity);
    m_data = fresh;
    m_capacity = m_length;
  }

  template <typename Compare>
  fn sort(Compare is_less) throws -> void
  {
    if (m_length < 2) return;

    if (m_length <= INSERTION_SORT_THRESHOLD)
      insertion_sort(is_less);
    else
      heap_sort(is_less);
  }

  fn sort() throws -> void
  {
    sort([](const T &a, const T &b) { return a < b; });
  }

private:
  /* A default list is heap-backed and empty, so it can serve as the value a
     StringMap slot holds before a real list is placed into it. The friend keeps
     it reachable to the table while every call site must name its lifetime. */
  template <class Value>
  friend class StringMap;
  ArrayList() : m_allocator(fake_allocator()) {}

  static constexpr usize INSERTION_SORT_THRESHOLD = 16;

  /* A trivially copyable type relocates as one memcpy, skipping the per-element
     move constructor and destructor the compiler would otherwise emit. */
  fn relocate_to(T *fresh) throws -> void
  {
    if constexpr (std::is_trivially_copyable_v<T>) {
      if (m_length > 0) __builtin_memcpy(fresh, m_data, m_length * sizeof(T));
      return;
    }
    usize constructed_count = 0;
    try {
      for (; constructed_count < m_length; constructed_count++) {
        if constexpr (std::is_nothrow_move_constructible_v<T> ||
                      !std::is_copy_constructible_v<T>)
          new (&fresh[constructed_count]) T(steal(m_data[constructed_count]));
        else
          new (&fresh[constructed_count]) T(m_data[constructed_count]);
      }
    } catch (...) {
      for (usize i = 0; i < constructed_count; i++)
        fresh[i].~T();
      throw;
    }

    for (usize i = 0; i < m_length; i++)
      m_data[i].~T();
  }

  fn swap_elements(usize a, usize b) throws -> void
  {
    T temporary = steal(m_data[a]);
    m_data[a] = steal(m_data[b]);
    m_data[b] = steal(temporary);
  }

  template <typename Compare>
  fn insertion_sort(Compare is_less) throws -> void
  {
    for (usize i = 1; i < m_length; i++) {
      T key = steal(m_data[i]);
      usize j = i;
      while (j > 0 && is_less(key, m_data[j - 1])) {
        m_data[j] = steal(m_data[j - 1]);
        j--;
      }
      m_data[j] = steal(key);
    }
  }

  /* Restore the max-heap property at root within the first heap_length
     elements, sinking the root past any larger child. */
  template <typename Compare>
  fn sift_down(usize root, usize heap_length, Compare is_less) throws -> void
  {
    loop
    {
      usize largest = root;
      let const left = 2 * root + 1;
      let const right = 2 * root + 2;

      if (left < heap_length && is_less(m_data[largest], m_data[left])) {
        largest = left;
      }
      if (right < heap_length && is_less(m_data[largest], m_data[right])) {
        largest = right;
      }

      if (largest == root) break;

      swap_elements(root, largest);
      root = largest;
    }
  }

  template <typename Compare>
  fn heap_sort(Compare is_less) throws -> void
  {
    for (usize parent = m_length / 2; parent > 0; parent--)
      sift_down(parent - 1, m_length, is_less);

    for (usize end = m_length; end > 1; end--) {
      swap_elements(0, end - 1);
      sift_down(0, end - 1, is_less);
    }
  }

  fn destroy_all() wontthrow -> void
  {
    for (usize i = 0; i < m_length; i++)
      m_data[i].~T();
    if (m_data != nullptr) m_allocator.free_array(m_data, m_capacity);
    m_data = nullptr;
    m_length = 0;
    m_capacity = 0;
  }

  Allocator m_allocator;
  T *m_data{nullptr};
  usize m_length{0};
  usize m_capacity{0};
};

/* A list that is empty on almost every instance it is a member of. An empty one
   is one pointer, and the forty-byte list is allocated only when a fill carries
   elements. The read interface matches ArrayList, so a member can be swapped
   over without touching its readers. */
template <class T>
class SparseList
{
public:
  SparseList() = default;
  ~SparseList() { release(); }

  SparseList(const SparseList &) = delete;
  SparseList &operator=(const SparseList &) = delete;

  SparseList(SparseList &&other) noexcept : m_list(other.m_list)
  {
    other.m_list = nullptr;
  }

  fn operator=(SparseList &&other) wontthrow->SparseList &
  {
    if (this != &other) {
      release();
      m_list = other.m_list;
      other.m_list = nullptr;
    }
    return *this;
  }

  /* An empty fill leaves the instance at one null pointer. */
  fn fill(ArrayList<T> &&filled) throws -> void
  {
    if (filled.is_empty()) {
      release();
      return;
    }

    if (m_list == nullptr) {
      let const block = heap_allocator().alloc_array<ArrayList<T>>(1);
      m_list = new (block) ArrayList<T>{heap_allocator()};
    }

    *m_list = steal(filled);
    m_list->shrink_to_fit();
  }

  hot mustuse pure fn is_empty() const wontthrow -> bool
  {
    return m_list == nullptr;
  }
  hot mustuse pure fn count() const wontthrow -> usize
  {
    return m_list == nullptr ? 0 : m_list->count();
  }
  hot mustuse pure fn operator[](usize i) const wontthrow->const T &
  {
    ASSERT(m_list != nullptr, "array index is past the end");
    return (*m_list)[i];
  }

  hot mustuse pure fn begin() const wontthrow -> const T *
  {
    return m_list == nullptr ? nullptr : m_list->begin();
  }
  hot mustuse pure fn end() const wontthrow -> const T *
  {
    return m_list == nullptr ? nullptr : m_list->end();
  }

private:
  fn release() wontthrow -> void
  {
    if (m_list == nullptr) return;

    m_list->~ArrayList<T>();
    heap_allocator().free_array(m_list, 1);
    m_list = nullptr;
  }

  ArrayList<T> *m_list{nullptr};
};

} /* namespace koshka */
