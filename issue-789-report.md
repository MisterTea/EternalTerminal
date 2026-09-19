# Issue #789 — Comma-separated SSH-style reverse tunnels

## Findings from source inspection

- `src/base/TunnelUtils.cpp:128` — `parseRangesToRequests` splits by comma (line 138) but always routes each element through `processEtStyleTunnelArg` (line 142), which only handles 2-part `source:destination`. Four-part SSH-style args (`bind:port:host:hostport`) are ignored when comma-separated.
- `test/unit_tests/TunnelUtilsTest.cpp` — existing tests cover single SSH-style and mixed 2-part comma-separated, but no test for multiple four-part entries (e.g. `localhost:8888:1.2.3.4:7777,localhost:9999:5.6.7.8:6666`).
- Duplicate-listener fatal path: `PortForwardHandler::createSource` pushes source handlers without checking for identical bind addresses; fatal path (FATAL_FAIL / STFATAL) exists in listener creation but no minimal reproducer is available.

## Tests added / proposed
- `test/unit_tests/TunnelUtilsTest.cpp`: add `MultipleCommaSeparatedSshStyle` covering two 4-part reverse tunnel entries.

## Fix status
- Parsing fix is possible (modify `parseRangesToRequests` to detect 4-part entries per comma element), but requires design decision on precedence with ET-style and bracket IPv6 handling.
- No minimal reproducer for fatal duplicate-listener path found in source, so elimination deferred.
- Reported back as investigation complete; tests written to cover gap.
