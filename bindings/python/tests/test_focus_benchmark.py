"""Scientific invariants for opt-in native focus/difference benchmarking."""
import math
import cv2
import numpy as np
import pytest
from mib_processing import benchmark_frame, laplacian_variance, compute_processed_frame

CONFIG = dict(gaussian_blur_size=1, bg_subtract_threshold=8,
              morph_kernel_size=1, morph_iterations=1,
              enable_area_range_check=False, enable_ring_ratio_check=False,
              enable_area_ratio_check=False, enable_deformability_range_check=False,
              enable_border_check=False, require_single_inner_contour=False)


def test_constant_has_no_mask_boundary_artifact():
    image = np.full((40, 40), 123, np.uint8)
    assert laplacian_variance(image, [(10, 10), (25, 10), (25, 25), (10, 25)]) == 0


@pytest.mark.parametrize("points", [[], [(1, 1)], [(1, 1), (2, 2), (3, 3)],
                                      [(-1, 1), (10, 1), (10, 10)]])
def test_invalid_contours_unavailable(points):
    assert math.isnan(laplacian_variance(np.zeros((20, 20), np.uint8), points))


@pytest.mark.parametrize("points", [[(5, 5), (18, 5), (18, 19), (5, 19)],
                                      [(0, 0), (18, 0), (18, 19), (0, 19)]])
def test_matches_full_frame_unmasked_reference_and_inversion(points):
    image = np.random.default_rng(123).integers(0, 256, (30, 30), dtype=np.uint8)
    mask = np.zeros_like(image)
    cv2.fillPoly(mask, [np.array(points, np.int32)], 255)
    full = cv2.Laplacian(image, cv2.CV_64F, ksize=1, borderType=cv2.BORDER_REFLECT_101)
    expected = np.var(full[mask != 0])
    assert laplacian_variance(image, points) == pytest.approx(expected, rel=1e-12)
    assert laplacian_variance(255-image, points) == pytest.approx(expected, rel=1e-12)


def test_progressive_blur_and_remote_object_isolation():
    image = np.full((96, 128), 128, np.uint8)
    image[20:60, 20:60] = np.random.default_rng(42).integers(40, 210, (40, 40), dtype=np.uint8)
    points = [(24, 24), (55, 24), (55, 55), (24, 55)]
    values = [laplacian_variance(cv2.GaussianBlur(image, (0, 0), s), points)
              for s in (0.5, 1.0, 2.0, 4.0)]
    assert all(a > b for a, b in zip(values, values[1:]))
    before = laplacian_variance(image, points)
    image[:, 80:] = 255
    assert laplacian_variance(image, points) == before


def test_absdiff_detects_dark_object_without_changing_legacy_default():
    bg = np.full((96, 128), 180, np.uint8)
    image = bg.copy()
    cv2.circle(image, (40, 48), 12, 60, -1)
    sub = benchmark_frame(image, bg, CONFIG, "subtract")
    absolute = benchmark_frame(image, bg, CONFIG, "absdiff")
    assert not sub["objects"]
    assert len(absolute["objects"]) == 1
    assert absolute["objects"][0]["ring_ratio"] is None
    assert absolute["objects"][0]["laplacian_variance"] > 0
    legacy = compute_processed_frame(image, bg, CONFIG, include_mask=True)
    np.testing.assert_array_equal(legacy["mask"], sub["mask"])


def test_two_objects_independent_and_no_metric_side_effects():
    bg = np.zeros((96, 160), np.uint8)
    image = bg.copy()
    cv2.circle(image, (40, 48), 18, 210, -1)
    cv2.circle(image, (40, 48), 9, 40, -1)
    cv2.circle(image, (115, 48), 18, 120, -1)
    cv2.circle(image, (115, 48), 9, 20, -1)
    config = dict(CONFIG, bg_subtract_threshold=60)
    for policy in ("subtract", "absdiff"):
        scored = benchmark_frame(image, bg, config, policy, True)
        plain = benchmark_frame(image, bg, config, policy, False)
        assert len(scored["objects"]) == 2
        assert scored["objects"][0]["laplacian_variance"] != scored["objects"][1]["laplacian_variance"]
        for a, b in zip(scored["objects"], plain["objects"]):
            score = a.pop("laplacian_variance")
            assert score is not None and score > 0
            assert b.pop("laplacian_variance") is None
            assert a == b
        np.testing.assert_array_equal(scored["mask"], plain["mask"])


def test_invalid_policy_or_background_rejected():
    image = np.zeros((20, 20), np.uint8)
    with pytest.raises(ValueError): benchmark_frame(image, image, CONFIG, "other")
    with pytest.raises(ValueError): benchmark_frame(image, image[:10], CONFIG)


def test_focus_cost_is_bounded_relative_to_native_baseline():
    from mib_processing import set_opencv_threads
    set_opencv_threads(1)
    bg = np.full((240, 1184), 128, np.uint8)
    image = bg.copy()
    for x in range(60, 1180, 100):
        cv2.circle(image, (x, 120), 12, 210, -1)
    for _ in range(5):
        benchmark_frame(image, bg, CONFIG, "subtract", True)
    base, scored = [], []
    for _ in range(31):
        base.append(benchmark_frame(image, bg, CONFIG, "subtract", False)["pipeline_us"])
        scored.append(benchmark_frame(image, bg, CONFIG, "subtract", True)["pipeline_us"])
    # Ratio, not hardware-dependent milliseconds; catch accidental whole-frame
    # convolution per object or otherwise unbounded repeated work.
    assert np.median(scored) < 8 * np.median(base)
