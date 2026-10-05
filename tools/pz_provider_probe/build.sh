#!/usr/bin/env bash
# Cross-build pz_provider_probe for the PZ7035 PS with the Yocto SDK
# (bitbake yofo-image -c populate_sdk). Usage: build.sh [SDK_DIR] [OUT]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${1:-/mnt/hdd/developer-data/IMX426/yofo-sdk-20261001}
OUT=${2:-$ROOT/build/pz_provider_probe}
# shellcheck disable=SC1090
source "$SDK"/environment-setup-*
$CXX -std=c++17 -O2 -Wall -Wextra -Wno-psabi -DSPDLOG_FMT_EXTERNAL -I"$ROOT/include" -I"$ROOT/third_party/pz7035-abi" -I"$SDKTARGETSYSROOT/usr/include/opencv4" \
    "$ROOT/tools/pz_provider_probe/main.cpp" \
    "$ROOT/src/backend/processing/pz/PzExecutionProviders.cpp" \
    "$ROOT/src/backend/pz/PzRecords.cpp" \
    "$ROOT/src/backend/pz/PzPlatformMonitor.cpp" \
    "$ROOT/src/backend/processing/pz/PzProfileCompiler.cpp" \
    "$ROOT/src/backend/processing/EModulusLut.cpp" \
    -o "$OUT" -lspdlog -lfmt -lpthread
echo "built $OUT"
