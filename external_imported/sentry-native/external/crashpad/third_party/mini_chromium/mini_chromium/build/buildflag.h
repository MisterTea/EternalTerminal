// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BUILD_BUILDFLAG_H_
#define BUILD_BUILDFLAG_H_

#define BUILDFLAG_CAT_INDIRECT(a, b) a ## b
#define BUILDFLAG_CAT(a, b) BUILDFLAG_CAT_INDIRECT(a, b)

#define BUILDFLAG(flag) (BUILDFLAG_CAT(BUILDFLAG_INTERNAL_, flag)())

#endif  // BUILD_BUILDFLAG_H_
