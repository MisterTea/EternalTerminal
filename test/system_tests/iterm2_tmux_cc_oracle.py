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

import json
import os
import unittest

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_parity import cosmetic_title, parse_panes
from htm_gui_parity import (
    affinities_json,
    expected_affinities_cmd_t_tabs,
    layout_affinities,
    parse_affinities_from_dump,
    rank_affinity_groups,
)

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
# Windows ConPTY automatic-rename uses the leaf process image base name.
TITLE_SLEEP_WIN = "timeout"

# Multi-OS-window affinities suite markers (see run_gui_affinities).
AFF_A0 = "AFF_A0"
AFF_A1 = "AFF_A1"
AFF_A2 = "AFF_A2"
AFF_B0 = "AFF_B0"
AFF_B1 = "AFF_B1"
AFF_AFTER_REATTACH = "AFF_AFTER_REATTACH"

# Ranked affinity groups recorded from stock iTerm2 + tmux -CC corners.
# Absolute window ids are remapped to ranks so Ghostty/htm can differ on
# @0 vs @3 while still matching the window/tab partition.
_AFFINITIES_PATH = Path(__file__).with_name("iterm2_tmux_cc_affinities.json")
AFFINITIES: dict[str, list[list[int]]] = json.loads(
    _AFFINITIES_PATH.read_text(encoding="utf-8")
)

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
        "window_name_win": TITLE_SLEEP_WIN,
    },
    # Multi-OS-window affinities suite (Cmd+N new window, Cmd+T into older).
    "aff-after-first-window": {
        "panes": 1,
        "windows": 1,
        "contains": [AFF_A0],
    },
    "aff-after-tab-on-a": {
        "panes": 2,
        "windows": 2,
        "contains": [AFF_A0, AFF_A1],
    },
    "aff-after-second-os-window": {
        "panes": 3,
        "windows": 3,
        "contains": [AFF_A0, AFF_A1, AFF_B0],
    },
    "aff-after-tab-on-older-a": {
        "panes": 4,
        "windows": 4,
        "contains": [AFF_A0, AFF_A1, AFF_A2, AFF_B0],
    },
    "aff-after-tab-on-b": {
        "panes": 5,
        "windows": 5,
        "contains": [AFF_A0, AFF_A1, AFF_A2, AFF_B0, AFF_B1],
    },
    "aff-after-reattach": {
        "panes": 5,
        "windows": 5,
        "contains": [
            AFF_A0,
            AFF_A1,
            AFF_A2,
            AFF_B0,
            AFF_B1,
            AFF_AFTER_REATTACH,
        ],
    },
}


def window_ids(panes: list[dict]) -> set[str]:
    return {pane["wid"] for pane in panes}


def check_affinities(step_id: str, dump: str) -> list[str]:
    """Require window/tab affinities whenever pane contents are asserted.

    Corners steps use the recorded iTerm2+tmux -CC JSON. Layout/stress (and
    any other content dump without a recorded table entry) must still match
    iTerm2 Cmd+T semantics: every live window is a tab in one OS window.
    """
    got_aff = parse_affinities_from_dump(dump)
    if got_aff is None:
        return [
            "missing # affinities: JSON (iTerm2 persists @affinities on the session)"
        ]
    want = AFFINITIES.get(step_id)
    if want is not None:
        want_ranked = rank_affinity_groups(want)
        source = "iTerm2+tmux-CC"
    else:
        want_ranked = rank_affinity_groups(expected_affinities_cmd_t_tabs(dump))
        source = "iTerm2 Cmd+T (all live windows as tabs)"
    got_ranked = layout_affinities(dump)
    if got_ranked != want_ranked:
        return [
            f"affinities {source}={affinities_json(want_ranked)} "
            f"got={affinities_json(got_ranked)} "
            f"(abs={affinities_json(got_aff)})"
        ]
    return []


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
        # Terminal grids divide an odd cell count around a splitter. Windows
        # Terminal also reserves cells for its pane chrome, so its two halves
        # can differ by up to three columns while retaining the same rows.
        if max(cols) - min(cols) > 3 or len(set(rows)) != 1:
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
    if os.name == "nt" and spec.get("window_name_win"):
        want_name = spec["window_name_win"]
    if want_name:
        names = [cosmetic_title(pane["name"]) for pane in panes]
        if want_name not in names:
            errors.append(
                f"window name {want_name!r} missing; tmux -CC automatic-rename "
                f"names the pane's window {want_name!r} (got {names})"
            )
    errors.extend(check_affinities(step_id, dump))
    return errors


class OracleTests(unittest.TestCase):
    def test_root_checkpoint(self) -> None:
        dump = (
            "# affinities: [[0]]\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            f"echo {CORNER_ROOT}\n{CORNER_ROOT}\n"
        )
        self.assertEqual(check_step("after-root", dump), [])

    def test_kill_pane_drops_split_marker(self) -> None:
        dump = (
            "# affinities: [[0]]\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            f"{CORNER_ROOT}\n"
        )
        self.assertEqual(check_step("after-kill-pane", dump), [])
        dump_bad = dump + f"{CORNER_SPLIT}\n"
        self.assertTrue(check_step("after-kill-pane", dump_bad))

    def test_sleep_title(self) -> None:
        dump = (
            "# affinities: [[0,1,2]]\n"
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
        header = (
            "# affinities: [[0,1,2]]\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,1\n"
        )
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

    def test_affinities_must_match_iterm_ground_truth(self) -> None:
        # iTerm2 recorded [[0,1]] (ranked) after Cmd+T; singleton groups fail.
        dump = (
            "# affinities: [[0],[1]]\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,0\n"
            f"{CORNER_ROOT}\n"
            "--- window @0 name=zsh pane %1 active=1 80x24 cursor=0,0\n"
            "x\n"
            "--- window @1 name=zsh pane %2 active=1 80x24 cursor=0,0\n"
            f"{CORNER_WIN2}\n"
        )
        errors = check_step("after-new-window", dump)
        self.assertTrue(any("affinities" in e for e in errors))

    def test_affinity_ranks_ignore_absolute_ids(self) -> None:
        dump = (
            "# affinities: [[5,9]]\n"
            "--- window @5 name=zsh pane %0 active=0 80x24 cursor=0,0\n"
            f"{CORNER_ROOT}\n"
            "--- window @5 name=zsh pane %1 active=1 80x24 cursor=0,0\n"
            "x\n"
            "--- window @9 name=zsh pane %2 active=1 80x24 cursor=0,0\n"
            f"{CORNER_WIN2}\n"
        )
        self.assertEqual(check_step("after-new-window", dump), [])

    def test_stale_dead_window_ids_are_ignored_for_layout(self) -> None:
        # iTerm2 may keep destroyed @4 in @affinities; layout uses live ids only.
        dump = (
            "# affinities: [[0,1,3,4]]\n"
            "--- window @0 name=zsh pane %0 active=0 80x24 cursor=0,0\n"
            f"{CORNER_ROOT}\n"
            "--- window @0 name=zsh pane %1 active=1 80x24 cursor=0,0\n"
            "x\n"
            "--- window @1 name=zsh pane %2 active=1 80x24 cursor=0,0\n"
            f"{CORNER_WIN2}\n"
            "--- window @3 name=zsh pane %3 active=1 80x24 cursor=0,0\n"
            f"{AFTER_KILL_WIN_WRITER}\n"
        )
        self.assertEqual(check_step("after-kill-writer-window", dump), [])

    def test_layout_checkpoint_requires_affinities(self) -> None:
        dump = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,0\n"
            "HTM_E2E_PARITY\n"
        )
        errors = check_affinities("layout-after-marker", dump)
        self.assertTrue(any("affinities" in e for e in errors))

    def test_layout_tabs_must_share_one_os_window(self) -> None:
        # Cmd+T creates a sibling tab; separate OS windows fail.
        dump = (
            "# affinities: [[0],[1]]\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,0\n"
            "x\n"
            "--- window @1 name=zsh pane %1 active=1 80x24 cursor=0,0\n"
            "y\n"
        )
        errors = check_affinities("layout-after-tabs-splits", dump)
        self.assertTrue(any("affinities" in e for e in errors))
        dump_ok = (
            "# affinities: [[0,1]]\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,0\n"
            "x\n"
            "--- window @1 name=zsh pane %1 active=1 80x24 cursor=0,0\n"
            "y\n"
        )
        self.assertEqual(check_affinities("layout-after-tabs-splits", dump_ok), [])


if __name__ == "__main__":
    unittest.main()
