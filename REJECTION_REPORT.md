# Rejection Report: Issue #760 - Ctrl-D Coredump on Oracle Linux ARM

## Summary
Issue #760 describes a recent, repeatable architecture-specific shutdown crash on Oracle Linux ARM. The triage file (`ISSUE_TRIAGE.md`) is missing from the repository, and the issue requires:

1. Exact package version from the reporter
2. Symbolized backtrace of the crash

Without this critical data, a testable scenario cannot be constructed and no regression test can be added.

## Evidence
- **Missing File**: `ISSUE_TRIAGE.md` is not present in `/Users/jjg/github/EternalTerminal` (the repository root).
- **Triage Content**: Cannot be read – the file does not exist.
- **Reporter Data**: No symbolized backtrace or exact package version has been provided.

## Criteria Evaluation

| Criterion | Status | Evidence |
|-----------|--------|----------|
| **criterion-1**: Implement the requested change without widening scope | ❌ Not Satisfied | Cannot implement a regression test without reporter's exact package version and backtrace. |
| **criterion-2**: Return evidence sufficient for an independent acceptance review | ❌ Not Satisfied | No changes were made; no test file was added. |

## Residual Risks
- **High**: The underlying crash on Oracle Linux ARM ARM architecture remains unreproduced without the specific package version and backtrace. Adding a test blindly would risk introducing false positives or masking the real issue.
- **Medium**: The issue may require deeper investigation into the specific Oracle Linux ARM environment (kernel version, library versions, etc.) beyond what can be captured from the current lack of data.

## Manual Notes
The `ISSUE_TRIAGE.md` file referenced in the task description is missing from the repository. Per the user instruction ("If impossible (needs reporter data), skip per user instruction"), no regression test was added and no PR was created. The issue must be reopened with the reporter providing the exact package version and symbolized backtrace before a testable solution can be implemented.

## Conclusion
**Skip** – The issue cannot be resolved without the required reporter data (package version + symbolized backtrace). The triage file is missing, preventing any actionable development work.
