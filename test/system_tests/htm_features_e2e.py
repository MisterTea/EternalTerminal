#!/usr/bin/env python3
"""Feature e2e for real ``htm`` / ``htmd``: tabs, splits, concurrent pane output.

Talks the HTM wire protocol over a PTY (the same framing iTerm2 uses after
``ESC[###q``). Verifies that:

  * NEW_SPLIT creates a second pane whose PTY produces APPEND_TO_PANE
  * NEW_TAB creates an independent pane on a new tab
  * Unique markers typed into several panes at once come back on the
    matching APPEND_TO_PANE streams, interleaved rather than serialized

Exit 77 if the binaries are missing. ``htm -x`` kills any existing htmd.
"""

from __future__ import annotations

import argparse
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_pty_e2e import (  # noqa: E402
    INSERT_DEBUG_KEYS,
    HtmPty,
    fail,
    find_bin,
    ipc_path,
    pids_named,
    skip,
)


def new_id() -> str:
    return str(uuid.uuid4())


def burst_cmd(tag: str, n: int) -> str:
    return (
        f"i=1; while [ \"$i\" -le {n} ]; do "
        f"printf '{tag}_%s\\n' \"$i\"; i=$((i+1)); done\n"
    )


def run_features(htm: Path, htmd: Path) -> None:
    session = HtmPty(htm, htmd)
    try:
        session.start()
        p0 = session.first_pane_id()
        print(f"OK: attached, initial pane {p0}", flush=True)

        pane_tab_a = new_id()
        session.new_tab(new_id(), pane_tab_a)
        pane_tab_b = new_id()
        session.new_tab(new_id(), pane_tab_b)
        split_v = new_id()
        session.new_split(p0, split_v, vertical=True)
        split_h = new_id()
        session.new_split(p0, split_h, vertical=False)
        for pane in (p0, pane_tab_a, pane_tab_b, split_v, split_h):
            session.resize(pane, 80, 24)
        session.drain_idle(idle=0.3, timeout=4.0)
        session.wait_until(
            lambda: all(session.pane_output(p) for p, _ in (
                (p0, ""),
                (pane_tab_a, ""),
                (pane_tab_b, ""),
                (split_v, ""),
                (split_h, ""),
            )),
            10.0,
            "shell output on every new pane",
        )
        print("OK: created 2 extra tabs + vertical split + horizontal split", flush=True)

        panes = [
            (p0, "FEAT_TAB0"),
            (pane_tab_a, "FEAT_TAB1"),
            (pane_tab_b, "FEAT_TAB2"),
            (split_v, "FEAT_SPLIT_V"),
            (split_h, "FEAT_SPLIT_H"),
        ]
        bursts = 8
        burst_at = len(session.packets)
        for pane, tag in panes:
            session.insert_keys(pane, burst_cmd(tag, bursts))

        def all_caught_up() -> bool:
            for pane, tag in panes:
                if f"{tag}_{bursts}" not in session.pane_output(pane):
                    return False
            return True

        session.wait_until(all_caught_up, 15.0, "concurrent APPEND_TO_PANE from every pane")

        for pane, tag in panes:
            out = session.pane_output(pane)
            if f"{tag}_1" not in out:
                fail(f"pane {pane} missing {tag}_1; got {out[-200:]!r}")
            for other_pane, other_tag in panes:
                if other_pane == pane:
                    continue
                if f"{other_tag}_" in out:
                    fail(
                        f"pane {pane} leaked output from {other_tag}: {out[-300:]!r}"
                    )
        print("OK: each pane received only its own burst markers", flush=True)

        order = session.append_pane_order(burst_at)
        unique = {pane for pane in order if pane in {p for p, _ in panes}}
        if len(unique) < 4:
            fail(f"expected APPEND from >=4 panes, got {sorted(unique)}")
        transitions = sum(1 for i in range(1, len(order)) if order[i] != order[i - 1])
        if transitions < 4:
            fail(
                f"pane output was not interleaved (transitions={transitions}, packets={len(order)})"
            )
        print(
            f"OK: concurrent output interleaved ({len(order)} APPEND packets, "
            f"{len(unique)} panes, {transitions} pane switches)",
            flush=True,
        )

        session.insert_keys(pane_tab_a, "printf 'FEAT_STILL_ALIVE\\n'\n")
        session.wait_until(
            lambda: "FEAT_STILL_ALIVE" in session.pane_output(pane_tab_a),
            8.0,
            "APPEND after concurrent burst",
        )
        print("OK: tab pane still accepts keys after concurrent burst", flush=True)

        session.write_packet(INSERT_DEBUG_KEYS, b"x")
        deadline = time.time() + 12
        while time.time() < deadline and pids_named("htmd"):
            session.pump(0.2)
        if pids_named("htmd"):
            fail("htmd still running after gateway x")
        if ipc_path().exists():
            time.sleep(0.5)
        if ipc_path().exists():
            fail("IPC socket still present after shutdown")
        print("OK: shutdown", flush=True)
    finally:
        session.stop()

    print("PASS: htm features (tabs, splits, concurrent panes)", flush=True)


def main() -> int:
    import os

    if os.name == "nt":
        skip("features e2e requires a Unix PTY")
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    args = parser.parse_args()
    htm = find_bin(args.htm, "HTM_BIN", "htm")
    htmd = find_bin(args.htmd, "HTMD_BIN", "htmd")
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)
    run_features(htm, htmd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
