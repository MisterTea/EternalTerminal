# Issue #365 — Reattach to session (findings)

Triage (ISSUE_TRIAGE.md): strategic; keep open until named listing/attachment and stale-client takeover behavior land and are documented.

State of repo (branch issue-365, commit db4f6f631):
- Named sessions implemented (newSession/renameSession/listSessions/attachSession in MultiplexerState).
- Control mode supports attach-session / list-sessions (ControlCommands.cpp).
- Reattach / recovery already works in daemon (evicts old bridge, sends snapshot).
- Missing: documented stale-client takeover semantics and attachment-by-name workflow.

Work done:
- Added this findings doc.
- Added test/issue_365_reattach_notes.md describing expected named-attach behavior.

No new source code needed; feature is present. Closing prerequisites: docs.
