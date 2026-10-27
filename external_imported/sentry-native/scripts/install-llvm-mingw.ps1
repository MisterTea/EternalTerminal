Start-Sleep -Milliseconds 1 # See: https://stackoverflow.com/a/49859001

$LLVM_MINGW_RELEASE = "20251202";
$LLVM_MINGW_PKG = "llvm-mingw-${LLVM_MINGW_RELEASE}-ucrt-x86_64"
$LLVM_MINGW_DL_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_RELEASE}/${LLVM_MINGW_PKG}.zip"
$LLVM_MINGW_DL_SHA256 = "f2adfc859287f4051abf5c23c7476b16a982d0f8e6c32aa2f929a06535454f4b"
$DL_BASEDIR = "$env:GITHUB_WORKSPACE\dl"
$LLVM_MINGW_DL_PATH = "${DL_BASEDIR}\llvm-mingw.zip"
if (!(Test-Path -Path "$DL_BASEDIR")) { New-Item -ItemType Directory -Force -Path "$DL_BASEDIR" }

# Download LLVM-mingw
$CurlArguments = '-s', '-Lf', '-o', "${LLVM_MINGW_DL_PATH}", "${LLVM_MINGW_DL_URL}"
& curl.exe @CurlArguments
$dl_zip_hash = Get-FileHash -LiteralPath "${LLVM_MINGW_DL_PATH}" -Algorithm SHA256
if ($dl_zip_hash.Hash -eq $LLVM_MINGW_DL_SHA256) {
	Write-Host "Successfully downloaded LLVM-mingw .zip"
}
Else {
	Write-Error "The downloaded LLVM-mingw zip hash '$($dl_zip_hash.Hash)' does not match the expected hash: '$LLVM_MINGW_DL_SHA256'"
}

# Extract LLVM-mingw
Write-Host "Extracting LLVM-mingw..."
$LLVM_MINGW_INSTALL_PATH = "$env:GITHUB_WORKSPACE\buildtools\llvm-mingw"
New-Item -ItemType Directory -Force -Path "${LLVM_MINGW_INSTALL_PATH}"
Expand-Archive -LiteralPath "${LLVM_MINGW_DL_PATH}" -DestinationPath "${LLVM_MINGW_INSTALL_PATH}"
# Export the LLVM-mingw install path
$LLVM_MINGW_INSTALL_PATH = "${LLVM_MINGW_INSTALL_PATH}\${LLVM_MINGW_PKG}"
"LLVM_MINGW_INSTALL_PATH=${LLVM_MINGW_INSTALL_PATH}" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
# Prepend bin path to the system PATH
Write-Host "Path to LLVM-mingw bin folder: ${LLVM_MINGW_INSTALL_PATH}\bin"
"${LLVM_MINGW_INSTALL_PATH}\bin" | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append

# Download ninja-build
$NINJA_DL_URL = "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip"
$NINJA_DL_SHA512 = "d6715c6458d798bcb809f410c0364dabd937b5b7a3ddb4cd5aba42f9fca45139b2a8a3e7fd9fbd88fd75d298ed99123220b33c7bdc8966a9d5f2a1c9c230955f"
$NINJA_DL_PATH = "${DL_BASEDIR}\ninja-win.zip"
$CurlArguments = '-s', '-Lf', '-o', "${NINJA_DL_PATH}", "${NINJA_DL_URL}"
& curl.exe @CurlArguments
$ninja_zip_hash = Get-FileHash -LiteralPath "${NINJA_DL_PATH}" -Algorithm SHA512
if ($ninja_zip_hash.Hash -eq $NINJA_DL_SHA512) {
	Write-Host "Successfully downloaded Ninja-Build .zip"
}
Else {
	Write-Error "The downloaded Ninja-build zip hash '$($ninja_zip_hash.Hash)' does not match the expected hash: '$NINJA_DL_SHA512'"
}

Write-Host "Extracting Ninja-Build..."
$NINJA_INSTALL_PATH = "$env:GITHUB_WORKSPACE\buildtools\ninja"
New-Item -ItemType Directory -Force -Path "${NINJA_INSTALL_PATH}"
Expand-Archive -LiteralPath "${NINJA_DL_PATH}" -DestinationPath "${NINJA_INSTALL_PATH}"

# Export the NINJA executable path
"NINJA_INSTALL_PATH=${NINJA_INSTALL_PATH}" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
"PATH=${NINJA_INSTALL_PATH}; $env:PATH" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

# export CMAKE_DEFINES to the runner environment
$cmakeDefines = @()

if ($env:CMAKE_DEFINES) {
  $cmakeDefines += $env:CMAKE_DEFINES
}

$cmakeDefines += @(
  "-DCMAKE_C_COMPILER=$($env:MINGW_PKG_PREFIX)-gcc"
  "-DCMAKE_CXX_COMPILER=$($env:MINGW_PKG_PREFIX)-g++"
  "-DCMAKE_RC_COMPILER=$($env:MINGW_PKG_PREFIX)-windres"
  "-DCMAKE_ASM_MASM_COMPILER:FILEPATH=$($LLVM_MINGW_INSTALL_PATH -replace '\\','/')/bin/$($env:MINGW_ASM_MASM_COMPILER).exe"
  "-DCMAKE_ASM_MASM_FLAGS=$env:MINGW_ASM_MASM_FLAGS"
)

"CMAKE_DEFINES=$($cmakeDefines -join ' ')" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append


