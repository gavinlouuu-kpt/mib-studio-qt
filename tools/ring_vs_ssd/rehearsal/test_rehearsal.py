#!/usr/bin/env python3
"""Tests of the host rehearsal pieces (tools/ring_vs_ssd/rehearsal): the fake pzrec's run table and records, and the frame content the C++ rehearsal provider must equal.

  python3 test_rehearsal.py            run the tests
  python3 test_rehearsal.py --write    regenerate testdata/frames.json (the golden content the C++ test `processing.rehearsal_provider` compares fillFrame with)

POSIX only (the fake uses flock); numpy needed.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import fake_pzrec as F  # noqa: E402

GOLDEN = HERE / "testdata" / "frames.json"
IDS = [1, 2, 3, 4, 5, 6, 97, 1000, 4999, 30120]
FPS = 5000.0
TICK_BASE, TICK_HZ = 1_000_000, 100_000_000


def fnv(b: bytes) -> str:
    h = 0xCBF29CE484222325
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def expectation(fid):
    gray, packed, objs = F.frame_content(fid)
    return {"gray_fnv": fnv(gray.tobytes()), "mask_fnv": fnv(packed), "cells": F.cell_words(objs), "ticks": F.ticks_of(fid, TICK_BASE, TICK_HZ, FPS)}


def golden():
    return {"fps": FPS, "tick_base": TICK_BASE, "tick_hz": TICK_HZ, "frames": {str(i): expectation(i) for i in IDS}}


class Rehearsal(unittest.TestCase):
    def test_golden_content_is_current(self):
        self.assertEqual(json.loads(GOLDEN.read_text()), golden(), "frames.json is stale: python3 test_rehearsal.py --write (and the C++ RehearsalProvider must follow)")

    def fake(self, d, *args, **env):
        e = dict(os.environ, MIB_REHEARSAL_DIR=str(d), REHEARSAL_STOP_S="0", **env)
        return subprocess.run([sys.executable, str(HERE / "fake_pzrec.py"), *args], capture_output=True, env=e)

    def emu(self, d, running, fps=1000.0, seconds=0.05):
        t0 = time.monotonic_ns() - int(seconds * 1e9)
        (d / "emu.json").write_text(json.dumps({"epoch": 7, "fps": fps, "tick_hz": TICK_HZ, "tick_base": TICK_BASE, "t0_ns": t0, "stop_ns": 0 if running else t0 + int(seconds * 1e9),
                                                "running": running, "ring_frames": 5000}))

    def test_a_run_from_start_to_read(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            self.assertNotEqual(self.fake(d, "start", "disk", "--filter", "all").returncode, 0, "start refuses while the provider is not armed")
            self.emu(d, True)
            r = self.fake(d, "start", "disk", "--filter", "all", "--sampler", "1", "--start-ms", "1791530000000", "--wall-source", "1", "--tag", "9")
            self.assertEqual(r.returncode, 0, r.stderr)
            st = json.loads(r.stdout)
            self.assertTrue(st["open_run"] and st["state"] == "RECORDING" and st["open_run_id"] == 1 and st["open_client_tag"] == 9)
            self.assertNotEqual(self.fake(d, "start", "disk").returncode, 0, "a second start refuses")
            self.emu(d, False, seconds=0.05)                       # the provider stopped: 50 frames at 1 kHz
            r = self.fake(d, "stop", "disk")
            row = json.loads(self.fake(d, "runs", "disk").stdout)[0]
            self.assertEqual((row["open"], row["filter"], row["first_frame_id"], row["last_frame_id"], row["written"], row["seen"], row["dropped"], row["tick_hz"]), (False, 0, 1, 50, 50, 50, 0, TICK_HZ))
            self.assertEqual(json.loads(r.stdout)["state"], "READY")
            rd = self.fake(d, "read", "1", "disk", "--summary", "--from", "10", "--count", "3")
            self.assertEqual(rd.returncode, 0, rd.stderr)
            self.assertEqual(len(rd.stdout), 3 * F.REC)
            self.assertEqual(json.loads(rd.stderr.strip().splitlines()[-1])["records"], 3)
            import ring_vs_ssd as R  # the tool's own decoder reads what the fake wrote

            dec, _ = R.P.load_decoder()
            p = d / "r.bin"
            p.write_bytes(rd.stdout)
            ids, epochs, ticks = R.scan_headers(p, dec)
            self.assertEqual(ids, [11, 12, 13])
            self.assertEqual(set(epochs), {7})
            self.assertEqual(ticks, [F.ticks_of(i, TICK_BASE, TICK_HZ, 1000.0) for i in ids])
            gray, packed, _ = F.frame_content(11)
            self.assertEqual(rd.stdout[4096:4096 + 49152], gray.tobytes())
            self.assertEqual(rd.stdout[53248:53248 + 6144], packed)

    def test_drops_are_counted_and_gaps_are_drops_before(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            self.emu(d, True)
            self.assertEqual(self.fake(d, "start", "disk", "--filter", "all", REHEARSAL_DROP_EVERY="10").returncode, 0)
            self.emu(d, False, seconds=0.05)
            self.fake(d, "stop", "disk", REHEARSAL_DROP_EVERY="10")
            row = json.loads(self.fake(d, "runs", "disk", REHEARSAL_DROP_EVERY="10").stdout)[0]
            self.assertEqual((row["seen"], row["dropped"], row["written"], row["passed"]), (50, 5, 45, 50))
            rd = self.fake(d, "read", "1", "disk", "--summary", REHEARSAL_DROP_EVERY="10")
            self.assertEqual(len(rd.stdout), 45 * F.REC)
            import ring_vs_ssd as R

            dec, _ = R.P.load_decoder()
            p = d / "r.bin"
            p.write_bytes(rd.stdout)
            ids, _, _ = R.scan_headers(p, dec)
            self.assertNotIn(10, ids)
            self.assertIn(11, ids)

    def test_the_corrupt_knob_changes_exactly_one_byte(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            self.emu(d, True)
            self.fake(d, "start", "disk", "--filter", "all")
            self.emu(d, False)
            self.fake(d, "stop", "disk")
            a = self.fake(d, "read", "1", "disk", "--summary", "--count", "5").stdout
            b = self.fake(d, "read", "1", "disk", "--summary", "--count", "5", REHEARSAL_CORRUPT_ID="3").stdout
            diff = [i for i in range(len(a)) if a[i] != b[i]]
            self.assertEqual(len(diff), 1)
            self.assertEqual(diff[0], 2 * F.REC + 4096 + 1000)


if __name__ == "__main__":
    if "--write" in sys.argv:
        GOLDEN.write_text(json.dumps(golden(), indent=1, sort_keys=True) + "\n")
        print(f"wrote {GOLDEN}")
        sys.exit(0)
    sys.exit(0 if unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Rehearsal)).wasSuccessful() else 1)
