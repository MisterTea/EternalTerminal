#!/usr/bin/env python3
"""Verify Ghostty's iTerm2-compatible tmux control gateway."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from ghostty_htm_e2e import (
    GhosttyHtmSession,
    apply_args,
    find_ghostty_app,
)
from htm_gui_e2e import (
    find_htm_bin,
    find_htmd_bin,
    find_tmux_bin,
    kill_named,
    parse_mux,
    run_control_plane_suite,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--htm")
    parser.add_argument("--htmd")
    parser.add_argument("--ghostty")
    parser.add_argument("--ghostty-app")
    parser.add_argument("--mux", default="both")
    parser.add_argument("--record-video", nargs="?", const="/tmp/htm-e2e-videos")
    args = parser.parse_args()
    apply_args(args)

    htm = find_htm_bin(args.htm)
    htmd = find_htmd_bin(args.htmd, htm)
    tmux = find_tmux_bin()
    muxes = parse_mux(args.mux)
    video_dir = Path(args.record_video).resolve() if args.record_video else None
    text_dir = video_dir or Path("/tmp/htm-e2e-videos")
    text_dir.mkdir(parents=True, exist_ok=True)

    for mux in muxes:
        session = GhosttyHtmSession(find_ghostty_app(), htm, htmd)
        session.mux = mux
        session.tmux_bin = tmux
        session.tmux_socket = f"et-control-{os.getpid()}-{mux}"
        session.text_dir = text_dir
        session.video_dir = video_dir
        print(f"== Ghostty / control-plane / {mux} ==", flush=True)
        try:
            run_control_plane_suite(session)
        finally:
            session.end_htm_window_recording()
            session.stop()
            if mux == "htm":
                kill_named("htmd")
            else:
                session.shutdown_multiplexer()
            session.warn_leftovers()

    print(f"PASS: Ghostty control plane mux={','.join(muxes)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
