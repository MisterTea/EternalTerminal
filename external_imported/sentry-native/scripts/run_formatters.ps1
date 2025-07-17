$clangFormat = ".venv\Scripts\clang-format.exe"
$black = ".venv\Scripts\black.exe"

$patterns = @(
    "examples\*.c",
    "include\*.h",
    "src\*.c",
    "src\*.h",
    "src\*\*.c",
    "src\*\*.cpp",
    "src\*\*.h",
    "tests\unit\*.c",
    "tests\unit\*.h"
)

$files = $patterns | ForEach-Object {
    Get-ChildItem -Path $_ -File -ErrorAction SilentlyContinue
}

if ($files.Count -eq 0) {
    Write-Host "No files matched for clang-format"
} else {
    $files | ForEach-Object {
        & $clangFormat -i $_.FullName
    }
}

& $black tests
