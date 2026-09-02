#!/usr/bin/env python3
"""Stress e2e: many windows/splits with concurrent bulk I/O over tmux -CC."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_pty_e2e import HtmPty, fail, find_bin, pids_named  # noqa: E402


def run_stress(htm: Path, htmd: Path) -> None:
    session = HtmPty(htm, htmd)
    try:
        session.start()
        for _ in range(4):
            session.send("new-window")
            session.send("split-window -h")
            session.send("split-window -v")
        session.send("refresh-client -C 80x24")
        session.drain_idle(idle=0.3, timeout=5.0)
        print("OK: created many windows/splits", flush=True)

        for i in range(8):
            tag = f"STRESS{i}"
            session.send(
                f"send-keys -t %{i} i=1 Space while Space [ Space \\\"$i\\\" Space -le Space 20 Space ] Space "
                f"do Space printf Space {tag}_%s\\\\n Space \\\"$i\\\"\\; Space i=$((i+1))\\; Space done Enter"
            )
        session.drain_idle(idle=0.5, timeout=12.0)
        if session.proc is not None and session.proc.poll() is not None:
            fail("htm died under stress")
        if not pids_named("htmd"):
            fail("htmd died under stress")
        print("OK: stayed alive under concurrent output", flush=True)
        session.send("kill-server")
        deadline = time.time() + 12
        while time.time() < deadline and pids_named("htmd"):
            session.pump(0.2)
        print("OK: shutdown", flush=True)
    finally:
        session.stop()
    print("PASS: htm stress", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    args = parser.parse_args()
    run_stress(
        find_bin(args.htm, "HTM_BIN", "htm"),
        find_bin(args.htmd, "HTMD_BIN", "htmd"),
    )


if __name__ == "__main__":
    main()
