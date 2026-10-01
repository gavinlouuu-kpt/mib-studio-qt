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

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
BUNDLED_REGISTRY = os.path.join(REPO, "resources", "defaults", "dot_grid", "registry.json")

if np is not None:
    from dotgrid import Codebook, decode_image, generate_codebook, render_view
    from dotgrid.registry import Design, Registry, decode_registry
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


def _design(design_id, seed, **kw):
    params = dict(columns=1200, rows=1200, pitch_um=30.0, dot_diameter_um=12.0, displacement_um=5.0)
    params.update(kw)
    return Design(id=design_id, name=design_id.upper(), seed=seed, **params)


@unittest.skipIf(np is None, "numpy/opencv not installed")
class RegistryTests(unittest.TestCase):
    def test_bundled_registry_regenerates_archived_codebook(self):
        reg = Registry.load(BUNDLED_REGISTRY)
        design = reg.find("wafer-sort-rt")
        self.assertIsNotNone(design)
        archived = Codebook.load(os.path.join(os.path.dirname(BUNDLED_REGISTRY), "wafer_soRT_2025-03-16_seed7_p30.json"))
        cb = reg.codebook("wafer-sort-rt")
        self.assertEqual((cb.seed, cb.columns, cb.rows), (archived.seed, archived.columns, archived.rows))
        self.assertEqual(cb.phi, archived.phi)
        self.assertEqual(cb.psi, archived.psi)
        self.assertEqual([c.name for c in cb.chips], [c.name for c in archived.chips])

    def test_validation_rules(self):
        self.assertEqual(Registry([_design("a", 1), _design("b", 2)]).validate(), [])
        self.assertTrue(any("duplicate id" in e for e in Registry([_design("a", 1), _design("a", 2)]).validate()))
        self.assertTrue(any("seed 1 used by both" in e for e in Registry([_design("a", 1), _design("b", 1)]).validate()))
        self.assertTrue(Registry([_design("Bad Id", 1)]).validate())
        self.assertTrue(Registry([_design("a", 1, dot_diameter_um=22.0)]).validate())
        reg = Registry([_design("a", 1)])
        with self.assertRaises(ValueError):
            reg.add(_design("b", 1))
        self.assertEqual(len(reg.designs), 1)

    def test_next_seed_never_reuses(self):
        reg = Registry([_design("a", 7), _design("b", 3)])
        self.assertEqual(reg.next_seed(), 8)
        reg.designs[0].status = "retired"
        self.assertEqual(reg.next_seed(), 8)  # retired seeds stay taken

    def test_save_load_roundtrip(self):
        reg = Registry([_design("b", 9), _design("a", 4)])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "registry.json")
            reg.save(path)
            back = Registry.load(path)
        self.assertEqual([x.id for x in back.designs], ["a", "b"])  # sorted by seed
        self.assertEqual(back.find("b").to_dict(), reg.find("b").to_dict())

    def test_decode_identifies_design(self):
        reg = Registry([_design("alpha", 11), _design("beta", 12)])
        for design_id, xy, mirrored in (("alpha", (10000.0, 9000.0), True), ("beta", (25000.0, 11000.0), False)):
            with self.subTest(design=design_id):
                pose = ViewPose(centre_um=xy, theta_deg=40.0, um_per_px=0.293, mirrored=mirrored)
                r, got = decode_registry(reg, render_view(reg.codebook(design_id), pose, seed=1), 0.31)
                self.assertTrue(r.ok, r.reason)
                self.assertEqual(got, design_id)
                self.assertLess(abs(r.centre_um[0] - xy[0]), 1.0)

    def test_unregistered_design_never_decodes(self):
        reg = Registry([_design("alpha", 11), _design("beta", 12)])
        stranger = _design("stranger", 777).codebook()
        for k in range(4):
            pose = ViewPose(centre_um=(6000.0 + 4000 * k, 20000.0 - 3000 * k), theta_deg=-120.0 + 70 * k,
                            um_per_px=0.293, mirrored=bool(k % 2))
            r, got = decode_registry(reg, render_view(stranger, pose, seed=k), 0.31)
            self.assertFalse(r.ok)
            self.assertIsNone(got)


def _mini_dxf(path):
    import ezdxf
    doc = ezdxf.new("R2010")
    doc.header["$INSUNITS"] = 13
    msp = doc.modelspace()
    msp.add_circle((15000, 15000), 14000)
    for x0, y0 in [(5000, 8000), (16000, 8000), (5000, 17000), (16000, 17000)]:
        msp.add_lwpolyline([(x0, y0), (x0 + 8000, y0), (x0 + 8000, y0 + 6000), (x0, y0 + 6000)], close=True)
        msp.add_line((x0 + 500, y0 + 3000), (x0 + 7500, y0 + 3000))
    doc.saveas(path)


@unittest.skipIf(np is None, "numpy/opencv not installed")
class RegisterCliTests(unittest.TestCase):
    def test_register_then_mask_then_decode(self):
        try:
            import ezdxf  # noqa: F401
            import scipy  # noqa: F401
        except ImportError:
            self.skipTest("ezdxf/scipy not installed")
        import contextlib
        import io
        import shutil
        import cv2
        import dotgrid_cli
        with tempfile.TemporaryDirectory() as d:
            reg_path = os.path.join(d, "registry.json")
            shutil.copy(BUNDLED_REGISTRY, reg_path)
            dxf = os.path.join(d, "mini.dxf")
            _mini_dxf(dxf)
            quiet = contextlib.redirect_stderr(io.StringIO())
            with quiet, contextlib.redirect_stdout(io.StringIO()):
                dotgrid_cli.main(["--registry", reg_path, "register", dxf, "--id", "mini", "--name", "Mini",
                                  "--out-dir", os.path.join(d, "out"), "--formats", "csv", "--views", "2"])
            reg = Registry.load(reg_path)
            mini = reg.find("mini")
            self.assertIsNotNone(mini)
            self.assertEqual(mini.seed, 8)  # one past wafer-sort-rt's seed 7
            self.assertEqual(len(mini.chips), 4)
            self.assertEqual(len(mini.source["sha256"]), 64)
            self.assertTrue(os.path.exists(os.path.join(d, "out", "codebook.json")))
            # The same DXF cannot be registered twice by accident.
            with quiet, self.assertRaises(SystemExit):
                dotgrid_cli.main(["--registry", reg_path, "register", dxf, "--id", "mini2",
                                  "--out-dir", os.path.join(d, "out2"), "--skip-cross-check"])
            self.assertIsNone(Registry.load(reg_path).find("mini2"))
            # Regenerating the mask of the registered design is bit-identical.
            with quiet, contextlib.redirect_stdout(io.StringIO()):
                dotgrid_cli.main(["--registry", reg_path, "mask", "mini", dxf,
                                  "--out-dir", os.path.join(d, "again"), "--formats", "csv"])
            with open(os.path.join(d, "out", "dots.csv")) as f1, open(os.path.join(d, "again", "dots.csv")) as f2:
                self.assertEqual(f1.read(), f2.read())
            # A frame of the new design is attributed to it, with its chip.
            pose = ViewPose(centre_um=(9000.0, 10000.0), theta_deg=33.0, um_per_px=0.293, mirrored=True)
            img = render_view(reg.codebook("mini"), pose, seed=2)
            cv2.imwrite(os.path.join(d, "v.png"), img)
            r, got = decode_registry(reg, img, 0.31)
            self.assertTrue(r.ok, r.reason)
            self.assertEqual(got, "mini")
            self.assertEqual(r.chip, "R0C0")


if __name__ == "__main__":
    if np is None:
        print("SKIP: numpy/opencv not installed")
        sys.exit(77)
    unittest.main()
