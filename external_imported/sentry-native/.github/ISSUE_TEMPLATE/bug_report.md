---
name: Bug report
about: Create a report to help us improve
title: ""
labels: ""
assignees: ""
---

**Description**

<!-- A clear and concise description of what the bug is. -->

**When does the problem happen**

<!-- Put an `x` where relevant. -->

- [ ] During build
- [ ] During run-time
- [ ] When capturing a hard crash

**Environment**

<!-- Some issues are very OS and compiler-dependent. -->

- OS: [e.g. Windows 10, 64-bit]
- Compiler: [e.g. MSVC 19]
- CMake version and config: [e.g. 3.17.2, SENTRY_BACKEND=inproc]

**Steps To Reproduce**

<!-- The best way is to provide a minimal code snippet -->

**Log output**

<!--
For build-related problems, paste the relevant CMake output.
For runtime problems, please use `sentry_options_set_debug(options, true)` and
paste the log output coming from sentry.
When doing so, please make sure to remove or censor your DSN from the http log output.
-->
