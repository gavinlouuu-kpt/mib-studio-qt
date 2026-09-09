#!/usr/bin/env python3
"""Regression guards for the processing-core wheel architecture matrix."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "python-wheel.yml"


class ProcessingCoreWheelArchitectureTest(unittest.TestCase):
    def test_workflow_builds_all_supported_linux_architectures(self) -> None:
        workflow = WORKFLOW_PATH.read_text(encoding="utf-8")

        image_settings = {
            "x86_64": "manylinux_2_28",
            "aarch64": "manylinux_2_28",
        }
        for architecture, image in image_settings.items():
            with self.subTest(architecture=architecture):
                for python_tag in ("cp310", "cp311", "cp312", "cp313"):
                    self.assertIn(
                        f"{python_tag}-manylinux_{architecture}",
                        workflow,
                    )
                image_variable = architecture.upper()
                self.assertIn(
                    f"CIBW_MANYLINUX_{image_variable}_IMAGE:",
                    workflow,
                )
                self.assertIn(image, workflow)

        self.assertIn("docker/setup-qemu-action@v3", workflow)
        self.assertRegex(workflow, r"platforms:\s*arm64")
        self.assertIn(
            "CIBW_BEFORE_ALL_LINUX: >-\n"
            "            dnf install -y --setopt=install_weak_deps=False "
            "epel-release",
            workflow,
        )
        self.assertIn("CIBW_TEST_COMMAND:", workflow)
        self.assertIn("assert mp.CONTRACT_VERSION == 1", workflow)
        self.assertNotIn("manylinux_i686", workflow)
        self.assertNotIn("CIBW_MANYLINUX_I686_IMAGE", workflow)

    def test_release_gate_requires_every_python_architecture_pair(self) -> None:
        workflow = WORKFLOW_PATH.read_text(encoding="utf-8")

        self.assertGreaterEqual(workflow.count("len(wheels) != 8"), 2)
        self.assertGreaterEqual(
            workflow.count('{"x86_64", "aarch64"}'),
            2,
        )
        self.assertGreaterEqual(
            len(
                re.findall(
                    r"manylinux_2_28_\(x86_64\|aarch64\)",
                    workflow,
                )
            ),
            2,
        )

if __name__ == "__main__":
    unittest.main()
