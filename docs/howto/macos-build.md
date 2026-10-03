# Building YOFO Review on macOS

YOFO Review is the standalone review app (plan
`docs/exec-plans/active/2026-10-01-standalone-review-app.md`, ADR 0008). On
macOS it is built from the same `desktop/` tree as MIB Studio's React + Tauri
shell, with the review core linked statically: the app carries no third-party
dylibs, so the DMG is just `YOFO Review.app`. CI does exactly this on every
push (`.github/workflows/review-ci.yml`, job `review-macos`); the steps below
are the same commands.

The DMG is **not signed or notarised** (decision log 2026-10-01). The bundle
carries an ad-hoc signature (`bundle.macOS.signingIdentity: "-"`), so on first
launch macOS shows "cannot be opened" — open it from **System Settings ▸
Privacy & Security ▸ Open Anyway** (or right-click ▸ Open).

## Prerequisites

- Apple silicon Mac, macOS 13 or later, Xcode command-line tools.
- Python 3.10+ with `pip install conan ninja`, CMake 3.21+.
- Rust (`rustup`, stable) and Node 22.

## 1. Static dependencies (Conan)

```bash
conan profile detect --force       # once: apple-clang version from your Xcode
conan install . -of build/review-core --build=missing \
  -o "&:review_core=True" \
  -pr:h conan/profiles/macos-appleclang-arm64 \
  -pr:b conan/profiles/macos-appleclang-arm64
```

`review_core=True` (in `conanfile.py`) resolves only spdlog, HDF5, OpenCV
(core / imgproc / imgcodecs / videoio, no FFmpeg) and nlohmann_json, all
static; no Qt. The profile pins `os.version=13.0`, the bundle's
`minimumSystemVersion`. The first run builds the packages from source (tens of
minutes); later runs reuse `~/.conan2`.

## 2. Review core and link manifest

```bash
cmake --preset macos-review-core
cmake --build --preset macos-review-core-build
./build/review-core/mib_review_link_probe        # prints "review core linked and running"
python3 tools/gen_review_link_manifest.py        # → build/review-core/mib-bridge-link-manifest.json
```

The preset configures the processing-only tree (`mib_processing`,
`mib_review_core`) plus `mib_review_link_probe`; the generator records the
probe's CMake-resolved link line (archives, frameworks, system libraries, in
order) and compile settings for the Rust bridge.

## 3. Test and bundle

```bash
export MACOSX_DEPLOYMENT_TARGET=13.0 MIB_BRIDGE_NO_CMAKE=1
(cd crates/mib-bridge && cargo test --features review-only)
cd desktop && npm install
npm run tauri:review:build -- --features review-only --bundles app,dmg
```

`crates/mib-bridge/build.rs` reads `build/review-core/mib-bridge-link-manifest.json`
by default on macOS (override with `MIB_BRIDGE_LINK_MANIFEST`). Output:
`desktop/src-tauri/target/release/bundle/dmg/YOFO Review_<version>_aarch64.dmg`.

Check it the way CI does: `otool -L` on
`YOFO Review.app/Contents/MacOS/yofo-review` lists only `/usr/lib` and
`/System` libraries, and `codesign -dv` reports `Signature=adhoc`.

## Troubleshooting

- **`review bridge link manifest … unreadable`** — run step 2; the bridge does
  not fall back to Homebrew libraries.
- **Undefined symbols from OpenCV / HDF5** — the manifest is stale (a new
  Conan graph or preset change): rebuild step 2 and re-run the generator.
- **`building for macOS-13.0, but linking with dylib built for 14.x`** — a
  dependency came from Homebrew instead of Conan; make sure
  `MACOSX_DEPLOYMENT_TARGET=13.0` is exported and the Conan step used the
  profile above.
