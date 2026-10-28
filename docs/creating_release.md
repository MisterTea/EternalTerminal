# Creating an ET release

The package version comes from three places that must all say the same `A.B.C`:

- `version-string` in `vcpkg.json` (CMake reads this as the project version)
- the upstream version in `deployment/debian_SOURCE/changelog`
- the Git tag `et-vA.B.C`

Debian, Ubuntu, and Arch workflows read the changelog and the tag from the GitHub release tarball. Put both version edits on `master` before merging to `release`.

## 1. Version the commit on master

Add a changelog stanza at the top of `deployment/debian_SOURCE/changelog`. Leave the `##DISTRO##` tokens in place; the package workflows substitute the suite or series name.

```
et (A.B.C-##DISTRO##1) ##DISTRO##; urgency=medium

* https://github.com/MisterTea/EternalTerminal/releases/tag/et-vA.B.C

-- Your Name <you@example.com>  Day, DD Mon YYYY HH:MM:SS -TZ00
```

Set `version-string` in `vcpkg.json` to `A.B.C` and push `master`.

## 2. Update the release branch

`master` builds against the `external/` submodules. The `release` branch builds against a vendored `external_imported/` tree so the GitHub source archive (which has no submodule checkouts) still configures.

```sh
git checkout release
git merge master
```

If the merge keeps `master`'s `CMakeLists.txt`, point the release tree back at the vendored copy:

```cmake
set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external_imported")
```

Refresh the vendored tree. `import_submodules.sh` lives on `release`. It resets submodules, deletes `external_imported`, and copies `external/` back in, so run it on a clean checkout:

```sh
git add CMakeLists.txt
./import_submodules.sh
git commit -m "Update external_imported for et-vA.B.C"
git push origin release
```

Wait for these workflows on that push:

- Vcpkg Build Release
- NoVcpkg Build Release
- Portability CI
- OpenWrt CI

OpenWrt CI builds the package; it does not publish a feed.

## 3. Publish the GitHub release

Create a GitHub release from the `release` commit you just pushed and tag it `et-vA.B.C`. Publish the release. A draft does not start the Windows workflow.

Publishing runs **Deploy Windows Release Asset** (`.github/workflows/deploy_windows_winget.yml`). It checks out the tag with submodules, builds, and uploads `EternalTerminal-A.B.C-windows-x64.zip` containing `et.exe` and `LICENSE`. Re-run it with `workflow_dispatch` to rebuild; set `overwrite_asset` to replace an existing zip.

## 4. Publish packages

Run these with `workflow_dispatch` after the release exists. Leave `release_tag` empty to use the latest GitHub release, or set it to `et-vA.B.C`.

Repository secrets:

| Secret | Used by |
| --- | --- |
| `GPG_PRIVATE_KEY`, `GPG_PASSPHRASE` | Debian repo, Ubuntu PPA. The key must already be able to sign for Launchpad. |
| `DEBIAN_REPO_SSH_PRIVATE_KEY` | Debian repo. Push access to `git@github.com:MisterTea/debian-et.git`. |
| `AUR_SSH_PRIVATE_KEY` | AUR. Registered on the AUR account that owns `eternalterminal`. |

**Deploy Debian Repo** (`.github/workflows/deploy_debian_repo.yml`) builds `oldstable`, `stable`, and `testing` for `amd64` and `arm64` by default, with vcpkg and telemetry disabled and without dbgsym packages. `push_changes` defaults to true and commits the apt repository to `debian-et`.

**Deploy Ubuntu PPA** (`.github/workflows/deploy_ubuntu_ppa.yml`) builds a source package for each supported Ubuntu series and signs it. The upstream tarball comes from the GitHub release. `deployment/debian_SOURCE` comes from the ref you dispatch, so a packaging fix can ship without a new upstream tag. `simulate_upload` defaults to true, which runs `dput --simulate`. Set it to false to upload to `ppa:jgmath2000/et`. The first series uploads `orig.tar.gz`; later series omit it. Set `include_orig` to false when that tarball is already in the PPA. Set `ubuntu_series` to retry specific series. Debug symbol packages are disabled in `debian/rules` because they exceed the PPA quota.

**Deploy Arch Linux AUR** (`.github/workflows/deploy_archlinux_aur.yml`) updates `pkgver` and `sha256sums` in `ssh://aur@aur.archlinux.org/eternalterminal.git` and pushes. `push_changes` defaults to true.

Homebrew, MacPorts, and Fedora pick up the tag from outside this repository.

## Local dry runs

`deployment/deploy_*_act.sh` runs the same workflows locally through [`act`](https://github.com/nektos/act). Each script exports the signing key from the local GPG keyring (`GPG_KEY_ID`, or the first secret key; passphrase from `GPG_PASSPHRASE` or `~/.gnupg/pphrase`).

```sh
# Debian. PUSH_CHANGES defaults to false.
RELEASE_TAG=et-vA.B.C ./deployment/deploy_debian_repo_act.sh

# PPA. SIMULATE_UPLOAD defaults to true.
RELEASE_TAG=et-vA.B.C SIMULATE_UPLOAD=false ./deployment/deploy_ubuntu_ppa_act.sh

# AUR. PUSH_CHANGES defaults to false.
RELEASE_TAG=et-vA.B.C ./deployment/deploy_archlinux_aur_act.sh
```

SSH private keys default to `~/.ssh/id_rsa` (`DEBIAN_REPO_SSH_KEY_PATH`, `AUR_SSH_KEY_PATH`).
