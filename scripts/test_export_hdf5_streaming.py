#!/usr/bin/env python3
"""Streaming, transactional-output, cancellation and fault tests for the
bounded-memory export engine (issue #344).

Requires h5py, numpy and OpenCV; skips cleanly when they are unavailable so
the script lane still runs on minimal CI images.
"""

from __future__ import annotations

import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import export_hdf5  # noqa: E402
import hdf_export_engine as engine  # noqa: E402
from hdf_export_engine import (  # noqa: E402
    ExportFormat,
    ExportJob,
    ExportPhase,
    ExportState,
    FrameSelection,
    next_available_name,
    run_export_job,
)

try:
    import numpy as np
    import cv2  # noqa: F401
    import export_test_fixture as fixture

    HAS_DEPS = True
except Exception:  # noqa: BLE001
    HAS_DEPS = False


# ---------------------------------------------------------------------------
# Regression: the engine never asks for the whole image dataset
# ---------------------------------------------------------------------------


class _WholeArrayForbidden(Exception):
    pass


class _IndexOnlyDataset:
    """Image dataset that only tolerates integer (and 2-int) indexing."""

    def __init__(self, frames):
        self._frames = frames
        self.shape = (len(frames),) + tuple(frames[0].shape) if frames else (0,)

    def __getitem__(self, key):
        if isinstance(key, tuple):
            if any(not isinstance(k, (int, np.integer)) for k in key):
                raise _WholeArrayForbidden(f"non-integer index {key!r}")
            frame = self._frames[int(key[0])]
            for k in key[1:]:
                frame = frame[int(k)]
            return frame
        if isinstance(key, (int, np.integer)):
            return self._frames[int(key)]
        raise _WholeArrayForbidden(f"whole-dataset access via {key!r}")

    def __array__(self, *args, **kwargs):
        raise _WholeArrayForbidden("np.array(dataset)")

    def __iter__(self):
        raise _WholeArrayForbidden("iteration materializes rows")


class _MetadataDataset:
    def __init__(self, rows):
        self._rows = rows
        self.shape = (len(rows),)

    def __getitem__(self, key):
        return self._rows[key]


class _FakeFile(dict):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _FakeH5Py:
    def __init__(self, file):
        self._file = file

    def File(self, _path, _mode):
        return self._file


@unittest.skipUnless(HAS_DEPS, "h5py/numpy/opencv not installed")
class StreamingRegressionTest(unittest.TestCase):
    def _fake_file(self, frames=6, series=2):
        h, w = 8, 12
        valid = [fixture.frame_pixels(i, h, w) for i in range(frames)]
        invalid = [fixture.frame_pixels(100 + i, h, w) for i in range(3)]
        series_frames = [np.stack([fixture.frame_pixels(i, h, w, offset=s * 9) for s in range(series)])
                         for i in range(frames)]
        meta_v = fixture._metadata(range(frames), True)
        meta_i = fixture._metadata(range(100, 103), False)
        series_ds = _IndexOnlyDataset(series_frames)
        series_ds.shape = (frames, series, h, w)
        return _FakeFile({
            "/valid_frames/metadata": _MetadataDataset(meta_v),
            "/valid_frames/images": _IndexOnlyDataset(valid),
            "/valid_frames/series_images": series_ds,
            "/invalid_frames/metadata": _MetadataDataset(meta_i),
            "/invalid_frames/images": _IndexOnlyDataset(invalid),
        })

    def test_legacy_reader_materializes_whole_dataset(self) -> None:
        file = self._fake_file()
        with self.assertRaises(_WholeArrayForbidden):
            export_hdf5.read_hdf5_images(file, "/valid_frames/images")

    def test_engine_requests_integer_indices_only(self) -> None:
        file = self._fake_file(frames=6, series=2)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(export_hdf5, "h5py", _FakeH5Py(file)):
            root = Path(tmp)
            source = root / "sample.h5"
            source.write_bytes(b"not really hdf5")
            job = ExportJob(source, root, ExportFormat.ALL, FrameSelection.BOTH)
            result = run_export_job(job)
            self.assertEqual(result.state, ExportState.COMPLETED, result.error)
            self.assertEqual(result.images_exported, 9)
            self.assertEqual(result.series_exported, 12)
            self.assertTrue((root / "sample" / "metrics.csv").is_file())
            self.assertEqual(len(list((root / "sample").glob("*.tiff"))), 21)


# ---------------------------------------------------------------------------
# Real fixture round-trip, cancellation, faults, transactional output
# ---------------------------------------------------------------------------


@unittest.skipUnless(HAS_DEPS, "h5py/numpy/opencv not installed")
class EngineFixtureTest(unittest.TestCase):
    VALID, INVALID, SERIES = 10, 5, 3

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.source = fixture.write_fixture(self.root / "run one.h5", valid_frames=self.VALID,
                                            invalid_frames=self.INVALID, series_count=self.SERIES,
                                            height=16, width=24)
        self.source_sha = fixture.sha256_of_file(self.source)
        self.out = self.root / "out"

    def tearDown(self) -> None:
        self.assertEqual(fixture.sha256_of_file(self.source), self.source_sha, "source HDF5 modified")
        self._tmp.cleanup()

    def _job(self, fmt, selection=FrameSelection.BOTH, **kw):
        return ExportJob(self.source, self.out, fmt, selection, **kw)

    def _no_partials(self):
        leftovers = [p.name for p in self.out.iterdir() if ".partial-" in p.name] if self.out.exists() else []
        self.assertEqual(leftovers, [], "partial output leaked")

    def test_all_export_round_trip_and_repeatable_manifest(self) -> None:
        first = run_export_job(self._job(ExportFormat.ALL))
        self.assertEqual(first.state, ExportState.COMPLETED, first.error)
        self.assertEqual(first.final_path, self.out / "run one")
        expected = fixture.expected_image_count(self.VALID, self.INVALID, self.SERIES)
        self.assertEqual(first.images_exported + first.series_exported, expected)
        self.assertEqual((first.valid_count, first.invalid_count), (self.VALID, self.INVALID))
        # Pixel content of one valid frame, one invalid frame and one series frame.
        img = cv2.imread(str(first.final_path / "valid_frame_000004.tiff"), cv2.IMREAD_UNCHANGED)
        np.testing.assert_array_equal(img, fixture.frame_pixels(4, 16, 24))
        inv = cv2.imread(str(first.final_path / "invalid_frame_000003.tiff"), cv2.IMREAD_UNCHANGED)
        np.testing.assert_array_equal(inv, fixture.frame_pixels(3, 16, 24, offset=50))
        ser = cv2.imread(str(first.final_path / "valid_frame_000002_series_01.tiff"), cv2.IMREAD_UNCHANGED)
        np.testing.assert_array_equal(ser, fixture.frame_pixels(2, 16, 24, offset=117))
        csv_lines = (first.final_path / "metrics.csv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(csv_lines), 1 + self.VALID + self.INVALID)
        self.assertTrue(csv_lines[1].startswith("Valid,0,1000000,0,1,"))
        # Second run: new folder, identical relative manifest.
        second = run_export_job(self._job(ExportFormat.ALL))
        self.assertEqual(second.final_path, self.out / "run one_2")
        self.assertEqual(fixture.output_manifest(first.final_path), fixture.output_manifest(second.final_path))
        self._no_partials()

    def test_csv_json_images_formats(self) -> None:
        csv_result = run_export_job(self._job(ExportFormat.CSV, FrameSelection.VALID))
        self.assertEqual(csv_result.state, ExportState.COMPLETED)
        self.assertEqual(csv_result.final_path, self.out / "run one_metrics.csv")
        self.assertEqual(csv_result.invalid_count, 0)
        json_result = run_export_job(self._job(ExportFormat.JSON))
        self.assertEqual(json_result.final_path, self.out / "run one_metrics.json")
        images = run_export_job(self._job(ExportFormat.IMAGES, FrameSelection.INVALID))
        self.assertEqual(images.state, ExportState.COMPLETED)
        self.assertEqual(images.images_exported, self.INVALID)
        self.assertEqual(images.series_exported, 0)
        self.assertFalse((images.final_path / "metrics.csv").exists())
        self._no_partials()

    def test_series_range_and_skip(self) -> None:
        ranged = run_export_job(self._job(ExportFormat.IMAGES, FrameSelection.VALID, series_range=(1, 1)))
        self.assertEqual(ranged.series_exported, self.VALID)
        skipped = run_export_job(self._job(ExportFormat.IMAGES, FrameSelection.VALID, export_series=False))
        self.assertEqual(skipped.series_exported, 0)
        self.assertEqual(skipped.images_exported, self.VALID)

    def _cancel_after(self, prefix: str, writes: int):
        event = threading.Event()
        seen = {"n": 0}

        def writer(image, destination, index):
            ok = engine.write_frame_image(image, destination, index)
            if destination.name.startswith(prefix):
                seen["n"] += 1
                if seen["n"] >= writes:
                    event.set()
            return ok

        return event, writer

    def test_cancel_during_each_image_phase(self) -> None:
        phases = {
            "valid": ("valid_frame_000", 3),
            "series": ("valid_frame_000002_series", 1),
            "invalid": ("invalid_frame_", 2),
        }
        for name, (prefix, writes) in phases.items():
            with self.subTest(phase=name):
                event, writer = self._cancel_after(prefix, writes)
                phases_seen = []
                result = run_export_job(self._job(ExportFormat.ALL), cancel_event=event,
                                        on_progress=lambda p: phases_seen.append(p.phase), image_writer=writer)
                self.assertEqual(result.state, ExportState.CANCELLED)
                self.assertIsNone(result.final_path)
                self.assertFalse((self.out / "run one").exists(), "cancelled job published a normal-looking folder")
                self.assertIn(ExportPhase.CLEANUP, phases_seen)
                self._no_partials()

    def test_cancel_before_open(self) -> None:
        event = threading.Event()
        event.set()
        result = run_export_job(self._job(ExportFormat.CSV), cancel_event=event)
        self.assertEqual(result.state, ExportState.CANCELLED)
        self.assertFalse(self.out.exists() and any(self.out.iterdir()))

    def test_image_write_failure_fails_and_discards(self) -> None:
        calls = {"n": 0}

        def failing_writer(image, destination, index):
            calls["n"] += 1
            if calls["n"] == 4:
                with mock.patch.object(export_hdf5.cv2, "imwrite", return_value=False):
                    return engine.write_frame_image(image, destination, index)
            return engine.write_frame_image(image, destination, index)

        result = run_export_job(self._job(ExportFormat.IMAGES), image_writer=failing_writer)
        self.assertEqual(result.state, ExportState.FAILED)
        self.assertIn("failed to write image", result.error)
        self.assertFalse((self.out / "run one").exists())
        self._no_partials()

    def test_retained_partial_is_visibly_partial(self) -> None:
        event, writer = self._cancel_after("valid_frame_000", 2)
        result = run_export_job(self._job(ExportFormat.ALL, keep_partial_on_failure=True),
                                cancel_event=event, image_writer=writer)
        self.assertEqual(result.state, ExportState.CANCELLED)
        self.assertIsNotNone(result.partial_path)
        self.assertTrue(result.partial_path.name.startswith(".run one.partial-"))
        self.assertTrue((result.partial_path / "export-failure.json").is_file())
        self.assertFalse((self.out / "run one").exists())

    def test_unwritable_destination_fails_before_reading(self) -> None:
        blocker = self.root / "blocker.txt"
        blocker.write_text("x", encoding="utf-8")
        result = run_export_job(ExportJob(self.source, blocker / "out", ExportFormat.ALL))
        self.assertEqual(result.state, ExportState.FAILED)
        self.assertIn("Failed to create output directory", result.error)
        # An output root that is a file (not a directory).
        result2 = run_export_job(ExportJob(self.source, blocker, ExportFormat.CSV))
        self.assertEqual(result2.state, ExportState.FAILED)
        self.assertIn("not a directory", result2.error)

    def test_missing_source(self) -> None:
        result = run_export_job(ExportJob(self.root / "nope.h5", self.out, ExportFormat.CSV))
        self.assertEqual(result.state, ExportState.FAILED)
        self.assertIn("does not exist", result.error)

    def test_progress_is_monotonic_and_bounded(self) -> None:
        events = []
        result = run_export_job(self._job(ExportFormat.ALL), on_progress=events.append)
        self.assertEqual(result.state, ExportState.COMPLETED)
        completed = [e.completed for e in events]
        self.assertEqual(completed, sorted(completed))
        self.assertTrue(all(e.completed <= e.total for e in events if e.total))
        self.assertEqual(events[-1].total, 1 + fixture.expected_image_count(self.VALID, self.INVALID, self.SERIES))


class NameLookupTest(unittest.TestCase):
    def test_single_listing_and_max_plus_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sample").mkdir()
            for suffix in range(2, 2002):
                (root / f"sample_{suffix}").mkdir()
            (root / "sample_9999_notes").mkdir()  # does not match the pattern
            calls = {"scandir": 0}
            real_scandir = os.scandir

            def counting_scandir(path):
                calls["scandir"] += 1
                return real_scandir(path)

            with mock.patch.object(engine.os, "scandir", counting_scandir), \
                    mock.patch.object(Path, "exists", side_effect=AssertionError("exists() probing")):
                chosen = next_available_name(root, "sample", "sample_{suffix}")
            self.assertEqual(chosen, root / "sample_2002")
            self.assertEqual(calls["scandir"], 1)

    def test_first_name_when_unused_and_gap_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(next_available_name(root, "a_metrics.csv", "a_metrics_{suffix}.csv"),
                             root / "a_metrics.csv")
            (root / "a_metrics.csv").write_text("", encoding="utf-8")
            (root / "a_metrics_7.csv").write_text("", encoding="utf-8")
            self.assertEqual(next_available_name(root, "a_metrics.csv", "a_metrics_{suffix}.csv"),
                             root / "a_metrics_8.csv")
            self.assertEqual(next_available_name(root / "missing", "x", "x_{suffix}"), root / "missing" / "x")


if __name__ == "__main__":
    unittest.main()
