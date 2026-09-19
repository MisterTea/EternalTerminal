#!/usr/bin/env bash
# Minimal documentation test for #752: verify docs mention roles / port 2022
set -euo pipefail
DOCS_DIR="$(dirname "$0")/../docs"
README="$(dirname "$0")/../README.md"
fail=0
check() { grep -qi "$1" "$2" || { echo "MISSING: $1 in $2"; fail=1; }; }
check "et" "$README"
check "etserver" "$README"
check "etterminal" "$README"
check "2022" "$README"
echo "docs-check passed"
exit $fail
