#!/usr/bin/env python3
"""Mechanical checks for the repository knowledge base.

Enforces the harness invariants documented in docs/golden-principles.md:

1. [[WikiLinks]] in knowledge_map/ resolve to real vault notes.
2. Relative markdown links in agent-facing docs resolve to real files.
3. Root AGENTS.md stays a short map (<= 120 lines), not an encyclopedia.
4. Every active execution plan declares a Status: line.
5. Every Hugging Face Hub id referenced by scripts, tests, tools, workflows or
   how-tos is declared in env/assets.json (assets are pinned, not ad hoc).

Error messages include remediation instructions so an agent can fix
violations without extra context. Exit code 0 = clean, 1 = violations.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VAULT = REPO_ROOT / "knowledge_map"
AGENTS_MD_MAX_LINES = 120

WIKILINK_RE = re.compile(r"\[\[([^\]\|#]+)(?:#[^\]\|]*)?(?:\|[^\]]*)?\]\]")
MD_LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)\)")

# Markdown sources whose relative links must resolve. The vendor pump docs and
# Obsidian config are out of scope.
LINK_CHECK_FILES = [
    REPO_ROOT / "AGENTS.md",
    REPO_ROOT / "CLAUDE.md",
    REPO_ROOT / "README.md",
    *sorted(p for p in (REPO_ROOT / "docs").rglob("*.md") if "Longer Pump" not in str(p)),
]


def vault_notes() -> dict[str, list[Path]]:
    """Map note stem (and vault-relative stem path) to note files."""
    index: dict[str, list[Path]] = {}
    for note in VAULT.rglob("*.md"):
        if ".obsidian" in note.parts:
            continue
        rel = note.relative_to(VAULT)
        index.setdefault(note.stem, []).append(note)
        index.setdefault(str(rel.with_suffix("")), []).append(note)
    return index


def check_wikilinks(errors: list[str]) -> None:
    index = vault_notes()
    for note in sorted(VAULT.rglob("*.md")):
        if ".obsidian" in note.parts:
            continue
        text = note.read_text(encoding="utf-8")
        # Skip fenced code blocks so examples like [[WikiLink]] syntax demos
        # inside ``` blocks are not treated as live links.
        text = re.sub(r"```.*?```", "", text, flags=re.DOTALL)
        for match in WIKILINK_RE.finditer(text):
            target = match.group(1).strip()
            if not target or target == "WikiLinks" or target == "WikiLink":
                continue
            # Relative-path style: [[../services/Foo]] resolved from the note's dir.
            relative_hit = (note.parent / target).with_suffix(".md")
            if target not in index and not relative_hit.exists():
                rel = note.relative_to(REPO_ROOT)
                errors.append(
                    f"{rel}: broken wikilink '[[{target}]]'. Create the note, "
                    f"fix the name, or remove the link. Existing notes: "
                    f"ls knowledge_map/**/*.md"
                )


def check_md_links(errors: list[str]) -> None:
    for md_file in LINK_CHECK_FILES:
        if not md_file.exists():
            continue
        text = md_file.read_text(encoding="utf-8")
        for match in MD_LINK_RE.finditer(text):
            target = match.group(1)
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            path_part = target.split("#", 1)[0]
            if not path_part:
                continue
            resolved = (md_file.parent / path_part).resolve()
            if not resolved.exists():
                rel = md_file.relative_to(REPO_ROOT)
                errors.append(
                    f"{rel}: broken link '{target}'. Fix the path or remove "
                    f"the link; if the target moved, update every reference "
                    f"(rg -l '{path_part}')."
                )


def check_agents_md_length(errors: list[str]) -> None:
    agents_md = REPO_ROOT / "AGENTS.md"
    lines = agents_md.read_text(encoding="utf-8").count("\n") + 1
    if lines > AGENTS_MD_MAX_LINES:
        errors.append(
            f"AGENTS.md is {lines} lines (max {AGENTS_MD_MAX_LINES}). It must "
            f"stay a table of contents: move detail into docs/ or the vault "
            f"and link to it."
        )


def check_active_plans(errors: list[str]) -> None:
    active_dir = REPO_ROOT / "docs" / "exec-plans" / "active"
    for plan in sorted(active_dir.glob("*.md")):
        text = plan.read_text(encoding="utf-8")
        if not re.search(r"^Status:\s*\S", text, flags=re.MULTILINE):
            errors.append(
                f"{plan.relative_to(REPO_ROOT)}: missing 'Status:' line. Add "
                f"'Status: active|blocked|completed' near the top (see "
                f"docs/exec-plans/README.md)."
            )


HUB_ID_RE = re.compile(r"\bgavinlouuu/[A-Za-z0-9_.-]+")
HUB_ID_SCAN_DIRS = ["scripts", "tests", "tools", "docs/howto", ".github"]
HUB_ID_SCAN_SUFFIXES = {".py", ".sh", ".ps1", ".yml", ".yaml", ".md", ".cpp", ".h", ".json", ".txt", ".cmake"}


def check_hub_ids(errors: list[str]) -> None:
    """Every `gavinlouuu/<repo>` Hub id in code, harnesses, CI and how-tos must be
    an asset in env/assets.json, so nothing depends on an unpinned corpus."""
    manifest = REPO_ROOT / "env" / "assets.json"
    if not manifest.exists():
        errors.append("env/assets.json is missing; it declares every external dataset/model.")
        return
    declared = set(HUB_ID_RE.findall(manifest.read_text(encoding="utf-8")))
    for scan_dir in HUB_ID_SCAN_DIRS:
        for path in sorted((REPO_ROOT / scan_dir).rglob("*")):
            if not path.is_file() or path.suffix not in HUB_ID_SCAN_SUFFIXES:
                continue
            if "kedro_frame_detection/data" in str(path) or "__pycache__" in path.parts:
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            for hub_id in sorted(set(HUB_ID_RE.findall(text)) - declared):
                errors.append(
                    f"{path.relative_to(REPO_ROOT)}: Hub id '{hub_id}' is not declared in "
                    f"env/assets.json. Add an asset entry (repo, revision, files or viewer) "
                    f"and read it through scripts/assets_manifest.py."
                )


def main() -> int:
    errors: list[str] = []
    check_wikilinks(errors)
    check_md_links(errors)
    check_agents_md_length(errors)
    check_active_plans(errors)
    check_hub_ids(errors)

    if errors:
        print(f"check_docs: {len(errors)} violation(s)\n", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("check_docs: knowledge base OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
