#!/usr/bin/env python3
"""Emit the Windows link manifest the Rust bridge build script consumes.

On Linux `crates/mib-bridge/build.rs` links the Qt-free static archives plus a
short, fixed list of system libraries. On Windows the same `mib_backend.lib`
depends on ~190 Conan/MSVC import libraries whose exact paths only CMake
knows. Rather than duplicating that knowledge in Rust, this script reads the
Release|x64 link line and compile settings CMake generated for a backend-only
test executable and writes them to a JSON manifest:

    build/mib-bridge-link-manifest.json
      { "config": "Release",
        "lib_dirs": [...], "libs": [...],            # link line, in order
        "include_dirs": [...], "defines": [...],      # compile settings
        "runtime_dirs": ["<build>/Release"] }          # DLL search path

Usage (from the repo root, after `cmake --build build --config Release
--target mib_backend mib_processing mib_backend_smoke_test`):

    python tools/gen_bridge_link_manifest.py [--build-dir build] [--config Release]

The bridge's `build.rs` reads the manifest when `MIB_BRIDGE_NO_CMAKE=1` on
Windows (path overridable via `MIB_BRIDGE_LINK_MANIFEST`). Exit code 1 if the
project file or its link line cannot be found.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REFERENCE_PROJECT = "tests/mib_backend_smoke_test.vcxproj"
# The library project carries the full compile include/define set (Conan
# OpenCV, spdlog, Qt Core, ...) that the bridge shim needs to compile the
# backend's public headers; the test project only sees the public subset.
LIBRARY_PROJECT = "src/backend/mib_backend.vcxproj"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--out", default=None, help="manifest path (default <build-dir>/mib-bridge-link-manifest.json)")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    project = build_dir / REFERENCE_PROJECT
    if not project.is_file():
        print(f"reference project not found: {project}", file=sys.stderr)
        return 1
    text = project.read_text(encoding="utf-8")
    group = re.search(
        r"<ItemDefinitionGroup Condition=\"'\$\(Configuration\)\|\$\(Platform\)'=='%s\|x64'\">(.*?)</ItemDefinitionGroup>"
        % re.escape(args.config),
        text,
        re.S,
    )
    if not group:
        print(f"no {args.config}|x64 item definition group in {project}", file=sys.stderr)
        return 1
    seg = group.group(1)

    def items(tag: str) -> list[str]:
        m = re.search(rf"<{tag}>(.*?)</{tag}>", seg, re.S)
        if not m:
            return []
        return [x.strip() for x in m.group(1).split(";") if x.strip() and not x.strip().startswith("%")]

    project_dir = project.parent
    lib_dirs: list[str] = []
    libs: list[str] = []
    for dep in items("AdditionalDependencies"):
        p = Path(dep)
        if not p.is_absolute():
            candidate = (project_dir / p).resolve()
            if candidate.exists():
                p = candidate
        if p.suffix.lower() == ".lib" and (p.is_absolute() and p.exists()):
            d = str(p.parent)
            if d not in lib_dirs:
                lib_dirs.append(d)
            libs.append(p.stem)
        else:
            libs.append(p.stem if p.suffix.lower() == ".lib" else dep)
    for d in items("AdditionalLibraryDirectories"):
        d = str((project_dir / d).resolve()) if not Path(d).is_absolute() else d
        if d not in lib_dirs:
            lib_dirs.append(d)

    def compile_settings(seg_text: str, base: Path):
        def its(tag):
            m = re.search(rf"<{tag}>(.*?)</{tag}>", seg_text, re.S)
            return [x.strip() for x in m.group(1).split(";") if m and x.strip() and not x.strip().startswith("%")] if m else []
        incs = []
        for d in its("AdditionalIncludeDirectories") + its("ExternalIncludeDirectories"):
            pp = Path(d)
            incs.append(str((base / pp).resolve()) if not pp.is_absolute() else str(pp))
        defs = [d for d in its("PreprocessorDefinitions") if not d.startswith("CMAKE_INTDIR")]
        return incs, defs

    include_dirs, defines = compile_settings(seg, project_dir)
    lib_project = build_dir / LIBRARY_PROJECT
    if lib_project.is_file():
        lib_text = lib_project.read_text(encoding="utf-8")
        lib_group = re.search(
            r"<ItemDefinitionGroup Condition=\"'\$\(Configuration\)\|\$\(Platform\)'=='%s\|x64'\">(.*?)</ItemDefinitionGroup>"
            % re.escape(args.config), lib_text, re.S)
        if lib_group:
            li, ld = compile_settings(lib_group.group(1), lib_project.parent)
            for d in li:
                if d not in include_dirs:
                    include_dirs.append(d)
            for d in ld:
                if d not in defines and not d.startswith("mib_backend_EXPORTS"):
                    defines.append(d)
        # Conan (CMakeDeps) include paths are per-configuration generator
        # expressions, which the VS generator attaches to each source file's
        # <ClCompile> item rather than the target-wide group. Union every
        # include list that is unconditional or conditioned on this config.
        for cond, body in re.findall(r'<AdditionalIncludeDirectories(?: Condition="([^"]*)")?>(.*?)</AdditionalIncludeDirectories>', lib_text, re.S):
            if cond and args.config not in cond:
                continue
            for d in body.split(";"):
                d = d.strip()
                if not d or d.startswith("%"):
                    continue
                pp = Path(d)
                full = str((lib_project.parent / pp).resolve()) if not pp.is_absolute() else str(pp)
                if full not in include_dirs:
                    include_dirs.append(full)

    # DLL search path for running bridge binaries without the app's deploy
    # step: the build output, every Conan package's bin/ next to a lib/ we
    # link, and the vendored Coremor DLL folder.
    runtime_dirs = [str(build_dir / args.config)]
    for d in lib_dirs:
        bin_dir = Path(d).parent / "bin"
        if Path(d).name.lower() == "lib" and bin_dir.is_dir() and str(bin_dir) not in runtime_dirs:
            runtime_dirs.append(str(bin_dir))
        if Path(d).name.lower() == "coremor" and d not in runtime_dirs:
            runtime_dirs.append(d)
    manifest = {
        "config": args.config,
        "reference_project": str(project),
        "lib_dirs": lib_dirs,
        "libs": libs,
        "include_dirs": include_dirs,
        "defines": defines,
        "runtime_dirs": runtime_dirs,
    }
    out = Path(args.out) if args.out else build_dir / "mib-bridge-link-manifest.json"
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"wrote {out}: {len(libs)} libs, {len(lib_dirs)} lib dirs, {len(include_dirs)} include dirs, {len(defines)} defines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
