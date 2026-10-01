#!/usr/bin/env bash
# doctor.sh — report what this host is missing to build MIB Studio Qt.
#
# Checks only; never installs, downloads or writes outside stdout. Every
# MISSING/OLD line comes with the command that fixes it, and the summary
# repeats them as a paste-able block. Exit 0 when complete, 1 otherwise.
#
#   scripts/doctor.sh                          # base + backend (linux-backend-only)
#   scripts/doctor.sh --sections base,backend,frontend
#   scripts/doctor.sh --with-private            # also require HF_TOKEN for private assets
#
# Sources of truth it reads: env/toolchain.toml, env/apt-packages.txt,
# env/brew-packages.txt, env/assets.json (via scripts/provision-assets.py).
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SECTIONS="base,backend"
WITH_PRIVATE=false
QUIET=false

usage() { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sections) SECTIONS="$2"; shift 2 ;;
        --sections=*) SECTIONS="${1#*=}"; shift ;;
        --with-private) WITH_PRIVATE=true; shift ;;
        --quiet) QUIET=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

OS="$(uname -s)"
MISSING=0
FIXES=()

ok()   { $QUIET || printf '  \033[32mOK\033[0m       %s\n' "$1"; }
warn() { printf '  \033[33mWARN\033[0m     %s\n' "$1"; }
add_fix() { local f; for f in "${FIXES[@]+"${FIXES[@]}"}"; do [[ "$f" == "$1" ]] && return; done; FIXES+=("$1"); }
miss() { printf '  \033[31mMISSING\033[0m  %s\n' "$1"; MISSING=$((MISSING + 1)); [[ -n "${2:-}" ]] && add_fix "$2"; }
old()  { printf '  \033[31mOLD\033[0m      %s\n' "$1"; MISSING=$((MISSING + 1)); [[ -n "${2:-}" ]] && add_fix "$2"; }
section() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# version_ge A B  -> 0 when A >= B (dotted numeric versions)
version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }

# toolchain.toml is deliberately flat: `name = ">=x.y"` lines under [tools].
tool_min() { sed -n "s/^$1[[:space:]]*=[[:space:]]*\">=\([0-9.]*\).*/\1/p" "$REPO_ROOT/env/toolchain.toml" | head -n1; }

tool_version() {
    case "$1" in
        cmake)  cmake --version 2>/dev/null | head -n1 ;;
        ninja)  ninja --version 2>/dev/null ;;
        conan)  { conan --version 2>/dev/null || "$REPO_ROOT/.venv/bin/conan" --version 2>/dev/null; } ;;
        python) python3 --version 2>/dev/null ;;
        node)   node --version 2>/dev/null ;;
        rust)   cargo --version 2>/dev/null ;;
        git)    git --version 2>/dev/null ;;
    esac | grep -oE '[0-9]+(\.[0-9]+)+' | head -n1
}

has_section() { [[ ",$SECTIONS," == *",$1,"* ]]; }

# Packages for the requested sections from a `# section: name` delimited list.
packages_for() {
    awk -v want=",$SECTIONS," '
        /^# section:/ { sec=$3; next }
        /^#/ || /^[[:space:]]*$/ { next }
        index(want, "," sec ",") { print $1 }
    ' "$1"
}

pkg_manager_hint() {
    case "$OS" in
        Linux)  echo "sudo apt-get update && sudo apt-get install -y --no-install-recommends $*" ;;
        Darwin) echo "brew install $*" ;;
    esac
}

echo "MIB Studio Qt doctor — $OS, sections: $SECTIONS"

# ---------------------------------------------------------------- toolchain
section "Toolchain (env/toolchain.toml)"
for tool in git cmake ninja conan python; do
    min="$(tool_min "$tool")"; have="$(tool_version "$tool")"
    if [[ -z "$have" ]]; then
        case "$tool" in
            conan) miss "$tool (>= $min)" "python3 -m pip install 'conan>=2,<3'   # or: scripts/bootstrap.sh" ;;
            *)     miss "$tool (>= $min)" "$(pkg_manager_hint "$tool")" ;;
        esac
    elif [[ -n "$min" ]] && ! version_ge "$have" "$min"; then
        old "$tool $have (need >= $min)" "$(pkg_manager_hint "$tool")"
    else
        ok "$tool $have"
    fi
done
for tool in node rust; do
    min="$(tool_min "$tool")"; have="$(tool_version "$tool")"
    if has_section desktop-shell; then
        if [[ -z "$have" ]]; then
            case "$tool" in
                node) miss "node (>= $min, desktop-shell)" "nvm install 22   # or your package manager; pin: .nvmrc" ;;
                rust) miss "rust (desktop-shell, bridge)" "curl https://sh.rustup.rs -sSf | sh   # pin: rust-toolchain.toml" ;;
            esac
        elif [[ -n "$min" ]] && ! version_ge "$have" "$min"; then
            old "$tool $have (need >= $min)" "nvm install 22"
        else
            ok "$tool $have"
        fi
    else
        [[ -z "$have" ]] && warn "$tool not installed (only needed for --sections desktop-shell / the Rust bridge)" || ok "$tool $have (optional here)"
    fi
done
if [[ "$OS" == Darwin ]]; then
    # sevenzip is in env/brew-packages.txt; the package check below owns the fix command.
    if command -v 7zz >/dev/null || command -v 7z >/dev/null; then ok "7-Zip (MindVision macOS SDK extraction)"; else miss "7-Zip (needed by scripts/provision-mindvision-sdk.sh on macOS; formula sevenzip)"; fi
fi

# ---------------------------------------------------------------- packages
case "$OS" in
    Linux)
        section "System packages (env/apt-packages.txt)"
        if ! command -v dpkg-query >/dev/null; then
            warn "not a Debian/Ubuntu host; env/apt-packages.txt lists what to map to your package manager"
        else
            absent=()
            while read -r pkg; do
                if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed'; then ok "$pkg"; else absent+=("$pkg"); fi
            done < <(packages_for "$REPO_ROOT/env/apt-packages.txt")
            if ((${#absent[@]})); then
                miss "${#absent[@]} package(s): ${absent[*]}" "$(pkg_manager_hint "${absent[@]}")"
            fi
        fi ;;
    Darwin)
        section "Homebrew packages (env/brew-packages.txt)"
        if ! command -v brew >/dev/null; then
            miss "Homebrew" '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
        else
            absent=()
            while read -r pkg; do
                if brew list --versions "$pkg" >/dev/null 2>&1; then ok "$pkg"; else absent+=("$pkg"); fi
            done < <(packages_for "$REPO_ROOT/env/brew-packages.txt")
            if ((${#absent[@]})); then
                miss "${#absent[@]} formula(e): ${absent[*]}" "$(pkg_manager_hint "${absent[@]}")"
            fi
            warn "macOS has no CMake preset yet; see knowledge_map/build-and-run/Build.md before configuring"
        fi ;;
    *)  section "System packages"; warn "unsupported host OS '$OS' for package checks (Windows: use scripts/doctor.ps1)" ;;
esac

# ---------------------------------------------------------------- conan
section "Conan"
CONAN="$(command -v conan || true)"; [[ -z "$CONAN" && -x "$REPO_ROOT/.venv/bin/conan" ]] && CONAN="$REPO_ROOT/.venv/bin/conan"
if [[ -n "$CONAN" ]]; then
    [[ "$CONAN" == "$REPO_ROOT/.venv/bin/conan" ]] && warn "conan comes from .venv; run 'source .venv/bin/activate' before conan/cmake commands"
    if "$CONAN" profile path default >/dev/null 2>&1; then ok "default profile: $("$CONAN" profile path default)"; else miss "Conan default profile" "conan profile detect"; fi
    ok "repo profiles: $(ls "$REPO_ROOT"/conan/profiles | tr '\n' ' ')"
else
    warn "conan not on PATH (checked above)"
fi

# ---------------------------------------------------------------- vendored SDK
section "MindVision SDK (scripts/provision-mindvision-sdk.sh)"
sdk_root="${MIB_MINDVISION_SDK_ROOT:-$REPO_ROOT/build/vendor/mindvision-sdk/extracted}"
if [[ -f "$sdk_root/include/CameraApi.h" ]]; then
    ok "headers at $sdk_root"
else
    miss "MindVision SDK not provisioned ($sdk_root)" "./scripts/provision-mindvision-sdk.sh   # or configure with -DMIB_ENABLE_MINDVISION=OFF"
fi

# ---------------------------------------------------------------- assets
section "External assets (env/assets.json)"
if command -v python3 >/dev/null; then
    if out="$(python3 "$REPO_ROOT/scripts/provision-assets.py" --check --required-only 2>&1)"; then
        ok "required assets present"
    else
        miss "required assets incomplete" "python3 scripts/provision-assets.py --required-only"
        $QUIET || printf '%s\n' "$out" | sed 's/^/           /'
    fi
    if $WITH_PRIVATE; then
        if [[ -n "${HF_TOKEN:-}" || -s "${HF_HOME:-$HOME/.cache/huggingface}/token" ]]; then ok "Hugging Face token available for private assets"; else miss "HF_TOKEN (private assets requested)" "export HF_TOKEN=<token with read access>   # or: hf auth login"; fi
    fi
else
    warn "python3 missing; cannot check assets"
fi

# ---------------------------------------------------------------- summary
echo
if ((MISSING == 0)); then
    echo "doctor: everything present for sections '$SECTIONS'."
    case "$OS" in
        Linux)  has_section frontend && echo "next: cmake --preset linux-system-release" || echo "next: cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build" ;;
    esac
    exit 0
fi
echo "doctor: $MISSING item(s) missing. Run these:"
printf '  %s\n' "${FIXES[@]+"${FIXES[@]}"}"
echo "or let scripts/bootstrap.sh --sections $SECTIONS do it."
exit 1
