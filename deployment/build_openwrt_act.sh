#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"
workflow="${repo_root}/.github/workflows/openwrt.yml"

if ! command -v act >/dev/null 2>&1; then
  echo "act is required to run this script" >&2
  exit 1
fi

cmd=(
  act
  -W "${workflow}"
  --container-architecture linux/amd64
  -b
)

cd "${repo_root}"
exec "${cmd[@]}" "$@"
