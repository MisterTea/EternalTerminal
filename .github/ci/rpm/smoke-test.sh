#!/bin/sh
# Installs the et RPM on the distro this runs on and checks that it works.
#
# Usage: smoke-test.sh RPM
set -eu

rpm=$1

install_rpm() {
  if command -v dnf >/dev/null 2>&1; then
    dnf install -y "$rpm"
  elif command -v tdnf >/dev/null 2>&1; then
    tdnf install -y "$rpm"
  elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install --allow-unsigned-rpm "$rpm"
  else
    echo "no supported package manager" >&2
    exit 1
  fi
}

# Distro mirrors rate-limit CI runners now and then.
attempt=1
until install_rpm; do
  if [ "$attempt" -ge 3 ]; then
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep 20
done

rpm -q --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n' et

# Every shared library resolves on this distro.
for bin in et etserver etterminal etctl htm htmd; do
  if ldd "/usr/bin/$bin" | grep 'not found'; then
    echo "/usr/bin/$bin has unresolved libraries" >&2
    exit 1
  fi
done
/usr/bin/et --version
/usr/bin/etserver --version
/usr/bin/etctl --version

# Upgrades keep a locally edited configuration.
rpm -qc et | grep -qx /etc/et.cfg

test -f /usr/lib/systemd/system/et.service

rpm -e et
if rpm -q et >/dev/null; then
  echo "et is still installed" >&2
  exit 1
fi
