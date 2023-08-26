Start-Sleep -Milliseconds 1 # See: https://stackoverflow.com/a/49859001

$LLVM_MINGW_RELEASE = "20220906";
$LLVM_MINGW_PKG = "llvm-mingw-${LLVM_MINGW_RELEASE}-ucrt-x86_64"
$LLVM_MINGW_DL_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_RELEASE}/${LLVM_MINGW_PKG}.zip"
$LLVM_MINGW_DL_SHA512 = "3c724dd0663558c7247d2cdde196b37dc54e49fb8c4065aef0274d69d92d2d023440505fe6f23e83476d56f4a39c105d551f998a4342e823a2d2705d7a73fe7c"
$DL_BASEDIR = "$env:GITHUB_WORKSPACE\dl"
$LLVM_MINGW_DL_PATH = "${DL_BASEDIR}\llvm-mingw.zip"
if (!(Test-Path -Path "$DL_BASEDIR")) { New-Item -ItemType Directory -Force -Path "$DL_BASEDIR" }

# Download LLVM-mingw
$CurlArguments = '-s', '-Lf', '-o', "${LLVM_MINGW_DL_PATH}", "${LLVM_MINGW_DL_URL}"
& curl.exe @CurlArguments
$dl_zip_hash = Get-FileHash -LiteralPath "${LLVM_MINGW_DL_PATH}" -Algorithm SHA512
if ($dl_zip_hash.Hash -eq $LLVM_MINGW_DL_SHA512) {
	Write-Host "Successfully downloaded LLVM-mingw .zip"
} Else {
	Write-Error "The downloaded LLVM-mingw zip hash '$($dl_zip_hash.Hash)' does not match the expected hash: '$LLVM_MINGW_DL_SHA512'"
}

# Extract LLVM-mingw
Write-Host "Extracting LLVM-mingw..."
$LLVM_MINGW_INSTALL_PATH = "$env:GITHUB_WORKSPACE\buildtools\llvm-mingw"
New-Item -ItemType Directory -Force -Path "${LLVM_MINGW_INSTALL_PATH}"
Expand-Archive -LiteralPath "${LLVM_MINGW_DL_PATH}" -DestinationPath "${LLVM_MINGW_INSTALL_PATH}"
# Export the LLVM-mingw install path
$LLVM_MINGW_INSTALL_PATH = "${LLVM_MINGW_INSTALL_PATH}\${LLVM_MINGW_PKG}"
echo "LLVM_MINGW_INSTALL_PATH=${LLVM_MINGW_INSTALL_PATH}" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
# Prepend bin path to the system PATH
echo "Path to LLVM-mingw bin folder: ${LLVM_MINGW_INSTALL_PATH}\bin"
echo "${LLVM_MINGW_INSTALL_PATH}\bin" | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append

# Download ninja-build
$NINJA_DL_URL = "https://github.com/ninja-build/ninja/releases/download/v1.11.1/ninja-win.zip"
$NINJA_DL_SHA512 = "a700e794c32eb67b9f87040db7f1ba3a8e891636696fc54d416b01661c2421ff46fa517c97fd904adacdf8e621df3e68ea380105b909ae8b6651a78ae7eb3199"
$NINJA_DL_PATH = "${DL_BASEDIR}\ninja-win.zip"
$CurlArguments = '-s', '-Lf', '-o', "${NINJA_DL_PATH}", "${NINJA_DL_URL}"
& curl.exe @CurlArguments
$ninja_zip_hash = Get-FileHash -LiteralPath "${NINJA_DL_PATH}" -Algorithm SHA512
if ($ninja_zip_hash.Hash -eq $NINJA_DL_SHA512) {
	Write-Host "Successfully downloaded Ninja-Build .zip"
} Else {
	Write-Error "The downloaded Ninja-build zip hash '$($ninja_zip_hash.Hash)' does not match the expected hash: '$NINJA_DL_SHA512'"
}

Write-Host "Extracting Ninja-Build..."
$NINJA_INSTALL_PATH = "$env:GITHUB_WORKSPACE\buildtools\ninja"
New-Item -ItemType Directory -Force -Path "${NINJA_INSTALL_PATH}"
Expand-Archive -LiteralPath "${NINJA_DL_PATH}" -DestinationPath "${NINJA_INSTALL_PATH}"
# Export the NINJA executable path
echo "NINJA_INSTALL_PATH=${NINJA_INSTALL_PATH}" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

# Add CMAKE_DEFINES
echo "CMAKE_DEFINES=-DCMAKE_C_COMPILER=${env:MINGW_PKG_PREFIX}-gcc -DCMAKE_CXX_COMPILER=${env:MINGW_PKG_PREFIX}-g++ -DCMAKE_RC_COMPILER=${env:MINGW_PKG_PREFIX}-windres -DCMAKE_ASM_MASM_COMPILER=${env:MINGW_ASM_MASM_COMPILER} -GNinja" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
