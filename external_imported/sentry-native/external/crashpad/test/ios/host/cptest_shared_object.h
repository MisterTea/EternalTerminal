// Copyright 2020 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CRASHPAD_TEST_IOS_HOST_SHARED_OBJECT_H_
#define CRASHPAD_TEST_IOS_HOST_SHARED_OBJECT_H_

#import <UIKit/UIKit.h>

@interface CPTestSharedObject : NSObject
// Returns the string "crashpad" for testing EDO.
- (NSString*)testEDO;

// Triggers an EXC_BAD_ACCESS exception and crash.
- (void)crashBadAccess;

// Triggers a crash with a call to kill(SIGABRT).
- (void)crashKillAbort;

// Triggers a segfault crash.
- (void)crashSegv;

// Trigger a crash with a __builtin_trap.
- (void)crashTrap;

// Trigger a crash with an abort().
- (void)crashAbort;

// Trigger a crash with an uncaught exception.
- (void)crashException;

// Trigger a crash with an uncaught NSException.
- (void)crashNSException;

// Trigger an unrecognized selector after delay.
- (void)crashUnreocgnizedSelectorAfterDelay;

// Trigger a caught NSxception.
- (void)catchNSException;

// Trigger a crash with an infinite recursion.
- (void)crashRecursion;
@end

#endif  // CRASHPAD_TEST_IOS_HOST_SHARED_OBJECT_H_
