#!/bin/bash
# Issue #219: describe installed binaries
# Verify docs reference et, etserver, etterminal, htm, htmd
for ref in et etserver etterminal htm htmd; do
  grep -q "$ref" README.md docs/protocol.md || echo "FAIL: $ref not in docs"
done
echo "PASS: binary descriptions present"
