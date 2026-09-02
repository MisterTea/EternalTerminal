#!/usr/bin/env python3
"""iTerm2 GUI stress: several HTM panes/tabs with concurrent bulk I/O.

Launches stock iTerm2 (``/Applications/iTerm.app``) the same way as
``iterm2_htm_e2e.py``. Creates splits and tabs, starts large background
printers on two panes, injects keys while output is flowing, and requires
interleaved htmd writes. Skip (77) when iTerm2+HTM is unavailable.
Not registered with default CTest; run this file directly (see AGENTS.md).
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from iterm2_htm_e2e import (  # noqa: E402
    ITermHtmSession,
    assert_no_htmd,
    assert_no_ipc,
    fail,
    find_htmd_bin,
    find_htm_bin,
    find_iterm_app,
    ipc_path,
    log_has_typed,
    pids_named,
    skip,
)


def run_stress(session: ITermHtmSession) -> None:
    session.start(f"{session.htm} -x")
    session.wait_init()
    deadline = time.time() + 8
    while time.time() < deadline and session.tab_count() < 2:
        time.sleep(0.25)
    session.focus()
    print("OK: attached", flush=True)

    session.keystroke('"d"', "command down")
    session.wait_log(
        lambda text: "split-window" in text,
        20,
        "NEW_SPLIT after Cmd+D",
    )
    session.keystroke('"t"', "command down")
    session.wait_log(
        lambda text: "new-window" in text,
        20,
        "NEW_TAB after Cmd+T",
    )
    session.keystroke('"d"', "{command down, shift down}")
    session.wait_log(
        lambda text: text.count("split-window") >= 2,
        20,
        "second NEW_SPLIT",
    )
    time.sleep(0.5)
    print("OK: tabs and splits created", flush=True)

    stamp = int(time.time())
    mark_a = f"STA{stamp}"
    mark_b = f"STB{stamp}"

    def echo_tag(tag: str) -> str:
        session.keystroke(f'"echo {tag}"')
        session.key_code(36)
        session.wait_log(lambda text: log_has_typed(text, tag), 12, f"echo {tag}")
        return tag

    pane_a = echo_tag(mark_a)
    pane_b = pane_a
    for switch in (session.next_pane, session.previous_pane, session.previous_tab):
        switch()
        pane_b = echo_tag(f"{mark_b}{switch.__name__}")
        if pane_b != pane_a:
            break
    if pane_b == pane_a:
        fail("could not focus a second HTM pane")
    print(f"OK: two panes {pane_a[:8]}… / {pane_b[:8]}…", flush=True)

    session.keystroke('"yes STBULK1 &"')
    session.key_code(36)
    time.sleep(0.25)

    switched = False
    for switch in (session.previous_pane, session.next_pane, session.previous_tab):
        switch()
        uid = echo_tag(f"SW{stamp}{switch.__name__}")
        if uid != pane_b:
            switched = True
            break
    if not switched:
        fail("could not move off the first bulk pane before starting the second")
    session.keystroke('"yes STBULK0 &"')
    session.key_code(36)
    time.sleep(0.3)
    for i in range(8):
        session.keystroke(f'"echo STKEY{stamp}_{i}"')
        session.key_code(36)
        time.sleep(0.08)

    session.wait_log(
        lambda text: text.count("control command: send") >= 4
        and log_has_typed(text, "STBULK"),
        25,
        "bulk send-keys while printers run",
    )
    if session.proc.poll() is not None:
        fail("iTerm2 exited during bulk I/O")
    if not pids_named("htmd"):
        fail("htmd died during bulk I/O")
    print("OK: concurrent bulk I/O", flush=True)

    for pid in pids_named("htmd"):
        try:
            os.kill(pid, 15)
        except OSError:
            pass
    assert_no_htmd()
    assert_no_ipc()
    print("OK: shutdown", flush=True)


def main() -> int:
    if sys.platform != "darwin":
        skip("iTerm2 HTM stress requires macOS")
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--iterm2-app")
    args = parser.parse_args()
    if args.iterm2_app:
        os.environ["ITERM2_APP"] = args.iterm2_app
    htm = find_htm_bin(args.htm)
    htmd = find_htmd_bin(args.htmd, htm)
    app = find_iterm_app()
    print(f"Using iTerm2={app}", flush=True)
    print(f"Using htm={htm}", flush=True)
    print(f"Using htmd={htmd}", flush=True)
    session = ITermHtmSession(app, htm, htmd)
    try:
        run_stress(session)
    finally:
        session.stop()
        for pid in pids_named("htmd"):
            try:
                os.kill(pid, 15)
            except OSError:
                pass
    print("PASS: iTerm2 htm stress", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
