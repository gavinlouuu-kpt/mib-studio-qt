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
# GitHub refuses a release body above 125000 characters (HTTP 422, "body is too long").
# Notes stay under NOTES_LIMIT, which leaves a margin for how GitHub counts characters.
GITHUB_BODY_LIMIT = 125000
NOTES_LIMIT = 120000
REPO_URL = "https://github.com/gavinlouuu-kpt/mib-studio-qt"


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
    # Curated notes are never trimmed (that would drop reviewed content), so an oversized
    # file fails here, before the build, not at "gh release create" after it.
    if len(text) > NOTES_LIMIT:
        raise ValueError(f"notes are {len(text)} characters; GitHub accepts at most "
                         f"{GITHUB_BODY_LIMIT}, so keep them under {NOTES_LIMIT}")
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


def clip(text, size):
    # Cut at the last line break that fits, so the entry does not end mid-sentence.
    head = text[:max(size, 0)]
    return head[:head.rfind("\n")] if "\n" in head else head


def fit_entries(header, entries, limit, compare_url):
    """Join the entries (oldest first) after the header, within `limit` characters.

    Whole entries are dropped from the oldest end and replaced by one line that says how many
    were left out and links the full comparison. Output within the limit is returned unchanged.
    """
    full = header + "\n\n".join(entries)
    if len(full) <= limit:
        return full

    def notice(omitted):
        if not omitted:
            return ""
        return (f"The {omitted} oldest of {len(entries)} changes are not listed here, to stay within "
                f"GitHub's size limit for release notes. Full change list: {compare_url}\n\n")

    # Size the budget for the longest notice (the one that omits every entry but one).
    budget = limit - len(header) - len(notice(len(entries)))
    kept, used = [], 0
    for entry in reversed(entries):
        cost = len(entry) + (2 if kept else 0)
        if used + cost > budget:
            break
        kept.insert(0, entry)
        used += cost
    if not kept:
        # Not even the newest entry fits: keep its beginning and say so.
        cut = f"\n\nThe rest of this change is cut off. Full change list: {compare_url}\n"
        kept = [clip(entries[-1], budget - len(cut)) + cut]
    return header + notice(len(entries) - len(kept)) + "\n\n".join(kept)


def generate_beta(root, since, version, limit=NOTES_LIMIT):
    # Local git read only; the tag's commit date defines the fragment cutoff.
    cutoff = subprocess.check_output(
        ["git", "show", "-s", "--format=%cs", since + "^{commit}"], cwd=root, text=True).strip()
    fragments = sorted((root / "knowledge_map/current-state/recent").glob("*.md"))
    bodies = [p.read_text(encoding="utf-8") for p in fragments
              if re.match(r"\d{4}-\d{2}-\d{2}-", p.name) and p.name[:10] > cutoff]
    header = f"# MIB Studio v{version} — {date.today().isoformat()}\n\n"
    header += f"## Changes since {since} (beta, uncurated)\n\n"
    if not bodies:
        return header + "No recent changes recorded.\n"
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    return fit_entries(header, [unlink_vault(b) for b in bodies], limit,
                       f"{REPO_URL}/compare/{since}...{head}")


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
            # LF on every OS: Windows text mode would add a CR per line, which GitHub counts too.
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
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
