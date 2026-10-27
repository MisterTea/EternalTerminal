#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"

workflow="${repo_root}/.github/workflows/deploy_ubuntu_ppa.yml"
release_tag="${RELEASE_TAG:-}"
deployment_ref="${DEPLOYMENT_REF:-deployment}"
ppa="${PPA:-ppa:jgmath2000/et}"
simulate_upload="${SIMULATE_UPLOAD:-true}"
gpg_key_id="${GPG_KEY_ID:-}"
gpg_passphrase_file="${GPG_PASSPHRASE_FILE:-${HOME}/.gnupg/pphrase}"

if ! command -v act >/dev/null 2>&1; then
  echo "act is required to run this script" >&2
  exit 1
fi

if ! command -v gpg >/dev/null 2>&1; then
  echo "gpg is required to export the local signing key" >&2
  exit 1
fi

if [[ -z "${gpg_key_id}" ]]; then
  gpg_key_id="$(
    gpg --batch --list-secret-keys --with-colons |
      awk -F: '$1 == "fpr" { print $10; exit }'
  )"
fi

if [[ -z "${gpg_key_id}" ]]; then
  echo "No local GPG secret key found. Set GPG_KEY_ID to the key fingerprint or long key ID." >&2
  exit 1
fi

if [[ -z "${GPG_PASSPHRASE:-}" ]]; then
  if [[ -r "${gpg_passphrase_file}" ]]; then
    GPG_PASSPHRASE="$(< "${gpg_passphrase_file}")"
  elif [[ -t 0 && -r /dev/tty ]]; then
    read -r -s -p "GPG passphrase: " GPG_PASSPHRASE </dev/tty
    echo >/dev/tty
  else
    echo "GPG_PASSPHRASE must be set, or ${gpg_passphrase_file} must be readable." >&2
    exit 1
  fi
fi

export GPG_PRIVATE_KEY
if [[ -r "${gpg_passphrase_file}" ]]; then
  GPG_PRIVATE_KEY="$(
    gpg --batch --yes --pinentry-mode loopback \
      --passphrase-file "${gpg_passphrase_file}" \
      --armor --export-secret-keys "${gpg_key_id}"
  )"
else
  GPG_PRIVATE_KEY="$(
    printf '%s' "${GPG_PASSPHRASE}" |
      gpg --batch --yes --pinentry-mode loopback \
        --passphrase-fd 0 \
        --armor --export-secret-keys "${gpg_key_id}"
  )"
fi
export GPG_PASSPHRASE

cmd=(
  act workflow_dispatch
  -W "${workflow}"
  --container-architecture linux/amd64
  --secret GPG_PRIVATE_KEY
  --secret GPG_PASSPHRASE
  --input "deployment_ref=${deployment_ref}"
  --input "ppa=${ppa}"
  --input "simulate_upload=${simulate_upload}"
)

if [[ -n "${release_tag}" ]]; then
  cmd+=(--input "release_tag=${release_tag}")
fi

cd "${repo_root}"
exec "${cmd[@]}" "$@"
