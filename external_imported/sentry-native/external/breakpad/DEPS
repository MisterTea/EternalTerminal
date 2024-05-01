# Copyright 2010 Google LLC
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google LLC nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# This is used to mimic the svn:externals mechanism for gclient (both Git and
# SVN) based checkouts of Breakpad. As such, its use is entirely optional. If
# using a manually managed SVN checkout as opposed to a gclient managed checkout
# you can still use the hooks mechanism for generating project files by calling
# 'gclient runhooks' rather than 'gclient sync'.

deps = {
  # Testing libraries and utilities.
  "src/src/testing":
    "https://github.com/google/googletest.git" +
      "@release-1.11.0",

  # Protobuf.
  "src/src/third_party/protobuf/protobuf":
    "https://github.com/google/protobuf.git" +
      "@cb6dd4ef5f82e41e06179dcd57d3b1d9246ad6ac",

  # Linux syscall support.
  "src/src/third_party/lss":
    "https://chromium.googlesource.com/linux-syscall-support/" +
      "@9719c1e1e676814c456b55f5f070eabad6709d31",
}

hooks = [
  {
    # Keep the manifest up to date.
    "action": ["src/src/tools/python/deps-to-manifest.py",
               "src/DEPS", "src/default.xml"],
  },
]
