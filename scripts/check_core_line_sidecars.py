#!/usr/bin/env python3
"""Validate the built processing-core artifacts and sidecars of every line.

Usage: check_core_line_sidecars.py <build config dir> <os> <arch>
"""
import json
import sys
from pathlib import Path

LINES = {
    "subtract-ring": {"contract_version": 1, "engine_abi_version": 1, "entrypoint": "mib_processing_get_api"},
    "absdiff-laplacian": {"contract_version": 2, "engine_abi_version": 2, "entrypoint": "mib_processing_get_api_v2"},
}


def main(argv: list[str]) -> int:
    root, os_name, arch = Path(argv[1]), argv[2], argv[3]
    suffix = ".dll" if os_name == "windows" else ".so"
    failures: list[str] = []
    for line, expected in LINES.items():
        sidecars = sorted(root.glob(f"mib_processing_core-{line}-*-{os_name}_{arch}.json"))
        if len(sidecars) != 1:
            failures.append(f"{line}: expected one sidecar, found {[p.name for p in sidecars]}")
            continue
        meta = json.loads(sidecars[0].read_text(encoding="utf-8"))
        stem = f"mib_processing_core-{line}-{meta.get('version')}-{os_name}_{arch}"
        checks = {
            "algorithm": line,
            "filename": stem + suffix,
            "os": os_name,
            "arch": arch,
            **expected,
        }
        for key, want in checks.items():
            if meta.get(key) != want:
                failures.append(f"{line}: {key}={meta.get(key)!r}, expected {want!r}")
        if sidecars[0].name != stem + ".json":
            failures.append(f"{line}: sidecar name {sidecars[0].name} does not match {stem}.json")
        if not (root / (stem + suffix)).is_file():
            failures.append(f"{line}: artifact {stem + suffix} missing")
    for failure in failures:
        print(f"FAIL {failure}")
    print("core line sidecars OK" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
