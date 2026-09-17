"""Tests for the dot-grid reference implementation (scripts/dot_grid).

Runs under pytest or directly (`python3 test_dotgrid.py`, exit 77 = skipped
because numpy/OpenCV are not installed, matching the CTest SKIP_RETURN_CODE).
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import numpy as np
    import cv2  # noqa: F401
except ImportError:  # pragma: no cover
    np = None

if np is not None:
    from dotgrid import Codebook, decode_image, generate_codebook, render_view
    from dotgrid.codebook import MNS_PERIOD, WINDOW_SYMBOLS
    from dotgrid.render import ViewPose


@unittest.skipIf(np is None, "numpy/opencv not installed")
class DotGridTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cb = generate_codebook(7, 3700, 3700, pitch_um=30.0, dot_diameter_um=12.0, displacement_um=5.0)

    def test_codebook_golden_matches_cpp(self):
        # Pinned values shared with tests/processing/dot_grid_codebook_test.cpp.
        cb = self.cb
        self.assertEqual("".join(map(str, cb.mns)), "000001000011000101001111010001110010010110111011001101010111111")
        self.assertEqual(cb.phi[:12], [0, 51, 23, 58, 18, 37, 5, 7, 34, 51, 13, 9])
        self.assertEqual(cb.psi[:12], [0, 61, 35, 11, 25, 47, 24, 21, 19, 42, 57, 53])
        self.assertEqual((cb.phi[3699], cb.psi[3699]), (42, 1))
        self.assertEqual(cb.direction(100, 200), (1, 0))
        self.assertEqual(cb.dot_um(100, 200), (3005.0, 6000.0))

    def test_delta_windows_unique(self):
        for phases in (self.cb.phi, self.cb.psi):
            deltas = [(phases[i + 1] - phases[i]) % MNS_PERIOD for i in range(len(phases) - 1)]
            windows = [tuple(deltas[i:i + WINDOW_SYMBOLS]) for i in range(len(deltas) - WINDOW_SYMBOLS + 1)]
            self.assertEqual(len(set(windows)), len(windows))

    def test_json_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "cb.json")
            self.cb.save(path)
            back = Codebook.load(path)
        self.assertEqual(back.phi, self.cb.phi)
        self.assertEqual(back.psi, self.cb.psi)
        self.assertEqual(back.pitch_um, self.cb.pitch_um)

    def test_roundtrip_20x(self):
        for theta, mirrored in [(0.0, False), (37.5, True), (-120.0, False), (91.0, True)]:
            with self.subTest(theta=theta, mirrored=mirrored):
                pose = ViewPose(centre_um=(40000.0, 52000.0), theta_deg=theta, um_per_px=0.293, mirrored=mirrored)
                r = decode_image(self.cb, render_view(self.cb, pose, seed=3), 0.32)
                self.assertTrue(r.ok, r.reason)
                self.assertLess(abs(r.centre_um[0] - 40000.0), 1.0)
                self.assertLess(abs(r.centre_um[1] - 52000.0), 1.0)
                self.assertEqual(r.mirrored, mirrored)
                self.assertLess(abs(((r.theta_deg - theta + 180) % 360) - 180), 0.2)
                self.assertLess(abs(r.um_per_px - 0.293), 0.002)

    def test_channel_band_20x(self):
        pose = ViewPose(centre_um=(30000.0, 30000.0), theta_deg=2.0, um_per_px=0.293)
        c0 = np.array([30000.0, 30040.0]); d = np.array([1.0, 0.0]); nrm = np.array([0.0, 1.0])
        img = render_view(self.cb, pose, seed=5, channel_lines_um=[(tuple(c0 - d * 3000), tuple(c0 + d * 3000), 30.0)],
                          keepout_mask=lambda x, y: abs((np.array([x, y]) - c0) @ nrm) > 71.0)
        r = decode_image(self.cb, img, 0.32)
        self.assertTrue(r.ok, r.reason)
        self.assertLess(abs(r.centre_um[0] - 30000.0), 1.0)
        self.assertLess(abs(r.centre_um[1] - 30000.0), 1.0)

    def test_blank_image_fails_cleanly(self):
        r = decode_image(self.cb, np.full((1200, 1920), 180, np.uint8), 0.32)
        self.assertFalse(r.ok)
        self.assertIn("too few", r.reason)


if __name__ == "__main__":
    if np is None:
        print("SKIP: numpy/opencv not installed")
        sys.exit(77)
    unittest.main()
