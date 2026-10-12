#!/usr/bin/env python3
"""Use the pinned vcpkg recipes with assembly disabled for MSan crypto libs."""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[3]
destination = root / ".msan-ports"
for port in ("openssl", "libsodium"):
    shutil.copytree(root / "external/vcpkg/ports" / port, destination / port,
                    dirs_exist_ok=True)
    recipe = destination / port / "portfile.cmake"
    source = recipe.read_text()
    if port == "openssl":
        before = "    no-tests\n"
        after = "    no-tests\n    no-asm\n"
    else:
        before = "    vcpkg_make_configure(\n"
        after = "    list(APPEND OPTIONS --disable-asm)\n\n" + before
    if source.count(before) != 1:
        raise RuntimeError(f"Pinned {port} recipe changed; review the MSan overlay")
    recipe.write_text(source.replace(before, after))

shutil.copytree(root / ".github/ci/msan/ports", destination, dirs_exist_ok=True)
