"""Hardware-free calibration contracts, including subprocess orchestration."""
import csv
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
import subprocess
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/sync_tuning/src"))
from mib_sync_tuning import Policy, analyze_capture, select_candidate, tune
from mib_sync_tuning.workflow import apply_profile, make_plan, NativeSampler
from mib_sync_tuning.cli import main


def profile():
    return dict(exposure_time_us=2, acq_trigger_delay_us=0, strobe_pulse_width_us=100,
                live_view=dict(enabled=True, frequency_hz=5000, duty_percent=10),
                custom={"preserve": [1, "custom value"]})


def fixture(path, *, mean=180, amplitude=.4, clip=0, count=300, seconds=5, missing=0, bands=None):
    path.mkdir()
    meta = dict(schema_version=1, ok=True, shutdown_confirmed=True, frames=count, seconds=seconds,
                reader_missing=missing, reader_excluded=0, reader_considered=count+missing,
                capture_fps=1000, transport_lost=dict(value=0, validity="valid"),
                discarded=dict(value=0, validity="valid"))
    (path / "capture.json").write_text(json.dumps(meta))
    with (path / "frames.csv").open("w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["index", "host_us", "camera_ticks", "mean", "spatial_sd", "clipped_fraction", "dark_fraction", "top", "middle", "bottom"])
        for i in range(count):
            v = mean + amplitude * (-1 if i % 2 else 1)
            band_values = bands(i) if bands else [v] * 3
            w.writerow([i, 1000000 + int(i * seconds * 1e6 / count), i, v, 1, clip, 0, *band_values])


class FakeSampler:
    def __init__(self, fail=None, validation_bad=False):
        self.calls = []
        self.validated = []
        self.fail = fail
        self.validation_bad = validation_bad

    def validate(self, path):
        self.validated.append(path)

    def capture(self, path, mode, directory, seconds, settle):
        self.calls.append((path, mode, directory))
        if self.fail and len(self.calls) == self.fail: raise RuntimeError("injected capture failure")
        cfg = json.loads(path.read_text())
        amplitude = .4 if cfg["acq_trigger_delay_us"] >= 45 else 8
        if self.validation_bad and directory.name.startswith("validation"):
            amplitude = 10
        fixture(directory, amplitude=amplitude, seconds=seconds)
        meta = json.loads((directory / "capture.json").read_text())
        meta.update(mode=mode, profile=cfg)
        (directory / "capture.json").write_text(json.dumps(meta))


class CalibrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.profile = self.root / "profile.json"
        self.original = (json.dumps(profile(), indent=4) + "\n").encode()
        self.profile.write_bytes(self.original)

    def tearDown(self):
        self.temp.cleanup()

    def test_known_statistics_and_accounting(self):
        p = self.root / "run"
        fixture(p, amplitude=2)
        a = analyze_capture(p, Policy(max_cv_percent=2))
        self.assertAlmostEqual(a["mean"], 180)
        self.assertAlmostEqual(a["sd"], 2)
        self.assertAlmostEqual(a["cv_percent"], 100 * 2 / 180)
        self.assertTrue(a["accepted"])

    def test_dark_and_saturated_flat_streams_do_not_win(self):
        for i, options in enumerate([dict(mean=0, amplitude=0), dict(mean=255, amplitude=0, clip=1), dict(clip=.01)]):
            p = self.root / str(i)
            fixture(p, **options)
            self.assertFalse(analyze_capture(p)["accepted"])

    def test_opposing_band_flicker_not_hidden_by_stable_frame_mean(self):
        p = self.root / "bands"
        fixture(p, amplitude=0, bands=lambda i: [180 + 10*(-1)**i, 180, 180-10*(-1)**i])
        a = analyze_capture(p)
        self.assertEqual(a["cv_percent"], 0)
        self.assertFalse(a["accepted"])

    def test_reader_loss_and_transport_loss_are_distinct(self):
        p = self.root / "loss"
        fixture(p, missing=10)
        a = analyze_capture(p)
        self.assertIn("too many reader misses", a["reasons"])
        meta = json.loads((p / "capture.json").read_text())
        meta["transport_lost"] = dict(value=999, validity="unsupported")
        (p / "capture.json").write_text(json.dumps(meta))
        self.assertIn("unsupported", analyze_capture(p)["warnings"][0])
        meta["transport_lost"] = dict(value=2, validity="valid")
        (p / "capture.json").write_text(json.dumps(meta))
        self.assertIn("transport_lost: 2", analyze_capture(p)["reasons"])

    def test_truncation_nan_and_unconfirmed_shutdown_rejected(self):
        for i, kind in enumerate(("count", "nan", "shutdown", "identity")):
            p = self.root / str(i)
            fixture(p)
            meta = json.loads((p / "capture.json").read_text())
            if kind == "count": meta["frames"] += 1
            if kind == "shutdown": meta["shutdown_confirmed"] = False
            (p / "capture.json").write_text(json.dumps(meta))
            if kind == "nan":
                (p / "frames.csv").write_text((p / "frames.csv").read_text().replace("180.4", "nan"))
            if kind == "identity":
                data = (p / "frames.csv").read_text().splitlines()
                data[2] = data[1]
                (p / "frames.csv").write_text("\n".join(data))
            with self.assertRaises(ValueError): analyze_capture(p)

    def test_selection_uses_center_not_lucky_minimum(self):
        def candidate(delay, score, accepted=True):
            return dict(delay_us=delay, exposure_us=.8,
                        modes={m:dict(score=score, accepted=accepted) for m in ("overview", "experiment")})
        candidates = [candidate(0, .01, False), candidate(15, .7), candidate(30, .8),
                      candidate(45, .35), candidate(60, .36), candidate(75, .3)]
        selected = select_candidate(candidates)
        self.assertEqual(selected["delay_us"], 60)
        self.assertEqual(selected["plateau_delays_us"], [45, 60, 75])
        candidates[-2]["modes"]["overview"]["accepted"] = False
        with self.assertRaises(ValueError): select_candidate(candidates)

    def test_dry_run_has_no_side_effects_and_preserves_fields(self):
        sampler = FakeSampler()
        report = tune(self.profile, output=self.root/"unused", exposures=[.8, 2], sampler=sampler, dry_run=True)
        self.assertEqual(report["status"], "dry_run")
        self.assertFalse((self.root/"unused").exists())
        self.assertEqual(sampler.calls, [])
        self.assertEqual(self.profile.read_bytes(), self.original)
        p = make_plan(profile(), [60], [.8])[0]
        self.assertEqual(p["custom"], profile()["custom"])
        self.assertEqual(p["live_view"], profile()["live_view"])

    def test_invalid_plan_before_hardware(self):
        for delays, exposures in [([0, 0], [2]), ([200], [2]), ([-1], [2]), ([1], [math.nan]), ([1], [True])]:
            with self.assertRaises(ValueError): make_plan(profile(), delays, exposures)

    def run_tune(self, **kwargs):
        return tune(self.profile, output=self.root/"out", delays=[0, 45, 60, 75], exposures=[.8],
                    sampler=kwargs.pop("sampler", FakeSampler()), progress=lambda _: None, **kwargs)

    def test_full_workflow_validates_both_modes_without_applying_by_default(self):
        sampler = FakeSampler()
        report = self.run_tune(sampler=sampler)
        self.assertEqual(report["status"], "validated")
        self.assertEqual(report["selected"]["delay_us"], 60)
        self.assertEqual(len(sampler.calls), 12) # baseline + 4 candidates + validation, both modes
        self.assertEqual(len(sampler.validated), 5)
        self.assertEqual(self.profile.read_bytes(), self.original)
        self.assertTrue((self.root/"out/REPORT.md").is_file())

    def test_apply_roundtrip_preserves_unknown_fields_and_exact_backup(self):
        report = self.run_tune(apply=True)
        self.assertTrue(report["applied"])
        actual = json.loads(self.profile.read_bytes())
        expected = profile()
        expected.update(exposure_time_us=.8, acq_trigger_delay_us=60)
        self.assertEqual(actual, expected)
        self.assertEqual((self.root/"out/original.json").read_bytes(), self.original)

    def test_long_validation_failure_does_not_apply(self):
        with self.assertRaisesRegex(ValueError, "Long validation"):
            self.run_tune(sampler=FakeSampler(validation_bad=True), apply=True)
        self.assertEqual(self.profile.read_bytes(), self.original)
        self.assertFalse((self.root/"out/recommended.json").exists())
        self.assertIn("Long validation failed", (self.root/"out/REPORT.md").read_text(encoding="utf-8"))

    def test_capture_failure_stops_sweep_without_apply(self):
        sampler = FakeSampler(fail=3)
        with self.assertRaises(RuntimeError): self.run_tune(sampler=sampler, apply=True)
        self.assertEqual(len(sampler.calls), 3)
        self.assertEqual(self.profile.read_bytes(), self.original)
        self.assertEqual(json.loads((self.root/"out/report.json").read_text())["status"], "failed")

    def test_concurrent_profile_edit_is_preserved(self):
        self.profile.write_text('{"edited": true}')
        with self.assertRaisesRegex(RuntimeError, "changed"):
            apply_profile(self.profile, self.original, profile(), self.root/"backup")
        self.assertEqual(json.loads(self.profile.read_text()), {"edited": True})

    def test_atomic_replace_failure_retains_original_and_backup(self):
        with patch("mib_sync_tuning.workflow.os.replace", side_effect=OSError("disk fault")):
            with self.assertRaises(OSError):
                apply_profile(self.profile, self.original, {"new":1}, self.root/"backup")
        self.assertEqual(self.profile.read_bytes(), self.original)
        self.assertEqual((self.root/"backup").read_bytes(), self.original)
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_cli_rejects_apply_dry_run(self):
        self.assertEqual(main(["--profile", str(self.profile), "--dry-run", "--apply"]), 1)

    def test_rate_loss_rejects_an_apparently_stable_candidate(self):
        class SlowSampler(FakeSampler):
            def capture(self, *args):
                super().capture(*args)
                directory = args[2]
                if directory.name.startswith("candidate"):
                    path = directory / "capture.json"
                    meta = json.loads(path.read_text())
                    meta["capture_fps"] = 400
                    path.write_text(json.dumps(meta))
        with self.assertRaisesRegex(ValueError, "No unclipped"):
            self.run_tune(sampler=SlowSampler(), apply=True)
        self.assertEqual(self.profile.read_bytes(), self.original)

    def test_invalid_candidate_preflight_prevents_all_hardware_access(self):
        class InvalidSampler(FakeSampler):
            def validate(self, path):
                if path.name.startswith("candidate"): raise ValueError("invalid timing")
        sampler = InvalidSampler()
        with self.assertRaisesRegex(ValueError, "invalid timing"):
            self.run_tune(sampler=sampler)
        self.assertFalse(sampler.calls)

    def test_existing_output_is_not_overwritten(self):
        p = self.root / "out"
        p.mkdir()
        (p / "original.json").write_text("previous evidence")
        with self.assertRaises(FileExistsError): self.run_tune()
        self.assertEqual((p / "original.json").read_text(), "previous evidence")

    def test_report_failure_happens_before_apply(self):
        with patch("mib_sync_tuning.report.write_report", side_effect=OSError("disk full")):
            with self.assertRaises(OSError): self.run_tune(apply=True)
        self.assertEqual(self.profile.read_bytes(), self.original)

    def test_timeout_requests_cooperative_shutdown_and_aborts(self):
        sampler = NativeSampler(Path(sys.executable))
        directory = self.root / "capture"
        directory.mkdir()
        with patch("mib_sync_tuning.workflow.subprocess.Popen") as popen:
            child = popen.return_value
            child.wait.side_effect = [subprocess.TimeoutExpired("capture", 1), 1]
            with self.assertRaisesRegex(RuntimeError, "cancelled/timed out"):
                sampler.capture(self.profile, "overview", directory, 1, 0)
            self.assertTrue((directory/"cancel.request").exists())
            child.kill.assert_not_called()

    def test_wrong_mode_evidence_is_rejected(self):
        class WrongSampler(FakeSampler):
            def capture(self, *args):
                super().capture(*args)
                path = args[2] / "capture.json"
                meta = json.loads(path.read_text())
                meta["mode"] = "wrong"
                path.write_text(json.dumps(meta))
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.run_tune(sampler=WrongSampler(), apply=True)
        self.assertEqual(self.profile.read_bytes(), self.original)

    def test_wedged_driver_is_never_reported_off(self):
        sampler = NativeSampler(Path(sys.executable))
        directory = self.root / "capture"
        directory.mkdir()
        with patch("mib_sync_tuning.workflow.subprocess.Popen") as popen:
            child = popen.return_value
            child.wait.side_effect = [subprocess.TimeoutExpired("capture", 1), subprocess.TimeoutExpired("capture", 1), 1]
            with self.assertRaisesRegex(RuntimeError, "OFF is unconfirmed"):
                sampler.capture(self.profile, "overview", directory, 1, 0)
            child.kill.assert_called_once()


if __name__ == "__main__":
    unittest.main()
