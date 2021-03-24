#!/usr/bin/env bash

CFILES=`git diff-index --cached --name-only HEAD | grep -E "^(examples|include|src|tests/unit).*\.(c|h|cpp)$"`
PYFILES=`git diff-index --cached --name-only HEAD | grep -E "^tests.*\.py$"`

if [ -n "$CFILES" ]; then
    clang-format -i $CFILES
fi
if [ -n "$PYFILES" ]; then
    .venv/bin/black $PYFILES
fi
if [ -n "$CFILES$PYFILES" ]; then
    git add $CFILES $PYFILES
fi
