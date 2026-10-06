"""The desktop installer ships no processing-core plugins (ADR 0007: cores
reach rigs only as signed registry downloads)."""
import re
import unittest
from pathlib import Path

ISS = Path(__file__).resolve().parents[2] / "resources" / "installers" / "mib-studio-qt.iss"


class InstallerExcludesCores(unittest.TestCase):
    def test_wildcard_dll_line_excludes_processing_cores(self) -> None:
        text = ISS.read_text(encoding="utf-8")
        lines = [
            line for line in text.splitlines()
            if line.startswith("Source:") and re.search(r'\\\*\.dll"', line)
        ]
        self.assertEqual(len(lines), 1, f"expected one *.dll Source line, found {lines}")
        self.assertIn('Excludes: "mib_processing_core*.dll"', lines[0])

    def test_old_cores_are_removed_on_upgrade(self) -> None:
        text = ISS.read_text(encoding="utf-8")
        self.assertRegex(text, r"(?m)^\[InstallDelete\]\s*$")
        self.assertRegex(text, r'(?m)^Type: files; Name: "\{app\}\\mib_processing_core\*\.dll"\s*$')


if __name__ == "__main__":
    unittest.main()
