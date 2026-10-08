#!/usr/bin/env bash
# Build and stage the standing YOFO Studio package for the PZ7035 PS (armv7): the stripped server,
# the UI as dist.tar, the unit, the PL-ready guard, install.sh, MD5SUMS and BUILD_INFO.
#
#   YOFO_SDK=<sdk> [YOFO_PKG_NOTES='...'] scripts/yofo/package_studio.sh [PKG_DIR] [--no-build]
#
# PKG_DIR defaults to /mnt/hdd/developer-data/IMX426/yofo-studio-pkg/<short commit>. Run
# install.sh from that directory on the board after each Linux boot (the RAM root resets).
# --no-build packages the existing armv7 build (cmake --preset linux-armv7-yocto, cargo-armv7.sh
# build --release, npm run build in desktop/).
set -euo pipefail
cd "$(dirname "$0")/../.."
commit=$(git rev-parse --short=8 HEAD)
pkg=""; build=1
for arg in "$@"; do
    case "$arg" in
        --no-build) build=0 ;;
        *) pkg=$arg ;;
    esac
done
pkg=${pkg:-/mnt/hdd/developer-data/IMX426/yofo-studio-pkg/$commit}
sdk=${YOFO_SDK:?set YOFO_SDK to the installed YOFO Yocto SDK}
mkdir -p "$(dirname "$pkg")"
findmnt -T "$(dirname "$pkg")" >/dev/null || { echo "package: $pkg is not on a mounted filesystem" >&2; exit 1; }
if [ "$build" = 1 ]; then
    (unset PKG_CONFIG_PATH LD_LIBRARY_PATH; . "$sdk"/environment-setup-*;
     cmake --preset linux-armv7-yocto >/dev/null && cmake --build --preset linux-armv7-yocto-build -j"$(nproc)" --target mib_backend mib_review_core mib_processing oeabt_serial oeabt_core stage_zc300 stage_zc300_protocol)
    scripts/yofo/cargo-armv7.sh build --release --manifest-path crates/mib-bridge-server/Cargo.toml
    (cd desktop && npm run build >/dev/null)
fi
mkdir -p "$pkg"
cp crates/mib-bridge-server/target/armv7-unknown-linux-gnueabihf/release/yofo-studio-server "$pkg/yofo-studio-server"
(unset PKG_CONFIG_PATH LD_LIBRARY_PATH; . "$sdk"/environment-setup-*; $STRIP "$pkg/yofo-studio-server")
tar -C desktop -cf "$pkg/dist.tar" dist
cp deploy/yofo-studio/yofo-studio.service deploy/yofo-studio/install.sh "$pkg/"
cp deploy/yofo-studio/pl-ready.sh "$pkg/pl-ready.sh"
{
    echo "yofo-studio standing package, commit $commit"
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(hostname)"
    echo "branch: $(git rev-parse --abbrev-ref HEAD)  dirty: $(git status --porcelain | grep -v '^??' | wc -l) tracked file(s)"
    echo "sdk: $(basename "$sdk")  profile: cargo release, cmake preset linux-armv7-yocto"
    echo "listens: 127.0.0.1:8427, no token (SSH tunnel); unit waits for DEVCFG PCFG_DONE"
    echo "expects: /usr/lib/genicam/libpz7035_gentl.cti (pz7035-imx426 main 0f67861d, md5 bc89cd389dbc0ccfa2400e1e5dc0512f) and /etc/yofo/expected-core.json"
    [ -z "${YOFO_PKG_NOTES:-}" ] || printf '%s\n' "$YOFO_PKG_NOTES"   # e.g. the open PRs not included
} > "$pkg/BUILD_INFO"
(cd "$pkg" && md5sum yofo-studio-server dist.tar yofo-studio.service install.sh pl-ready.sh BUILD_INFO > MD5SUMS)
chmod +x "$pkg/install.sh" "$pkg/pl-ready.sh"
echo "package $pkg"; cat "$pkg/MD5SUMS"
