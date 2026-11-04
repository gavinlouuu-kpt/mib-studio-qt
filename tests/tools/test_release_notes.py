import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_notes", ROOT / "scripts/release_notes.py")
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)


def fixture():
    return "# MIB Studio v9.8.7 — 2026-10-07\n\n" + "\n\n".join(
        "## " + name + "\n\n" + ("Operators can read Help offline (#573)." if name in ("Highlights", "Added") else "None.")
        for name in notes.SECTIONS) + "\n"


class ReleaseNotesTest(unittest.TestCase):
    def test_gate(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(notes, "ROOT", Path(tmp)):
            self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 1)
            path = Path(tmp) / "docs/release-notes/v9.8.7.md"
            path.parent.mkdir(parents=True)
            path.write_text(fixture(), encoding="utf-8")
            self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 0)
            path.write_text(fixture().replace("## Added\n\nOperators can read Help offline (#573).", "## Added\n\nNone."), encoding="utf-8")
            self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 0)
            for text in (fixture().replace("## Added\n\nOperators can read Help offline (#573).", "## Added\n\n"),
                         fixture().replace("Operators can read Help offline (#573).", "TODO"),
                         fixture().replace("Operators can read Help offline (#573).", "")):
                path.write_text(text, encoding="utf-8")
                self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 1)

    def test_summary(self):
        self.assertLessEqual(len(notes.plain("é" * 20000).encode()), 16384)
        self.assertEqual(notes.plain("# Title\n\n**Hello** [manual](index.md)"), "Title\n\nHello manual")

    def test_beta_cutoff(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / "knowledge_map/current-state/recent"
            folder.mkdir(parents=True)
            (folder / "2026-10-06-old.md").write_text("old", encoding="utf-8")
            (folder / "2026-10-07-new.md").write_text(
                "new, see [[architecture/AppBackend]] and [[build-and-run/Assets|the assets note]]", encoding="utf-8")
            with patch.object(notes.subprocess, "check_output", return_value="2026-10-06\n"):
                text = notes.generate_beta(root, "v9.8.6", "9.8.7-beta.1")
            self.assertIn("Changes since v9.8.6 (beta, uncurated)", text)
            self.assertIn("new", text)
            self.assertNotIn("old", text)
            self.assertIn("see AppBackend and the assets note", text)
            self.assertNotIn("[[", text)
            self.assertNotIn("not listed here", text)

    @staticmethod
    def git(args, **_):
        return "abc1234def\n" if args[1] == "rev-parse" else "2026-10-06\n"

    @staticmethod
    def fragments(root, count, size):
        folder = root / "knowledge_map/current-state/recent"
        folder.mkdir(parents=True)
        for i in range(count):
            (folder / f"2026-10-07-{i:03d}.md").write_text(
                f"## 2026-10-07 — entry-{i:03d}\n\n" + "A line of the change text.\n" * (size // 27), encoding="utf-8")

    def test_beta_oversized_keeps_newest(self):
        # 100 entries of about 3.3 KB are far past GitHub's 125000 character limit.
        with tempfile.TemporaryDirectory() as tmp:
            self.fragments(Path(tmp), 100, 3300)
            with patch.object(notes.subprocess, "check_output", side_effect=self.git):
                text = notes.generate_beta(Path(tmp), "v9.8.6", "9.8.7-beta.1")
        self.assertLessEqual(len(text), notes.NOTES_LIMIT)
        self.assertLess(notes.NOTES_LIMIT, notes.GITHUB_BODY_LIMIT)
        kept = [int(n) for n in re.findall(r"(?m)^## 2026-10-07 — entry-(\d{3})$", text)]
        omitted = 100 - len(kept)
        self.assertTrue(0 < len(kept) < 100)
        self.assertEqual(kept, list(range(omitted, 100)))  # the newest, in the original order
        self.assertTrue(text.startswith("# MIB Studio v9.8.7-beta.1 — "))
        self.assertIn("## Changes since v9.8.6 (beta, uncurated)\n\nThe %d oldest of 100 changes" % omitted, text)
        self.assertIn(notes.REPO_URL + "/compare/v9.8.6...abc1234def", text)
        self.assertNotIn("entry-000", text)

    def test_beta_single_entry_over_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.fragments(Path(tmp), 1, notes.GITHUB_BODY_LIMIT * 2)
            with patch.object(notes.subprocess, "check_output", side_effect=self.git):
                text = notes.generate_beta(Path(tmp), "v9.8.6", "9.8.7-beta.1")
        self.assertLessEqual(len(text), notes.NOTES_LIMIT)
        self.assertIn("## 2026-10-07 — entry-000", text)
        self.assertIn("The rest of this change is cut off", text)
        self.assertNotIn("oldest of", text)

    def test_beta_file_uses_lf(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(notes, "ROOT", Path(tmp)), \
                patch.object(notes, "generate_beta", return_value="# t\n\nbody\n"):
            self.assertEqual(notes.main(["--version", "9.8.7-beta.1", "--beta", "--since", "v9.8.6"]), 0)
            self.assertEqual((Path(tmp) / "docs/release-notes/v9.8.7-beta.1.md").read_bytes(), b"# t\n\nbody\n")

    def test_stable_over_limit_fails_gate(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(notes, "ROOT", Path(tmp)):
            path = Path(tmp) / "docs/release-notes/v9.8.7.md"
            path.parent.mkdir(parents=True)
            for extra, code in (("A line of text.\n" * 5000, 0), ("A line of text.\n" * 9000, 1)):
                path.write_text(fixture().replace("None.", extra, 1), encoding="utf-8")
                self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), code, len(extra))


if __name__ == "__main__":
    unittest.main()
