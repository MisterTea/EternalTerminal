[CmdletBinding()]
param(
    $badParam,
    [Parameter(Mandatory=$False)][switch]$win64 = $false,
    [Parameter(Mandatory=$False)][string]$withVSPath = "",
    [Parameter(Mandatory=$False)][string]$withWinSDK = "",
    [Parameter(Mandatory=$False)][switch]$disableMetrics = $false
)
Set-StrictMode -Version Latest
# Powershell2-compatible way of forcing named-parameters
if ($badParam)
{
    if ($disableMetrics -and $badParam -eq "1")
    {
        Write-Warning "'disableMetrics 1' is deprecated, please change to 'disableMetrics' (without '1')."
    }
    else
    {
        throw "Only named parameters are allowed."
    }
}

if ($win64)
{
    Write-Warning "-win64 no longer has any effect; ignored."
}

if (-Not [string]::IsNullOrWhiteSpace($withVSPath))
{
    Write-Warning "-withVSPath no longer has any effect; ignored."
}

if (-Not [string]::IsNullOrWhiteSpace($withWinSDK))
{
    Write-Warning "-withWinSDK no longer has any effect; ignored."
}

$scriptsDir = split-path -parent $script:MyInvocation.MyCommand.Definition
$vcpkgRootDir = $scriptsDir
while (!($vcpkgRootDir -eq "") -and !(Test-Path "$vcpkgRootDir\.vcpkg-root"))
{
    Write-Verbose "Examining $vcpkgRootDir for .vcpkg-root"
    $vcpkgRootDir = Split-path $vcpkgRootDir -Parent
}

Write-Verbose "Examining $vcpkgRootDir for .vcpkg-root - Found"

# Read the vcpkg-tool config file to determine what release to download
$Config = ConvertFrom-StringData (Get-Content "$PSScriptRoot\vcpkg-tool-metadata.txt" -Raw)
$versionDate = $Config.VCPKG_TOOL_RELEASE_TAG

if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_IDENTIFIER -match "ARMv[8,9] \(64-bit\)") {
    & "$scriptsDir/tls12-download-arm64.exe" github.com "/microsoft/vcpkg-tool/releases/download/$versionDate/vcpkg-arm64.exe" "$vcpkgRootDir\vcpkg.exe"
} else {
    & "$scriptsDir/tls12-download.exe" github.com "/microsoft/vcpkg-tool/releases/download/$versionDate/vcpkg.exe" "$vcpkgRootDir\vcpkg.exe"
}

Write-Host ""

if ($LASTEXITCODE -ne 0)
{
    Write-Error "Downloading vcpkg.exe failed. Please check your internet connection, or consider downloading a recent vcpkg.exe from https://github.com/microsoft/vcpkg-tool with a browser."
    throw
}

& "$vcpkgRootDir\vcpkg.exe" version --disable-metrics

if ($disableMetrics)
{
    Set-Content -Value "" -Path "$vcpkgRootDir\vcpkg.disable-metrics" -Force
}
elseif (-Not (Test-Path "$vcpkgRootDir\vcpkg.disable-metrics"))
{
    # Note that we intentionally leave any existing vcpkg.disable-metrics; once a user has
    # opted out they should stay opted out.
    Write-Host @"
Telemetry
---------
vcpkg collects usage data in order to help us improve your experience.
The data collected by Microsoft is anonymous.
You can opt-out of telemetry by re-running the bootstrap-vcpkg script with -disableMetrics,
passing --disable-metrics to vcpkg on the command line,
or by setting the VCPKG_DISABLE_METRICS environment variable.

Read more about vcpkg telemetry at docs/about/privacy.md
"@
}
