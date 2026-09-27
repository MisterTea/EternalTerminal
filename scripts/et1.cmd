@echo off
rem Cursor remote-SSH stand-in. Close the remote session on client hangup, and
rem drop it if the client stays disconnected for more than one week.
et --close-on-hangup --disconnect-timeout=10080 %*
