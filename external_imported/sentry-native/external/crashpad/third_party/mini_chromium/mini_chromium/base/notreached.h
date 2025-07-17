// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_NOTREACHED_H_
#define MINI_CHROMIUM_BASE_NOTREACHED_H_

#include "base/check.h"

// TODO(crbug.com/40580068): Redefine NOTREACHED() to be [[noreturn]] once
// Crashpad and Chromium have migrated off of the non-noreturn version. This is
// easiest done by defining it as std::abort() as Crashpad currently doesn't
// stream arguments to it. For a more complete implementation we should use
// LOG(FATAL) but that is currently not annotated as [[noreturn]] because
// ~LogMessage is not. See TODO in base/logging.h
#define NOTREACHED() DCHECK(false)

// TODO(crbug.com/40580068): Remove this once the NotReachedIsFatal experiment
// has been rolled out in Chromium.
#define NOTREACHED_IN_MIGRATION() DCHECK(false)

#endif  // MINI_CHROMIUM_BASE_NOTREACHED_H_
