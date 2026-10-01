#!/usr/bin/env python3
"""Regression tests for external HDF5 conformance inputs."""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

import numpy as np

import run_processing_conformance as conformance


class _FakeDataset:
    def __init__(self, values: np.ndarray) -> None:
        self._values = values
        self.shape = values.shape
        self.dtype = values.dtype

    def __getitem__(self, selection):
        return self._values[selection]


class _FakeFile:
    def __init__(self, datasets: dict[str, _FakeDataset]) -> None:
        self._datasets = datasets

    def __enter__(self):
        return self

    def __exit__(self, *_args) -> None:
        return None

    def __contains__(self, path: str) -> bool:
        return path in self._datasets

    def __getitem__(self, path: str) -> _FakeDataset:
        return self._datasets[path]


class _FakeH5Py:
    def __init__(self, datasets: dict[str, _FakeDataset]) -> None:
        self._datasets = datasets

    def File(self, _path: Path, _mode: str) -> _FakeFile:  # noqa: N802 - mirrors h5py
        return _FakeFile(self._datasets)


class Hdf5ConformanceInputTest(unittest.TestCase):
    def test_loads_a_bounded_contiguous_frame_window(self) -> None:
        values = np.arange(5 * 4 * 6, dtype=np.uint8).reshape(5, 4, 6)
        fake_h5py = _FakeH5Py({"/recorded_frames/images": _FakeDataset(values)})

        with mock.patch.dict("sys.modules", {"h5py": fake_h5py}):
            frames, fixture_id = conformance.load_hdf5_frames(
                Path("fixture.h5"),
                "/recorded_frames/images",
                frame_offset=1,
                frame_limit=2,
            )

        self.assertEqual(len(frames), 2)
        np.testing.assert_array_equal(frames[0], values[1])
        self.assertTrue(frames[0].flags.c_contiguous)
        self.assertEqual(
            fixture_id,
            "hdf5:fixture.h5:/recorded_frames/images[1:3]",
        )

    def test_rejects_missing_or_non_grayscale_datasets(self) -> None:
        color = np.zeros((2, 4, 6, 3), dtype=np.uint8)
        fake_h5py = _FakeH5Py({"/recorded_frames/images": _FakeDataset(color)})

        with mock.patch.dict("sys.modules", {"h5py": fake_h5py}):
            with self.assertRaisesRegex(ValueError, "must have shape"):
                conformance.load_hdf5_frames(
                    Path("fixture.h5"), "/recorded_frames/images", 0, 2
                )
            with self.assertRaisesRegex(ValueError, "dataset not found"):
                conformance.load_hdf5_frames(Path("fixture.h5"), "/missing", 0, 2)



class RealFixtureInputTest(unittest.TestCase):
    def _write(self, directory: Path, owner: list[int]) -> Path:
        path = directory / "real.npz"
        np.savez_compressed(
            path,
            frames=np.arange(3 * 4 * 6, dtype=np.uint8).reshape(3, 4, 6),
            backgrounds=np.zeros((2, 4, 6), dtype=np.uint8),
            frame_background=np.asarray(owner, dtype=np.int16),
            config_json=np.asarray('{"processing_contract_version": 1}'),
            pixel_to_micron=np.asarray(0.5),
        )
        return path

    def test_groups_frames_by_background_in_order(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            groups, config, pixel_to_micron, fixture_id = conformance.load_real_fixture(
                self._write(Path(tmp), [0, 0, 1])
            )
        self.assertEqual([len(frames) for frames, _ in groups], [2, 1])
        self.assertEqual(config["processing_contract_version"], 1)
        self.assertEqual(pixel_to_micron, 0.5)
        self.assertEqual(fixture_id, "npz-real:real.npz")

    def test_rejects_out_of_order_background_mapping(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                conformance.load_real_fixture(self._write(Path(tmp), [1, 0, 0]))


if __name__ == "__main__":
    unittest.main()
