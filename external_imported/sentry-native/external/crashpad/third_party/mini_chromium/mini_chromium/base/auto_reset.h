// Copyright 2009 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_AUTO_RESET_H_
#define MINI_CHROMIUM_BASE_AUTO_RESET_H_

namespace base {

template<typename T>
class AutoReset {
 public:
  AutoReset(T* scoped_variable, T new_value)
      : scoped_variable_(scoped_variable),
        original_value_(*scoped_variable) {
    *scoped_variable_ = new_value;
  }

  AutoReset(const AutoReset&) = delete;
  AutoReset& operator=(const AutoReset&) = delete;

  ~AutoReset() { *scoped_variable_ = original_value_; }

 private:
  T* scoped_variable_;
  T original_value_;
};

}  // namespace base

#endif  // MINI_CHROMIUM_BASE_AUTO_RESET_H_
