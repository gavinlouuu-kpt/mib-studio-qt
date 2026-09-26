#!/usr/bin/env python3
"""V2-6 e2e (review side): a Contract-2 recording exports to a reviewable
gold-standard document.

The C++ side (recording.experiment_roundtrip) proves the per-object
`laplacianVariance` is written to and read back from HDF5. This test drives the
*export/review* half over a numpy structured array shaped exactly like that HDF5
compound: a Contract-2 export carries `laplacian_variance` and omits ring width,
a Contract-1 export carries `ring_ratio`, and both validate against
docs/gold_standard_metrics.schema.json.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import export_hdf5  # noqa: E402

SCHEMA = REPO_ROOT / "docs" / "gold_standard_metrics.schema.json"

# Mirrors the HDF5 ProcessedFrameMetadataRecord compound (Hdf5Service.cpp).
RECORD_DTYPE = np.dtype([
    ("index", "<u8"), ("timestampNs", "<u8"),
    ("objectId", "<i4"), ("objectCount", "<i4"),
    ("deformability", "<f8"), ("area", "<f8"), ("areaRatio", "<f8"),
    ("ringRatio", "<f8"), ("laplacianVariance", "<f8"),
    ("isValid", "u1"), ("touchesBorder", "u1"),
    ("hasSingleInnerContour", "u1"), ("inRange", "u1"),
    ("innerContourCount", "<i4"),
    ("brightness_q1", "<f8"), ("brightness_q2", "<f8"),
    ("brightness_q3", "<f8"), ("brightness_q4", "<f8"),
    ("youngsModulus", "<f8"), ("isTargetGroup", "u1"),
])


def make_records(laplacians, ring=20.0):
    arr = np.zeros(len(laplacians), dtype=RECORD_DTYPE)
    for i, lap in enumerate(laplacians):
        arr[i]["index"] = i
        arr[i]["timestampNs"] = (i + 1) * 1000
        arr[i]["objectId"] = i + 1
        arr[i]["objectCount"] = len(laplacians)
        arr[i]["deformability"] = 0.05
        arr[i]["area"] = 120.0 + i
        arr[i]["areaRatio"] = 1.02
        arr[i]["ringRatio"] = ring
        arr[i]["laplacianVariance"] = lap
        arr[i]["isValid"] = 1
        arr[i]["inRange"] = 1
        arr[i]["hasSingleInnerContour"] = 1
        arr[i]["innerContourCount"] = 1
        arr[i]["youngsModulus"] = float("nan")
    return arr


def load_schema_frame():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    return schema["$defs"]["frame"]


def validate_frame(frame_def, obj) -> bool:
    props = frame_def["properties"]
    if not all(k in obj for k in frame_def["required"]):
        return False
    if frame_def.get("additionalProperties", True) is False:
        if not all(k in props for k in obj):
            return False
    return True


class Contract2ExportReviewTest(unittest.TestCase):
    def setUp(self) -> None:
        self.frame_def = load_schema_frame()

    def _export(self, records, contract_version):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "metrics.json"
            export_hdf5.export_metrics_to_json(
                records, None, out, 0.4886, "valid", "test",
                contract_version=contract_version,
            )
            return json.loads(out.read_text(encoding="utf-8"))

    def test_contract2_export_is_reviewable(self) -> None:
        records = make_records([42.0, 7.5])
        doc = self._export(records, contract_version=2)

        self.assertEqual(len(doc["frames"]), 2)
        self.assertGreaterEqual(doc["contract_version"], 2)
        for i, frame in enumerate(doc["frames"]):
            self.assertIn("laplacian_variance", frame, "Contract-2 export carries the focus metric")
            self.assertNotIn("ring_ratio", frame, "Contract-2 export omits ring width")
            self.assertTrue(validate_frame(self.frame_def, frame), "frame validates against schema")
        self.assertEqual(doc["frames"][0]["laplacian_variance"], 42.0)
        self.assertEqual(doc["frames"][1]["laplacian_variance"], 7.5)

    def test_contract1_export_keeps_ring(self) -> None:
        # A Contract-1 recording (no laplacian computed -> NaN) keeps ring width.
        records = make_records([float("nan"), float("nan")], ring=18.0)
        doc = self._export(records, contract_version=1)
        for frame in doc["frames"]:
            self.assertIn("ring_ratio", frame, "Contract-1 export keeps ring width")
            self.assertNotIn("laplacian_variance", frame, "NaN laplacian is omitted")
            self.assertTrue(validate_frame(self.frame_def, frame))


if __name__ == "__main__":
    unittest.main()
