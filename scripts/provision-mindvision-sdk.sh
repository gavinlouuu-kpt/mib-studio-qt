#!/usr/bin/env bash
# Download and extract the pinned MindVision SDK for Linux or macOS.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
destination="${script_dir}/../build/vendor/mindvision-sdk"
github_env=""

usage() {
    echo "Usage: $0 [--destination PATH] [--github-env PATH]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --destination)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            destination="$2"
            shift 2
            ;;
        --github-env)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            github_env="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

linux_url="https://updates.yofo.bio/mindvision-sdk/linuxSDK_V2.1.0.49202602041120.tar.gz"
linux_sha256="246d374dc7f91a8fa7120ceced020680a2b249bbfcf2d974d4e0ed0c04cc6313"
macos_url="https://updates.yofo.bio/mindvision-sdk/mac-sdk.rar"
macos_sha256="ae3358bcb24a10275248ef5e7cc0c5507dbe804436112528b38c266eed140014"

os_name="$(uname -s)"
machine="$(uname -m)"
case "$os_name" in
    Linux)
        sdk_url="$linux_url"
        sdk_sha256="$linux_sha256"
        archive_name="mindvision-linux-sdk.tar.gz"
        runtime_name="libMVSDK.so"
        case "$machine" in
            x86_64|amd64) archive_arch="x64" ;;
            aarch64|arm64) archive_arch="arm64" ;;
            i386|i486|i586|i686|x86) archive_arch="x86" ;;
            armv7*|armv8l|arm) archive_arch="arm" ;;
            *) echo "Unsupported Linux architecture for MindVision SDK: $machine" >&2; exit 1 ;;
        esac
        ;;
    Darwin)
        sdk_url="$macos_url"
        sdk_sha256="$macos_sha256"
        archive_name="mindvision-macos-sdk.rar"
        runtime_name="libmvsdk.dylib"
        case "$machine" in
            arm64) nested_archive="mac sdk/macsdk_m1(250704).zip" ;;
            x86_64) nested_archive="mac sdk/macsdk_x86(250120).zip" ;;
            *) echo "Unsupported macOS architecture for MindVision SDK: $machine" >&2; exit 1 ;;
        esac
        ;;
    *)
        echo "MindVision SDK provisioning is unsupported on $os_name" >&2
        exit 1
        ;;
esac

mkdir -p -- "$destination"
destination="$(cd -- "$destination" && pwd)"
archive_path="${destination}/${archive_name}"
sdk_root="${destination}/extracted"
include_dir="${sdk_root}/include"
runtime_dir="${sdk_root}/lib"
runtime_path="${runtime_dir}/${runtime_name}"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

archive_is_valid=false
if [[ -f "$archive_path" ]] && [[ "$(sha256_file "$archive_path")" == "$sdk_sha256" ]]; then
    archive_is_valid=true
fi
if [[ "$archive_is_valid" != true ]]; then
    echo "Downloading pinned MindVision SDK from ${sdk_url}"
    curl --fail --location --retry 3 --output "$archive_path" "$sdk_url"
fi

actual_sha256="$(sha256_file "$archive_path")"
if [[ "$actual_sha256" != "$sdk_sha256" ]]; then
    echo "MindVision SDK SHA-256 mismatch: expected $sdk_sha256, got $actual_sha256" >&2
    exit 1
fi

stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/mib-mindvision-sdk.XXXXXX")"
trap 'rm -rf -- "$stage_dir"' EXIT
mkdir -p -- "$stage_dir/extracted/include" "$stage_dir/extracted/lib"

if [[ "$os_name" == Linux ]]; then
    tar -xzf "$archive_path" -C "$stage_dir" \
        include/CameraApi.h \
        include/CameraDefine.h \
        include/CameraStatus.h \
        "lib/${archive_arch}/libMVSDK.so" \
        88-mvusb.rules \
        99-mvusb.rules
    cp "$stage_dir/include/CameraApi.h" \
       "$stage_dir/include/CameraDefine.h" \
       "$stage_dir/include/CameraStatus.h" \
       "$stage_dir/extracted/include/"
    cp "$stage_dir/lib/${archive_arch}/libMVSDK.so" "$stage_dir/extracted/lib/"
    cp "$stage_dir/88-mvusb.rules" "$stage_dir/99-mvusb.rules" "$stage_dir/extracted/"
else
    seven_zip="$(command -v 7zz || command -v 7z || true)"
    if [[ -z "$seven_zip" ]]; then
        echo "7-Zip is required to extract the macOS MindVision SDK (brew install sevenzip)" >&2
        exit 1
    fi
    "$seven_zip" x -y "-o${stage_dir}/outer" "$archive_path" "$nested_archive" >/dev/null
    nested_path="${stage_dir}/outer/${nested_archive}"
    unzip -q -o "$nested_path" \
        include/CameraApi.h \
        include/CameraDefine.h \
        include/CameraStatus.h \
        lib/libmvsdk.dylib \
        -d "$stage_dir/extracted"

    # The vendor dylib is built for an app bundle's Frameworks directory.
    # Use @rpath for CMake build trees and re-apply its original ad-hoc signing.
    install_name_tool -id @rpath/libmvsdk.dylib "$stage_dir/extracted/lib/libmvsdk.dylib"
    codesign --force --sign - "$stage_dir/extracted/lib/libmvsdk.dylib"
fi

for required in \
    "$stage_dir/extracted/include/CameraApi.h" \
    "$stage_dir/extracted/include/CameraDefine.h" \
    "$stage_dir/extracted/include/CameraStatus.h" \
    "$stage_dir/extracted/lib/$runtime_name"; do
    if [[ ! -f "$required" ]]; then
        echo "Extracted MindVision SDK is missing required file: $required" >&2
        exit 1
    fi
done
required_apis=(
    CameraGetImageBufferPriority
    CameraSetExtTrigSignalType
    CameraSetExtTrigJitterTime
    CameraSetTriggerDelayTime
    CameraSetTriggerCount
    CameraSetStrobeMode
    CameraSetStrobePulseWidth
    CameraSetStrobeDelayTime
    CameraSetStrobePolarity
    CameraSetOutPutIOMode
    CameraSetIOStateEx
    CameraSoftTrigger
)
for api in "${required_apis[@]}"; do
    if ! grep -q "$api" "$stage_dir/extracted/include/CameraApi.h"; then
        echo "Extracted MindVision CameraApi.h lacks required API: $api" >&2
        exit 1
    fi
done

mkdir -p -- "$include_dir" "$runtime_dir"
cp "$stage_dir/extracted/include/CameraApi.h" \
   "$stage_dir/extracted/include/CameraDefine.h" \
   "$stage_dir/extracted/include/CameraStatus.h" \
   "$include_dir/"
cp "$stage_dir/extracted/lib/$runtime_name" "$runtime_path"
if [[ "$os_name" == Linux ]]; then
    cp "$stage_dir/extracted/88-mvusb.rules" "$stage_dir/extracted/99-mvusb.rules" "$sdk_root/"
fi

echo "MindVision SDK ready: $sdk_root"
echo "MindVision runtime ready: $runtime_path"
if [[ -n "$github_env" ]]; then
    {
        echo "MIB_MINDVISION_SDK_ROOT=$sdk_root"
        echo "MIB_MINDVISION_RUNTIME_DIR=$runtime_dir"
    } >> "$github_env"
fi
