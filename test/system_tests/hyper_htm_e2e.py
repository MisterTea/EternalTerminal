#!/usr/bin/env python3
"""End-to-end tests for htm/htmd native Hyper integration.

Launches stock Hyper (``/Applications/Hyper.app``) with the hyper-htm plugin
and drives splits/tabs via Accessibility. Protocol correctness is checked
against htmd logs.

Skip (exit 77) when Hyper, the plugin, htm/htmd, or Accessibility is
unavailable. Not registered with default CTest; run this file directly
(see AGENTS.md). Expects Hyper's tmux -CC plugin (DCS 1000p from ``htm``).

Environment:
  HYPER_APP         Path to Hyper.app (default: /Applications/Hyper.app)
  HYPER_HTM_PLUGIN  Path to the hyper-htm plugin (default: ~/.hyper_plugins/local/hyper-htm)
  HTM_BIN           Path to the ``htm`` binary (overridden by --htm)
  HTMD_BIN          Path to the ``htmd`` binary (overridden by --htmd)
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from htm_gui_e2e import (  # noqa: E402
    GuiTerminalSession,
    applescript_quote,
    fail,
    kill_htm_daemons,
    process_is_running,
    run_emulator_main,
    run_osascript,
    skip,
    wait_until,
)


def is_hyper_app(app: Path) -> bool:
    return (app / "Contents" / "MacOS" / "Hyper").is_file()


def candidate_hyper_apps() -> list[Path]:
    env = os.environ.get("HYPER_APP")
    paths: list[Path] = []
    if env:
        paths.append(Path(env))
    paths.append(Path("/Applications/Hyper.app"))
    seen = set()
    unique: list[Path] = []
    for path in paths:
        resolved = path.resolve() if path.exists() else path
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def find_hyper_app() -> Path:
    for path in candidate_hyper_apps():
        if path.is_dir() and is_hyper_app(path):
            return path
    skip("no Hyper.app found; install Hyper in /Applications or set HYPER_APP")


def find_hyper_plugin() -> Path:
    env = os.environ.get("HYPER_HTM_PLUGIN")
    if env:
        path = Path(env)
        if (path / "index.js").is_file() and (path / "htm-core.js").is_file():
            return path.resolve()
        fail(f"--plugin not found: {path}")
    installed = Path.home() / ".hyper_plugins" / "local" / "hyper-htm"
    if (installed / "index.js").is_file() and (installed / "htm-core.js").is_file():
        return installed.resolve()
    hyper_js = Path.home() / ".hyper.js"
    if hyper_js.is_file() and "hyper-htm" in hyper_js.read_text(errors="replace"):
        return hyper_js
    skip(
        "hyper-htm plugin is not installed; symlink it to "
        "~/.hyper_plugins/local/hyper-htm and add it to localPlugins in ~/.hyper.js"
    )


NAME = "Hyper"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--hyper-app")
    parser.add_argument("--plugin")


def apply_args(args: argparse.Namespace) -> None:
    if args.hyper_app:
        os.environ["HYPER_APP"] = args.hyper_app
    if args.plugin:
        os.environ["HYPER_HTM_PLUGIN"] = args.plugin
    plugin = find_hyper_plugin()
    print(f"Using plugin={plugin}", flush=True)


def open_session(htm: Path, htmd: Path, args: argparse.Namespace) -> "HyperHtmSession":
    return HyperHtmSession(find_hyper_app(), htm, htmd)


class HyperHtmSession(GuiTerminalSession):
    name = "Hyper"

    def __init__(self, app: Path, htm: Path, htmd: Path):
        super().__init__(htm, htmd)
        self.app = app
        self.was_running = False

    def osascript_hyper(self, body: str) -> str:
        script = f'''
tell application "System Events"
  tell process "Hyper"
    {body}
  end tell
end tell
'''
        return run_osascript(script)

    def focus(self) -> None:
        run_osascript('tell application "Hyper" to activate')
        try:
            self.osascript_hyper("set frontmost to true")
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.15)

    def keystroke(self, keys: str, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.focus()
        run_osascript(
            f'tell application "System Events" to keystroke {keys}{using_clause}'
        )
        time.sleep(0.08)

    def key_code(self, code: int, using: str = "") -> None:
        using_clause = f" using {using}" if using else ""
        self.focus()
        run_osascript(
            f"tell application \"System Events\" to key code {code}{using_clause}"
        )
        time.sleep(0.08)

    def window_count(self) -> int:
        try:
            out = self.osascript_hyper("get count of windows").strip()
            return int(out)
        except (ValueError, subprocess.CalledProcessError):
            return 0

    def start(self, command: str = "") -> None:
        self.was_running = process_is_running("Hyper")
        kill_htm_daemons()
        try:
            run_osascript('tell application "Hyper" to quit')
        except subprocess.CalledProcessError:
            pass
        time.sleep(1.5)
        self.started_at = time.time() - 1.0
        run_osascript('tell application "Hyper" to activate')
        wait_until(
            lambda: self.window_count() > 0,
            15,
            description="Hyper window",
        )
        self.focus()
        time.sleep(0.8)
        launch = command.strip() if command else f"{self.htm} -x"
        self.keystroke(applescript_quote(launch))
        self.key_code(36)

    def after_attach(self) -> None:
        time.sleep(2.0)
        self.focus()
        time.sleep(0.3)

    def stop(self) -> None:
        try:
            self.keystroke('"w"', "{command down, shift down}")
            time.sleep(0.3)
        except subprocess.CalledProcessError:
            pass
        if not self.was_running:
            try:
                run_osascript('tell application "Hyper" to quit')
            except subprocess.CalledProcessError:
                pass
        kill_htm_daemons()


def main() -> int:
    return run_emulator_main(sys.modules[__name__], default_suite="layout")


if __name__ == "__main__":
    raise SystemExit(main())
