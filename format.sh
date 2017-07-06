#!/bin/bash
find ./ -type f | grep "\.[hc]pp" | grep -v /ext/ | grep -v /external/ | grep -v /build/ | xargs clang-format --style=Google -i
