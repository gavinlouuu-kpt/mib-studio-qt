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


    def test_native_gold_ctest_is_not_registered_under_sanitizers(self) -> None:
        # ctypes loads the core into an uninstrumented python: an ASan/TSan
        # build of the core cannot be dlopened there.
        cmake = (REPO / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        start = cmake.index("NAME processing.native_core_contract2_gold")
        guard = cmake.rfind("if(NOT MIB_SANITIZER)", 0, start)
        self.assertNotEqual(guard, -1, "gold ctest must sit inside if(NOT MIB_SANITIZER)")
        self.assertEqual(cmake.count("endif()", guard, start), 0, "guard closes before the test")

    def test_release_runbooks_use_the_line_tag_prefix(self) -> None:
        # The release workflow only triggers on mib-processing-subtract-ring-v*;
        # the old prefix may appear only as legacy.
        stale = []
        for path in sorted((REPO / "docs" / "howto").glob("*.md")) + sorted(
            (REPO / "docs" / "architecture").glob("*.md")
        ):
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if "mib-processing-v" in line and "legacy" not in line:
                    stale.append(f"{path.relative_to(REPO)}:{number}: {line.strip()}")
        self.assertEqual(stale, [], "runbook lines still name the pre-rename tag prefix")


if __name__ == "__main__":
    unittest.main()
