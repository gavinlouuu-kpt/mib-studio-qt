"""Each processing-core line has exactly one version source (ADR 0007)."""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")


class CoreLineVersions(unittest.TestCase):
    def test_absdiff_laplacian_version_file(self) -> None:
        text = (REPO / "processing-cores" / "absdiff-laplacian.version").read_text(encoding="utf-8")
        self.assertEqual(text.count("\n"), 1, "one line, newline-terminated")
        self.assertRegex(text.strip(), SEMVER)

    def test_cmake_reads_each_line_version(self) -> None:
        cmake = (REPO / "src" / "backend" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("processing-cores/absdiff-laplacian.version", cmake)
        self.assertRegex(
            cmake,
            r"mib_add_processing_core_plugin\(mib_processing_core 1 subtract-ring "
            r"\$\{MIB_PROCESSING_CORE_VERSION\}\)",
        )
        self.assertRegex(
            cmake,
            r"mib_add_processing_core_plugin\(mib_processing_core_absdiff_laplacian 2 absdiff-laplacian "
            r"\$\{MIB_ABSDIFF_LAPLACIAN_VERSION\}\)",
        )


if __name__ == "__main__":
    unittest.main()
