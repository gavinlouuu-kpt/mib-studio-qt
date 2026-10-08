#!/usr/bin/env bash
# Cross-compile a Rust crate of this repository for the PZ7035 PS (armv7 hard-float) with the
# YOFO Yocto SDK, linking the backend archives from build/linux-armv7-yocto
# (cmake --preset linux-armv7-yocto first).
#
#   YOFO_SDK=/path/to/sdk scripts/yofo/cargo-armv7.sh build --release \
#       --manifest-path crates/mib-bridge-server/Cargo.toml
#
# The SDK's generic CC/CXX/CFLAGS would also hit host-side build scripts, so they are mapped to
# the target-specific variables cargo and the cc crate read, and the generic ones are cleared.
set -euo pipefail
cd "$(dirname "$0")/../.."
sdk=${YOFO_SDK:?set YOFO_SDK to the installed YOFO Yocto SDK}
unset PKG_CONFIG_PATH LD_LIBRARY_PATH
# shellcheck disable=SC1090
. "$sdk"/environment-setup-*
target=armv7-unknown-linux-gnueabihf
tool=$(pwd)/build/linux-armv7-yocto/cargo-tools
mkdir -p "$tool"
# cargo wants one linker path; the SDK's CC carries --sysroot and the CPU flags.
printf '#!/bin/sh\nexec %s "$@"\n' "$CC $LDFLAGS" > "$tool/linker"
chmod +x "$tool/linker"
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABIHF_LINKER="$tool/linker"
export CC_armv7_unknown_linux_gnueabihf="$CC" CXX_armv7_unknown_linux_gnueabihf="$CXX" AR_armv7_unknown_linux_gnueabihf="$AR"
export CFLAGS_armv7_unknown_linux_gnueabihf="$CFLAGS"
# The SDK builds spdlog against the external fmt, as the CMake targets do; the bridge shims include spdlog.
export CXXFLAGS_armv7_unknown_linux_gnueabihf="$CXXFLAGS -DSPDLOG_COMPILED_LIB -DSPDLOG_SHARED_LIB -DSPDLOG_FMT_EXTERNAL -DFMT_SHARED"
unset CC CXX CPP AR LD CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
# The SDK flags already select the CPU; the cc crate's own -march would conflict.
export CRATE_CC_NO_DEFAULTS=1
export MIB_BRIDGE_NO_CMAKE=1 MIB_BRIDGE_BUILD_DIR="$(pwd)/build/linux-armv7-yocto" MIB_BRIDGE_SYSROOT="$OECORE_TARGET_SYSROOT"
exec cargo "$@" --target "$target"
