"""Structure of the absdiff-laplacian release workflow (what CI cannot run on a PR)."""
import unittest
from pathlib import Path

try:
    import yaml
except ImportError:  # python3-yaml (env/apt-packages.txt test-extras)
    yaml = None

# A module-level SkipTest is an error when ctest runs this file as a script.
needs_yaml = unittest.skipUnless(yaml is not None, "PyYAML is not installed")

WF = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "processing-core-line.yml"
TAG_REF = "refs/tags/mib-processing-absdiff-laplacian-v"


@needs_yaml
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


PROMOTE = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "processing-core-promote.yml"


@needs_yaml
class PromoteWorkflow(unittest.TestCase):
    def test_line_input_routes_absdiff_to_the_line_publisher(self) -> None:
        wf = yaml.safe_load(PROMOTE.read_text(encoding="utf-8"))
        on = wf[True] if True in wf else wf["on"]
        line = on["workflow_dispatch"]["inputs"]["line"]
        self.assertEqual(line["default"], "subtract-ring")
        self.assertEqual(line["options"], ["subtract-ring", "absdiff-laplacian"])
        text = PROMOTE.read_text(encoding="utf-8")
        self.assertIn("publish-processing-core-line.py", text)
        self.assertIn("processing-core/absdiff-laplacian/latest.json", text)
        self.assertIn("inputs.line", wf["concurrency"]["group"])


WHEEL = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "python-wheel.yml"


@needs_yaml
class RegistryConcurrency(unittest.TestCase):
    def test_subtract_ring_promote_shares_the_release_publish_group(self) -> None:
        # Both write {channel}/processing-core/index.json: they must serialize.
        promote = yaml.safe_load(PROMOTE.read_text(encoding="utf-8"))["concurrency"]["group"]
        release = yaml.safe_load(WHEEL.read_text(encoding="utf-8"))["jobs"]["release"]["concurrency"]["group"]
        self.assertEqual(release, "processing-core-registry-${{ needs.validate-source-version.outputs.channel }}")
        self.assertTrue(promote.startswith("processing-core-registry-${{ inputs.channel }}${{ inputs.line != 'subtract-ring'"),
                        promote)

    def test_absdiff_promote_shares_the_line_release_group(self) -> None:
        line = yaml.safe_load(WF.read_text(encoding="utf-8"))["jobs"]["release"]["concurrency"]["group"]
        self.assertEqual(line, "processing-core-registry-beta-absdiff-laplacian")
        promote = yaml.safe_load(PROMOTE.read_text(encoding="utf-8"))["concurrency"]["group"]
        self.assertIn("format('-{0}', inputs.line)", promote)


if __name__ == "__main__":
    unittest.main()
