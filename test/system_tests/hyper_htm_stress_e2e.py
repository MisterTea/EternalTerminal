#!/usr/bin/env python3
"""Hyper GUI stress: shared bulk-I/O suite against the Hyper driver."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hyper_htm_e2e  # noqa: E402
from htm_gui_e2e import run_emulator_main  # noqa: E402


def main() -> int:
    return run_emulator_main(hyper_htm_e2e, default_suite="stress")


if __name__ == "__main__":
    raise SystemExit(main())
