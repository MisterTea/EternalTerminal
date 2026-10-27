#!/usr/bin/env python3

#              Copyright Catch2 Authors
# Distributed under the Boost Software License, Version 1.0.
#   (See accompanying file LICENSE.txt or copy at
#        https://www.boost.org/LICENSE_1_0.txt)

# SPDX-License-Identifier: BSL-1.0

import os
import sys
import subprocess

def generate_path_suffix() -> str:
    return os.urandom(16).hex()[:16]


def run_common(cmd, env = None):
    cmd_env = env if env is not None else os.environ.copy()
    print('Running:', cmd)

    try:
        ret = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
            universal_newlines=True,
            env=cmd_env
        )
    except subprocess.SubprocessError as ex:
        print('Could not run "{}"'.format(cmd))
        print("Return code: {}".format(ex.returncode))
        print("stdout: {}".format(ex.stdout))
        print("stderr: {}".format(ex.stderr))
        raise


def test_bazel_env_vars(bin_path, guard_path):
    env = os.environ.copy()
    env["TEST_PREMATURE_EXIT_FILE"] = guard_path
    env["BAZEL_TEST"] = '1'
    run_common([bin_path], env)

def test_cli_parameter(bin_path, guard_path):
    cmd = [
      bin_path,
      '--premature-exit-guard-file',
      guard_path
    ]
    run_common(cmd)

def test_no_crash(bin_path, guard_path):
    cmd = [
      bin_path,
      '--premature-exit-guard-file',
      guard_path,
      # Disable the quick-exit test
      '~[quick_exit]'
    ]
    run_common(cmd)

checks = {
  'bazel': (test_bazel_env_vars, True),
  'cli': (test_cli_parameter, True),
  'no-crash': (test_no_crash, False),
}


bin_path = os.path.abspath(sys.argv[1])
output_dir = os.path.abspath(sys.argv[2])
test_kind = sys.argv[3]
guard_file_path = os.path.join(output_dir, f"guard_file.{generate_path_suffix()}")
print(f'Guard file path: "{guard_file_path}"')

check_func, file_should_exist = checks[test_kind]
check_func(bin_path, guard_file_path)

assert os.path.exists(guard_file_path) == file_should_exist
# With randomly generated file suffix, we should not run into name conflicts.
# However, we try to cleanup anyway, to avoid having infinity files in
# long living build directories.
if file_should_exist:
    try:
        os.remove(guard_file_path)
    except Exception as ex:
        print(f'Could not remove file {guard_file_path} because: {ex}')

