# 2026-09-09 — Windows Ninja fast local loop (`windows-ninja` preset)

> Moved from `build-and-run/Build.md` on 2026-09-21; the current preset
> description lives there, this note keeps the measurements and the Rust
> bridge procedure.

## Why

The VS generator relinks everything and ignores compiler launchers: on the
bench PC a no-op build took 56 s, a header touch 107 s, a full build ~10 min.

## Preset

`windows-ninja` is a single-config Release tree in `build-ninja/` (executables
in `build-ninja/Release/`, the same layout as the VS tree: every target sets
`RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/$<CONFIG>`, which ConfigTabs'
dev config dir `<exe>/../include` and the packaging paths rely on). It needs a
VS 2022 x64 developer shell (`vcvars64.bat`) and its own Conan toolchain:

```
conan install . -of build-ninja --build=missing -s build_type=Release -pr conan/profiles/windows-msvc194-ninja
```

(add `-r conancenter` if the team remote prompts for credentials). Set
`MIB_MINDVISION_SDK_ROOT` in the environment before the first configure of a
new build dir (`scripts/bootstrap.ps1 -Generator Ninja` does all of this).

## Measurements (bench PC, sccache warm)

| Scenario | Ninja | VS generator |
|---|---|---|
| cold full build | 67 s | ~10 min |
| no-op | 0.1 s | 56 s |
| header touch | 16 s | 107 s |
| clean rebuild | 26 s | — |

`cmake/MIBCompilerSettings.cmake` uses `sccache` as the C/C++ compiler
launcher whenever it is on PATH (`winget install Mozilla.sccache`; override
with `-DMIB_COMPILER_LAUNCHER=`). Release compiles with `/Z7` (debug info in
the object, cacheable) — the PDB is still produced at link by `/DEBUG`. Works
with Ninja/Makefiles; the VS generator ignores compiler launchers.

`windows-ninja-ci` is the same layout in `build/` and is what
`build-windows.yml` uses since 2026-09-09 (`ilammy/msvc-dev-cmd` provides
cl.exe, `mozilla-actions/sccache-action` the cache).

## Rust bridge on the Ninja tree (2026-09-16)

`python tools/gen_bridge_link_manifest_ninja.py` reads `build-ninja/build.ninja`
(the `mib_backend_smoke_test` link line and `mib_backend` compile settings)
and writes `build-ninja/mib-bridge-link-manifest.json`; then, from a VS 2022
x64 shell with `MIB_BRIDGE_NO_CMAKE=1`,
`MIB_BRIDGE_LINK_MANIFEST=<repo>\build-ninja\mib-bridge-link-manifest.json`
and `build-ninja\Release` on `PATH`, `cargo test --release` in
`crates/mib-bridge` runs the contract tests against the fast local build (the
VS-generator `tools/gen_bridge_link_manifest.py` path still works).

Related: [[../build-and-run/Build]], [[../architecture/Rust-Bridge]].
