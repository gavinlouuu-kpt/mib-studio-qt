#!/usr/bin/env python3
"""Guards against processing contract and package-version declaration drift.

contract_version (docs/gold_standard_metrics.md, "Portable Processing
Contract") is currently hardcoded in several places rather than read from one
source of truth -- a real drift risk flagged when publish-processing-core.py
(A4) was added. This test is the cheap safety net until/unless that's
refactored into a single source: it fails loudly if any declaration is
bumped without the others, rather than letting them silently diverge.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class ContractVersionConsistencyTest(unittest.TestCase):
    def test_all_declarations_agree(self) -> None:
        declarations: dict[str, int] = {}

        export_hdf5 = (REPO_ROOT / "scripts" / "export_hdf5.py").read_text(encoding="utf-8")
        match = re.search(r"GOLD_STANDARD_SCHEMA_VERSION\s*=\s*(\d+)", export_hdf5)
        self.assertIsNotNone(match, "GOLD_STANDARD_SCHEMA_VERSION not found in scripts/export_hdf5.py")
        declarations["scripts/export_hdf5.py"] = int(match.group(1))

        convert_csv = (REPO_ROOT / "scripts" / "convert_legacy_csv_to_json.py").read_text(encoding="utf-8")
        match = re.search(r"GOLD_STANDARD_SCHEMA_VERSION\s*=\s*(\d+)", convert_csv)
        self.assertIsNotNone(match, "GOLD_STANDARD_SCHEMA_VERSION not found in scripts/convert_legacy_csv_to_json.py")
        declarations["scripts/convert_legacy_csv_to_json.py"] = int(match.group(1))

        schema = json.loads((REPO_ROOT / "docs" / "gold_standard_metrics.schema.json").read_text(encoding="utf-8"))
        declarations["docs/gold_standard_metrics.schema.json"] = schema["properties"]["version"]["const"]

        bindings_cpp = (REPO_ROOT / "bindings" / "python" / "src" / "bindings.cpp").read_text(encoding="utf-8")
        match = re.search(r'm\.attr\("CONTRACT_VERSION"\)\s*=\s*(\d+)', bindings_cpp)
        self.assertIsNotNone(match, "CONTRACT_VERSION not found in bindings/python/src/bindings.cpp")
        declarations["bindings/python/src/bindings.cpp"] = int(match.group(1))

        publish_script = (REPO_ROOT / "scripts" / "release" / "publish-processing-core.py").read_text(encoding="utf-8")
        match = re.search(r"DEFAULT_CONTRACT_VERSION\s*=\s*(\d+)", publish_script)
        self.assertIsNotNone(match, "DEFAULT_CONTRACT_VERSION not found in scripts/release/publish-processing-core.py")
        declarations["scripts/release/publish-processing-core.py"] = int(match.group(1))

        abi_header = (
            REPO_ROOT / "include" / "backend" / "processing" / "ProcessingCoreAbi.h"
        ).read_text(encoding="utf-8")
        match = re.search(r"#define\s+MIB_PROCESSING_CONTRACT_VERSION\s+(\d+)u?", abi_header)
        self.assertIsNotNone(match, "MIB_PROCESSING_CONTRACT_VERSION not found in ABI header")
        declarations["include/backend/processing/ProcessingCoreAbi.h"] = int(match.group(1))

        backend_cmake = (REPO_ROOT / "src" / "backend" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        match = re.search(r'\\"contract_version\\":\s*(\d+)', backend_cmake)
        self.assertIsNotNone(match, "native sidecar contract_version not found in backend CMake")
        declarations["src/backend/CMakeLists.txt native sidecar"] = int(match.group(1))

        config = json.loads((REPO_ROOT / "resources" / "defaults" / "config.json").read_text(encoding="utf-8"))
        declarations["resources/defaults/config.json"] = config["config_schema_version"]

        values = set(declarations.values())
        self.assertEqual(
            len(values),
            1,
            f"contract_version declarations disagree: {declarations}. "
            "Bump every declaration together, or fold them into one source of truth.",
        )

    def test_native_engine_abi_declarations_agree(self) -> None:
        abi_header = (
            REPO_ROOT / "include" / "backend" / "processing" / "ProcessingCoreAbi.h"
        ).read_text(encoding="utf-8")
        header_match = re.search(
            r"#define\s+MIB_PROCESSING_ENGINE_ABI_VERSION\s+(\d+)u?", abi_header
        )
        self.assertIsNotNone(header_match, "MIB_PROCESSING_ENGINE_ABI_VERSION not found")

        backend_cmake = (REPO_ROOT / "src" / "backend" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        sidecar_match = re.search(r'\\"engine_abi_version\\":\s*(\d+)', backend_cmake)
        self.assertIsNotNone(sidecar_match, "native sidecar engine_abi_version not found")
        self.assertEqual(
            int(sidecar_match.group(1)),
            int(header_match.group(1)),
            "native sidecar engine_abi_version must mirror ProcessingCoreAbi.h",
        )

    def test_wheel_version_literals_agree(self) -> None:
        """pyproject is authoritative; the import-time wrapper literal mirrors it."""
        pyproject = (REPO_ROOT / "bindings" / "python" / "pyproject.toml").read_text(encoding="utf-8")
        match = re.search(r'(?m)^version\s*=\s*"([^"]+)"\s*$', pyproject)
        self.assertIsNotNone(match, "[project].version not found in bindings/python/pyproject.toml")
        authoritative = match.group(1)

        package = (
            REPO_ROOT / "bindings" / "python" / "python" / "mib_processing" / "__init__.py"
        ).read_text(encoding="utf-8")
        match = re.search(r'(?m)^__version__\s*=\s*"([^"]+)"\s*$', package)
        self.assertIsNotNone(match, "__version__ not found in mib_processing/__init__.py")
        wrapper_version = match.group(1)

        backend_cmake = (REPO_ROOT / "src" / "backend" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            'file(READ "${PROJECT_SOURCE_DIR}/bindings/python/pyproject.toml"',
            backend_cmake,
            "Native plugin CMake must derive its release identity from the authoritative pyproject",
        )
        self.assertIn(
            'MIB_PROCESSING_CORE_VERSION="${MIB_PROCESSING_CORE_VERSION}"',
            backend_cmake,
            "Native and bundled targets must receive the version derived from pyproject",
        )

        self.assertEqual(
            wrapper_version,
            authoritative,
            "mib-processing version literals disagree. Use "
            "scripts/bump_mib_processing_version.py to update both atomically.",
        )


if __name__ == "__main__":
    unittest.main()
