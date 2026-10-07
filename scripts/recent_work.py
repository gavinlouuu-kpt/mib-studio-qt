#!/usr/bin/env python3
"""Print the most recent Recent-Work entries, newest first.

Entries live one per file in knowledge_map/current-state/recent/ (see the README
there); knowledge_map/current-state/Recent-Work.md is the archive of older entries.

    python3 scripts/recent_work.py            # newest 10 entries
    python3 scripts/recent_work.py -n 30      # newest 30
    python3 scripts/recent_work.py --archive  # then continue into the archive
"""

from __future__ import annotations

import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RECENT_DIR = REPO_ROOT / "knowledge_map" / "current-state" / "recent"
ARCHIVE = REPO_ROOT / "knowledge_map" / "current-state" / "Recent-Work.md"


def fragments() -> list[Path]:
    # Names start with the ISO date, so a reverse name sort is newest first.
    return sorted((p for p in RECENT_DIR.glob("*.md") if p.name != "README.md"), reverse=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-n", type=int, default=10, help="number of entries to print (default 10)")
    parser.add_argument("--archive", action="store_true", help="continue into Recent-Work.md")
    args = parser.parse_args()

    printed = 0
    for path in fragments():
        if printed >= args.n:
            return 0
        print(path.read_text(encoding="utf-8").rstrip() + "\n")
        printed += 1
    if args.archive and printed < args.n:
        print(ARCHIVE.read_text(encoding="utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
