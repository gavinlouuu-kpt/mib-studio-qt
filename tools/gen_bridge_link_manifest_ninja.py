#!/usr/bin/env python3
"""Emit the Windows link manifest for the Rust bridge from a Ninja build tree.

`tools/gen_bridge_link_manifest.py` reads the Visual Studio project files of
the `windows-default` preset. The fast local preset (`windows-ninja`,
knowledge_map/build-and-run/Build.md) has no .vcxproj, but `build.ninja`
carries the same information: the link line CMake generated for the
backend-only reference executable (`mib_backend_smoke_test`) and the compile
settings of `mib_backend`. This script writes them to the JSON manifest
`crates/mib-bridge/build.rs` consumes on Windows:

    {"config": "Release",
     "lib_dirs": [...], "libs": [...],           # link line, in order
     "include_dirs": [...], "defines": [...],     # compile settings
     "runtime_dirs": ["<build>/Release"]}         # DLL search path

Usage (repo root, after `cmake --build --preset windows-ninja-build`):

    python tools/gen_bridge_link_manifest_ninja.py [--build-dir build-ninja]
    set MIB_BRIDGE_LINK_MANIFEST=%CD%\\build-ninja\\mib-bridge-link-manifest.json
    cargo test -p mib-bridge            # from a VS 2022 x64 developer shell

Exit code 1 when the link line cannot be found.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REFERENCE_TARGET = "mib_backend_smoke_test.exe"
LIBRARY_OBJECT = r"mib_backend.dir\app\BackendFacade.cpp.obj"


def _block(text: str, header_regex: str) -> dict[str, str]:
    """Return the `  KEY = value` variables of the first build block whose
    header matches `header_regex`."""
    match = re.search(header_regex, text, re.MULTILINE)
    if not match:
        return {}
    variables: dict[str, str] = {}
    for line in text[match.end():].splitlines()[1:]:
        if not line.startswith("  "):
            break
        key, sep, value = line.strip().partition(" = ")
        if sep:
            variables[key] = value
    return variables


_TOKEN = re.compile(r'(?:[^\s"]+|"[^"]*")+')


def _split(value: str) -> list[str]:
    # Ninja lines are Windows command fragments: quotes protect spaces and may
    # start mid-token (-I"C:\Program Files\..."); backslashes are plain path
    # separators, so shlex is not suitable.
    return [tok for tok in _TOKEN.findall(value) if tok]


def _unquote(tok: str) -> str:
    return tok.replace('"', "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build-ninja")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--output", default=None, help="default: <build-dir>/mib-bridge-link-manifest.json")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    ninja = build_dir / "build.ninja"
    if not ninja.exists():
        print(f"error: {ninja} not found (configure the windows-ninja preset first)", file=sys.stderr)
        return 1
    text = ninja.read_text(encoding="utf-8", errors="replace")

    link = _block(text, rf"^build \S*{re.escape(REFERENCE_TARGET)}: CXX_EXECUTABLE_LINKER")
    if "LINK_LIBRARIES" not in link:
        print(f"error: link line for {REFERENCE_TARGET} not found in {ninja}", file=sys.stderr)
        return 1
    compile_vars = _block(text, rf"^build \S*{re.escape(LIBRARY_OBJECT)}: CXX_COMPILER")
    if "INCLUDES" not in compile_vars:
        print(f"error: compile settings for {LIBRARY_OBJECT} not found in {ninja}", file=sys.stderr)
        return 1

    lib_dirs = [str(build_dir)]
    for tok in _split(link.get("LINK_PATH", "")):
        tok = _unquote(tok)
        if tok.upper().startswith("-LIBPATH:"):
            lib_dirs.append(_unquote(tok[len("-LIBPATH:"):]))

    libs: list[str] = []
    for tok in _split(link["LINK_LIBRARIES"]):
        tok = _unquote(tok)
        if not tok.lower().endswith(".lib"):
            continue
        path = Path(tok)
        if not path.is_absolute():
            candidate = build_dir / path
            if candidate.exists():
                lib_dirs.append(str(candidate.parent))
                libs.append(candidate.stem)
                continue
            libs.append(path.stem)  # system library (kernel32.lib, ...)
            continue
        lib_dirs.append(str(path.parent))
        libs.append(path.stem)

    include_dirs: list[str] = []
    for tok in _split(compile_vars["INCLUDES"]):
        tok = _unquote(tok)
        for prefix in ("-external:I", "-I", "/external:I", "/I"):
            if tok.startswith(prefix):
                include_dirs.append(_unquote(tok[len(prefix):]))
                break

    defines: list[str] = []
    for tok in _split(compile_vars.get("DEFINES", "")):
        tok = _unquote(tok)
        if tok.startswith("-D") or tok.startswith("/D"):
            defines.append(tok[2:].replace('\\"', '"'))

    def dedupe(items: list[str]) -> list[str]:
        seen: set[str] = set()
        out: list[str] = []
        for item in items:
            key = item.lower()
            if key in seen:
                continue
            seen.add(key)
            out.append(item)
        return out

    manifest = {
        "config": args.config,
        "lib_dirs": dedupe(lib_dirs),
        "libs": libs,
        "include_dirs": dedupe(include_dirs),
        "defines": dedupe(defines),
        "runtime_dirs": [str(build_dir / args.config)],
    }
    output = Path(args.output) if args.output else build_dir / "mib-bridge-link-manifest.json"
    output.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"wrote {output}: {len(manifest['libs'])} libs, {len(manifest['lib_dirs'])} lib dirs, "
          f"{len(manifest['include_dirs'])} include dirs, {len(manifest['defines'])} defines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
