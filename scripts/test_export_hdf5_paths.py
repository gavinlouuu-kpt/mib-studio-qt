#!/usr/bin/env python3
"""Focused path-policy tests for scripts/export_hdf5.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import export_hdf5  # noqa: E402
import hdf_export_engine  # noqa: E402

EXPECTED_CSV_HEADER = (
    "Frame Type,Index,Timestamp,Object Id,Object Count,Deformability,Area,Area (um²),"
    "Area Ratio,Ring Ratio,Valid,Touches Border,Single Inner,In Range,Inner Count,"
    "Bright Q1,Bright Q2,Bright Q3,Bright Q4"
)


class _FakeDType:
    names = (
        "index",
        "timestampNs",
        "objectId",
        "objectCount",
        "trackId",
        "trackFirstFrame",
        "trackLastFrame",
        "trackObservationCount",
        "deformability",
        "area",
        "areaRatio",
        "ringRatio",
        "isValid",
        "touchesBorder",
        "hasSingleInnerContour",
        "inRange",
        "innerContourCount",
        "brightness_q1",
        "brightness_q2",
        "brightness_q3",
        "brightness_q4",
        "youngsModulus",
        "isTargetGroup",
    )


class _FakeRow(dict):
    dtype = _FakeDType()


class _FakeDataset:
    def __init__(self, rows=None, shape=None):
        self._rows = rows or []
        self.shape = shape or (len(self._rows),)

    def __getitem__(self, key):
        if isinstance(key, slice):
            return self._rows
        return self._rows[key]


class _FakeH5File:
    def __init__(self):
        row = _FakeRow(
            index=7,
            timestampNs=1234,
            objectId=2,
            objectCount=1,
            trackId=3,
            trackFirstFrame=5,
            trackLastFrame=7,
            trackObservationCount=3,
            deformability=0.125,
            area=42.0,
            areaRatio=0.75,
            ringRatio=1.5,
            isValid=True,
            touchesBorder=False,
            hasSingleInnerContour=True,
            inRange=True,
            innerContourCount=1,
            brightness_q1=1.0,
            brightness_q2=2.0,
            brightness_q3=3.0,
            brightness_q4=4.0,
            youngsModulus=5.5,
            isTargetGroup=True,
        )
        self._datasets = {
            "/valid_frames/metadata": _FakeDataset([row]),
            "/valid_frames/images": _FakeDataset([object()]),
            "/invalid_frames/metadata": _FakeDataset([]),
            "/invalid_frames/images": _FakeDataset([]),
        }

    def __contains__(self, key):
        return key in self._datasets

    def __getitem__(self, key):
        return self._datasets[key]

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


class _FakeH5Py:
    @staticmethod
    def File(_path, _mode):
        return _FakeH5File()


class ExportHdf5PathPolicyTest(unittest.TestCase):
    def _hdf5_dependency_patch(self):
        return mock.patch.multiple(
            export_hdf5,
            HAS_HDF5_DEPS=True,
            HDF5_IMPORT_ERROR=None,
            h5py=_FakeH5Py,
        )

    def test_source_derived_csv_names_and_suffixes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "cell run.v1.hdf5"

            csv_path, data_dir = export_hdf5.resolve_export_targets(source, root, "csv")
            self.assertEqual(csv_path, root / "cell run.v1_metrics.csv")
            self.assertEqual(data_dir, root)

            csv_path.write_text("existing", encoding="utf-8")
            csv_path_2, _ = export_hdf5.resolve_export_targets(source, root, "csv")
            self.assertEqual(csv_path_2, root / "cell run.v1_metrics_2.csv")

            csv_path_2.write_text("existing", encoding="utf-8")
            csv_path_3, _ = export_hdf5.resolve_export_targets(source, root, "csv")
            self.assertEqual(csv_path_3, root / "cell run.v1_metrics_3.csv")

    def test_images_and_all_use_source_folder_with_suffixes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "sample.h5"

            csv_path, image_dir = export_hdf5.resolve_export_targets(source, root, "images")
            self.assertIsNone(csv_path)
            self.assertEqual(image_dir, root / "sample")

            (root / "sample").mkdir()
            csv_path, image_dir_2 = export_hdf5.resolve_export_targets(source, root, "images")
            self.assertIsNone(csv_path)
            self.assertEqual(image_dir_2, root / "sample_2")

            (root / "sample_2").mkdir()
            all_csv, all_dir = export_hdf5.resolve_export_targets(source, root, "all")
            self.assertEqual(all_dir, root / "sample_3")
            self.assertEqual(all_csv, root / "sample_3" / "metrics.csv")

    def test_export_hdf5_repeated_csv_exports_use_generated_suffixes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir, self._hdf5_dependency_patch():
            root = Path(temp_dir)
            source = root / "cell run.v1.h5"
            source.write_text("fake hdf5", encoding="utf-8")

            self.assertEqual(export_hdf5.export_hdf5(source, root, "csv"), 0)
            self.assertEqual(export_hdf5.export_hdf5(source, root, "csv"), 0)

            first = root / "cell run.v1_metrics.csv"
            second = root / "cell run.v1_metrics_2.csv"
            self.assertTrue(first.is_file())
            self.assertTrue(second.is_file())
            self.assertEqual(first.read_text(encoding="utf-8").splitlines()[0], EXPECTED_CSV_HEADER)
            self.assertEqual(second.read_text(encoding="utf-8").splitlines()[0], EXPECTED_CSV_HEADER)

    def test_export_hdf5_repeated_image_and_all_exports_use_generated_folders(self) -> None:
        # Issue #344: images stream through hdf_export_engine.write_frame_image;
        # the fake image payloads are not real arrays, so stub the writer.
        with tempfile.TemporaryDirectory() as temp_dir, self._hdf5_dependency_patch(), \
             mock.patch.object(export_hdf5, "HAS_CV2", True), \
             mock.patch.object(hdf_export_engine, "write_frame_image", return_value=True):
            root = Path(temp_dir)
            source = root / "sample.hdf5"
            source.write_text("fake hdf5", encoding="utf-8")

            self.assertEqual(export_hdf5.export_hdf5(source, root, "images"), 0)
            self.assertEqual(export_hdf5.export_hdf5(source, root, "all"), 0)

            self.assertTrue((root / "sample").is_dir())
            self.assertTrue((root / "sample_2").is_dir())
            self.assertTrue((root / "sample_2" / "metrics.csv").is_file())

    def test_output_root_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            missing_dir = root / "new" / "exports"
            ok, error = export_hdf5.ensure_output_root(missing_dir)
            self.assertTrue(ok, error)
            self.assertTrue(missing_dir.is_dir())

            output_file = root / "not_a_dir"
            output_file.write_text("x", encoding="utf-8")
            ok, error = export_hdf5.ensure_output_root(output_file)
            self.assertFalse(ok)
            self.assertIn("not a directory", error)

            csv_output = root / "metrics.csv"
            ok, error = export_hdf5.ensure_output_root(csv_output)
            self.assertFalse(ok)
            self.assertIn("not a CSV file", error)
            self.assertFalse(csv_output.exists())

    def test_export_rejects_file_like_output_before_dependency_checks(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            input_file = root / "experiment.h5"
            input_file.write_text("not real hdf5", encoding="utf-8")

            self.assertEqual(
                export_hdf5.export_hdf5(input_file, root / "manual.csv", "csv"),
                1,
            )

            output_file = root / "existing_file"
            output_file.write_text("x", encoding="utf-8")
            self.assertEqual(
                export_hdf5.export_hdf5(input_file, output_file, "csv"),
                1,
            )

    def test_json_export_matches_gold_standard_contract(self) -> None:
        """Lightweight structural check mirroring docs/gold_standard_metrics.schema.json.

        Avoids a hard dependency on the `jsonschema` package; asserts the same
        required keys, enum values, and field types the committed schema enforces.
        """
        import json as json_module

        with tempfile.TemporaryDirectory() as temp_dir, self._hdf5_dependency_patch():
            root = Path(temp_dir)
            source = root / "cell run.v1.h5"
            source.write_text("fake hdf5", encoding="utf-8")

            self.assertEqual(export_hdf5.export_hdf5(source, root, "json", pixel_to_micron=0.5), 0)

            json_path = root / "cell run.v1_metrics.json"
            self.assertTrue(json_path.is_file())
            document = json_module.loads(json_path.read_text(encoding="utf-8"))

            self.assertEqual(document["version"], 1)
            self.assertEqual(document["contract_version"], 1)
            self.assertEqual(document["pixel_to_micron"], 0.5)
            self.assertEqual(document["source"], "cell run.v1")
            self.assertEqual(len(document["frames"]), 1)

            frame = document["frames"][0]
            required_keys = {
                "frame_type", "index", "timestamp_ns", "object_id", "object_count",
                "deformability", "area", "area_um2", "area_ratio", "ring_ratio",
                "is_valid", "touches_border", "has_single_inner_contour", "in_range",
                "inner_contour_count", "brightness_q1", "brightness_q2",
                "brightness_q3", "brightness_q4", "youngs_modulus",
                "is_target_group", "track_id", "track_first_frame",
                "track_last_frame", "track_observation_count",
            }
            self.assertEqual(set(frame.keys()), required_keys)
            self.assertIn(frame["frame_type"], ("valid", "invalid"))
            self.assertEqual(frame["frame_type"], "valid")
            self.assertEqual(frame["index"], 7)
            self.assertEqual(frame["timestamp_ns"], 1234)
            self.assertEqual(frame["object_id"], 2)
            self.assertEqual(frame["object_count"], 1)
            self.assertAlmostEqual(frame["area"], 42.0)
            self.assertAlmostEqual(frame["area_um2"], 42.0 * 0.5 * 0.5)
            self.assertAlmostEqual(frame["youngs_modulus"], 5.5)
            self.assertIs(frame["is_target_group"], True)
            self.assertEqual(frame["track_id"], 3)
            self.assertIs(frame["is_valid"], True)
            self.assertIs(frame["touches_border"], False)

    def test_youngs_modulus_omitted_when_absent_from_older_hdf5_files(self) -> None:
        class _OlderFakeDType:
            names = tuple(
                n for n in _FakeDType.names
                if n not in {
                    "youngsModulus", "isTargetGroup", "trackId",
                    "trackFirstFrame", "trackLastFrame", "trackObservationCount",
                }
            )

        class _OlderFakeRow(dict):
            dtype = _OlderFakeDType()

        row = _OlderFakeRow(
            index=1, timestampNs=1, objectId=1, objectCount=1, deformability=0.1,
            area=10.0, areaRatio=1.0, ringRatio=1.0, isValid=True, touchesBorder=False,
            hasSingleInnerContour=True, inRange=True, innerContourCount=1,
            brightness_q1=0.0, brightness_q2=0.0, brightness_q3=0.0, brightness_q4=0.0,
        )
        frame = export_hdf5._frame_to_gold_standard_dict(row, "valid", 0.5)
        self.assertNotIn("youngs_modulus", frame)
        self.assertNotIn("is_target_group", frame)
        self.assertNotIn("track_id", frame)

    def test_cli_still_requires_output_argument(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "export_hdf5.py"), "-i", "experiment.h5"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--output", result.stderr)


if __name__ == "__main__":
    unittest.main()
