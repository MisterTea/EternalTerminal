// Copyright 2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/apple/foundation_util.h"

#include "base/check.h"

namespace base {
namespace apple {

#define CF_CAST_DEFN(TypeCF)                                       \
  template <>                                                      \
  TypeCF##Ref CFCast<TypeCF##Ref>(const CFTypeRef& cf_val) {       \
    if (cf_val == NULL) {                                          \
      return NULL;                                                 \
    }                                                              \
    if (CFGetTypeID(cf_val) == TypeCF##GetTypeID()) {              \
      return (TypeCF##Ref)(cf_val);                                \
    }                                                              \
    return NULL;                                                   \
  }                                                                \
                                                                   \
  template <>                                                      \
  TypeCF##Ref CFCastStrict<TypeCF##Ref>(const CFTypeRef& cf_val) { \
    TypeCF##Ref rv = CFCast<TypeCF##Ref>(cf_val);                  \
    DCHECK(cf_val == NULL || rv);                                  \
    return rv;                                                     \
  }

CF_CAST_DEFN(CFArray)
CF_CAST_DEFN(CFBag)
CF_CAST_DEFN(CFBoolean)
CF_CAST_DEFN(CFData)
CF_CAST_DEFN(CFDate)
CF_CAST_DEFN(CFDictionary)
CF_CAST_DEFN(CFNull)
CF_CAST_DEFN(CFNumber)
CF_CAST_DEFN(CFSet)
CF_CAST_DEFN(CFString)
CF_CAST_DEFN(CFURL)
CF_CAST_DEFN(CFUUID)

CF_CAST_DEFN(CGColor)

CF_CAST_DEFN(CTFont)
CF_CAST_DEFN(CTRun)

#undef CF_CAST_DEFN

}  // namespace apple
}  // namespace base
