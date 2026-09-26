#!/usr/bin/env python3
"""V2-6: the gold-standard metrics schema is contract-aware.

Contract-1 documents carry `ring_ratio`; Contract-2 documents carry
`laplacian_variance` and omit ring width. Neither field is required, so a
document of either contract validates, but both are declared in properties (the
schema uses additionalProperties:false, so an undeclared field is rejected).
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA = REPO_ROOT / "docs" / "gold_standard_metrics.schema.json"


def frame_def(schema: dict) -> dict:
    # The per-object frame record lives under $defs/frame.
    return schema["$defs"]["frame"]


def object_keys_ok(frame: dict, obj: dict) -> bool:
    """Minimal structural validation: required present, no undeclared keys."""
    props = frame["properties"]
    required = frame["required"]
    if not all(k in obj for k in required):
        return False
    if frame.get("additionalProperties", True) is False:
        if not all(k in props for k in obj):
            return False
    return True


class GoldStandardSchemaContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
        self.frame = frame_def(self.schema)

    def test_focus_metrics_are_optional_and_declared(self) -> None:
        props = self.frame["properties"]
        required = self.frame["required"]
        self.assertIn("ring_ratio", props, "ring_ratio must remain declared (legacy Contract 1)")
        self.assertIn("laplacian_variance", props, "laplacian_variance declared (Contract 2)")
        self.assertNotIn("ring_ratio", required, "ring_ratio must be optional so v2 docs validate")
        self.assertNotIn(
            "laplacian_variance", required, "laplacian_variance optional so v1 docs validate"
        )
        self.assertEqual(props["laplacian_variance"]["type"], "number")

    def _base_object(self) -> dict:
        # All required fields with placeholder values.
        return {
            "frame_type": "valid",
            "index": 0,
            "timestamp_ns": 0,
            "object_id": 1,
            "object_count": 1,
            "deformability": 0.1,
            "area": 100.0,
            "area_ratio": 1.0,
            "is_valid": True,
            "touches_border": False,
            "has_single_inner_contour": True,
            "in_range": True,
            "inner_contour_count": 1,
            "brightness_q1": 1.0,
            "brightness_q2": 2.0,
            "brightness_q3": 3.0,
            "brightness_q4": 4.0,
        }

    def test_contract1_document_validates(self) -> None:
        obj = self._base_object()
        obj["ring_ratio"] = 18.0  # Contract-1 focus metric, no laplacian
        self.assertTrue(object_keys_ok(self.frame, obj), "Contract-1 object validates")

    def test_contract2_document_validates(self) -> None:
        obj = self._base_object()
        obj["laplacian_variance"] = 42.0  # Contract-2 focus metric, no ring
        self.assertTrue(object_keys_ok(self.frame, obj), "Contract-2 object validates")
        self.assertNotIn("ring_ratio", obj, "Contract-2 object omits ring width")

    def test_undeclared_field_rejected(self) -> None:
        obj = self._base_object()
        obj["ring_width"] = 3.0  # not a declared property
        self.assertFalse(
            object_keys_ok(self.frame, obj), "undeclared field violates additionalProperties:false"
        )


if __name__ == "__main__":
    unittest.main()
