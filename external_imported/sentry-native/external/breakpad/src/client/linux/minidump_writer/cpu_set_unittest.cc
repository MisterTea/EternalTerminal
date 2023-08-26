// Copyright 2013 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "breakpad_googletest_includes.h"
#include "client/linux/minidump_writer/cpu_set.h"
#include "common/linux/scoped_tmpfile.h"

using namespace google_breakpad;

namespace {

typedef testing::Test CpuSetTest;

}

TEST(CpuSetTest, EmptyCount) {
  CpuSet set;
  ASSERT_EQ(0, set.GetCount());
}

TEST(CpuSetTest, OneCpu) {
  ScopedTmpFile file;
  ASSERT_TRUE(file.InitString("10"));

  CpuSet set;
  ASSERT_TRUE(set.ParseSysFile(file.GetFd()));
  ASSERT_EQ(1, set.GetCount());
}

TEST(CpuSetTest, OneCpuTerminated) {
  ScopedTmpFile file;
  ASSERT_TRUE(file.InitString("10\n"));

  CpuSet set;
  ASSERT_TRUE(set.ParseSysFile(file.GetFd()));
  ASSERT_EQ(1, set.GetCount());
}

TEST(CpuSetTest, TwoCpusWithComma) {
  ScopedTmpFile file;
  ASSERT_TRUE(file.InitString("1,10"));

  CpuSet set;
  ASSERT_TRUE(set.ParseSysFile(file.GetFd()));
  ASSERT_EQ(2, set.GetCount());
}

TEST(CpuSetTest, TwoCpusWithRange) {
  ScopedTmpFile file;
  ASSERT_TRUE(file.InitString("1-2"));

  CpuSet set;
  ASSERT_TRUE(set.ParseSysFile(file.GetFd()));
  ASSERT_EQ(2, set.GetCount());
}

TEST(CpuSetTest, TenCpusWithRange) {
  ScopedTmpFile file;
  ASSERT_TRUE(file.InitString("9-18"));

  CpuSet set;
  ASSERT_TRUE(set.ParseSysFile(file.GetFd()));
  ASSERT_EQ(10, set.GetCount());
}

TEST(CpuSetTest, MultiItems) {
  ScopedTmpFile file;
  ASSERT_TRUE(file.InitString("0, 2-4, 128"));

  CpuSet set;
  ASSERT_TRUE(set.ParseSysFile(file.GetFd()));
  ASSERT_EQ(5, set.GetCount());
}

TEST(CpuSetTest, IntersectWith) {
  ScopedTmpFile file1;
  ASSERT_TRUE(file1.InitString("9-19"));

  CpuSet set1;
  ASSERT_TRUE(set1.ParseSysFile(file1.GetFd()));
  ASSERT_EQ(11, set1.GetCount());

  ScopedTmpFile file2;
  ASSERT_TRUE(file2.InitString("16-24"));

  CpuSet set2;
  ASSERT_TRUE(set2.ParseSysFile(file2.GetFd()));
  ASSERT_EQ(9, set2.GetCount());

  set1.IntersectWith(set2);
  ASSERT_EQ(4, set1.GetCount());
  ASSERT_EQ(9, set2.GetCount());
}

TEST(CpuSetTest, SelfIntersection) {
  ScopedTmpFile file1;
  ASSERT_TRUE(file1.InitString("9-19"));

  CpuSet set1;
  ASSERT_TRUE(set1.ParseSysFile(file1.GetFd()));
  ASSERT_EQ(11, set1.GetCount());

  set1.IntersectWith(set1);
  ASSERT_EQ(11, set1.GetCount());
}

TEST(CpuSetTest, EmptyIntersection) {
  ScopedTmpFile file1;
  ASSERT_TRUE(file1.InitString("0-19"));

  CpuSet set1;
  ASSERT_TRUE(set1.ParseSysFile(file1.GetFd()));
  ASSERT_EQ(20, set1.GetCount());

  ScopedTmpFile file2;
  ASSERT_TRUE(file2.InitString("20-39"));

  CpuSet set2;
  ASSERT_TRUE(set2.ParseSysFile(file2.GetFd()));
  ASSERT_EQ(20, set2.GetCount());

  set1.IntersectWith(set2);
  ASSERT_EQ(0, set1.GetCount());

  ASSERT_EQ(20, set2.GetCount());
}

