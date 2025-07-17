<#
.SYNOPSIS
    A helper script to execute Native SDK tests in Windows PowerShell.
.DESCRIPTION
    Maintains the venv, wraps pytest for execution, and invokes update_test_discovery.ps1 to collect newly added unit tests.
#>
param (
    ## Lets you run only the unit tests (default: false)
    [switch]$Unit = $false,
    ## Cleanly reinstalls packages into the venv (default: false)
    [switch]$Clean = $false,
    ## Lets you limit the test discovery to part of a name (default: all)
    [string]$Keyword = "",
    ## Defines the number of parallel runners via pytest-xdist. This is highly experimental since tests remove database paths. (default: 1)
    [int]$Parallelism = 1,
    ## Disables tests that require the crashpad WER module. (default: false)
    [switch]$WithoutCrashpadWer = $false,
    ## Disables stdout/stderr capture through pytest (default: false)
    [switch]$DisableCapture = $false,
    ## Defines the maximum number of failing tests before the test session is stopped. 0 means infinite. Will not do what you expect, together with Parallelism > 1 (default: 0)
    [int]$MaxFail = 0,
    ## Repeatedly runs the test suite (default: false)
    [switch]$Repeat = $false
)

$update_test_discovery = Join-Path -Path $PSScriptRoot -ChildPath "update_test_discovery.ps1"
& $update_test_discovery

if ($Clean -or -not (Test-Path .\.venv))
{
    Remove-Item -Recurse -Force .\.venv\ -ErrorAction SilentlyContinue
    python3.exe -m venv .venv
    .\.venv\Scripts\pip.exe install --upgrade --requirement .\tests\requirements.txt
}

$pytestCommand = ".\.venv\Scripts\pytest.exe .\tests\ --verbose --maxfail=$MaxFail"

if ($DisableCapture) {
    $pytestCommand += " --capture=no"
}

if ($Parallelism -gt 1)
{
    $pytestCommand += " -n $Parallelism"
}

if (-not $WithoutCrashpadWer -and -not $Unit)
{
    $pytestCommand += " --with_crashpad_wer"
}

if ($Keyword)
{
    $pytestCommand += " -k `"$Keyword`""
}
elseif ($Unit)
{
    $pytestCommand += " -k `"unit`""
}

do {
    try {
        Invoke-Expression $pytestCommand

        Start-Sleep -Seconds 1
    } catch {
        Write-Host "An error occurred: $_"
    }
} while($Repeat)
