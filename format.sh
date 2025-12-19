#!/bin/bash
find ./src ./test -type f | grep "\.[hc]pp" | xargs clang-format-18 --style=Google -i
