#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"

workflow="${repo_root}/.github/workflows/deploy_debian_repo.yml"
release_tag="${RELEASE_TAG:-}"
deployment_ref="${DEPLOYMENT_REF:-deployment}"
debian_repo="${DEBIAN_REPO:-git@github.com:MisterTea/debian-et.git}"
push_changes="${PUSH_CHANGES:-false}"
ssh_key_path="${DEBIAN_REPO_SSH_KEY_PATH:-${HOME}/.ssh/id_rsa}"
debian_suites="${DEBIAN_SUITES:-bookworm}"
debian_arches="${DEBIAN_ARCHES:-amd64 arm64}"
verbose_build="${VERBOSE_BUILD:-false}"
quiet_cowbuilder="${QUIET_COWBUILDER:-true}"
deb_build_jobs="${DEB_BUILD_JOBS:-1}"

if ! command -v act >/dev/null 2>&1; then
  echo "act is required to run this script" >&2
  exit 1
fi

cmd=(
  act workflow_dispatch
  -W "${workflow}"
  --container-architecture linux/amd64
  --container-options "--privileged"
  --input "deployment_ref=${deployment_ref}"
  --input "debian_repo=${debian_repo}"
  --input "push_changes=${push_changes}"
  --input "debian_suites=${debian_suites}"
  --input "debian_arches=${debian_arches}"
  --input "verbose_build=${verbose_build}"
  --input "quiet_cowbuilder=${quiet_cowbuilder}"
  --input "deb_build_jobs=${deb_build_jobs}"
)

if [[ "${push_changes}" == "true" ]]; then
  if [[ ! -r "${ssh_key_path}" ]]; then
    echo "Debian repo SSH key not found or not readable: ${ssh_key_path}" >&2
    echo "Set DEBIAN_REPO_SSH_KEY_PATH to use a different key." >&2
    exit 1
  fi

  export DEBIAN_REPO_SSH_PRIVATE_KEY
  DEBIAN_REPO_SSH_PRIVATE_KEY="$(< "${ssh_key_path}")"
  cmd+=(--secret DEBIAN_REPO_SSH_PRIVATE_KEY)
elif [[ -r "${ssh_key_path}" ]]; then
  export DEBIAN_REPO_SSH_PRIVATE_KEY
  DEBIAN_REPO_SSH_PRIVATE_KEY="$(< "${ssh_key_path}")"
  cmd+=(--secret DEBIAN_REPO_SSH_PRIVATE_KEY)
fi

if [[ -n "${release_tag}" ]]; then
  cmd+=(--input "release_tag=${release_tag}")
fi

cd "${repo_root}"
exec "${cmd[@]}" "$@"
