#!/usr/bin/env python3
"""et1 must exec et with hangup-close and a one-week disconnect timeout.

Flag strings alone are not enough: et must honor --disconnect-timeout for the
session it creates. That path is covered by TerminalServerLifecycle
("Client InitialPayload disconnect timeout closes the session").
"""

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class Et1ScriptTest(unittest.TestCase):
    def test_unix_wrapper_flags(self):
        text = (ROOT / "scripts" / "et1").read_text()
        self.assertIn("#!/bin/sh", text)
        self.assertIn(
            'exec et --close-on-hangup --disconnect-timeout=10080 "$@"',
            text,
        )

    def test_windows_wrapper_flags(self):
        text = (ROOT / "scripts" / "et1.cmd").read_text()
        self.assertIn(
            "et --close-on-hangup --disconnect-timeout=10080 %*",
            text,
        )

    def test_cmake_installs_et1_with_client_binaries(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("scripts/et1", cmake)
        self.assertIn("scripts/et1.cmd", cmake)
        self.assertGreaterEqual(cmake.count('DESTINATION "bin"'), 2)

    def test_et_client_declares_disconnect_timeout_option(self):
        """Catch the silent no-op where et1 passes a flag et never registers."""
        main = (ROOT / "src" / "terminal" / "TerminalClientMain.cpp").read_text()
        self.assertRegex(
            main,
            r'\(\s*"disconnect-timeout"\s*,',
            msg="et must register --disconnect-timeout (etserver-only is a bug)",
        )
        self.assertIn("disconnectTimeoutMinutes", main)
        client = (ROOT / "src" / "terminal" / "TerminalClient.cpp").read_text()
        self.assertIn("set_disconnect_timeout_seconds", client)
        proto = (ROOT / "proto" / "ETerminal.proto").read_text()
        self.assertIn("disconnect_timeout_seconds", proto)


if __name__ == "__main__":
    unittest.main()
