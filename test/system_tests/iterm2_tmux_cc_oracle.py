#!/usr/bin/env python3
"""iTerm2 + tmux -CC ground truth for the GUI corners suite.

Stock iTerm2 speaking ``tmux -L … -f /dev/null -CC new-session`` is the
oracle: default base-index, automatic-rename on, visible-screen capture-pane.
Each checkpoint is what that setup contains after the same iTerm2 keys.
htm must match these results, except cosmetic mux naming (``[htm]`` vs
``[tmux]``) and timing (cursor, extra writer ticks).

The corners e2e asserts every mux against this table, then additionally
diffs htm checkpoints against the tmux -CC checkpoints from the same run.
"""

from __future__ import annotations

import unittest

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_parity import cosmetic_title, parse_panes

# Markers the suite types. They are the comparable "results" from tmux -CC.
CORNER_ROOT = "CORNER_ROOT"
CORNER_UNICODE = "CORNER_UNICODE_é_中_😀"
CORNER_SCROLL_FIRST = "SCROLLBACK_1"
CORNER_SCROLL_LAST = "SCROLLBACK_40"
CORNER_AFTER_ALT = "AFTER_ALT"
CORNER_SPLIT = "CORNER_SPLIT"
CORNER_SPLIT2 = "CORNER_SPLIT2"
CORNER_WIN2 = "CORNER_WIN2"
CORNER_WIN3 = "CORNER_WIN3"
CORNER_WIN4 = "CORNER_WIN4"
AFTER_KILL_PANE_WRITER = "AFTER_KILL_PANE_WRITER"
AFTER_KILL_WIN_WRITER = "AFTER_KILL_WIN_WRITER"
AFTER_REATTACH = "AFTER_REATTACH"
WRTICKR = "WRTICKR"
WRTICKP = "WRTICKP"
WRTICKW = "WRTICKW"
TITLE_SLEEP = "sleep"

# Checkpoints recorded against iTerm2 + tmux -CC. pane/window counts are the
# live session after each action (not historical command watermarks).
STEPS: dict[str, dict] = {
    "after-attach": {"panes": 1, "windows": 1},
    "after-root": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_ROOT],
    },
    "after-unicode": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_ROOT, CORNER_UNICODE],
    },
    "after-scrollback": {
        "panes": 1,
        "windows": 1,
        "contains": [
            CORNER_ROOT,
            CORNER_UNICODE,
            CORNER_SCROLL_FIRST,
            CORNER_SCROLL_LAST,
        ],
    },
    "after-alternate-screen": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_SCROLL_LAST, CORNER_AFTER_ALT],
        "absent": ["ALT_SCREEN"],
    },
    "after-native-resize": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_SCROLL_FIRST, CORNER_SCROLL_LAST, CORNER_AFTER_ALT],
        "min_cols": 100,
        "min_rows": 30,
    },
    "before-writer-detach": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_ROOT, WRTICKR],
        "writer": WRTICKR,
    },
    "after-writer-detach-reattach": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_ROOT, WRTICKR],
        "writer": WRTICKR,
    },
    "after-detach-reattach": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_ROOT, AFTER_REATTACH],
    },
    "after-split": {
        "panes": 2,
        "windows": 1,
        "contains": [CORNER_ROOT],
        "balanced_horizontal": True,
    },
    "after-split-echo": {
        "panes": 2,
        "windows": 1,
        "contains": [CORNER_ROOT, CORNER_SPLIT],
    },
    "after-kill-pane": {
        "panes": 1,
        "windows": 1,
        "contains": [CORNER_ROOT],
        "absent": [CORNER_SPLIT],
    },
    "after-split-again": {
        "panes": 2,
        "windows": 1,
        "contains": [CORNER_ROOT, CORNER_SPLIT2],
        "balanced_horizontal": True,
    },
    "after-new-window": {
        "panes": 3,
        "windows": 2,
        "contains": [CORNER_ROOT, CORNER_WIN2],
    },
    "after-third-window": {
        "panes": 4,
        "windows": 3,
        "contains": [CORNER_ROOT, CORNER_WIN2, CORNER_WIN3],
    },
    "after-kill-window": {
        "panes": 3,
        "windows": 2,
        "contains": [CORNER_ROOT, CORNER_WIN2],
        "absent": [CORNER_WIN3],
    },
    "after-replace-window": {
        "panes": 4,
        "windows": 3,
        "contains": [CORNER_ROOT, CORNER_WIN2, CORNER_WIN4],
    },
    "after-writer-pane": {
        "panes": 5,
        "windows": 3,
        "contains": [WRTICKP],
        "writer": WRTICKP,
    },
    "after-kill-writer-pane": {
        "panes": 4,
        "windows": 3,
        "contains": [CORNER_ROOT, AFTER_KILL_PANE_WRITER],
        "absent": [WRTICKP],
    },
    "after-writer-window": {
        "panes": 5,
        "windows": 4,
        "contains": [WRTICKW],
        "writer": WRTICKW,
    },
    "after-kill-writer-window": {
        "panes": 4,
        "windows": 3,
        "contains": [CORNER_ROOT],
        "absent": [WRTICKW],
    },
    "after-title-sleep": {
        "panes": 4,
        "windows": 3,
        "window_name": TITLE_SLEEP,
    },
}


def window_ids(panes: list[dict]) -> set[str]:
    return {pane["wid"] for pane in panes}


def check_step(step_id: str, dump: str) -> list[str]:
    """Return human-readable mismatches against the tmux -CC oracle."""
    if step_id not in STEPS:
        return [f"unknown checkpoint {step_id}"]
    spec = STEPS[step_id]
    panes = parse_panes(dump)
    errors: list[str] = []
    if spec.get("panes") is not None and len(panes) != spec["panes"]:
        errors.append(f"panes tmux-CC={spec['panes']} got={len(panes)}")
    if spec.get("windows") is not None and len(window_ids(panes)) != spec["windows"]:
        errors.append(
            f"windows tmux-CC={spec['windows']} got={len(window_ids(panes))}"
        )
    if panes and spec.get("min_cols") is not None:
        if max(int(pane["cols"]) for pane in panes) < spec["min_cols"]:
            errors.append(f"no pane reached {spec['min_cols']} columns")
    if panes and spec.get("min_rows") is not None:
        if max(int(pane["rows"]) for pane in panes) < spec["min_rows"]:
            errors.append(f"no pane reached {spec['min_rows']} rows")
    if spec.get("balanced_horizontal") and len(panes) == 2:
        cols = [int(pane["cols"]) for pane in panes]
        rows = [int(pane["rows"]) for pane in panes]
        if max(cols) - min(cols) > 1 or len(set(rows)) != 1:
            errors.append(
                f"horizontal split not balanced: cols={cols} rows={rows}"
            )
    for needle in spec.get("contains") or []:
        if needle not in dump:
            errors.append(f"missing {needle!r} (present in iTerm2+tmux -CC)")
    for needle in spec.get("absent") or []:
        if needle in dump:
            errors.append(f"unexpected {needle!r} (gone in iTerm2+tmux -CC)")
    writer = spec.get("writer")
    if writer and dump.count(writer) < 2:
        errors.append(f"writer {writer!r} did not emit output")
    want_name = spec.get("window_name")
    if want_name:
        names = [cosmetic_title(pane["name"]) for pane in panes]
        if want_name not in names:
            errors.append(
                f"window name {want_name!r} missing; tmux -CC automatic-rename "
                f"names the pane's window {want_name!r} (got {names})"
            )
    return errors


class OracleTests(unittest.TestCase):
    def test_root_checkpoint(self) -> None:
        dump = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            f"echo {CORNER_ROOT}\n{CORNER_ROOT}\n"
        )
        self.assertEqual(check_step("after-root", dump), [])

    def test_kill_pane_drops_split_marker(self) -> None:
        dump = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            f"{CORNER_ROOT}\n"
        )
        self.assertEqual(check_step("after-kill-pane", dump), [])
        dump_bad = dump + f"{CORNER_SPLIT}\n"
        self.assertTrue(check_step("after-kill-pane", dump_bad))

    def test_sleep_title(self) -> None:
        dump = (
            "--- window @2 name=sleep pane %4 active=1 80x24 cursor=0,0\n"
            "sleep 25\n"
            "--- window @0 name=zsh pane %0 active=0 80x24 cursor=0,0\n"
            f"{CORNER_ROOT}\n"
            "--- window @1 name=zsh pane %1 active=0 80x24 cursor=0,0\n"
            "x\n"
            "--- window @1 name=zsh pane %2 active=0 80x24 cursor=0,0\n"
            "y\n"
        )
        self.assertEqual(check_step("after-title-sleep", dump), [])

    def test_writer_requires_emitted_tick(self) -> None:
        header = "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,1\n"
        panes = "".join(
            f"--- window @{wid} name=zsh pane %{pid} active=1 80x24 cursor=0,1\n"
            for pid, wid in ((1, 0), (2, 1), (3, 2), (4, 2))
        )
        command = f"while :; do echo {WRTICKP}; sleep 0.05; done\n"
        self.assertTrue(check_step("after-writer-pane", header + panes + command))
        self.assertEqual(
            check_step("after-writer-pane", header + panes + command + WRTICKP),
            [],
        )


if __name__ == "__main__":
    unittest.main()
