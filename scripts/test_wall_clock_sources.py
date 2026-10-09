#!/usr/bin/env python3
"""G14: every wall-clock time that ends up in a file goes through backend::app::WallClock.

The PZ7035 has no RTC, so `std::chrono::system_clock::now()` / `std::time(nullptr)` in the backend would stamp
the boot date. This guard lists the only places allowed to read the system clock directly; any new one must
either use WallClock::nowNs() or be added here with the reason it is not a persisted date."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r"system_clock::now|std::time\s*\(|\btime\s*\(\s*(nullptr|NULL|0)\s*\)|QDateTime::currentDateTime|currentMSecsSinceEpoch")
ALLOWED = {
    "include/backend/app/WallClock.h": "the source itself",
    "src/backend/app/ExperimentCoordinator.cpp": "provider run id (an opaque identifier, not a date)",
    "src/backend/app/ProfileStore.cpp": "unique suffix of backup and archive names",
    "src/backend/recording/HdfExportService.cpp": "export job id (an opaque identifier)",
    "src/backend/profiles/ProfileRegistryWorker.cpp": "registry network timestamps and token refresh: real time",
    "src/backend/profiles/SupabaseAuth.cpp": "token expiry: real time",
}


def main() -> int:
    bad = []
    for path in sorted((ROOT / "src" / "backend").rglob("*.cpp")) + sorted((ROOT / "include" / "backend").rglob("*.h")):
        rel = path.relative_to(ROOT).as_posix()
        if rel in ALLOWED:
            continue
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            code = line.split("//")[0]
            if PATTERN.search(code):
                bad.append(f"{rel}:{number}: {line.strip()}")
    # the allowed spots that stamp a *date* must not creep back: HdfExportService exportTime, the coordinator's wall-clock stamps
    coord = (ROOT / "src/backend/app/ExperimentCoordinator.cpp").read_text()
    if coord.count("system_clock::now") != 1:
        bad.append("ExperimentCoordinator.cpp: only the provider run id may read system_clock directly")
    export = (ROOT / "src/backend/recording/HdfExportService.cpp").read_text()
    if "std::time(nullptr)" in export:
        bad.append("HdfExportService.cpp: exportTime must use WallClock")
    if bad:
        print("system-clock reads outside backend::app::WallClock (route them through WallClock::nowNs()):")
        print("\n".join(bad))
        return 1
    print("every persisted wall-clock time goes through WallClock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
