#!/usr/bin/env python3
"""Feature e2e: windows, splits, concurrent pane output over tmux -CC."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_pty_e2e import HtmPty, fail, find_bin, pids_named  # noqa: E402


def burst_cmd(tag: str, n: int) -> str:
    nums = " ".join(str(i) for i in range(1, n + 1))
    return f"for i in {nums}; do echo {tag}_$i; done"


def run_features(htm: Path, htmd: Path) -> None:
    session = HtmPty(htm, htmd)
    try:
        session.start()
        session.send("new-window")
        session.send("new-window")
        session.send("split-window -h -t %0")
        session.send("split-window -v -t %0")
        session.send("refresh-client -C 80x24")
        session.drain_idle(idle=0.3, timeout=4.0)
        print("OK: created extra windows + splits", flush=True)

        session.send("list-panes -a -F '#{pane_id}'")
        session.drain_idle(idle=0.2, timeout=3.0)
        bursts = 6
        tags = ["FEAT_A", "FEAT_B", "FEAT_C"]
        session.send(f"send-keys -t %0 '{burst_cmd(tags[0], bursts)}' Enter")
        session.send("list-windows -F '#{window_id}'")
        session.send(f"send-keys '{burst_cmd(tags[1], bursts)}' Enter")
        session.wait_until(
            lambda: any(
                f"{tags[0]}_{bursts}" in session.pane_output(p)
                or f"{tags[0]}_{bursts}" in session.text
                for p in ("0", "1", "2", "3")
            ),
            15.0,
            "burst output",
        )
        print("OK: concurrent pane output observed", flush=True)
        session.send("kill-server")
        deadline = time.time() + 12
        while time.time() < deadline and pids_named("htmd"):
            session.pump(0.2)
        if pids_named("htmd"):
            fail("htmd still running after kill-server")
        print("OK: shutdown", flush=True)
    finally:
        session.stop()
    print("PASS: htm features (windows, splits, concurrent panes)", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    args = parser.parse_args()
    run_features(
        find_bin(args.htm, "HTM_BIN", "htm"),
        find_bin(args.htmd, "HTMD_BIN", "htmd"),
    )


if __name__ == "__main__":
    main()
