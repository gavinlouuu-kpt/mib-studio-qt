#!/usr/bin/env bash
# Stage the YOFO Studio server and UI for the PZ7035 image (pz7035-imx426 meta-yofo
# recipes-yofo/yofo-studio): the stripped ARMv7 yofo-studio-server and desktop/dist as dist.tar.
#
#   YOFO_SDK=<sdk> scripts/yofo/stage_image.sh [STAGE_DIR]
#
# Builds first: cmake --preset linux-armv7-yocto (backend archives), cargo-armv7.sh (server),
# npm run build (UI). STAGE_DIR defaults to the recipe's YOFO_STUDIO_STAGE.
set -euo pipefail
cd "$(dirname "$0")/../.."
stage=${1:-/mnt/hdd/developer-data/IMX426/yofo-studio-stage}
sdk=${YOFO_SDK:?set YOFO_SDK to the installed YOFO Yocto SDK}
(unset PKG_CONFIG_PATH LD_LIBRARY_PATH; . "$sdk"/environment-setup-*;
 cmake --preset linux-armv7-yocto >/dev/null && cmake --build --preset linux-armv7-yocto-build -j"$(nproc)" --target mib_backend mib_processing oeabt_serial oeabt_core)
scripts/yofo/cargo-armv7.sh build --release --manifest-path crates/mib-bridge-server/Cargo.toml
(cd desktop && npm run build >/dev/null)
mkdir -p "$stage"
cp crates/mib-bridge-server/target/armv7-unknown-linux-gnueabihf/release/yofo-studio-server "$stage/yofo-studio-server"
(unset PKG_CONFIG_PATH LD_LIBRARY_PATH; . "$sdk"/environment-setup-*; $STRIP "$stage/yofo-studio-server")
tar -C desktop -cf "$stage/dist.tar" dist
echo "staged $(du -h "$stage/yofo-studio-server" | cut -f1) server and $(du -h "$stage/dist.tar" | cut -f1) UI in $stage"
