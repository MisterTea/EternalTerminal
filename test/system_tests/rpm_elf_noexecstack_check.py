#!/usr/bin/env python3
"""RPM/ELF hardening check: verify binaries have no executable stack."""
import subprocess
import sys

if len(sys.argv) < 2:
    raise SystemExit("usage: rpm_elf_noexecstack_check.py BINARY...")

binaries = sys.argv[1:]

failed = False
for b in binaries:
    try:
        result = subprocess.run(
            ["readelf", "-W", "-l", b], capture_output=True, text=True,
            check=True,
        )
        stack_lines = [
            line for line in result.stdout.splitlines() if "GNU_STACK" in line
        ]
        if not stack_lines:
            print(f"FAIL: {b} has no GNU_STACK program header")
            failed = True
            continue
        if any("RWE" in line for line in stack_lines):
            print(f"FAIL: executable stack detected in {b}")
            failed = True
            continue
        print(f"PASS: {b}")
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        print(f"FAIL: could not inspect {b}: {error}")
        failed = True

sys.exit(1 if failed else 0)
