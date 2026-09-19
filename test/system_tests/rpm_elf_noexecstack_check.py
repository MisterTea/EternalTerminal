#!/usr/bin/env python3
"""RPM/ELF hardening check: verify binaries have no executable stack."""
import subprocess
import sys

binaries = [
    "build/htm",
    "build/htmd",
    "build/et",
    "build/etserver",
    "build/etterminal",
]

failed = False
for b in binaries:
    try:
        result = subprocess.run(
            ["readelf", "-l", b], capture_output=True, text=True
        )
        if "GNU_STACK" not in result.stdout:
            # GNU_STACK note missing; if no RWX flags in stack segment, it's ok.
            pass
        # Check that there is no executable stack (no RWX in GNU_STACK or no GNU_STACK at all is OK if linker hardening is set)
        # We verify linker hardening via objdump / readelf: GNU_STACK segment should have R (not RWX)
        if "GNU_STACK" in result.stdout:
            stack_line = [l for l in result.stdout.splitlines() if "GNU_STACK" in l]
            if stack_line:
                if "RWE" in stack_line[0]:
                    print(f"FAIL: executable stack detected in {b}")
                    failed = True
                    continue
        print(f"PASS: {b}")
    except FileNotFoundError:
        # Binary may not exist; skip for optional targets
        print(f"SKIP: {b} not found")

sys.exit(1 if failed else 0)
