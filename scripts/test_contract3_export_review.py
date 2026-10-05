#!/usr/bin/env python3
"""Contract 3 (unet-cells) review side: a recording exports to a reviewable
gold-standard document and CSV.

The C++ side (recording.experiment_roundtrip) proves the cell members
(brightness_mean, brightness_variance, contourArea, pixelCount, blemishCount,
degenerateContour) round-trip through HDF5. This test drives the export half
over a numpy structured array shaped like that compound: a Contract-3 JSON
export carries brightness mean/variance and the cell fields instead of the
quartiles, omits ring width, and validates against
docs/gold_standard_metrics.schema.json ($defs/unet_cell_frame); a Contract-2
document keeps the quartiles. The CSV export swaps the quartile columns.
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
    ("brightness_mean", "<f8"), ("brightness_variance", "<f8"),
    ("contourArea", "<f8"), ("pixelCount", "<i4"), ("blemishCount", "<i4"),
    ("degenerateContour", "u1"),
])


def make_records():
    arr = np.zeros(2, dtype=RECORD_DTYPE)
    for i in range(2):
        arr[i]["index"] = i
        arr[i]["timestampNs"] = (i + 1) * 1000
        arr[i]["objectId"] = i + 1
        arr[i]["objectCount"] = 2
        arr[i]["deformability"] = 0.05
        arr[i]["area"] = 820.0 + i
        arr[i]["areaRatio"] = 1.02
        arr[i]["ringRatio"] = float("nan")
        arr[i]["laplacianVariance"] = 140.0 + i
        arr[i]["isValid"] = 1
        arr[i]["inRange"] = 1
        arr[i]["brightness_q1"] = arr[i]["brightness_q2"] = float("nan")
        arr[i]["brightness_q3"] = arr[i]["brightness_q4"] = float("nan")
        arr[i]["youngsModulus"] = float("nan")
        arr[i]["brightness_mean"] = 116.5 + i
        arr[i]["brightness_variance"] = 30.25 + i
        arr[i]["contourArea"] = 800.5 + i
        arr[i]["pixelCount"] = 840 + i
        arr[i]["blemishCount"] = 3
    arr[1]["brightness_variance"] = float("nan")  # not computed -> null
    return arr


try:
    import jsonschema  # bindings/python[test]; installed in the wheel CI lane
except ImportError:  # backend lane: structural check of the selected frame record
    jsonschema = None


class SchemaError(Exception):
    pass


def validate(doc) -> None:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    if jsonschema is not None:
        try:
            jsonschema.validate(doc, schema)
        except jsonschema.ValidationError as exc:
            raise SchemaError(exc.message) from exc
        return
    frame = schema["$defs"]["unet_cell_frame" if doc.get("contract_version") == 3 else "frame"]
    for obj in doc["frames"]:
        if not all(k in obj for k in frame["required"]) or not all(k in frame["properties"] for k in obj):
            raise SchemaError(f"frame does not match its record definition: {sorted(obj)}")


class Contract3ExportReviewTest(unittest.TestCase):
    def _json(self, records, contract_version):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "metrics.json"
            export_hdf5.export_metrics_to_json(records, None, out, 0.4886, "valid", "test",
                                               contract_version=contract_version)
            return json.loads(out.read_text(encoding="utf-8"))

    def test_contract3_json_is_reviewable(self) -> None:
        doc = self._json(make_records(), 3)
        self.assertEqual(doc["contract_version"], 3)
        validate(doc)
        first, second = doc["frames"]
        for frame in doc["frames"]:
            self.assertNotIn("ring_ratio", frame)
            self.assertFalse(any(k.startswith("brightness_q") for k in frame), "no quartiles")
        self.assertEqual(first["brightness_mean"], 116.5)
        self.assertEqual(first["brightness_variance"], 30.25)
        self.assertIsNone(second["brightness_variance"], "NaN is serialized as null")
        self.assertEqual((first["pixel_count"], first["blemish_count"], first["contour_area"]), (840, 3, 800.5))
        self.assertEqual(first["laplacian_variance"], 140.0)

    def test_contract3_document_with_quartiles_is_rejected(self) -> None:
        doc = self._json(make_records(), 3)
        doc["frames"][0]["brightness_q1"] = 1.0
        with self.assertRaises(SchemaError):
            validate(doc)

    def test_contract2_document_keeps_quartiles(self) -> None:
        records = make_records()
        for name in ("brightness_q1", "brightness_q2", "brightness_q3", "brightness_q4"):
            records[name] = 10.0
        doc = self._json(records, 2)
        validate(doc)
        self.assertIn("brightness_q4", doc["frames"][0])
        self.assertNotIn("brightness_mean", doc["frames"][0])

    def test_contract3_csv_swaps_brightness_columns(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "metrics.csv"
            export_hdf5.export_metrics_to_csv(make_records(), None, out, 0.4886, "valid", contract_version=3)
            header, row = out.read_text(encoding="utf-8").splitlines()[:2]
        self.assertTrue(header.endswith("Bright Mean,Bright Var,Pixels,Blemishes"))
        self.assertTrue(row.endswith("116.50,30.25,840,3"))


if __name__ == "__main__":
    unittest.main()
