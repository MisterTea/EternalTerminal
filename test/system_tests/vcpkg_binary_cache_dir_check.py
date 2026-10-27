#!/usr/bin/env python3
"""Assert vcpkg_build_master.yml creates .vcpkg-binary-cache before cmake.

When VCPKG_DEFAULT_BINARY_CACHE is set, vcpkg requires that path to already
exist as a directory (msgDefaultBinaryCacheRequiresDirectory). On a cache
miss, actions/cache does not create the path, so the workflow must mkdir it
before any cmake step that invokes vcpkg.

On windows-latest the default shell is PowerShell, where mkdir is New-Item
and errors if the path already exists (e.g. after an actions/cache hit)
because GHA sets $ErrorActionPreference = 'Stop'. The ensure step must
therefore be idempotent on both bash and Windows PowerShell — typically by
forcing shell: bash so mkdir -p is safe, or by using New-Item -Force.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

WORKFLOW = (
    Path(__file__).resolve().parents[2]
    / ".github"
    / "workflows"
    / "vcpkg_build_master.yml"
)
CACHE_DIR = ".vcpkg-binary-cache"
MKDIR_RE = re.compile(
    r"mkdir\b[^\n]*" + re.escape(CACHE_DIR),
    re.IGNORECASE,
)
NEW_ITEM_FORCE_RE = re.compile(
    r"New-Item\b[^\n]*-Force\b[^\n]*" + re.escape(CACHE_DIR),
    re.IGNORECASE,
)
CMAKE_STEP_RE = re.compile(
    r"(?m)^ {6}- name: Install dependencies and generate project files"
)
STEP_RE = re.compile(r"(?m)^ {6}- (?:name: |uses: )")
SHELL_BASH_RE = re.compile(r"(?m)^ {8}shell:\s*bash\s*$")
# Bare mkdir -p without an idempotent shell is unsafe on Windows PowerShell.
BARE_MKDIR_P_RE = re.compile(
    r"mkdir\s+-p\b[^\n]*" + re.escape(CACHE_DIR),
    re.IGNORECASE,
)


def step_blocks(text: str) -> list[tuple[int, str]]:
    """Return (start_offset, step_text) for each top-level job step."""
    starts = [m.start() for m in STEP_RE.finditer(text)]
    if not starts:
        return []
    blocks = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(text)
        blocks.append((start, text[start:end]))
    return blocks


def mkdir_step_is_idempotent(block: str) -> bool:
    """True if creating CACHE_DIR in this step is safe when it already exists."""
    if NEW_ITEM_FORCE_RE.search(block):
        return True
    if MKDIR_RE.search(block) and SHELL_BASH_RE.search(block):
        return True
    return False


def main() -> int:
    if not WORKFLOW.is_file():
        print(f"FAIL: missing workflow {WORKFLOW}")
        return 1

    text = WORKFLOW.read_text()
    if "VCPKG_DEFAULT_BINARY_CACHE" not in text:
        print("FAIL: workflow does not set VCPKG_DEFAULT_BINARY_CACHE")
        return 1
    if CACHE_DIR not in text:
        print(f"FAIL: workflow does not reference {CACHE_DIR}")
        return 1

    cmake_match = CMAKE_STEP_RE.search(text)
    if not cmake_match:
        print("FAIL: could not find cmake/generate project files steps")
        return 1

    prefix = text[: cmake_match.start()]
    mkdir_before_cmake = False
    idempotent_mkdir = False
    bare_mkdir_p = False
    for _, block in step_blocks(prefix):
        # Only count an explicit mkdir / New-Item of the binary-cache path.
        if MKDIR_RE.search(block) or NEW_ITEM_FORCE_RE.search(block):
            mkdir_before_cmake = True
            if mkdir_step_is_idempotent(block):
                idempotent_mkdir = True
            if BARE_MKDIR_P_RE.search(block) and not SHELL_BASH_RE.search(block):
                bare_mkdir_p = True
            break

    if not mkdir_before_cmake:
        print(
            "FAIL: workflow sets VCPKG_DEFAULT_BINARY_CACHE to "
            f"{CACHE_DIR} but does not create that directory before cmake "
            "invokes vcpkg (needed on cache miss)"
        )
        return 1

    if bare_mkdir_p or not idempotent_mkdir:
        print(
            "FAIL: ensure-directory step uses mkdir that is not idempotent "
            "under Windows PowerShell (mkdir/New-Item errors if "
            f"{CACHE_DIR} already exists after an actions/cache hit). "
            "Use shell: bash with mkdir -p, or New-Item -Force."
        )
        return 1

    print(
        f"PASS: {WORKFLOW.name} creates {CACHE_DIR} before cmake/vcpkg steps "
        "(idempotent on bash and Windows PowerShell)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
