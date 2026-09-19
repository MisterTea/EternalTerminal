# Issue #428: Unified Session Management for Unattached Sessions

## Problem
List unattached sessions requires session identity, last-attachment time, listing, and expiry in the same management model.

## Design
Extend `TerminalServer` (derived from `ServerConnection`) with a `SessionRegistry` that:
- Tracks session identity (`string id`, `string name`)
- Records last-attachment time (`std::chrono::system_clock::time_point`)
- Provides unified listing (`listSessions()`)
- Applies expiry rules coordinated with #779/#365.

Avoid separate one-off introspection. Reuse existing `clientConnections` and `clientKeys` structures in `ServerConnection`.

## Files Modified / Added
- `src/terminal/SessionRegistry.hpp` (new unified model)
- `tests/session_registry_test.cpp` (tests)
