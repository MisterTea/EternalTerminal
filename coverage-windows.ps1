[CmdletBinding()]
param(
  [string]$BuildDirectory = "build",
  [string]$Configuration = "RelWithDebInfo",
  [double]$MinimumLineCoverage = 80
)

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot
$buildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
  [IO.Path]::GetFullPath($BuildDirectory)
} else {
  [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
}
$testExecutable = Join-Path $buildPath "$Configuration\et-test.exe"
$outputDirectory = Join-Path $buildPath "coverage-windows"
$coverageFile = Join-Path $outputDirectory "coverage.cobertura.xml"
$coverageLog = Join-Path $outputDirectory "coverage.log"

$collector = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" `
  -Recurse -Filter "Microsoft.CodeCoverage.Console.exe" -ErrorAction SilentlyContinue |
  Sort-Object FullName -Descending |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $collector) {
  throw "Microsoft.CodeCoverage.Console.exe was not found. Install Visual Studio Code Coverage tools."
}

cmake --build $buildPath --config $Configuration --target et-test --parallel
if ($LASTEXITCODE -ne 0) {
  throw "The Windows unit-test build failed."
}

New-Item -ItemType Directory -Force $outputDirectory | Out-Null
& $collector collect $testExecutable --reporter compact `
  --output $coverageFile --output-format cobertura `
  --log-file $coverageLog --nologo
if ($LASTEXITCODE -ne 0) {
  throw "The Windows unit tests or coverage collection failed."
}

[xml]$report = Get-Content $coverageFile
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "src")) + [IO.Path]::DirectorySeparatorChar
$lines = @{}
foreach ($class in $report.coverage.packages.package.classes.class) {
  $filename = [string]$class.filename
  if (-not $filename.StartsWith($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
      -not $filename.EndsWith(".cpp", [StringComparison]::OrdinalIgnoreCase)) {
    continue
  }
  foreach ($line in @($class.lines.line)) {
    $key = "${filename}:$($line.number)"
    $hits = [int]$line.hits
    if (-not $lines.ContainsKey($key) -or $hits -gt $lines[$key]) {
      $lines[$key] = $hits
    }
  }
}

if ($lines.Count -eq 0) {
  throw "The coverage report contains no Eternal Terminal source lines."
}

$covered = @($lines.Values | Where-Object { $_ -gt 0 }).Count
$rate = 100.0 * $covered / $lines.Count
Write-Host ("Windows C++ implementation line coverage: {0:N2}% ({1}/{2})" -f $rate, $covered, $lines.Count)
Write-Host "Cobertura report: $coverageFile"

if ($rate -lt $MinimumLineCoverage) {
  throw ("Windows C++ implementation line coverage {0:N2}% is below the required {1:N2}%." -f $rate, $MinimumLineCoverage)
}
