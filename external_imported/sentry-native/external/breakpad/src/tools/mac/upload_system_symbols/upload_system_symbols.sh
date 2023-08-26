#!/bin/bash

# Copyright 2023 Google LLC
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google LLC nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Finds the dyld_shared_cache on a system, extracts it, and dumps the symbols
# in Breakpad format to the directory passed as the first argument
# The script must be in the same directory as `dump_syms`,
# `upload_system_symbols` and `dsc_extractor` binaries.
# Exits with 0 if all supported architectures for this OS version were found and
# dumped, and nonzero otherwise.

set -ex

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <destination_directory>" >& 2
  exit 1
fi

destination_dir="$1"

dir="$(dirname "$0")"
dir="$(cd "${dir}"; pwd)"
major_version=$(sw_vers -productVersion | cut -d . -f 1)
if [[ "${major_version}" -lt 13 ]]; then
  dsc_directory="/System/Library/dyld"
else
  dsc_directory="/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld"
fi

working_dir=$(mktemp -d)
mkdir "${destination_dir}"
trap 'rm -rf "${working_dir}" "${destination_dir}"' EXIT

architectures=(x86_64h)
missing_architectures=()
# macOS >= 13 on arm64 still has a x86_64 cache for Rosetta.
if [[ "${major_version}" -lt 13 ]] || [[ $(uname -p) == "arm" ]]; then
  architectures+=( x86_64 )
fi
if [[ "${major_version}" -ge 11 ]]; then
  architectures+=( arm64e )
fi

for arch in "${architectures[@]}"; do
  cache="${dsc_directory}/dyld_shared_cache_${arch}"
  if [[ ! -f "${cache}" ]]; then
    missing_architectures+=("${arch}")
    continue
  fi
  "${dir}/dsc_extractor" \
      "${cache}" \
      "${working_dir}/${arch}"
  "${dir}/upload_system_symbols" \
      --breakpad-tools="${dir}" \
      --system-root="${working_dir}/${arch}" \
      --dump-to="${destination_dir}"
done
if [[ "${#missing_architectures[@]}" -eq "${#architectures[@]}" ]]; then
  echo "Couldn't locate dyld_shared_cache for any architectures" >& 2
  echo "in ${dsc_directory}. Exiting." >& 2
  exit 1
fi

rm -rf "${working_dir}"
# We have results now, so let's keep `destination_dir`.
trap '' EXIT

"${dir}/upload_system_symbols" \
    --breakpad-tools="${dir}" \
    --system-root=/ \
    --dump-to="${destination_dir}"

set +x
echo
echo "Dumped!"
echo "To upload, run:"
echo
echo "'${dir}/upload_system_symbols'" \\
echo "    --breakpad-tools='${dir}'" \\
echo "    --api-key=<YOUR API KEY>" \\
echo "    --upload-from='${destination_dir}'"

if [[ "${#missing_architectures[@]}" -gt 0 ]]; then
  echo "dyld_shared_cache not found for architecture(s):" >& 2
  echo "  " "${missing_architectures[@]}" >& 2
  echo "You'll need to get symbols for them elsewhere." >& 2
  exit 1
fi
