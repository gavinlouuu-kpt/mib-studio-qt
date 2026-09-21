#!/usr/bin/env bash
# bootstrap.sh — install what scripts/doctor.sh reports missing (Linux, macOS).
#
# Idempotent: safe to rerun; every step skips work already done. Reads the
# same files as the doctor: env/apt-packages.txt or env/brew-packages.txt,
# env/requirements-build.txt, env/assets.json. Uses sudo only for the system
# package manager (and only when not already root).
#
#   scripts/bootstrap.sh                         # base + backend
#   scripts/bootstrap.sh --sections base,backend,frontend
#   scripts/bootstrap.sh --public-assets-only    # containers: never needs HF_TOKEN
#   scripts/bootstrap.sh --dry-run               # print the commands only
#   flags: --skip-packages --skip-python --skip-sdk --skip-assets
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SECTIONS="base,backend"
ASSETS_SCOPE="--required-only"
DRY_RUN=false
SKIP_PACKAGES=false SKIP_PYTHON=false SKIP_SDK=false SKIP_ASSETS=false

usage() { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sections) SECTIONS="$2"; shift 2 ;;
        --sections=*) SECTIONS="${1#*=}"; shift ;;
        --public-assets-only) ASSETS_SCOPE="--public-only"; shift ;;
        --dry-run) DRY_RUN=true; shift ;;
        --skip-packages) SKIP_PACKAGES=true; shift ;;
        --skip-python) SKIP_PYTHON=true; shift ;;
        --skip-sdk) SKIP_SDK=true; shift ;;
        --skip-assets) SKIP_ASSETS=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

OS="$(uname -s)"
step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
run()  { printf '    $ %s\n' "$*"; $DRY_RUN || "$@"; }
SUDO=""; [[ "$(id -u)" -ne 0 ]] && command -v sudo >/dev/null && SUDO="sudo"

packages_for() {
    awk -v want=",$SECTIONS," '
        /^# section:/ { sec=$3; next }
        /^#/ || /^[[:space:]]*$/ { next }
        index(want, "," sec ",") { print $1 }
    ' "$1"
}

cd "$REPO_ROOT"
echo "MIB Studio Qt bootstrap — $OS, sections: $SECTIONS$($DRY_RUN && echo ' (dry run)')"

# ---------------------------------------------------------------- packages
if ! $SKIP_PACKAGES; then
    case "$OS" in
        Linux)
            if command -v apt-get >/dev/null; then
                step "System packages (env/apt-packages.txt)"
                pkgs=(); while IFS= read -r p; do pkgs+=("$p"); done < <(packages_for env/apt-packages.txt)
                absent=()
                for p in "${pkgs[@]}"; do dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || absent+=("$p"); done
                if ((${#absent[@]})); then
                    run $SUDO apt-get update
                    run $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${absent[@]}"
                else
                    echo "    all ${#pkgs[@]} packages already installed"
                fi
            else
                echo "    non-apt Linux: install the equivalents of env/apt-packages.txt yourself" >&2
            fi ;;
        Darwin)
            step "Homebrew packages (env/brew-packages.txt)"
            command -v brew >/dev/null || { echo "Homebrew is required: https://brew.sh" >&2; exit 1; }
            pkgs=(); while IFS= read -r p; do pkgs+=("$p"); done < <(packages_for env/brew-packages.txt)
            absent=()
            for p in "${pkgs[@]}"; do brew list --versions "$p" >/dev/null 2>&1 || absent+=("$p"); done
            if ((${#absent[@]})); then
                run brew install "${absent[@]}"
            else
                echo "    all ${#pkgs[@]} formulae already installed"
            fi ;;
        *)  echo "unsupported OS '$OS' (Windows: scripts/bootstrap.ps1)" >&2; exit 1 ;;
    esac
fi

# ---------------------------------------------------------------- python + conan
if ! $SKIP_PYTHON; then
    step "Python virtualenv .venv + env/requirements-build.txt (conan, numpy)"
    if [[ ! -x .venv/bin/python ]]; then run python3 -m venv .venv; fi
    run .venv/bin/python -m pip install --quiet --upgrade pip
    run .venv/bin/python -m pip install --quiet -r env/requirements-build.txt
    if ! command -v conan >/dev/null; then
        echo "    conan is installed in .venv; activate it or prefix commands:  source .venv/bin/activate"
    fi
fi
CONAN="$(command -v conan || echo "$REPO_ROOT/.venv/bin/conan")"
if [[ -x "$CONAN" ]] || $DRY_RUN; then
    step "Conan default profile"
    if "$CONAN" profile path default >/dev/null 2>&1; then
        echo "    exists: $("$CONAN" profile path default)"
    else
        run "$CONAN" profile detect
    fi
fi

# ---------------------------------------------------------------- vendored SDK
if ! $SKIP_SDK; then
    step "MindVision SDK (scripts/provision-mindvision-sdk.sh)"
    sdk_root="${MIB_MINDVISION_SDK_ROOT:-$REPO_ROOT/build/vendor/mindvision-sdk/extracted}"
    if [[ -f "$sdk_root/include/CameraApi.h" ]]; then
        echo "    already provisioned at $sdk_root"
    else
        run ./scripts/provision-mindvision-sdk.sh
    fi
fi

# ---------------------------------------------------------------- assets
if ! $SKIP_ASSETS; then
    step "External assets (env/assets.json) $ASSETS_SCOPE"
    run python3 scripts/provision-assets.py "$ASSETS_SCOPE"
fi

# ---------------------------------------------------------------- next step
step "Done"
case "$OS" in
    Linux)
        if [[ ",$SECTIONS," == *",frontend,"* ]]; then
            echo "    cmake --preset linux-system-release && cmake --build --preset linux-system-release-build"
        else
            echo "    cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build"
            echo "    ctest --preset linux-backend-only-test        # network-labelled tests: linux-network-test"
        fi ;;
    Darwin)
        echo "    macOS has no CMake preset yet; start from knowledge_map/build-and-run/Build.md"
        echo "    (Qt-free core: cmake -S . -B build/mac -G Ninja -DMIB_BUILD_PROCESSING_ONLY=ON -DMIB_BUILD_BACKEND_ONLY=ON -DBUILD_TESTING=OFF -DMIB_USE_SENTRY=OFF)" ;;
esac
echo "    verify any time with: scripts/doctor.sh --sections $SECTIONS"
