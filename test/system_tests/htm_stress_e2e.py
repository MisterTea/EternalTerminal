#!/usr/bin/env python3
"""Stress test: many HTM tabs/splits with concurrent bulk read and write.

Creates several tabs and nested splits, then on every pane at once:

  * background printers emit tens of KB of uniquely tagged lines
  * INSERT_KEYS injects many commands (and a larger blob) while that
    output is still flowing
  * the client PTY is then drained slowly so htm must keep forwarding
    stdin under stdout backpressure

Asserts htm/htmd stay alive, each pane's APPEND_TO_PANE stream contains
only its own tags plus its own injected keys, and the streams interleave.
Exit 77 if the binaries are missing. ``htm -x`` kills any existing htmd.
"""

from __future__ import annotations

import argparse
import os
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


def bulky_bg(tag: str, lines: int, width: int) -> str:
    pad = "x" * width
    return (
        f"i=1; while [ \"$i\" -le {lines} ]; do "
        f"printf '%s_%04d %s\\n' '{tag}' \"$i\" '{pad}'; "
        f"i=$((i+1)); done &\n"
    )


def key_line(tag: str, i: int) -> str:
    return f"printf '{tag}_KEY_{i}\\n'\n"


def blob_line(tag: str, width: int) -> str:
    pad = "B" * width
    return f"printf '%s_BLOB %s\\n' '{tag}' '{pad}'\n"


def run_stress(htm: Path, htmd: Path) -> None:
    lines = 600
    width = 120
    keys_per_pane = 60
    blob_width = 1536

    session = HtmPty(htm, htmd)
    try:
        session.start()
        if session.proc is None or session.proc.poll() is not None:
            fail("htm exited during attach")
        p0 = session.first_pane_id()
        print(f"OK: attached, initial pane {p0}", flush=True)

        tab_panes = [new_id() for _ in range(3)]
        for pane in tab_panes:
            session.new_tab(new_id(), pane)

        splits = []
        for source, vertical in (
            (p0, True),
            (p0, False),
            (tab_panes[0], True),
            (tab_panes[1], False),
        ):
            pane = new_id()
            session.new_split(source, pane, vertical=vertical)
            splits.append(pane)

        panes = [(p0, "ST0")]
        for i, pane in enumerate(tab_panes, start=1):
            panes.append((pane, f"ST{i}"))
        for i, pane in enumerate(splits, start=4):
            panes.append((pane, f"ST{i}"))

        for pane, _tag in panes:
            session.resize(pane, 80, 24)
        session.drain_idle(idle=0.25, timeout=5.0)
        session.wait_until(
            lambda: all(session.pane_output(p) for p, _ in panes),
            12.0,
            "shell prompt on every pane",
        )
        if not pids_named("htmd"):
            fail("htmd died during layout")
        print(
            f"OK: layout with {len(panes)} panes "
            f"(1 root + {len(tab_panes)} tabs + {len(splits)} splits)",
            flush=True,
        )

        burst_at = len(session.packets)
        for pane, tag in panes:
            session.insert_keys(pane, bulky_bg(tag, lines, width))

        # Writes while every pane is still printing.
        for i in range(keys_per_pane):
            for pane, tag in panes:
                session.insert_keys(pane, key_line(tag, i))
            if i % 4 == 0:
                session.pump(0.02)
            if session.proc.poll() is not None:
                fail(f"htm died during INSERT_KEYS flood (i={i})")
            if not pids_named("htmd"):
                fail(f"htmd died during INSERT_KEYS flood (i={i})")

        for pane, tag in panes:
            session.insert_keys(pane, blob_line(tag, blob_width))
        print("OK: started printers and injected keys on every pane", flush=True)

        last_key = keys_per_pane - 1

        def caught_up() -> bool:
            if session.proc is not None and session.proc.poll() is not None:
                return False
            for pane, tag in panes:
                out = session.pane_output(pane)
                if f"{tag}_{lines:04d}" not in out:
                    return False
                if f"{tag}_KEY_{last_key}" not in out:
                    return False
                if f"{tag}_BLOB" not in out:
                    return False
            return True

        session.wait_until(caught_up, 45.0, "bulk output and injected keys on every pane")
        if session.proc.poll() is not None:
            fail("htm exited before bulk output completed")
        if not pids_named("htmd"):
            fail("htmd exited before bulk output completed")
        print("OK: every pane received its bulk output, keys, and blob", flush=True)

        min_bytes = lines * (width // 2)
        for pane, tag in panes:
            out = session.pane_output(pane)
            if f"{tag}_0001" not in out:
                fail(f"pane {tag} missing first printer line")
            if f"{tag}_KEY_0" not in out:
                fail(f"pane {tag} missing first injected key")
            if len(out) < min_bytes:
                fail(f"pane {tag} only produced {len(out)} bytes")
            for other_pane, other_tag in panes:
                if other_pane == pane:
                    continue
                if f"{other_tag}_" in out:
                    fail(f"pane {tag} leaked output from {other_tag}")
        print("OK: pane streams isolated and large enough", flush=True)

        order = session.append_pane_order(burst_at)
        unique = {pane for pane in order if pane in {p for p, _ in panes}}
        if len(unique) < min(6, len(panes)):
            fail(f"APPEND from too few panes under load: {len(unique)}")
        transitions = sum(1 for i in range(1, len(order)) if order[i] != order[i - 1])
        if transitions < 20:
            fail(
                f"bulk pane output was not interleaved "
                f"(transitions={transitions}, packets={len(order)})"
            )
        print(
            f"OK: interleaved APPEND ({len(order)} packets, {len(unique)} panes, "
            f"{transitions} switches)",
            flush=True,
        )

        # Backpressure: keep writing keys while the client PTY drains slowly.
        session.read_size = 256
        slow_tag = "SLOW"
        slow_n = 12
        for i in range(slow_n):
            for pane, tag in panes:
                session.insert_keys(pane, f"printf '{slow_tag}_{tag}_{i}\\n'\n")
            session.pump(0.03)
            if session.proc.poll() is not None:
                fail("htm died under stdout backpressure")
            if not pids_named("htmd"):
                fail("htmd died under stdout backpressure")
        session.read_size = 65536
        session.wait_until(
            lambda: all(
                f"{slow_tag}_{tag}_{slow_n - 1}" in session.pane_output(pane)
                for pane, tag in panes
            ),
            30.0,
            "keys injected during slow PTY drain",
        )
        print("OK: keys kept flowing under slow client drain", flush=True)

        session.write_packet(INSERT_DEBUG_KEYS, b"x")
        deadline = time.time() + 15
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

    print("PASS: htm stress (tabs, splits, concurrent bulk read/write)", flush=True)


def main() -> int:
    if os.name == "nt":
        skip("stress e2e requires a Unix PTY")
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    args = parser.parse_args()
    htm = find_bin(args.htm, "HTM_BIN", "htm")
    htmd = find_bin(args.htmd, "HTMD_BIN", "htmd")
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)
    run_stress(htm, htmd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
