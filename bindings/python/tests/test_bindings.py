"""Tests for the mib_processing pybind11 bindings.

Uses the same synthetic "ring frame" pattern as
tests/processing/processing_pipeline_smoke_test.cpp (filled outer circle +
smaller filled hole -> one nested contour) so binding-correctness is checked
against a known-good C++ fixture, rather than re-deriving segmentation
behavior already covered by the 48-test C++ suite.
"""

from __future__ import annotations

import math
import os

import numpy as np
import pytest

import mib_processing as mp


def make_ring_frame() -> np.ndarray:
    image = np.zeros((80, 80), dtype=np.uint8)
    yy, xx = np.ogrid[:80, :80]
    dist = np.sqrt((xx - 40) ** 2 + (yy - 40) ** 2)
    image[dist <= 20] = 255
    image[dist <= 8] = 0
    return image


def make_smoke_config() -> dict:
    config = dict(mp.DEFAULT_PROCESSING_CONFIG)
    config.update(
        gaussian_blur_size=1,
        bg_subtract_threshold=127,
        morph_kernel_size=1,
        morph_iterations=1,
        enable_area_range_check=False,
        enable_deformability_range_check=False,
        enable_ring_ratio_check=False,
        enable_area_ratio_check=False,
        enable_border_check=True,
        require_single_inner_contour=True,
        empty_frame_pixel_threshold=1,
    )
    return config


GOLD_STANDARD_KEYS = {
    "frame_type", "index", "timestamp_ns", "object_id", "object_count",
    "deformability", "area", "area_um2", "area_ratio", "ring_ratio",
    "is_valid", "touches_border", "has_single_inner_contour", "in_range",
    "inner_contour_count", "brightness_q1", "brightness_q2", "brightness_q3",
    "brightness_q4",
    "is_target_group", "track_id", "track_first_frame", "track_last_frame",
    "track_observation_count",
}


class TestConfigConversion:
    def test_default_config_round_trips(self) -> None:
        defaults = mp.DEFAULT_PROCESSING_CONFIG
        round_tripped = mp.config_from_dict(dict(defaults))
        assert round_tripped == defaults

    def test_missing_fields_fall_back_to_struct_defaults(self) -> None:
        partial = mp.config_from_dict({"area_threshold_min": 999})
        assert partial["area_threshold_min"] == 999
        assert partial["area_threshold_max"] == mp.DEFAULT_PROCESSING_CONFIG["area_threshold_max"]


class TestProcessBatch:
    def test_empty_frame_yields_one_invalid_record(self) -> None:
        # Frame accounting is conserved: every input frame yields exactly one
        # output record when no object is found, marked invalid -- not
        # silence. See AGENTS.md "Pipeline tests assert frame accounting is
        # conserved (captured == processed + explicitly dropped)".
        frame = np.zeros((80, 80), dtype=np.uint8)
        results = mp.process_batch([frame], make_smoke_config())
        assert len(results) == 1
        assert results[0]["frame_type"] == "invalid"
        assert results[0]["is_valid"] is False

    def test_ring_frame_detected_matches_gold_standard_shape(self) -> None:
        frame = make_ring_frame()
        results = mp.process_batch([frame], make_smoke_config(), pixel_to_micron=0.5)

        assert len(results) == 1
        result = results[0]
        assert set(result.keys()) <= GOLD_STANDARD_KEYS | {"youngs_modulus", "laplacian_variance", "processing_contract_version"}
        assert GOLD_STANDARD_KEYS - {"youngs_modulus"} <= set(result.keys())
        assert result["frame_type"] == "valid"
        assert result["is_valid"] is True
        assert result["area"] > 0
        assert result["area_um2"] == pytest.approx(result["area"] * 0.5 * 0.5)
        assert 0.0 <= result["deformability"] <= 1.0
        assert result["inner_contour_count"] >= 1
        assert result["has_single_inner_contour"] is True

    def test_include_masks_adds_mask_array(self) -> None:
        frame = make_ring_frame()
        results = mp.process_batch([frame], make_smoke_config(), include_masks=True)
        assert len(results) == 1
        mask = results[0]["mask"]
        assert isinstance(mask, np.ndarray)
        assert mask.dtype == np.uint8
        assert mask.shape == frame.shape

    def test_target_group_and_tracking_metadata_are_exposed(self) -> None:
        config = make_smoke_config()
        config.update(
            enable_target_group=True,
            target_group_area_min=0,
            target_group_area_max=100000,
            target_group_deformability_min=0.0,
            target_group_deformability_max=1.0,
        )
        results = mp.process_batch([make_ring_frame()], config)

        assert len(results) == 1
        assert results[0]["is_target_group"] is True
        assert results[0]["track_id"] == 1
        assert results[0]["track_first_frame"] == 0
        assert results[0]["track_last_frame"] == 0
        assert results[0]["track_observation_count"] == 1

    def test_include_series_images_returns_trigger_and_following_frames(self) -> None:
        frames = [make_ring_frame(), np.full((80, 80), 17, dtype=np.uint8),
                  np.full((80, 80), 23, dtype=np.uint8)]
        config = make_smoke_config()
        config.update(multi_image_enabled=True, multi_image_count=3)

        results = mp.process_batch(frames, config, include_series_images=True)

        valid = next(result for result in results if result["is_valid"])
        assert len(valid["series_images"]) == 3
        assert np.array_equal(valid["series_images"][0], frames[0])
        assert np.array_equal(valid["series_images"][1], frames[1])
        assert np.array_equal(valid["series_images"][2], frames[2])

    def test_index_and_timestamp_preserved_via_compute_processed_frame(self) -> None:
        frame = make_ring_frame()
        result = mp.compute_processed_frame(
            frame, config=make_smoke_config(), index=42, timestamp_ns=4200
        )
        assert result["index"] == 42
        assert result["timestamp_ns"] == 4200
        assert result["is_valid"] is True


class TestEModulusLut:
    def test_lookup_returns_nan_before_load(self) -> None:
        lut = mp.EModulusLut()
        assert lut.is_loaded() is False
        assert math.isnan(lut.lookup(100.0, 0.1))

    def test_load_and_lookup(self, tmp_path) -> None:
        lut_file = tmp_path / "lut.txt"
        lines = []
        for area in (50, 100, 150):
            for deform in (0.0, 0.5, 1.0):
                emodulus = area * 0.1 + deform
                lines.append(f"{area}\t{deform}\t{emodulus}")
        lut_file.write_text("\n".join(lines) + "\n")

        lut = mp.EModulusLut()
        assert lut.load_from_file(str(lut_file)) is True
        assert lut.is_loaded() is True

        value = lut.lookup(100.0, 0.5)
        assert value == pytest.approx(10.5, abs=0.5)

    def test_lookup_outside_coverage_is_nan(self, tmp_path) -> None:
        lut_file = tmp_path / "lut.txt"
        lut_file.write_text("50\t0.0\t5.0\n100\t1.0\t10.0\n150\t0.0\t15.0\n")
        lut = mp.EModulusLut()
        lut.load_from_file(str(lut_file))
        assert math.isnan(lut.lookup(10000.0, 0.1))


class TestHdf5AndFolderRoundTrip:
    def test_save_and_reload_masks_to_hdf5(self, tmp_path) -> None:
        frame = make_ring_frame()
        results = mp.process_batch([frame], make_smoke_config(), include_masks=True)
        assert len(results) == 1

        frame_dicts = [{k: v for k, v in r.items() if k != "mask"} for r in results]
        masks = [r["mask"] for r in results]
        out_path = str(tmp_path / "test.h5")

        assert mp.save_masks_to_hdf5(frame_dicts, [frame], masks, out_path, make_smoke_config()) is True
        assert os.path.exists(out_path)

        ok, loaded_images = mp.load_images_from_hdf5(out_path, "/valid_frames/images", 0, 10)
        assert ok is True
        assert len(loaded_images) == 1
        assert np.array_equal(loaded_images[0], frame)

        ok, loaded_masks = mp.load_images_from_hdf5(out_path, "/valid_frames/masks", 0, 10)
        assert ok is True
        assert len(loaded_masks) == 1

    def test_save_masks_to_hdf5_rejects_mismatched_lengths(self, tmp_path) -> None:
        frame = make_ring_frame()
        results = mp.process_batch([frame], make_smoke_config(), include_masks=True)
        frame_dicts = [{k: v for k, v in r.items() if k != "mask"} for r in results]
        masks = [r["mask"] for r in results]
        with pytest.raises(ValueError):
            mp.save_masks_to_hdf5(
                frame_dicts, [frame, frame], masks, str(tmp_path / "x.h5"), make_smoke_config()
            )

    def test_load_from_avi_reports_failure_for_missing_file(self, tmp_path) -> None:
        ok, images, filenames, errors = mp.load_from_avi(str(tmp_path / "does_not_exist.avi"))
        assert ok is False
        assert images == []

    def test_load_from_folder_round_trip(self, tmp_path) -> None:
        cv2 = pytest.importorskip("cv2")
        frame = make_ring_frame()
        folder = tmp_path / "imgs"
        folder.mkdir()
        cv2.imwrite(str(folder / "a.png"), frame)

        ok, images, filenames, errors = mp.load_from_folder(str(folder))
        assert ok is True
        assert errors == []
        assert filenames == ["a.png"]
        assert len(images) == 1
        assert np.array_equal(images[0], frame)


def test_contract_version_exposed() -> None:
    assert mp.CONTRACT_VERSION == 1
