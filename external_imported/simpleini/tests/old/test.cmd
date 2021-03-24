@echo off

Debug\testsi.exe -u -m -l test1-input.ini > test1-blah.ini
fc test1-expected.ini test1-output.ini
if errorlevel 1 goto error

"Debug Unicode\testsi.exe" -u -m -l test1-input.ini > test1-blah.ini
fc test1-expected.ini test1-output.ini
if errorlevel 1 goto error

Release\testsi.exe -u -m -l test1-input.ini > test1-blah.ini
fc test1-expected.ini test1-output.ini
if errorlevel 1 goto error

"Release Unicode\testsi.exe" -u -m -l test1-input.ini > test1-blah.ini
fc test1-expected.ini test1-output.ini
if errorlevel 1 goto error

exit /b 0

:error
echo Failed during test run. Output file doesn't match expected file.
pause
exit /b 1
