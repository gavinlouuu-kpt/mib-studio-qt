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
        assert set(result.keys()) <= GOLD_STANDARD_KEYS | {"youngs_modulus", "laplacian_variance", "processing_contract_version", "bbox_xywh", "centroid_xy", "in_channel"}
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


# --- Processing Contract v2 selection (ADR 0006) -----------------------------

def _dark_object_frame():
    bg = np.full((60, 80), 128, dtype=np.uint8)
    frame = bg.copy()
    frame[20:40, 30:50] = 88  # dark-on-bright object: invisible to saturating subtraction
    return frame, bg


def _contract_config(contract: int) -> dict:
    cfg = dict(mp.DEFAULT_PROCESSING_CONFIG)
    cfg.update(
        processing_contract_version=contract,
        enable_area_range_check=False,
        enable_border_check=False,
        require_single_inner_contour=False,
        enable_ring_ratio_check=True,
        bg_subtract_threshold=8,
    )
    return cfg


def test_supported_contract_versions_exposed():
    assert mp.CONTRACT_VERSION == 1
    assert tuple(mp.SUPPORTED_CONTRACT_VERSIONS) == (1, 2)


def test_contract_1_default_and_unchanged_surface():
    frame, bg = _dark_object_frame()
    cfg = _contract_config(1)
    del cfg["processing_contract_version"]  # omitted -> Contract 1
    r = mp.compute_processed_frame(frame, bg, cfg, (0, 0, 80, 60), 0, 0, 0.4886, False)
    assert r["processing_contract_version"] == 1
    assert r["object_count"] == 0  # saturating subtraction ignores a dark object
    assert "ring_ratio" in r and not math.isnan(r["ring_ratio"])


def test_contract_2_absdiff_no_ring_laplacian():
    frame, bg = _dark_object_frame()
    r = mp.compute_processed_frame(frame, bg, _contract_config(2), (0, 0, 80, 60), 0, 0, 0.4886, True)
    assert r["processing_contract_version"] == 2
    assert r["object_count"] == 1 and r["is_valid"]
    assert "ring_ratio" not in r  # ring width abolished under Contract 2
    assert math.isfinite(r["laplacian_variance"]) and r["laplacian_variance"] > 0
    assert r["mask"].shape == frame.shape and int((r["mask"] > 0).sum()) > 0


def test_contract_2_canonical_difference_threshold_key():
    frame, bg = _dark_object_frame()
    cfg = _contract_config(2)
    del cfg["bg_subtract_threshold"]
    cfg["difference_threshold"] = 200  # nothing exceeds this -> no object
    r = mp.compute_processed_frame(frame, bg, cfg, (0, 0, 80, 60), 0, 0, 0.4886, False)
    assert r["object_count"] == 0


def test_unsupported_contract_fails_closed():
    frame, bg = _dark_object_frame()
    with pytest.raises(ValueError):
        mp.compute_processed_frame(frame, bg, _contract_config(3), (0, 0, 80, 60), 0, 0, 0.4886, False)


def test_compute_processed_objects_expands_per_object():
    bg = np.full((80, 120), 128, dtype=np.uint8)
    frame = bg.copy()
    frame[20:40, 20:40] = 88   # dark object
    frame[20:40, 70:90] = 168  # bright object
    cfg = _contract_config(2)
    objs = mp.compute_processed_objects(frame, bg, cfg, (0, 0, 120, 80), 7, 0, 0.4886, True)
    assert [o["object_id"] for o in objs] == [1, 2]
    assert all(o["object_count"] == 2 and o["index"] == 7 for o in objs)
    assert all(o["processing_contract_version"] == 2 and "ring_ratio" not in o for o in objs)
    assert all(math.isfinite(o["laplacian_variance"]) for o in objs)
    assert objs[0]["mask"].shape == frame.shape
    # Contract 1 only sees the bright object.
    objs1 = mp.compute_processed_objects(frame, bg, _contract_config(1), (0, 0, 120, 80))
    assert [o["object_id"] for o in objs1] == [1] and objs1[0]["object_count"] == 1
    # No detection -> single empty record.
    empty = mp.compute_processed_objects(bg, bg, cfg, (0, 0, 120, 80))
    assert len(empty) == 1 and empty[0]["object_id"] == -1


def test_contract_2_objects_are_top_level_contours():
    frame, bg = _dark_object_frame()
    frame[28:32, 38:42] = 128  # hole inside the dark object
    cfg = _contract_config(2)
    cfg["require_single_inner_contour"] = True  # Contract-1 rule, ignored under Contract 2
    objs = mp.compute_processed_objects(frame, bg, cfg, (0, 0, 80, 60))
    assert len(objs) == 1 and objs[0]["object_id"] == 1 and objs[0]["area"] > 300


def test_object_geometry_is_in_frame_coordinates():
    frame, bg = _dark_object_frame()  # dark object at rows 20:40, cols 30:50
    objs = mp.compute_processed_objects(frame, bg, _contract_config(2), (10, 5, 60, 50))
    assert len(objs) == 1
    cx, cy = objs[0]["centroid_xy"]
    assert abs(cx - 39.5) < 1.5 and abs(cy - 29.5) < 1.5
    x, y, w, h = objs[0]["bbox_xywh"]
    assert 28 <= x <= 31 and 18 <= y <= 21 and 18 <= w <= 22 and 18 <= h <= 22


def test_channel_band_rejects_objects_outside_by_centroid():
    bg = np.full((80, 120), 128, dtype=np.uint8)
    frame = bg.copy()
    frame[4:14, 20:40] = 88    # on the top wall (centroid row ~8.5)
    frame[34:54, 70:90] = 88   # in the channel (centroid row ~43.5)
    cfg = _contract_config(2)
    cfg["enable_area_range_check"] = False
    objs = mp.compute_processed_objects(frame, bg, cfg, (0, 0, 120, 80))
    assert len(objs) == 2 and all(o["in_channel"] and o["is_valid"] for o in objs)
    cfg["channel_band_y"], cfg["channel_band_h"] = 20, 50  # rows 20..69
    objs = mp.compute_processed_objects(frame, bg, cfg, (0, 0, 120, 80))
    by_row = sorted(objs, key=lambda o: o["centroid_xy"][1])
    assert not by_row[0]["in_channel"] and not by_row[0]["is_valid"]
    assert by_row[1]["in_channel"] and by_row[1]["is_valid"]
