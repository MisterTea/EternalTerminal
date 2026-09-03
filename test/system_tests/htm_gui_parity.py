#!/usr/bin/env python3
"""Compare tmux vs htm GUI e2e pane dumps.

tmux -CC is ground truth. A pair of snapshots is acceptable when they are
identical, differ only by cosmetic mux naming (htm vs tmux), or differ only
in ways that are guaranteed to be timing (cursor, extra ticks from a writer
still running, trailing blank lines that appear later).
"""

from __future__ import annotations

import difflib
import re
import unittest
from pathlib import Path
from typing import Iterable, Optional

PANE_HEADER = re.compile(
    r"^--- window @(?P<wid>\d+) name=(?P<name>.*) pane %(?P<pid>\d+) "
    r"active=(?P<active>\d+) (?P<cols>\d+)x(?P<rows>\d+) "
    r"cursor=(?P<cx>-?\d+),(?P<cy>-?\d+)"
    r"(?: shell_pid=(?P<shell_pid>-?\d+))?"
    r"(?: current=(?P<current>\d+))?\s*$"
)
EMULATOR_WIN = re.compile(
    r"^win\d+\s+\S+\s+name=(?P<name>.*?)\s+frame="
)
ACTION_HEADER = re.compile(r"^# action:\s*(?P<action>.*)\s*$")
CLICK_ACTION = re.compile(r"^click-\d+-\d+$")
WRITER_LINE = re.compile(r"^(?:WRTICK[A-Z0-9]*|STBULK\d*)(?:_.*)?$")
PROMPTISH = re.compile(r"[%$#>]\s*$")
WORKLOAD_TOKEN = re.compile(
    r"(?:CORNER|AFTER)_[A-Z0-9_]+|"
    r"(?:WRTICK|HTM_E2E_|MA|MB|GUI0|GUI1|STA|STB|SW|STKEY|STBULK|"
    r"SCROLLBACK_|AFTER_ALT)"
    r"[A-Z0-9_]*"
)
STRESS_RESULT = re.compile(
    r"^(?:STAPARITY|STBPARITY[a-z_]+|SWPARITY[a-z_]+|STKEYPARITY_\d+)$"
)
SHELL_PROMPT_ONLY = re.compile(r"^(?:➜\s+\S+|.*[%$#>])\s*$")
EARLY_MARKER_REDRAW = re.compile(
    r"^(?:echo\s+)?((?:CORNER|AFTER)_[A-Z0-9_]+)[%$#>]?\s*$"
)


def cosmetic_title(name: str) -> str:
    """Map htm mux chrome onto tmux chrome without touching other words."""
    n = name or ""
    n = n.replace("htm htm", "tmux tmux")
    n = re.sub(r" \[htm\]\s*$", " [tmux]", n)
    n = re.sub(r"\[↣ htm\b", "[↣ tmux", n)
    return n


def normalize_action(action: str) -> str:
    action = (action or "").strip()
    if CLICK_ACTION.match(action):
        return "click"
    return action


def _strip_meta(text: str) -> str:
    lines = []
    for line in text.splitlines():
        if line.startswith("# t=") or line.startswith("# n=") or line.startswith("# action:"):
            continue
        if re.match(r"^# t=", line) or re.match(r"^# .*mux=", line):
            continue
        lines.append(line)
    return "\n".join(lines)


def parse_action(text: str, fallback: str) -> str:
    for line in text.splitlines():
        match = ACTION_HEADER.match(line)
        if match:
            return match.group("action").strip()
    return fallback


def parse_panes(text: str) -> list[dict]:
    panes: list[dict] = []
    current: Optional[dict] = None
    body: list[str] = []
    emulator: list[str] = []
    in_emulator = False
    for line in text.splitlines():
        if line.startswith("--- emulator windows ---"):
            in_emulator = True
            continue
        em = EMULATOR_WIN.match(line)
        if em and in_emulator:
            emulator.append(cosmetic_title(em.group("name")))
            continue
        match = PANE_HEADER.match(line)
        if match:
            in_emulator = False
            if current is not None:
                current["body"] = "\n".join(body)
                panes.append(current)
            current = match.groupdict()
            current["name"] = current["name"]
            body = []
            continue
        if current is not None:
            body.append(line.rstrip())
    if current is not None:
        current["body"] = "\n".join(body)
        panes.append(current)
    for pane in panes:
        pane["emulator_titles"] = emulator
    return panes


def _collapse_writer(body: str) -> str:
    """Ignore tick count/placement but retain proof that writer output occurred."""
    out: list[str] = []
    saw_writer = False
    for line in body.splitlines():
        if WRITER_LINE.match(line.strip()):
            saw_writer = True
            continue
        out.append(line.rstrip())
    while out and out[-1] == "":
        out.pop()
    if saw_writer:
        out.append("<writer-ticks>")
    return "\n".join(out)


def _body_without_cursors(pane: dict) -> str:
    return _collapse_writer(pane.get("body") or "")


def comparable_panes(text: str) -> list[tuple[str, str, str]]:
    """(window_name, size, body) with cosmetic names and timing-collapsed bodies."""
    rows = []
    for pane in parse_panes(text):
        rows.append(
            (
                cosmetic_title(pane["name"]),
                f"{pane['cols']}x{pane['rows']}",
                _body_without_cursors(pane),
            )
        )
    return rows


def emulator_titles(text: str) -> list[str]:
    titles = []
    for line in text.splitlines():
        match = EMULATOR_WIN.match(line)
        if match:
            titles.append(cosmetic_title(match.group("name")))
    return titles


def _only_cursor_or_ids(tmux: str, htm: str) -> bool:
    def drop(text: str) -> str:
        text = re.sub(r"cursor=-?\d+,-?\d+", "cursor=X,Y", text)
        text = re.sub(r"window @\d+", "window @N", text)
        text = re.sub(r"pane %\d+", "pane %N", text)
        text = re.sub(r"frame=-?\d+,-?\d+,-?\d+,-?\d+", "frame=F", text)
        text = re.sub(r"\bid:\d+", "id:N", text)
        return cosmetic_title(text)

    return drop(_strip_meta(tmux)) == drop(_strip_meta(htm))


def _timing_bodies(tmux_body: str, htm_body: str) -> bool:
    return _collapse_writer(tmux_body) == _collapse_writer(htm_body)


def _corner_body(body: str) -> str:
    """Normalize only observed shell redraw and geometry-dependent wrapping."""
    lines = [line.rstrip() for line in body.splitlines()]
    interrupted: list[str] = []
    for line in lines:
        if line.startswith("^C"):
            prompt = line.find("➜")
            interrupted.append("^C")
            if prompt >= 0:
                interrupted.append(line[prompt:])
        else:
            interrupted.append(line)
    lines = interrupted
    lines = [
        re.sub(r"^(\s*\[\d+\](?:\s+[+~-])?\s+)\d+", r"\1PID", line)
        if line.lstrip().startswith("[")
        else line
        for line in lines
    ]
    # The ASCII escape command wraps at different columns as the native
    # window width changes; the emitted Unicode line is compared separately.
    lines = [line for line in lines if "printf 'CORNER_UNICODE_" not in line]
    filtered: list[str] = []
    skipping_alt_command = False
    for line in lines:
        if "printf '\\033[?1049h" in line:
            skipping_alt_command = "AFTER_ALT" not in line
            continue
        if skipping_alt_command:
            if "AFTER_ALT" in line:
                skipping_alt_command = False
            continue
        filtered.append(line)
    lines = filtered
    lines = [
        match.group(1)
        if (match := re.search(r"((?:GUI0|GUI1)[A-Z0-9_]+)$", line))
        and "echo " not in line
        else line
        for line in lines
    ]
    while lines and (not lines[0] or SHELL_PROMPT_ONLY.match(lines[0])):
        lines.pop(0)

    # iTerm may send the first command while a new shell is painting its
    # prompt. tmux retains that incomplete redraw above the completed command.
    if lines:
        match = EARLY_MARKER_REDRAW.match(lines[0])
        if match and any(match.group(1) in line for line in lines[1:]):
            lines.pop(0)
    if lines and lines[0].startswith("while :; do echo WRTICK"):
        if any(lines[0] in line for line in lines[1:]):
            lines.pop(0)

    # The same command can wrap at different columns because iTerm cascades
    # native windows between the sequential tmux and htm runs. capture-pane -J
    # joins actual wrapped rows, but zsh sometimes repaints them as separate
    # logical rows, so join only this test's known writer command.
    joined: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if "while :; do" in line and "done" not in line:
            while index + 1 < len(lines) and "done" not in line:
                index += 1
                line += lines[index].lstrip()
        elif (
            line.lstrip().startswith("[")
            and "done" in line
            and index + 1 < len(lines)
            and re.match(
                r"^\d+(?:\.\d+)?;\s+done$", lines[index + 1].strip()
            )
        ):
            while index + 1 < len(lines) and re.match(
                r"^\d+(?:\.\d+)?;\s+done$", lines[index + 1].strip()
            ):
                index += 1
                line += lines[index].lstrip()
        joined.append(line)
        index += 1
    normalized = "\n".join(joined)
    normalized = re.sub(r"\bsleep\s*(\d+\.\d+)", r"sleep \1", normalized)
    return _collapse_writer(normalized)


def _corner_semantics(panes: list[dict]) -> tuple[list[int], list[str]]:
    """Window topology and complete normalized pane text."""
    window_sizes: list[int] = []
    last_wid = None
    bodies: list[str] = []
    for pane in panes:
        wid = pane["wid"]
        if wid != last_wid:
            window_sizes.append(0)
            last_wid = wid
        window_sizes[-1] += 1
        bodies.append(_corner_body(pane.get("body") or ""))
    return window_sizes, bodies


def _stress_semantics(
    panes: list[dict],
) -> tuple[list[int], list[list[str]], list[bool]]:
    """Stable outputs while background writer bytes race with shell redraws."""
    topology: list[int] = []
    last_wid = None
    results: list[list[str]] = []
    writers: list[bool] = []
    for pane in panes:
        if pane["wid"] != last_wid:
            topology.append(0)
            last_wid = pane["wid"]
        topology[-1] += 1
        lines = [line.strip() for line in (pane.get("body") or "").splitlines()]
        results.append([line for line in lines if STRESS_RESULT.match(line)])
        writers.append(any(WRITER_LINE.match(line) for line in lines))
    return topology, results, writers


def classify_snapshot(tmux_text: str, htm_text: str, action: str) -> dict:
    """Return status identical | cosmetic | timing | diverge."""
    if tmux_text == htm_text:
        return {"action": action, "status": "identical", "detail": ""}
    t_panes = parse_panes(tmux_text)
    h_panes = parse_panes(htm_text)
    t_comp = comparable_panes(tmux_text)
    h_comp = comparable_panes(htm_text)
    t_titles = [cosmetic_title(t) for t in emulator_titles(tmux_text)]
    h_titles = [cosmetic_title(t) for t in emulator_titles(htm_text)]

    if t_comp == h_comp and t_titles == h_titles:
        if tmux_text.replace("htm", "tmux") == htm_text.replace("htm", "tmux"):
            return {
                "action": action,
                "status": "cosmetic",
                "detail": "mux name htm vs tmux",
            }
        if _only_cursor_or_ids(tmux_text, htm_text):
            return {
                "action": action,
                "status": "timing",
                "detail": "cursor, pane id, window id, or frame",
            }
        return {
            "action": action,
            "status": "timing",
            "detail": "writer ticks or equivalent collapsed text",
        }

    if len(t_comp) != len(h_comp):
        return {
            "action": action,
            "status": "diverge",
            "detail": f"pane count tmux={len(t_comp)} htm={len(h_comp)}",
        }

    timing = True
    details: list[str] = []
    for idx, (t_row, h_row) in enumerate(zip(t_comp, h_comp)):
        t_name, t_size, t_body = t_row
        h_name, h_size, h_body = h_row
        if t_name != h_name:
            details.append(f"pane {idx} name {t_name!r} vs {h_name!r}")
            timing = False
        if t_size != h_size:
            details.append(f"pane {idx} size {t_size} vs {h_size}")
            timing = False
        if t_body != h_body and not _timing_bodies(
            t_panes[idx].get("body") or "", h_panes[idx].get("body") or ""
        ):
            diff = "\n".join(
                difflib.unified_diff(
                    t_body.splitlines(),
                    h_body.splitlines(),
                    fromfile="tmux",
                    tofile="htm",
                    lineterm="",
                )
            )
            details.append(f"pane {idx} text:\n{diff}")
            timing = False
    if t_titles != h_titles:
        # Gateway / native suffix already cosmetic-normalized; leftover mismatch
        # is a real title bug unless one side has not yet applied automatic-rename.
        if _titles_timing(t_titles, h_titles):
            details.append("emulator titles not yet renamed (timing)")
        else:
            details.append(f"titles {t_titles!r} vs {h_titles!r}")
            timing = False
    if timing:
        return {
            "action": action,
            "status": "timing",
            "detail": "; ".join(details) or "writer ticks / prefix",
        }
    if action.startswith("stress-"):
        t_stress = _stress_semantics(t_panes)
        h_stress = _stress_semantics(h_panes)
        if (
            t_stress == h_stress
            and any(any(results) for results in t_stress[1])
            and (t_titles == h_titles or _titles_timing(t_titles, h_titles))
        ):
            has_writer = any(t_stress[2])
            return {
                "action": action,
                "status": "timing" if has_writer else "cosmetic",
                "detail": (
                    "background writer bytes interleaved with shell redraw"
                    if has_writer
                    else "shell startup chrome or iTerm native-window geometry"
                ),
            }
    t_topology, t_bodies = _corner_semantics(t_panes)
    h_topology, h_bodies = _corner_semantics(h_panes)
    if (
        (
            WORKLOAD_TOKEN.search(tmux_text)
            or WORKLOAD_TOKEN.search(htm_text)
            or action == "after-attach"
        )
        and t_topology == h_topology
        and t_bodies == h_bodies
        and (t_titles == h_titles or _titles_timing(t_titles, h_titles))
    ):
        return {
            "action": action,
            "status": "cosmetic",
            "detail": "shell startup chrome or iTerm native-window geometry",
        }
    return {
        "action": action,
        "status": "diverge",
        "detail": "; ".join(details) or "unclassified difference",
    }


def _titles_timing(tmux_titles: list[str], htm_titles: list[str]) -> bool:
    """automatic-rename can lag; zsh vs sleep is timing if the other names match."""
    if len(tmux_titles) != len(htm_titles):
        return False
    transient = {"zsh", "bash", "sh", "fish", "tmux", "sleep", "yes"}
    for t, h in zip(tmux_titles, htm_titles):
        if t == h:
            continue
        t_core = re.sub(r" \[tmux\]\s*$", "", t).strip()
        h_core = re.sub(r" \[tmux\]\s*$", "", h).strip()
        if t_core in transient and h_core in transient:
            continue
        return False
    return True


def snapshot_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir() if p.suffix == ".txt")


def compare_suite_dirs(tmux_dir: Path, htm_dir: Path) -> list[dict]:
    t_files = snapshot_files(tmux_dir)
    h_files = snapshot_files(htm_dir)
    t_actions = [
        normalize_action(parse_action(p.read_text(encoding="utf-8", errors="replace"), p.stem))
        for p in t_files
    ]
    h_actions = [
        normalize_action(parse_action(p.read_text(encoding="utf-8", errors="replace"), p.stem))
        for p in h_files
    ]
    verdicts: list[dict] = []
    if t_actions != h_actions:
        verdicts.append(
            {
                "action": "(sequence)",
                "status": "diverge",
                "detail": (
                    "action lists differ\n"
                    f"tmux ({len(t_actions)}): {t_actions}\n"
                    f"htm  ({len(h_actions)}): {h_actions}"
                ),
            }
        )
        # Still compare the common prefix so later steps see real pane diffs.
        n = min(len(t_files), len(h_files))
    else:
        n = len(t_files)
    for i in range(n):
        t_text = t_files[i].read_text(encoding="utf-8", errors="replace")
        h_text = h_files[i].read_text(encoding="utf-8", errors="replace")
        action = t_actions[i] if i < len(t_actions) else t_files[i].stem
        verdicts.append(classify_snapshot(t_text, h_text, action))
    return verdicts


def format_verdicts(verdicts: Iterable[dict]) -> str:
    lines = []
    counts = {"identical": 0, "cosmetic": 0, "timing": 0, "diverge": 0}
    for item in verdicts:
        counts[item["status"]] = counts.get(item["status"], 0) + 1
        if item["status"] == "diverge":
            lines.append(f"DIVERGE {item['action']}: {item['detail']}")
        else:
            extra = f" ({item['detail']})" if item["detail"] else ""
            lines.append(f"{item['status'].upper()} {item['action']}{extra}")
    lines.append(
        "summary: "
        + ", ".join(f"{k}={v}" for k, v in counts.items())
    )
    return "\n".join(lines)


def divergences(verdicts: Iterable[dict]) -> list[dict]:
    return [item for item in verdicts if item["status"] == "diverge"]


def compare_step_dirs(tmux_dir: Path, htm_dir: Path) -> list[dict]:
    """Compare named tmux -CC vs htm checkpoints (not noisy per-keystroke dumps)."""
    t_files = {p.stem: p for p in snapshot_files(tmux_dir)}
    h_files = {p.stem: p for p in snapshot_files(htm_dir)}
    verdicts: list[dict] = []
    missing_h = sorted(set(t_files) - set(h_files))
    missing_t = sorted(set(h_files) - set(t_files))
    if missing_h or missing_t:
        verdicts.append(
            {
                "action": "(steps)",
                "status": "diverge",
                "detail": (
                    f"checkpoint names differ; only-tmux={missing_h} only-htm={missing_t}"
                ),
            }
        )
    for name in sorted(set(t_files) & set(h_files)):
        verdicts.append(
            classify_snapshot(
                t_files[name].read_text(encoding="utf-8", errors="replace"),
                h_files[name].read_text(encoding="utf-8", errors="replace"),
                name,
            )
        )
    return verdicts


class ParityHelperTests(unittest.TestCase):
    def test_cosmetic_title_suffix(self) -> None:
        self.assertEqual(cosmetic_title("zsh [htm]"), "zsh [tmux]")
        self.assertEqual(cosmetic_title("[↣ htm htm]"), "[↣ tmux tmux]")
        self.assertEqual(cosmetic_title("sleep [tmux]"), "sleep [tmux]")

    def test_writer_ticks_are_timing(self) -> None:
        tmux = (
            "# action: writer\n\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            "WRTICK\nWRTICK\n"
        )
        htm = (
            "# action: writer\n\n"
            "--- window @1 name=zsh pane %0 active=1 80x24 cursor=0,4\n"
            "WRTICK\nWRTICK\nWRTICK\nWRTICK\n"
        )
        verdict = classify_snapshot(tmux, htm, "writer")
        self.assertEqual(verdict["status"], "timing")

    def test_htm_suffix_is_cosmetic(self) -> None:
        tmux = (
            "# action: titles\n\n"
            "--- emulator windows ---\n"
            "win1 id:1 name=zsh [tmux] frame=0,0,100,100\n"
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,0\n"
            "hi\n"
        )
        htm = (
            "# action: titles\n\n"
            "--- emulator windows ---\n"
            "win1 id:2 name=zsh [htm] frame=1,1,100,100\n"
            "--- window @1 name=zsh pane %0 active=1 80x24 cursor=0,0\n"
            "hi\n"
        )
        verdict = classify_snapshot(tmux, htm, "titles")
        self.assertIn(verdict["status"], ("cosmetic", "timing"))

    def test_extra_prompt_is_diverge(self) -> None:
        tmux = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            "host% \n"
            "host% \n"
        )
        htm = (
            "--- window @1 name=zsh pane %0 active=1 80x24 cursor=0,1\n"
            "host% \n"
        )
        verdict = classify_snapshot(tmux, htm, "prompt")
        self.assertEqual(verdict["status"], "diverge")

    def test_identical_markers_pass(self) -> None:
        body = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,1\n"
            "echo CORNER_ROOT\nCORNER_ROOT\n"
        )
        verdict = classify_snapshot(body, body.replace("@0", "@1"), "root")
        self.assertIn(verdict["status"], ("identical", "cosmetic", "timing"))

    def test_corner_shell_chrome_and_geometry_are_cosmetic(self) -> None:
        tmux = (
            "--- window @0 name=tmux pane %0 active=1 39x24 cursor=0,3\n"
            "host% \nhost% echo CORNER_ROOT\nCORNER_ROOT\n"
        )
        htm = (
            "--- window @7 name=zsh pane %4 active=1 42x24 cursor=0,2\n"
            "host% echo CORNER_ROOT\nCORNER_ROOT\n"
        )
        verdict = classify_snapshot(tmux, htm, "after-root")
        self.assertEqual(verdict["status"], "cosmetic")

    def test_different_corner_marker_diverges(self) -> None:
        tmux = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,1\n"
            "CORNER_ROOT\n"
        )
        htm = tmux.replace("CORNER_ROOT", "CORNER_WRONG")
        verdict = classify_snapshot(tmux, htm, "after-root")
        self.assertEqual(verdict["status"], "diverge")

    def test_unrelated_output_with_same_marker_diverges(self) -> None:
        tmux = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            "CORNER_ROOT\nexpected output\n"
        )
        htm = tmux.replace("expected output", "wrong output")
        verdict = classify_snapshot(tmux, htm, "after-root")
        self.assertEqual(verdict["status"], "diverge")

    def test_stress_writer_interleaving_is_timing(self) -> None:
        tmux = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            "STAPARITY\nSTBULK0\nSTKEYPARITY_0\nSTBULK0\n"
        )
        htm = (
            "--- window @1 name=zsh pane %0 active=1 81x24 cursor=0,3\n"
            "STAPARITY\nSTBULK0\necho STKSTBULK0\n"
            "STKEYPARITY_0\nSTBULK0\n"
        )
        verdict = classify_snapshot(tmux, htm, "stress-after-bulk-output")
        self.assertEqual(verdict["status"], "timing")

    def test_stress_missing_result_diverges(self) -> None:
        tmux = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            "STAPARITY\nSTKEYPARITY_0\nSTKEYPARITY_1\n"
        )
        htm = tmux.replace("STKEYPARITY_1\n", "")
        verdict = classify_snapshot(tmux, htm, "stress-after-bulk-output")
        self.assertEqual(verdict["status"], "diverge")

    def test_interrupt_prompt_redraw_is_cosmetic(self) -> None:
        tmux = (
            "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,2\n"
            "CORNER_ROOT\n^C%\n➜  ~ echo AFTER_REATTACH\nAFTER_REATTACH\n"
        )
        htm = (
            "--- window @1 name=zsh pane %0 active=1 81x24 cursor=0,2\n"
            "CORNER_ROOT\n^C    ➜  ~ echo AFTER_REATTACH\nAFTER_REATTACH\n"
        )
        verdict = classify_snapshot(tmux, htm, "after-detach-reattach")
        self.assertEqual(verdict["status"], "cosmetic")

    def test_compare_step_dirs_by_name(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            tmux_dir = root / "tmux"
            htm_dir = root / "htm"
            tmux_dir.mkdir()
            htm_dir.mkdir()
            body = (
                "--- window @0 name=zsh pane %0 active=1 80x24 cursor=0,1\n"
                "echo CORNER_ROOT\nCORNER_ROOT\n"
            )
            (tmux_dir / "after-root.txt").write_text(body)
            (htm_dir / "after-root.txt").write_text(body.replace("@0", "@1"))
            verdicts = compare_step_dirs(tmux_dir, htm_dir)
            self.assertTrue(verdicts)
            self.assertFalse(divergences(verdicts))


if __name__ == "__main__":
    unittest.main()
