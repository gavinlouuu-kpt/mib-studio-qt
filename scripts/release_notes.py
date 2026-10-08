#!/usr/bin/env python3
"""Validate curated release notes, generate beta notes, or emit a bounded summary."""
import argparse
from datetime import date
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SECTIONS = ("Highlights", "Added", "Changed", "Fixed", "Removed", "Known issues",
            "Hardware & compatibility", "Upgrade notes")
LIMIT = 16 * 1024


def capped(text):
    return text.encode("utf-8")[:LIMIT].decode("utf-8", errors="ignore")


def unlink_vault(text):
    # Vault wikilinks mean nothing to an operator: [[a/B]] -> B, [[a/B|label]] -> label.
    return re.sub(r"\[\[(?:[^]|]*/)?([^]|]+)(?:\|([^]]+))?\]\]", lambda m: m[2] or m[1], text)


def plain(text):
    text = re.sub(r"!\[([^]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", text)
    return capped(re.sub(r"(?m)^#+\s*|[*`]", "", text).strip())


def validate(text, version):
    if not re.match(r"^# MIB Studio v" + re.escape(version) + r" — \d{4}-\d{2}-\d{2}\n", text):
        raise ValueError("use the required version/date title")
    if re.search(r"TODO|TBD|PLACEHOLDER|X\.Y\.Z|YYYY-MM-DD|<[^>]+>", text, re.I):
        raise ValueError("replace template placeholders")
    for section in SECTIONS:
        match = re.search(r"(?m)^## " + re.escape(section) + r"\s*\n(.*?)(?=^## |\Z)", text, re.S | re.M)
        if not match:
            raise ValueError("add section: " + section)
        if section in ("Highlights", "Added"):
            body = match[1].strip().strip("-* ")
            if not body:
                raise ValueError("fill section: " + section)


def generate_beta(root, since, version):
    # Local git read only; the tag's commit date defines the fragment cutoff.
    cutoff = subprocess.check_output(
        ["git", "show", "-s", "--format=%cs", since + "^{commit}"], cwd=root, text=True).strip()
    fragments = sorted((root / "knowledge_map/current-state/recent").glob("*.md"))
    bodies = [p.read_text(encoding="utf-8") for p in fragments
              if re.match(r"\d{4}-\d{2}-\d{2}-", p.name) and p.name[:10] > cutoff]
    text = f"# MIB Studio v{version} — {date.today().isoformat()}\n\n"
    text += f"## Changes since {since} (beta, uncurated)\n\n"
    return text + (unlink_vault("\n\n".join(bodies)) or "No recent changes recorded.\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--plain", action="store_true")
    parser.add_argument("--beta", action="store_true")
    parser.add_argument("--since")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-beta\.[A-Za-z0-9.-]+)?", args.version):
        parser.error("version must be X.Y.Z or X.Y.Z-beta.<id>")
    path = ROOT / "docs/release-notes" / f"v{args.version}.md"
    try:
        if args.beta:
            if not args.since:
                parser.error("--beta requires --since <last stable tag>")
            text = generate_beta(ROOT, args.since, args.version)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        else:
            text = path.read_text(encoding="utf-8")
        if args.check:
            validate(text, args.version)
        if args.plain:
            print(plain(text))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Release notes: {exc}. Fix {path}: copy the README template and curate every section.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
