#!/usr/bin/env python

# Copyright 2014 The Crashpad Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import sys


def ChooseDependencyPath(local_path, external_path):
    """Chooses between a dependency located at local path and an external path.

    The local path, used in standalone builds, is preferred. If it is not
    present but the external path is, the external path will be used. If neither
    path is present, the local path will be used, so that error messages
    uniformly refer to the local path.

    Args:
        local_path: The preferred local path to use for a standalone build.
        external_path: The external path to fall back to.

    Returns:
        A 2-tuple. The first element is None or 'external', depending on whether
        local_path or external_path was chosen. The second element is the chosen
        path.
    """
    if os.path.exists(local_path) or not os.path.exists(external_path):
        return (None, local_path)
    return ('external', external_path)


script_dir = os.path.dirname(__file__)
crashpad_dir = (os.path.dirname(script_dir)
                if script_dir not in ('', os.curdir) else os.pardir)

sys.path.insert(
    0,
    ChooseDependencyPath(
        os.path.join(crashpad_dir, 'third_party', 'gyp', 'gyp', 'pylib'),
        os.path.join(crashpad_dir, os.pardir, os.pardir, 'gyp', 'pylib'))[1])

import gyp


def main(args):
    if 'GYP_GENERATORS' not in os.environ:
        os.environ['GYP_GENERATORS'] = 'ninja'

    crashpad_dir_or_dot = crashpad_dir if crashpad_dir is not '' else os.curdir

    (dependencies, mini_chromium_common_gypi) = (ChooseDependencyPath(
        os.path.join(crashpad_dir, 'third_party', 'mini_chromium',
                     'mini_chromium', 'build', 'common.gypi'),
        os.path.join(crashpad_dir, os.pardir, os.pardir, 'mini_chromium',
                     'mini_chromium', 'build', 'common.gypi')))
    if dependencies is not None:
        args.extend(['-D', 'crashpad_dependencies=%s' % dependencies])
    args.extend(['--include', mini_chromium_common_gypi])
    args.extend(['--depth', crashpad_dir_or_dot])
    args.append(os.path.join(crashpad_dir, 'crashpad.gyp'))

    result = gyp.main(args)
    if result != 0:
        return result

    if sys.platform == 'win32':
        # Check to make sure that no target_arch was specified. target_arch may
        # be set during a cross build, such as a cross build for Android.
        has_target_arch = False
        for arg_index in range(0, len(args)):
            arg = args[arg_index]
            if (arg.startswith('-Dtarget_arch=') or
                (arg == '-D' and arg_index + 1 < len(args) and
                 args[arg_index + 1].startswith('target_arch='))):
                has_target_arch = True
                break

        if not has_target_arch:
            # Also generate the x86 build.
            result = gyp.main(args +
                              ['-D', 'target_arch=ia32', '-G', 'config=Debug'])
            if result != 0:
                return result
            result = gyp.main(
                args + ['-D', 'target_arch=ia32', '-G', 'config=Release'])

    return result


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
