"""Structure of the absdiff-laplacian release workflow (what CI cannot run on a PR)."""
import unittest
from pathlib import Path

try:
    import yaml
except ImportError:  # python3-yaml (env/apt-packages.txt test-extras)
    raise unittest.SkipTest("PyYAML is not installed")

WF = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "processing-core-line.yml"
TAG_REF = "refs/tags/mib-processing-absdiff-laplacian-v"


class LineWorkflow(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.wf = yaml.safe_load(WF.read_text(encoding="utf-8"))
        cls.jobs = cls.wf["jobs"]
        cls.text = WF.read_text(encoding="utf-8")

    def test_triggers_only_on_the_absdiff_tag_and_core_paths(self) -> None:
        on = self.wf[True] if True in self.wf else self.wf["on"]  # PyYAML parses `on:` as True
        self.assertEqual(on["push"]["tags"], ["mib-processing-absdiff-laplacian-v*"])
        self.assertIn("pull_request", on)

    def test_signing_and_release_are_tag_only_in_production(self) -> None:
        for job in ("sign-windows", "sign-linux", "release"):
            self.assertEqual(self.jobs[job].get("environment"), "Production", job)
            self.assertIn(TAG_REF, self.jobs[job]["if"], job)

    def test_version_gate_runs_before_signing(self) -> None:
        self.assertIn("processing-cores/absdiff-laplacian.version", self.text)
        for job in ("build-windows", "build-linux"):
            self.assertIn("validate", self.jobs[job]["needs"])

    def test_publishes_beta_through_the_line_publisher(self) -> None:
        self.assertIn("publish-processing-core-line.py", self.text)
        self.assertIn("--channel beta", self.text)
        self.assertNotIn("--channel stable", self.text)
        self.assertNotIn("publish-processing-core.py ", self.text)

    def test_only_absdiff_artifacts_are_signed(self) -> None:
        self.assertIn("mib_processing_core-absdiff-laplacian-*-windows_x86_64.dll", self.text)
        self.assertIn("mib_processing_core-absdiff-laplacian-*-linux_x86_64.so", self.text)
        self.assertNotIn("subtract-ring", self.text)


if __name__ == "__main__":
    unittest.main()
