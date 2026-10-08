#!/usr/bin/env bash
# Install apt packages on a CI runner without hanging on a slow mirror. Needs root.
#
#   sudo scripts/ci/apt-install.sh PACKAGE...
#
# Hosted runners fetch from the Azure Ubuntu mirror (mirror+file:/etc/apt/apt-mirrors.txt).
# That mirror sometimes serves packages at a crawl: on 2026-10-08 two jobs (#619, #631)
# fetched 19 packages in 9 minutes. apt never abandons a slow but live transfer, so the
# setup step hit its 10-minute timeout before any code ran. A healthy update + install
# takes under 90 s.
#
# Attempt 1 is therefore bounded (MIB_APT_FIRST_TIMEOUT, default 210 s). If it fails or
# times out, the Azure host is rewritten to archive.ubuntu.com and attempt 2
# (MIB_APT_SECOND_TIMEOUT, default 270 s) repairs any half-configured dpkg state and
# retries, reusing the .debs attempt 1 already downloaded. Both attempts plus their 10 s
# kill grace come to at most 500 s. That leaves the rest of the 10-minute setup step for
# Conan, the SDK and assets, which normally take well under a minute.
set -euo pipefail

FIRST_TIMEOUT="${MIB_APT_FIRST_TIMEOUT:-210}"
SECOND_TIMEOUT="${MIB_APT_SECOND_TIMEOUT:-270}"
MIRRORS=/etc/apt/apt-mirrors.txt
SOURCES=/etc/apt/sources.list.d/ubuntu.sources
AZURE='azure.archive.ubuntu.com'

if [ "$#" -eq 0 ]; then
  echo "apt-install: no packages given" >&2
  exit 2
fi

attempt() {
  local limit="$1" recover="$2"
  shift 2
  # One deadline for recovery, update and install. Acquire::http::Timeout drops dead
  # connections; the outer timeout catches live but crawling ones. Attempt 2 starts with
  # `dpkg --configure -a`, because a timeout during unpack can leave dpkg half-configured.
  # shellcheck disable=SC2016 # the inner script expands its own $RECOVER and "$@"
  timeout --kill-after=10 "$limit" env DEBIAN_FRONTEND=noninteractive RECOVER="$recover" bash -c '
    if [ "$RECOVER" = 1 ]; then dpkg --configure -a || true; fi
    apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 update &&
    apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 \
      install -y --no-install-recommends "$@"
  ' apt-install "$@"
}

if attempt "$FIRST_TIMEOUT" 0 "$@"; then
  exit 0
fi

echo "::warning::apt from the runner mirror failed or exceeded ${FIRST_TIMEOUT}s; retrying from archive.ubuntu.com"
# Point the Azure host at the Ubuntu archive in place, so a mirror list holding only Azure
# never becomes empty.
for f in "$MIRRORS" "$SOURCES"; do
  if [ -f "$f" ]; then
    sed -i "s#${AZURE}#archive.ubuntu.com#g" "$f"
  fi
done
attempt "$SECOND_TIMEOUT" 1 "$@"
