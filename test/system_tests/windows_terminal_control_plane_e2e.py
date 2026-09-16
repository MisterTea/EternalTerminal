#!/usr/bin/env python3
"""Verify Windows Terminal's iTerm2-compatible tmux control gateway."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import windows_terminal_htm_e2e  # noqa: E402
from htm_gui_e2e import run_emulator_main  # noqa: E402


def main() -> int:
    return run_emulator_main(windows_terminal_htm_e2e, default_suite="control-plane")


if __name__ == "__main__":
    raise SystemExit(main())
