// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_CONTAINERS_CHECKED_ITERATORS_H_
#define MINI_CHROMIUM_BASE_CONTAINERS_CHECKED_ITERATORS_H_

#include <concepts>
#include <iterator>
#include <memory>
#include <type_traits>

#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/util.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "build/build_config.h"

namespace base {

template <typename T>
class CheckedContiguousIterator {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = std::remove_cv_t<T>;
  using pointer = T*;
  using reference = T&;
  using iterator_category = std::contiguous_iterator_tag;
  using iterator_concept = std::contiguous_iterator_tag;

  // Required for converting constructor below.
  template <typename U>
  friend class CheckedContiguousIterator;

  // Required to be able to get to the underlying pointer without triggering
  // CHECK failures.
  template <typename Ptr>
  friend struct std::pointer_traits;

  constexpr CheckedContiguousIterator() = default;

  UNSAFE_BUFFER_USAGE constexpr CheckedContiguousIterator(T* start,
                                                          const T* end)
      : CheckedContiguousIterator(start, start, end) {}

  UNSAFE_BUFFER_USAGE constexpr CheckedContiguousIterator(const T* start,
                                                          T* current,
                                                          const T* end)
      : start_(start), current_(current), end_(end) {
    CHECK_LE(start, current);
    CHECK_LE(current, end);
  }

  constexpr CheckedContiguousIterator(const CheckedContiguousIterator& other) =
      default;

  // Converting constructor allowing conversions like CCI<T> to CCI<const T>,
  // but disallowing CCI<const T> to CCI<T> or CCI<Derived> to CCI<Base>, which
  // are unsafe. Furthermore, this is the same condition as used by the
  // converting constructors of std::span<T> and std::unique_ptr<T[]>.
  // See https://wg21.link/n4042 for details.
  template <typename U>
  constexpr CheckedContiguousIterator(const CheckedContiguousIterator<U>& other)
    requires(std::convertible_to<U (*)[], T (*)[]>)
      : start_(other.start_), current_(other.current_), end_(other.end_) {
    // We explicitly don't delegate to the 3-argument constructor here. Its
    // CHECKs would be redundant, since we expect |other| to maintain its own
    // invariant. However, DCHECKs never hurt anybody. Presumably.
    DCHECK_LE(other.start_, other.current_);
    DCHECK_LE(other.current_, other.end_);
  }

  ~CheckedContiguousIterator() = default;

  constexpr CheckedContiguousIterator& operator=(
      const CheckedContiguousIterator& other) = default;

  friend constexpr bool operator==(const CheckedContiguousIterator& lhs,
                                   const CheckedContiguousIterator& rhs) {
    lhs.CheckComparable(rhs);
    return lhs.current_ == rhs.current_;
  }

  friend constexpr auto operator<=>(const CheckedContiguousIterator& lhs,
                                    const CheckedContiguousIterator& rhs) {
    lhs.CheckComparable(rhs);
    return lhs.current_ <=> rhs.current_;
  }

  constexpr CheckedContiguousIterator& operator++() {
    CHECK_NE(current_, end_);
    ++current_;
    return *this;
  }

  constexpr CheckedContiguousIterator operator++(int) {
    CheckedContiguousIterator old = *this;
    ++*this;
    return old;
  }

  constexpr CheckedContiguousIterator& operator--() {
    CHECK_NE(current_, start_);
    --current_;
    return *this;
  }

  constexpr CheckedContiguousIterator operator--(int) {
    CheckedContiguousIterator old = *this;
    --*this;
    return old;
  }

  constexpr CheckedContiguousIterator& operator+=(difference_type rhs) {
    if (rhs > 0) {
      CHECK_LE(rhs, end_ - current_);
    } else {
      CHECK_LE(-rhs, current_ - start_);
    }
    current_ += rhs;
    return *this;
  }

  constexpr CheckedContiguousIterator operator+(difference_type rhs) const {
    CheckedContiguousIterator it = *this;
    it += rhs;
    return it;
  }

  constexpr friend CheckedContiguousIterator operator+(
      difference_type lhs,
      const CheckedContiguousIterator& rhs) {
    return rhs + lhs;
  }

  constexpr CheckedContiguousIterator& operator-=(difference_type rhs) {
    if (rhs < 0) {
      CHECK_LE(-rhs, end_ - current_);
    } else {
      CHECK_LE(rhs, current_ - start_);
    }
    current_ -= rhs;
    return *this;
  }

  constexpr CheckedContiguousIterator operator-(difference_type rhs) const {
    CheckedContiguousIterator it = *this;
    it -= rhs;
    return it;
  }

  constexpr friend difference_type operator-(
      const CheckedContiguousIterator& lhs,
      const CheckedContiguousIterator& rhs) {
    lhs.CheckComparable(rhs);
    return lhs.current_ - rhs.current_;
  }

  constexpr reference operator*() const {
    CHECK_NE(current_, end_);
    return *current_;
  }

  constexpr pointer operator->() const {
    CHECK_NE(current_, end_);
    return current_;
  }

  constexpr reference operator[](difference_type rhs) const {
    CHECK_GE(rhs, 0);
    CHECK_LT(rhs, end_ - current_);
    return current_[rhs];
  }

  [[nodiscard]] static bool IsRangeMoveSafe(
      const CheckedContiguousIterator& from_begin,
      const CheckedContiguousIterator& from_end,
      const CheckedContiguousIterator& to) {
    if (from_end < from_begin)
      return false;
    const auto from_begin_uintptr = get_uintptr(from_begin.current_);
    const auto from_end_uintptr = get_uintptr(from_end.current_);
    const auto to_begin_uintptr = get_uintptr(to.current_);
    const auto to_end_uintptr =
        get_uintptr((to + std::distance(from_begin, from_end)).current_);

    return to_begin_uintptr >= from_end_uintptr ||
           to_end_uintptr <= from_begin_uintptr;
  }

 private:
  constexpr void CheckComparable(const CheckedContiguousIterator& other) const {
    CHECK_EQ(start_, other.start_);
    CHECK_EQ(end_, other.end_);
  }

  // RAW_PTR_EXCLUSION: T can be a STACK_ALLOCATED class.
  RAW_PTR_EXCLUSION const T* start_ = nullptr;
  RAW_PTR_EXCLUSION T* current_ = nullptr;
  RAW_PTR_EXCLUSION const T* end_ = nullptr;
};

template <typename T>
using CheckedContiguousConstIterator = CheckedContiguousIterator<const T>;

}  // namespace base

// Specialize std::pointer_traits so that we can obtain the underlying raw
// pointer without resulting in CHECK failures. The important bit is the
// `to_address(pointer)` overload, which is the standard blessed way to
// customize `std::to_address(pointer)` in C++20 [1].
//
// [1] https://wg21.link/pointer.traits.optmem

template <typename T>
struct std::pointer_traits<::base::CheckedContiguousIterator<T>> {
  using pointer = ::base::CheckedContiguousIterator<T>;
  using element_type = T;
  using difference_type = ptrdiff_t;

  template <typename U>
  using rebind = ::base::CheckedContiguousIterator<U>;

  static constexpr pointer pointer_to(element_type& r) noexcept {
    return pointer(&r, &r);
  }

  static constexpr element_type* to_address(pointer p) noexcept {
    return p.current_;
  }
};

#endif  // MINI_CHROMIUM_BASE_CONTAINERS_CHECKED_ITERATORS_H_
