// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_APPLE_SCOPED_TYPEREF_H_
#define MINI_CHROMIUM_BASE_APPLE_SCOPED_TYPEREF_H_

#include "base/check.h"
#include "base/memory/scoped_policy.h"

namespace base {
namespace apple {

template <typename T>
struct ScopedTypeRefTraits;

template <typename T, typename Traits = ScopedTypeRefTraits<T>>
class ScopedTypeRef {
 public:
  using element_type = T;

  // Construction from underlying type

  explicit constexpr ScopedTypeRef(
      element_type object = Traits::InvalidValue(),
      base::scoped_policy::OwnershipPolicy policy = base::scoped_policy::ASSUME)
      : object_(object) {
    if (object_ != Traits::InvalidValue() &&
        policy == base::scoped_policy::RETAIN) {
      object_ = Traits::Retain(object_);
    }
  }

  // Copy construction

  ScopedTypeRef(const ScopedTypeRef<T, Traits>& that) : object_(that.get()) {
    if (object_ != Traits::InvalidValue()) {
      object_ = Traits::Retain(object_);
    }
  }

  template <typename R, typename RTraits>
  ScopedTypeRef(const ScopedTypeRef<R, RTraits>& that) : object_(that.get()) {
    if (object_ != Traits::InvalidValue()) {
      object_ = Traits::Retain(object_);
    }
  }

  // Copy assignment

  ScopedTypeRef& operator=(const ScopedTypeRef<T, Traits>& that) {
    reset(that.get(), base::scoped_policy::RETAIN);
    return *this;
  }

  template <typename R, typename RTraits>
  ScopedTypeRef& operator=(const ScopedTypeRef<R, RTraits>& that) {
    reset(that.get(), base::scoped_policy::RETAIN);
    return *this;
  }

  // Move construction

  ScopedTypeRef(ScopedTypeRef<T, Traits>&& that) : object_(that.release()) {}

  template <typename R, typename RTraits>
  ScopedTypeRef(ScopedTypeRef<R, RTraits>&& that) : object_(that.release()) {}

  // Move assignment

  ScopedTypeRef& operator=(ScopedTypeRef<T, Traits>&& that) {
    reset(that.release(), base::scoped_policy::ASSUME);
    return *this;
  }

  template <typename R, typename RTraits>
  ScopedTypeRef& operator=(ScopedTypeRef<R, RTraits>&& that) {
    reset(that.release(), base::scoped_policy::ASSUME);
    return *this;
  }

  // Resetting

  template <typename R, typename RTraits>
  void reset(const ScopedTypeRef<R, RTraits>& that) {
    reset(that.get(), base::scoped_policy::RETAIN);
  }

  void reset(element_type object = Traits::InvalidValue(),
             base::scoped_policy::OwnershipPolicy policy =
                 base::scoped_policy::ASSUME) {
    if (object != Traits::InvalidValue() &&
        policy == base::scoped_policy::RETAIN) {
      object = Traits::Retain(object);
    }
    if (object_ != Traits::InvalidValue()) {
      Traits::Release(object_);
    }
    object_ = object;
  }

  // Destruction

  ~ScopedTypeRef() {
    if (object_ != Traits::InvalidValue()) {
      Traits::Release(object_);
    }
  }

  [[nodiscard]] element_type* InitializeInto() {
    CHECK_EQ(object_, Traits::InvalidValue());
    return &object_;
  }

  bool operator==(const ScopedTypeRef& that) const {
    return object_ == that.object_;
  }
  bool operator!=(const ScopedTypeRef& that) const {
    return object_ != that.object_;
  }
  explicit operator bool() const { return object_ != Traits::InvalidValue(); }

  element_type get() const { return object_; }

  void swap(ScopedTypeRef& that) {
    element_type temp = that.object_;
    that.object_ = object_;
    object_ = temp;
  }

  [[nodiscard]] element_type release() {
    element_type temp = object_;
    object_ = Traits::InvalidValue();
    return temp;
  }

 private:
  element_type object_;
};

}  // namespace apple
}  // namespace base

#endif  // MINI_CHROMIUM_BASE_APPLE_SCOPED_TYPEREF_H_
