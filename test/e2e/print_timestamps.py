"""Prints timestamped output at high volume to test flow control.

Produces ~2.7MB/s of output (1000 lines per burst, bursts every 10ms).

If SIDECAR_FILE is set, writes the current timestamp and cumulative line
count to that file after each burst, enabling external measurement of:
  - process_lag: is the process keeping up with wall clock?
  - lines_per_sec: how many lines has the process written?
"""
import os
import sys
from datetime import datetime
import time

sidecar = os.environ.get("SIDECAR_FILE", "")
total_lines = 0
start_time = time.monotonic()

while True:
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
    print((ts + "\n") * 1000)
    total_lines += 1000
    if sidecar:
        elapsed = time.monotonic() - start_time
        with open(sidecar, "w") as f:
            f.write(f"{ts},{total_lines},{elapsed:.3f}\n")
    time.sleep(0.01)
