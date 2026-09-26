#!/usr/bin/env python3
"""Regression tests for the full-parity metrics comparator."""

from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

import compare_metrics


def frame(index: int = 0, object_id: int = 1) -> dict:
    return {
        "frame_type": "valid",
        "index": index,
        "timestamp_ns": 0,
        "object_id": object_id,
        "object_count": 1,
        "deformability": 0.1,
        "area": 10.0,
        "area_um2": 2.5,
        "area_ratio": 1.0,
        "ring_ratio": 4.0,
        "youngs_modulus": 5.0,
        "is_valid": True,
        "touches_border": False,
        "has_single_inner_contour": True,
        "in_range": True,
        "inner_contour_count": 1,
        "brightness_q1": 1.0,
        "brightness_q2": 2.0,
        "brightness_q3": 3.0,
        "brightness_q4": 4.0,
        "is_target_group": True,
        "track_id": 1,
        "track_first_frame": 0,
        "track_last_frame": 2,
        "track_observation_count": 3,
        "mask_sha256": "a" * 64,
        "series_images_sha256": ["b" * 64, "c" * 64],
    }


def document(frames: list[dict]) -> dict:
    return {
        "version": 1,
        "contract_version": 1,
        "pixel_to_micron": 0.5,
        "frames": frames,
    }


class CompareMetricsTest(unittest.TestCase):
    def compare(self, gold: dict, candidate: dict):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            gold_path = root / "gold.json"
            candidate_path = root / "candidate.json"
            gold_path.write_text(json.dumps(gold), encoding="utf-8")
            candidate_path.write_text(json.dumps(candidate), encoding="utf-8")
            return compare_metrics.run_comparison(
                gold_path, candidate_path, {}, 1e-6, "index_type_object"
            )

    def contract2(self, laplacian: float = 12.5) -> dict:
        record = frame()
        del record["ring_ratio"]
        record["laplacian_variance"] = laplacian
        doc = document([record])
        doc["contract_version"] = 2
        return doc

    def test_contract2_document_needs_no_ring_ratio(self) -> None:
        matched, total, results = self.compare(self.contract2(), self.contract2())
        self.assertEqual((matched, total), (1, 1))
        self.assertTrue(all(result[2] for result in results))

    def test_contract2_laplacian_drift_fails(self) -> None:
        _, _, results = self.compare(self.contract2(12.5), self.contract2(12.6))
        self.assertFalse(results[0][2])
        self.assertFalse(results[0][3]["laplacian_variance"]["match"])

    def test_contract1_document_still_requires_ring_ratio(self) -> None:
        gold = document([frame()])
        del gold["frames"][0]["ring_ratio"]
        _, _, results = self.compare(gold, document([frame()]))
        self.assertFalse(results[0][2])

    def test_full_parity_document_matches(self) -> None:
        matched, total, results = self.compare(document([frame()]), document([frame()]))
        self.assertEqual((matched, total), (1, 1))
        self.assertTrue(all(result[2] for result in results))

    def test_missing_mask_digest_fails(self) -> None:
        candidate = frame()
        del candidate["mask_sha256"]
        _matched, _total, results = self.compare(document([frame()]), document([candidate]))
        self.assertTrue(any(not result[2] for result in results))
        self.assertEqual(results[0][3]["mask_sha256"]["reason"], "missing_in_candidate")

    def test_series_or_target_drift_fails(self) -> None:
        candidate = frame()
        candidate["series_images_sha256"] = ["d" * 64]
        candidate["is_target_group"] = False
        _matched, _total, results = self.compare(document([frame()]), document([candidate]))
        self.assertFalse(results[0][2])
        self.assertFalse(results[0][3]["series_images_sha256"]["match"])
        self.assertFalse(results[0][3]["is_target_group"]["match"])

    def test_multi_object_records_match_independent_of_order(self) -> None:
        first = frame(object_id=1)
        second = frame(object_id=2)
        second["track_id"] = 2
        matched, total, results = self.compare(
            document([first, second]), document([copy.deepcopy(second), copy.deepcopy(first)])
        )
        self.assertEqual((matched, total), (2, 2))
        self.assertTrue(all(result[2] for result in results))

    def test_extra_candidate_record_and_contract_drift_fail(self) -> None:
        candidate = document([frame(), frame(index=9)])
        candidate["contract_version"] = 2
        matched, total, results = self.compare(document([frame()]), candidate)
        self.assertEqual((matched, total), (1, 1))
        reasons = {result[3].get("_reason") for result in results if not result[2]}
        self.assertIn("candidate_only_record", reasons)
        self.assertIn("document_metadata_mismatch", reasons)

    def test_fixture_or_input_count_drift_fails(self) -> None:
        gold = document([frame()])
        gold.update(fixture="hf:dataset@revision:/images[500:508]", input_frame_count=8)
        candidate = copy.deepcopy(gold)
        candidate.update(fixture="hf:dataset@revision:/images[0:8]", input_frame_count=3)

        _matched, _total, results = self.compare(gold, candidate)

        metadata = next(
            details
            for _gold, _candidate, ok, details in results
            if not ok and details.get("_reason") == "document_metadata_mismatch"
        )
        self.assertIn("fixture", metadata)
        self.assertIn("input_frame_count", metadata)


if __name__ == "__main__":
    unittest.main()
