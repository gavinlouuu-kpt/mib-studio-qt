#!/usr/bin/env bash
# Copy the linux-armv7-yocto build to the PZ7035 PS and run scripts/yofo/target_smoke.sh there.
#
#   scripts/yofo/deploy_target.sh [SOAK_SECONDS]
#
# YOFO_TARGET        ssh destination (default amd-edf@192.168.137.2, the bench link)
# YOFO_TARGET_DIR    directory on the target (default /tmp/yofo; the PS runs from a RAM root)
# YOFO_SSH_OPTS      extra ssh/scp options (e.g. a dedicated known_hosts file)
# STRIP              set by the SDK's environment-setup-*; binaries are stripped before copying
# YOFO_SUDO_PASSWORD_FILE  file with the target user's sudo password (the producer maps the PL
#                    through UIO and needs root); without it sudo must not ask for a password
set -euo pipefail
cd "$(dirname "$0")/../.."
target=${YOFO_TARGET:-amd-edf@192.168.137.2}
dir=${YOFO_TARGET_DIR:-/tmp/yofo}
read -r -a opts <<< "${YOFO_SSH_OPTS:-}"
build=build/linux-armv7-yocto
files=("$build/Release/mib_backend_tests" "$build/Release/mib_backend_smoke_test"
       "$build/yofo_preview_soak" scripts/yofo/target_smoke.sh)
for f in "${files[@]}"; do test -f "$f" || { echo "missing $f (cmake --build --preset linux-armv7-yocto-build)" >&2; exit 1; }; done
# Strip into a staging directory when the SDK environment is sourced ($STRIP): the unstripped
# test runner is ~130 MB and the target runs from RAM.
if [ -n "${STRIP:-}" ]; then
    stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
    for f in "${files[@]}"; do cp "$f" "$stage/"; done
    # shellcheck disable=SC2086
    $STRIP "$stage"/mib_backend_tests "$stage"/mib_backend_smoke_test "$stage"/yofo_preview_soak
    files=("$stage"/*)
fi
ssh "${opts[@]}" "$target" "mkdir -p $dir"
scp -q "${opts[@]}" "${files[@]}" "$target:$dir/"
if [ -n "${YOFO_SUDO_PASSWORD_FILE:-}" ]; then
    ssh "${opts[@]}" "$target" "sudo -S -p '' $dir/target_smoke.sh ${1:-20}" < "$YOFO_SUDO_PASSWORD_FILE"
else
    ssh "${opts[@]}" "$target" "sudo -n $dir/target_smoke.sh ${1:-20}"
fi
