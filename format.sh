#!/bin/bash
find ./src ./test -type f | grep "\.[hc]pp" | xargs clang-format --style=Google -i
