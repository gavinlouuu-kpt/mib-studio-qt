import importlib.util
from pathlib import Path
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
            path.write_text(fixture())
            self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 0)
            path.write_text(fixture().replace("## Added\n\nOperators can read Help offline (#573).", "## Added\n\nNone."))
            self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 0)
            for text in (fixture().replace("## Added\n\nOperators can read Help offline (#573).", "## Added\n\n"),
                         fixture().replace("Operators can read Help offline (#573).", "TODO"),
                         fixture().replace("Operators can read Help offline (#573).", "")):
                path.write_text(text)
                self.assertEqual(notes.main(["--version", "9.8.7", "--check"]), 1)

    def test_summary(self):
        self.assertLessEqual(len(notes.plain("é" * 20000).encode()), 16384)
        self.assertEqual(notes.plain("# Title\n\n**Hello** [manual](index.md)"), "Title\n\nHello manual")

    def test_beta_cutoff(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / "knowledge_map/current-state/recent"
            folder.mkdir(parents=True)
            (folder / "2026-10-06-old.md").write_text("old")
            (folder / "2026-10-07-new.md").write_text("new")
            with patch.object(notes.subprocess, "check_output", return_value="2026-10-06\n"):
                text = notes.generate_beta(root, "v9.8.6", "9.8.7-beta.1")
            self.assertIn("Changes since v9.8.6 (beta, uncurated)", text)
            self.assertIn("new", text)
            self.assertNotIn("old", text)


if __name__ == "__main__":
    unittest.main()
