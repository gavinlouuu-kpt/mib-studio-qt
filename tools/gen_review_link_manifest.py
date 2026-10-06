#!/usr/bin/env python3
"""Emit the link manifest the review-only Rust bridge links from (YOFO Review).

YOFO Review (plan 2026-10-01-standalone-review-app, ADR 0014) links only
`mib_review_core` + `mib_processing` and their static Conan dependencies.
Which libraries, in which order, with which frameworks / system libraries is
known only to CMake, and differs per platform (Apple frameworks, MSVC system
import libraries, Conan package paths). The *-review-core presets build a tiny
executable, `mib_review_link_probe`, that links `mib_review_core`; this script
reads its resolved link line from the Ninja build files and the review core's
compile settings from `compile_commands.json`, and writes:

    <build-dir>/mib-bridge-link-manifest.json
      { "format": "mib-review-link-v1", "scope": "review-core",
        "link": [{"kind": "static"|"dylib"|"framework", "name": ..., "dir": ...}, ...],
        "search_dirs": [...], "include_dirs": [...], "defines": [...] }

`crates/mib-bridge/build.rs` replays it (`MIB_BRIDGE_LINK_MANIFEST`, or the
default build/review-core path on macOS). Works for the Ninja generator on
macOS (clang) and Windows (MSVC); Linux is accepted too, which is how the
script is exercised outside those runners.

Usage (repo root, after `cmake --build --preset <os>-review-core-build`):

    python3 tools/gen_review_link_manifest.py [--build-dir build/review-core]

Exit code 1 when the probe's link edge or the compile entry is missing.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path, PureWindowsPath

PROBE = "mib_review_link_probe"
COMPILE_SOURCE = "src/backend/review/ReviewSession.cpp"
FORMAT = "mib-review-link-v1"


def split_windows(command: str) -> list[str]:
    """CommandLineToArgvW rules: backslashes are literal except before a quote."""
    args: list[str] = []
    cur: list[str] = []
    in_quotes = False
    have = False
    i = 0
    n = len(command)
    while i < n:
        c = command[i]
        if c == "\\":
            j = i
            while j < n and command[j] == "\\":
                j += 1
            count = j - i
            if j < n and command[j] == '"':
                cur.append("\\" * (count // 2))
                if count % 2:
                    cur.append('"')
                    i = j + 1
                else:
                    i = j
                have = True
                continue
            cur.append("\\" * count)
            i = j
            have = True
            continue
        if c == '"':
            if in_quotes and i + 1 < n and command[i + 1] == '"':
                cur.append('"')
                i += 2
                continue
            in_quotes = not in_quotes
            have = True
            i += 1
            continue
        if c in " \t" and not in_quotes:
            if have:
                args.append("".join(cur))
                cur, have = [], False
            i += 1
            continue
        cur.append(c)
        have = True
        i += 1
    if have:
        args.append("".join(cur))
    return args


def split_command(command: str, windows: bool) -> list[str]:
    return split_windows(command) if windows else shlex.split(command)


def ninja_unescape(value: str) -> str:
    return re.sub(r"\$([$ :\n])", lambda m: "" if m.group(1) == "\n" else m.group(1), value)


def split_build_line(line: str) -> tuple[list[str], str]:
    """`build <outs>: <rule> <ins>` → (unescaped outputs, rule); `$:` is an
    escaped colon (Windows drive letters), the first bare `:` ends the outputs."""
    body = line[len("build "):]
    m = re.search(r"(?<!\$):", body)
    if not m:
        return [], ""
    outs = [ninja_unescape(o) for o in re.split(r"(?<!\$) +", body[: m.start()].strip()) if o]
    rest = body[m.end():].split()
    return outs, rest[0] if rest else ""


def ninja_files(build_dir: Path) -> list[Path]:
    """build.ninja plus anything it includes / subninjas (multi-config splits)."""
    seen: list[Path] = []
    todo = [build_dir / "build.ninja"]
    while todo:
        p = todo.pop()
        if p in seen or not p.is_file():
            continue
        seen.append(p)
        for m in re.finditer(r"^(?:include|subninja)\s+(\S+)", p.read_text(encoding="utf-8", errors="replace"), re.M):
            todo.append((build_dir / ninja_unescape(m.group(1))).resolve())
    return seen


def probe_link_vars(build_dir: Path) -> dict[str, str]:
    """Variables of the build edge producing the probe executable."""
    for path in ninja_files(build_dir):
        text = path.read_text(encoding="utf-8", errors="replace").replace("$\r\n", "").replace("$\n", "")
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if not line.startswith("build "):
                continue
            outputs, rule = split_build_line(line)
            names = [Path(o.replace("\\", "/")).name for o in outputs]
            if not any(n in (PROBE, PROBE + ".exe") for n in names):
                continue
            if "LINKER" not in rule.upper():
                continue
            vars_: dict[str, str] = {}
            for v in lines[i + 1:]:
                if not v.startswith((" ", "\t")):
                    break
                key, _, value = v.strip().partition(" = ")
                vars_[key] = ninja_unescape(value)
            return vars_
    return {}


def classify_link(tokens: list[str], build_dir: Path, windows: bool):
    link: list[dict] = []
    search: list[str] = []
    ignored: list[str] = []

    def add_search(d: str):
        d = str((build_dir / d).resolve()) if not os.path.isabs(d) else d
        if d not in search:
            search.append(d)

    def lib_path(tok: str) -> Path:
        p = Path(tok)
        return p if p.is_absolute() else (build_dir / p).resolve()

    i = 0
    while i < len(tokens):
        tok = tokens[i]
        low = tok.lower()
        if windows:
            if low.startswith(("/libpath:", "-libpath:")):
                d = tok.split(":", 1)[1]
                if d not in search:
                    search.append(d if PureWindowsPath(d).is_absolute() else str(build_dir / PureWindowsPath(d).as_posix()))
            elif low.endswith(".lib"):
                # Windows path semantics on any host (the tests run on Linux).
                p = PureWindowsPath(tok)
                if p.is_absolute():
                    add_search(str(p.parent))
                    link.append({"kind": "static", "name": p.stem, "dir": str(p.parent)})
                elif len(p.parts) > 1 or (build_dir / p.as_posix()).is_file():
                    # Relative to the build tree (the review archives); a bare
                    # name the tree does not hold is a system / SDK import lib.
                    full = build_dir / p.as_posix()
                    add_search(str(full.parent))
                    link.append({"kind": "static", "name": p.stem, "dir": str(full.parent)})
                else:
                    link.append({"kind": "dylib", "name": p.stem, "dir": ""})
            else:
                ignored.append(tok)
        else:
            if tok == "-framework" and i + 1 < len(tokens):
                link.append({"kind": "framework", "name": tokens[i + 1], "dir": ""})
                i += 1
            elif tok.startswith("-framework "):
                link.append({"kind": "framework", "name": tok.split(None, 1)[1], "dir": ""})
            elif tok.startswith("-Wl,-framework,"):
                link.append({"kind": "framework", "name": tok.split(",", 2)[2], "dir": ""})
            elif tok.startswith("-L"):
                add_search(tok[2:] or tokens[i + 1])
                if tok == "-L":
                    i += 1
            elif tok.startswith("-l"):
                link.append({"kind": "dylib", "name": tok[2:], "dir": ""})
            elif re.search(r"\.(a)$", tok):
                full = lib_path(tok)
                add_search(str(full.parent))
                name = full.name[3:-2] if full.name.startswith("lib") else full.stem
                link.append({"kind": "static", "name": name, "dir": str(full.parent)})
            elif re.search(r"\.(dylib|tbd|so)(\.[0-9.]+)?$", tok):
                full = lib_path(tok)
                add_search(str(full.parent))
                base = re.sub(r"\.(dylib|tbd|so)(\.[0-9.]+)?$", "", full.name)
                link.append({"kind": "dylib", "name": base[3:] if base.startswith("lib") else base, "dir": str(full.parent)})
            else:
                ignored.append(tok)
        i += 1
    return link, search, ignored


def compile_settings(build_dir: Path, windows: bool):
    db = build_dir / "compile_commands.json"
    if not db.is_file():
        return None
    entries = json.loads(db.read_text(encoding="utf-8"))
    entry = next((e for e in entries if e["file"].replace("\\", "/").endswith(COMPILE_SOURCE)), None)
    if entry is None:
        return None
    args = entry.get("arguments") or split_command(entry["command"], windows)
    base = Path(entry.get("directory", build_dir))
    includes: list[str] = []
    defines: list[str] = []

    def inc(d: str):
        absolute = PureWindowsPath(d).is_absolute() if windows else os.path.isabs(d)
        d = d if absolute else str((base / d).resolve())
        if d not in includes:
            includes.append(d)

    i = 0
    prefixes_inc = ("-I", "/I", "-isystem", "-external:I", "/external:I", "-imsvc", "/imsvc")
    while i < len(args):
        a = args[i]
        matched = False
        for p in prefixes_inc:
            if a == p and i + 1 < len(args):
                inc(args[i + 1])
                i += 1
                matched = True
                break
            if a.startswith(p) and len(a) > len(p):
                inc(a[len(p):])
                matched = True
                break
        if not matched:
            if a in ("-D", "/D") and i + 1 < len(args):
                defines.append(args[i + 1])
                i += 1
            elif a.startswith(("-D", "/D")) and len(a) > 2:
                defines.append(a[2:])
        i += 1
    return includes, defines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build/review-core")
    parser.add_argument("--out", default=None, help="default <build-dir>/mib-bridge-link-manifest.json")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--windows", dest="windows", action="store_true", default=os.name == "nt",
                      help="parse MSVC command lines (default on Windows)")
    mode.add_argument("--posix", dest="windows", action="store_false",
                      help="parse POSIX command lines (default elsewhere)")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    vars_ = probe_link_vars(build_dir)
    if not vars_:
        print(f"no link edge for {PROBE} in {build_dir}/build.ninja (configure with a *-review-core preset)", file=sys.stderr)
        return 1
    tokens = split_command(" ".join(filter(None, [vars_.get("LINK_PATH", ""), vars_.get("LINK_LIBRARIES", "")])), args.windows)
    link, search, ignored = classify_link(tokens, build_dir, args.windows)
    names = [l["name"] for l in link]
    for required in ("mib_review_core", "mib_processing"):
        if required not in names:
            print(f"{required} is not on the probe's link line: {names}", file=sys.stderr)
            return 1
    settings = compile_settings(build_dir, args.windows)
    if settings is None:
        print(f"no compile entry for {COMPILE_SOURCE} in {build_dir}/compile_commands.json", file=sys.stderr)
        return 1
    include_dirs, defines = settings

    manifest = {
        "format": FORMAT,
        "scope": "review-core",
        "platform": sys.platform,
        "build_dir": str(build_dir),
        "link": link,
        "search_dirs": search,
        "include_dirs": include_dirs,
        "defines": defines,
        "ignored_link_tokens": ignored,
    }
    out = Path(args.out) if args.out else build_dir / "mib-bridge-link-manifest.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}: {len(link)} link entries, {len(search)} search dirs, "
          f"{len(include_dirs)} include dirs, {len(defines)} defines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
