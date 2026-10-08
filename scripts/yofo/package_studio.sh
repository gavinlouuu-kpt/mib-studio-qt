#!/usr/bin/env bash
# Build and stage the standing YOFO Studio package for the PZ7035 PS (armv7): the stripped server,
# the UI as dist.tar, the unit, the PL-ready guard, install.sh, MD5SUMS and BUILD_INFO.
#
#   YOFO_SDK=<sdk> [YOFO_PKG_NOTES='...'] scripts/yofo/package_studio.sh [PKG_DIR] [--no-build] [--bundle]
#
# PKG_DIR defaults to /mnt/hdd/developer-data/IMX426/yofo-studio-pkg/<short commit>. Run
# install.sh from that directory on the board after each Linux boot (the RAM root resets).
# --no-build packages the existing armv7 build (cmake --preset linux-armv7-yocto, cargo-armv7.sh
# build --release, npm run build in desktop/). --bundle then adds the PL image, firmware, Linux boot set,
# producer and slot tools of a pz7035-imx426 ref (PZ7035_REF, default pl-results9): the instrument
# bundle, see scripts/yofo/bundle_assemble.sh and docs/exec-plans/active/2026-10-08-yofo-studio-bundle.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
commit=$(git rev-parse --short=8 HEAD)
pkg=""; build=1; bundle=0
for arg in "$@"; do
    case "$arg" in
        --no-build) build=0 ;;
        --bundle) bundle=1 ;;
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
# The E-modulus LUT (G6): the server reads resources/isoelastic_curve/ under its --resource-dir.
rm -rf "$pkg/resources"; install -d "$pkg/resources/isoelastic_curve"
cp resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt "$pkg/resources/isoelastic_curve/"
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
(cd "$pkg" && md5sum yofo-studio-server dist.tar yofo-studio.service install.sh pl-ready.sh BUILD_INFO resources/isoelastic_curve/* > MD5SUMS)
chmod +x "$pkg/install.sh" "$pkg/pl-ready.sh"
[ "$bundle" = 0 ] || scripts/yofo/bundle_assemble.sh "$pkg"
echo "package $pkg"; cat "$pkg/MD5SUMS"
