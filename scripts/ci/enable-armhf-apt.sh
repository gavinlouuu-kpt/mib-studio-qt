#!/usr/bin/env bash
# Make apt able to install armhf (32-bit ARM) packages on an Ubuntu 24.04 amd64 host, for the
# armv7 compile smoke (env/apt-packages.txt section `armhf-cross`). Ubuntu serves armhf from
# ports.ubuntu.com, not the amd64 mirrors, so the existing sources become amd64-only and a
# separate armhf source is added. Idempotent. Needs root (run with sudo in CI).
set -euo pipefail

SRC=/etc/apt/sources.list.d/ubuntu.sources
PORTS=/etc/apt/sources.list.d/ubuntu-ports-armhf.sources
CODENAME="$(. /etc/os-release && echo "${UBUNTU_CODENAME:-${VERSION_CODENAME}}")"

if [ ! -f "$SRC" ]; then
  echo "enable-armhf-apt: $SRC not found (Ubuntu 24.04 deb822 sources expected)" >&2
  exit 1
fi

# Restrict every existing stanza to amd64 (once): without it apt also asks the amd64 mirrors for
# armhf indexes and fails with 404s.
if ! grep -q '^Architectures: amd64' "$SRC"; then
  sed -i '/^Architectures:/d' "$SRC"
  sed -i '/^Types:/a Architectures: amd64' "$SRC"
fi

cat > "$PORTS" <<SRC_EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: ${CODENAME} ${CODENAME}-updates ${CODENAME}-backports ${CODENAME}-security
Components: main restricted universe multiverse
Architectures: armhf
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
SRC_EOF

dpkg --add-architecture armhf
echo "enable-armhf-apt: armhf enabled for ${CODENAME}"
