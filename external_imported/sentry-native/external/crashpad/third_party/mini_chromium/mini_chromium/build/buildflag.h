// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BUILD_BUILDFLAG_H_
#define BUILD_BUILDFLAG_H_

#define MINI_CHROMIUM_INTERNAL_BUILDFLAG_CAT_INDIRECT(a, b) a##b
#define MINI_CHROMIUM_INTERNAL_BUILDFLAG_CAT(a, b) \
  MINI_CHROMIUM_INTERNAL_BUILDFLAG_CAT_INDIRECT(a, b)

#define BUILDFLAG(flag)                  \
  (MINI_CHROMIUM_INTERNAL_BUILDFLAG_CAT( \
      MINI_CHROMIUM_INTERNAL_BUILDFLAG_VALUE_, flag)())

#endif  // BUILD_BUILDFLAG_H_
