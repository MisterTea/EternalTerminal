find ./ -type f | grep "\.[hc]pp" | grep -v /external/ | xargs clang-format --style=Google -i
