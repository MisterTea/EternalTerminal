#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"

workflow="${repo_root}/.github/workflows/deploy_archlinux_aur.yml"
release_tag="${RELEASE_TAG:-}"
aur_repo="${AUR_REPO:-ssh://aur@aur.archlinux.org/eternalterminal.git}"
push_changes="${PUSH_CHANGES:-false}"
ssh_key_path="${AUR_SSH_KEY_PATH:-${HOME}/.ssh/id_rsa}"

if ! command -v act >/dev/null 2>&1; then
  echo "act is required to run this script" >&2
  exit 1
fi

if [[ ! -r "${ssh_key_path}" ]]; then
  echo "AUR SSH key not found or not readable: ${ssh_key_path}" >&2
  echo "Set AUR_SSH_KEY_PATH to use a different key." >&2
  exit 1
fi

export AUR_SSH_PRIVATE_KEY
AUR_SSH_PRIVATE_KEY="$(< "${ssh_key_path}")"

cmd=(
  act workflow_dispatch
  -W "${workflow}"
  --container-architecture linux/amd64
  --secret AUR_SSH_PRIVATE_KEY
  --input "aur_repo=${aur_repo}"
  --input "push_changes=${push_changes}"
)

if [[ -n "${release_tag}" ]]; then
  cmd+=(--input "release_tag=${release_tag}")
fi

cd "${repo_root}"
exec "${cmd[@]}" "$@"
