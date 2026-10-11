"""ring_vs_ssd: synthetic MIBR packets and synthetic stored records (built with the vendored ABI encoder, as the pzrec_to_h5 tests do), a baseline that passes and a mutant per criterion.

  python3 -m pytest tools/ring_vs_ssd   (or: python3 tools/ring_vs_ssd/test_ring_vs_ssd.py)
"""
import contextlib
import io
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "pzrec_to_h5"))
import ring_vs_ssd as R  # noqa: E402
import synthetic_records as T  # noqa: E402  (build_record, cell: the synthetic stored records; numpy only)

RUN, EPOCH, FIRST_ID, N = 19, 5, 2000, 40           # a ring of 40 frames: ids 2000..2039, sequences 0..39
T0, STEP = 62_000_000_000, 40_000
ARGS = ["--guard", "4", "--delta-max", "2"]         # the ring is tiny: minimum overlap = 40 - 4 - 2


def objects_of(k):
    return [T.cell(1), T.cell(2, reason=3)] if k % 3 == 0 else ([T.cell(1)] if k % 3 == 1 else [])


def cell_words(objs):
    out = []
    for i, o in enumerate(objs):
        words = list(o["payload"]) + [0] * (15 - len(o["payload"]))
        x, y, w, h = o["bbox"]
        words += [x | y << 16, w | h << 16, (len(objs) & 0xFF) << 24 | (i & 0xFF) << 16 | (1 if o.get("flags", 8) & 8 else 0), o.get("validity", 0x7FFF)]
        out.append(words)
    return out


def make_packet(seq, frame_id, ticks, img, mask, objs, frame_flags=0, flags=1, epoch_free=True):
    """What buildRingPacket produces (PzFrameRing.cpp)."""
    packed = np.packbits(mask, axis=1, bitorder="little").tobytes() if flags & 1 else bytes(len(img.tobytes()) // 8)
    cells = cell_words(objs)
    head = R.HEADER.pack(b"MIBR", 1, 48, seq, frame_id, ticks, 100_000_000, frame_flags, img.shape[1], img.shape[0], len(cells), flags)
    return head + img.tobytes() + packed + b"".join(struct.pack("<19I", *c) for c in cells)


class Scenario:
    """A ring of N frames and an SSD run whose tail covers it (from ring frame `tail_from`), plus older records with a hole so they are not part of the tail."""

    def __init__(self, tc: unittest.TestCase, tail_from=4, ssd_epoch=EPOCH, ssd_end=N, older_ids=(1990, 1991, 2000, 2001, 2002), invalid=(), ring_flags=None, ssd_ids=None):
        self.d = Path(tempfile.mkdtemp(prefix="rvs_"))
        tc.addCleanup(shutil.rmtree, self.d, True)
        (self.d / "ring").mkdir()
        (self.d / "ring2").mkdir()
        self.packets, self.records, self.ids = {}, [], []
        self.imgs = {}
        order = list(ssd_ids) if ssd_ids is not None else list(older_ids) + [FIRST_ID + k for k in range(tail_from, ssd_end)]
        for fid in order:
            k = fid - FIRST_ID
            objs = objects_of(abs(k))
            fl = 4 if fid in invalid else 0
            rec, img, mask = T.build_record(fid, T0 + STEP * (fid - 1900), run_id=RUN, epoch=ssd_epoch, objects=objs, seed=fid, frame_flags=fl)
            self.records.append(rec)
            self.ids.append(fid)
            self.imgs[fid] = (img, mask, objs, fl)
        for s in range(N):
            fid = FIRST_ID + s
            if fid in self.imgs:
                img, mask, objs, fl = self.imgs[fid]
            else:                                   # a ring frame the SSD never had: its own pixels
                _, img, mask = None, *T.build_record(fid, T0 + STEP * (fid - 1900), run_id=RUN, epoch=EPOCH, objects=[], seed=fid)[1:]
                objs, fl = [], 0
            flags = 1 | (4 if fl else 0)
            if ring_flags and fid in ring_flags:
                flags |= ring_flags[fid]
            self.packets[s] = make_packet(s, fid, T0 + STEP * (fid - 1900), img, mask, objs, frame_flags=fl, flags=flags)
        self.status = {"available": True, "frozen": True, "run_frozen": True, "invalid": False, "stop_incomplete": False, "restore_needed": False, "epoch": EPOCH,
                       "capacity_frames": N, "first_seq": 0, "last_seq": N - 1, "count": N, "head": N - 1, "state": 0}
        self.runs = [{"run_id": RUN, "written": len(self.ids), "last_frame_id": self.ids[-1], "filter": 0, "tick_hz": 100_000_000}]
        self.reread = list(range(0, N, 7)) + [N - 1]
        self.status2 = dict(self.status)
        self.status3 = None

    def write(self):
        for s, b in self.packets.items():
            (self.d / "ring" / f"seq-{s}.mibr").write_bytes(b)
        for s in self.reread:
            (self.d / "ring2" / f"seq-{s}.mibr").write_bytes(self.packets[s])
        (self.d / "status1.json").write_text(json.dumps(self.status))
        (self.d / "status2.json").write_text(json.dumps(self.status2))
        if self.status3 is not None:
            (self.d / "status3.json").write_text(json.dumps(self.status3))
        (self.d / "run.bin").write_bytes(b"".join(self.records))
        (self.d / "runs.json").write_text(json.dumps(self.runs))

    def run(self, *extra, reread=True):
        self.write()
        argv = ["--ring", str(self.d / "ring"), "--status", str(self.d / "status1.json"), "--run", str(self.d / "run.bin"), "--runs", str(self.d / "runs.json"),
                "--run-id", str(RUN), "--out", str(self.d / "out"), *ARGS]
        if reread:
            argv += ["--status2", str(self.d / "status2.json"), "--ring2", str(self.d / "ring2")]
        if self.status3 is not None:
            argv += ["--status3", str(self.d / "status3.json")]
        argv += list(extra)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = R.main(argv)
        return rc, buf.getvalue()


class Cases(unittest.TestCase):
    def failing(self, out, fragment):
        lines = [ln for ln in out.splitlines() if ln.startswith("FAIL")]
        self.assertTrue(any(fragment in ln for ln in lines), f"no FAIL line with {fragment!r} in:\n{out}")

    def test_baseline_passes_and_reports_the_overlap(self):
        sc = Scenario(self)
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)
        rep = json.loads((sc.d / "out" / "ring-vs-ssd.json").read_text())
        self.assertTrue(rep["ok"])
        self.assertEqual(rep["overlap"], 36)                        # ring frames 2004..2039 are in the SSD tail
        self.assertEqual(rep["older_absent"], 1)                    # 2003 is the hole before the tail
        self.assertEqual(rep["ssd"]["tail_start_frame_id"], 2004)
        self.assertEqual(rep["compared"]["tail"], 36)
        self.assertEqual(rep["compared"]["older"], 3)               # 2000..2002 are on the SSD below the hole
        self.assertTrue((sc.d / "out" / "summary.txt").read_text().rstrip().endswith("OVERALL PASS"))

    # --- a: content ---------------------------------------------------------------------------------------------------------------------------
    def mutate(self, seq, fn, fragment, **kw):
        sc = Scenario(self, **kw)
        pk = bytearray(sc.packets[seq])
        fn(pk)
        sc.packets[seq] = bytes(pk)
        sc.reread = [s for s in sc.reread if s != seq]      # the mutation is in the first read only, the re-read check must not hide it
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, fragment)
        return sc, out

    def test_a_a_gray_byte(self):
        self.mutate(20, lambda b: b.__setitem__(48 + 100, b[48 + 100] ^ 0xFF), "ring frames in the SSD tail are equal")

    def test_a_a_mask_bit(self):
        self.mutate(20, lambda b: b.__setitem__(48 + 49152 + 10, b[48 + 49152 + 10] ^ 0x04), "ring frames in the SSD tail are equal")

    def test_a_a_result_word(self):
        # frame 2022: k = 22, 22 % 3 == 1: one cell. The payload word 3 of the cell
        self.mutate(22, lambda b: b.__setitem__(48 + 49152 + 6144 + 4 * 3, b[48 + 49152 + 6144 + 4 * 3] ^ 1), "ring frames in the SSD tail are equal")

    def test_a_ticks(self):
        self.mutate(20, lambda b: b.__setitem__(24, b[24] ^ 1), "ring frames in the SSD tail are equal")

    def test_a_frame_flags(self):
        self.mutate(20, lambda b: b.__setitem__(36, b[36] | 0x10), "ring frames in the SSD tail are equal")

    def test_a_cell_count(self):
        # drop the last cell of frame 2021 (two cells... k = 21 % 3 == 0: two objects) and fix up the header count and the length
        sc = Scenario(self)
        pk = bytearray(sc.packets[21])
        n = struct.unpack_from("<H", pk, 44)[0]
        self.assertEqual(n, 2)
        struct.pack_into("<H", pk, 44, 1)
        sc.packets[21] = bytes(pk[:-R.CELL_WORDS * 4])
        sc.reread = [s for s in sc.reread if s != 21]
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "ring frames in the SSD tail are equal")

    def test_a_a_ring_frame_missing(self):
        sc = Scenario(self)
        del sc.packets[10]
        sc.reread = [s for s in sc.reread if s != 10]
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "a packet for every sequence")
        self.failing(out, "every SSD tail record inside the ring's range is in the ring")

    def test_a_epoch_mismatch(self):
        sc = Scenario(self, ssd_epoch=6)
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the SSD tail has one epoch and it is the ring's")
        self.failing(out, "every ring frame inside the SSD tail is on the SSD")

    def test_a_ssd_record_with_a_wrong_run_id_is_a_problem(self):
        sc = Scenario(self)
        rec, _, _ = T.build_record(2030, T0 + STEP * 130, run_id=RUN + 1, epoch=EPOCH, objects=objects_of(30), seed=2030)
        i = sc.ids.index(2030)
        sc.records[i] = rec
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "ring frames in the SSD tail are equal")

    # --- b: older frames ----------------------------------------------------------------------------------------------------------------------
    def test_b_an_older_frame_that_the_ssd_has_must_be_equal(self):
        self.mutate(1, lambda b: b.__setitem__(48 + 5, b[48 + 5] ^ 0xFF), "older ring frames that the SSD has are equal")

    def test_b_too_many_older_frames_absent(self):
        sc = Scenario(self, older_ids=())
        rc, out = sc.run("--guard", "0", "--delta-max", "1")      # later options win: 4 frames absent against a bound of 1
        self.assertEqual(rc, 1, out)
        self.failing(out, "older ring frames absent from the SSD stay within the guard")

    # --- c: the ring --------------------------------------------------------------------------------------------------------------------------
    def test_c_not_consecutive(self):
        sc = Scenario(self)
        pk = bytearray(sc.packets[15])
        struct.pack_into("<Q", pk, 16, struct.unpack_from("<Q", pk, 16)[0] + 1)
        sc.packets[15] = bytes(pk)
        sc.reread = [s for s in sc.reread if s != 15]
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "frame_id - sequence is constant")

    def test_c_the_packet_says_another_sequence(self):
        self.mutate(12, lambda b: struct.pack_into("<Q", b, 8, 13), "every packet parses")

    def test_c_a_truncated_packet(self):
        sc = Scenario(self)
        sc.packets[5] = sc.packets[5][:-100]
        sc.reread = [s for s in sc.reread if s != 5]
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "every packet parses")

    def test_c_a_ring_that_is_not_frozen_or_not_held(self):
        for key in ("frozen", "run_frozen"):
            sc = Scenario(self)
            sc.status[key] = False
            sc.status2 = dict(sc.status)
            rc, out = sc.run()
            self.assertEqual(rc, 1, out)
            self.failing(out, "frozen, held, valid")

    def test_status_without_epoch_is_unusable_input(self):
        sc = Scenario(self)
        del sc.status["epoch"]
        rc, out = sc.run()
        self.assertEqual(rc, 2, out)
        self.assertIn("ABI 37", out)

    def test_status_with_null_epoch_is_unusable_input(self):
        sc = Scenario(self)
        sc.status["epoch"] = None
        rc, out = sc.run()
        self.assertEqual(rc, 2, out)
        self.assertIn("null `epoch`", out)

    def test_hold_note_is_reported_as_info(self):
        sc = Scenario(self)
        sc.status["hold_note"] = "The Run is stopped and its frames are held, but the LED could not be switched off: x"
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)
        self.assertIn("INFO hold_note on the first status", (sc.d / "out" / "summary.txt").read_text())

    # --- d: the overlap -----------------------------------------------------------------------------------------------------------------------
    def test_d_the_overlap_is_too_small(self):
        sc = Scenario(self, tail_from=12, older_ids=())           # the SSD tail starts at frame 2012: overlap 28 < 40 - 4 - 2
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the overlap is at least ring frames - guard - the measured delta")

    def test_d_the_ring_is_far_ahead_of_the_run_end(self):
        sc = Scenario(self, ssd_end=N - 6)                         # the SSD ends 6 frames before the ring: more than delta 2
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the ring's newest frame is within the delta cap")
        self.failing(out, "ring frames beyond the run's end")

    def test_d_the_run_table_disagrees_with_the_stream(self):
        sc = Scenario(self)
        sc.runs[0]["written"] += 1
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the SSD stream holds the run table's record count")

    # --- e: re-reads --------------------------------------------------------------------------------------------------------------------------
    def test_e_the_status_changed(self):
        sc = Scenario(self)
        sc.status2 = dict(sc.status, first_seq=3)
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the ring-defining status after the export equals the first")

    def test_e_a_reread_packet_differs(self):
        sc = Scenario(self)
        sc.write()
        b = bytearray(sc.packets[14])
        b[48 + 7] ^= 1
        sc.packets_backup = sc.packets[14]
        sc.reread = [14]
        # write the first read as is, then a re-read that differs
        rc0, _ = sc.run()
        (sc.d / "ring2" / "seq-14.mibr").write_bytes(bytes(b))
        argv = ["--ring", str(sc.d / "ring"), "--status", str(sc.d / "status1.json"), "--run", str(sc.d / "run.bin"), "--runs", str(sc.d / "runs.json"), "--run-id", str(RUN),
                "--out", str(sc.d / "out2"), "--status2", str(sc.d / "status2.json"), "--ring2", str(sc.d / "ring2"), *ARGS]
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = R.main(argv)
        self.assertEqual(rc0, 0)
        self.assertEqual(rc, 1, buf.getvalue())
        self.failing(buf.getvalue(), "every re-read packet is byte-equal")

    def test_e_the_later_idle_status_changed(self):
        sc = Scenario(self)
        sc.status3 = dict(sc.status, frozen=False)
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the later (idle) ring-defining status equals the first")

    def test_e_a_later_equal_status_passes(self):
        sc = Scenario(self)
        sc.status3 = dict(sc.status)
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)

    def test_e_no_reread_given_fails_unless_skipped_knowingly(self):
        sc = Scenario(self)
        rc, out = sc.run(reread=False)
        self.assertEqual(rc, 1, out)
        self.failing(out, "the re-read status and packets were given")
        rc2, out2 = sc.run("--no-reread", reread=False)
        self.assertEqual(rc2, 0, out2)

    # --- f: flagged frames --------------------------------------------------------------------------------------------------------------------
    def test_f_an_invalid_frame_is_listed_and_excluded(self):
        sc = Scenario(self, invalid=(2025,))
        # its pixels differ on the ring side, which would fail a byte comparison: flagged frames are excluded from it
        pk = bytearray(sc.packets[25])
        pk[48 + 1] ^= 0xFF
        sc.packets[25] = bytes(pk)
        sc.reread = [s for s in sc.reread if s != 25]
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)
        rep = json.loads((sc.d / "out" / "ring-vs-ssd.json").read_text())
        self.assertEqual([f["frame_id"] for f in rep["flagged"]], [2025])
        self.assertIn("frame 2025", out)
        rc2, out2 = sc.run("--strict-flags")
        self.assertEqual(rc2, 1, out2)
        self.failing(out2, "frames flagged cut / mask incomplete / invalid")

    def test_f_the_two_sides_disagree_on_a_flag(self):
        sc = Scenario(self, ring_flags={2026: R.FLAG_CUT})          # the ring says the MONO8 block was cut, the SSD record says nothing
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "ring frames in the SSD tail are equal")

    def test_f_a_mask_not_delivered_on_both_sides_is_agreed(self):
        # the ring says no mask (flag 0), the SSD record has a MASK1 block: that is a disagreement on mask_present
        sc = Scenario(self, ring_flags=None)
        pk = bytearray(sc.packets[18])
        struct.pack_into("<H", pk, 46, struct.unpack_from("<H", pk, 46)[0] & ~1)
        sc.packets[18] = bytes(pk)
        sc.reread = [s for s in sc.reread if s != 18]
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "ring frames in the SSD tail are equal")

    # --- the review of #697 --------------------------------------------------------------------------------------------------------------------
    def test_a_ring_entirely_past_the_run_end_is_not_a_vacuous_pass(self):
        sc = Scenario(self, ssd_ids=range(1000, 1020))             # the SSD run ended long before the ring's frames: nothing overlaps
        rc, out = sc.run("--guard", "100")                          # a guard larger than the ring would make the old bound 0
        self.assertEqual(rc, 1, out)
        self.failing(out, "at least one ring frame lies in the SSD tail and was compared")

    def test_a_run_longer_than_the_ring_leaves_a_full_ring(self):
        sc = Scenario(self)
        sc.status["capacity_frames"] = 30                           # the run has 41 records, more than 30: a ring of 40 frames cannot be the full ring of 30
        sc.status2 = dict(sc.status)
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "a run longer than the ring leaves a full ring")

    def test_a_drain_drop_cannot_hide_a_ring_that_should_be_full(self):
        sc = Scenario(self)
        sc.status["capacity_frames"] = 30
        sc.status2 = dict(sc.status)
        sc.runs[0]["written"] = 20                                   # fewer records than the capacity, but the run offered 41 frames
        sc.runs[0]["seen"] = 41
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "a run longer than the ring leaves a full ring")

    def test_restore_needed_is_a_ring_defining_key(self):
        sc = Scenario(self)
        sc.status2 = dict(sc.status, restore_needed=True)
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the ring-defining status after the export equals the first")

    def test_a_short_run_may_leave_a_ring_with_room_to_spare(self):
        sc = Scenario(self)
        sc.status["capacity_frames"] = 5000
        sc.status2 = dict(sc.status)
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)

    def test_e_a_live_reading_that_moves_while_the_ring_idles_is_only_info(self):
        sc = Scenario(self)
        sc.status["sensor_fps"] = 5000.750112516877
        sc.status2 = dict(sc.status, sensor_fps=4999.1, fault_cleared=0, reason="")
        sc.status3 = dict(sc.status, sensor_fps=0.0)
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)
        self.assertIn("sensor_fps: 5000.750112516877 -> 4999.1", (sc.d / "out" / "summary.txt").read_text())

    def test_e_each_ring_defining_key_is_checked(self):
        for key, value in (("frozen", False), ("run_frozen", False), ("invalid", True), ("epoch", 6), ("first_seq", 1), ("last_seq", N - 2), ("count", N - 1), ("head", 3), ("state", 2),
                           ("capacity_frames", 41), ("stop_incomplete", True)):
            sc = Scenario(self)
            sc.status2 = dict(sc.status, **{key: value})
            rc, out = sc.run()
            self.assertEqual(rc, 1, f"{key}: {out}")
            self.failing(out, "the ring-defining status after the export equals the first")

    def test_c_the_packets_tick_rate_is_the_run_tables(self):
        self.mutate(10, lambda b: struct.pack_into("<I", b, 32, 99_000_000), "the packets' tick rate is the run table's")

    def test_filter_not_all_is_unusable_input(self):
        sc = Scenario(self)
        sc.runs[0]["filter"] = 2
        rc, out = sc.run()
        self.assertEqual(rc, 2, out)
        self.assertIn("not ALL", out)

    def test_an_empty_or_headerless_export_is_unusable_input_not_a_traceback(self):
        for blob in (b"", b"\x00" * 100, b"\x00" * R.REC_BYTES):
            sc = Scenario(self)
            sc.write()
            sc.records = [blob]
            rc, out = sc.run()
            self.assertEqual(rc, 2, f"{len(blob)} bytes: {out}")
            self.assertNotIn("Traceback", out)

    def test_d_the_bound_follows_the_measured_delta(self):
        sc = Scenario(self, ssd_end=N - 1, older_ids=())            # the ring is 1 frame ahead of the run end: minimum 40 - 4 - 1 = 35, the tail 2004..2038 gives exactly 35
        rc, out = sc.run()
        self.assertEqual(rc, 0, out)
        sc = Scenario(self, ssd_end=N - 1, tail_from=5, older_ids=())   # 34 would have passed the old bound (40 - 4 - delta cap 2)
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "the overlap is at least ring frames - guard - the measured delta")

    def test_f_ticks_and_frame_flags_are_compared_even_for_a_flagged_frame(self):
        sc = Scenario(self, invalid=(2025,))
        pk = bytearray(sc.packets[25])
        pk[24] ^= 1                                                 # ticks
        sc.packets[25] = bytes(pk)
        sc.reread = [s for s in sc.reread if s != 25]
        rc, out = sc.run()
        self.assertEqual(rc, 1, out)
        self.failing(out, "ring frames in the SSD tail are equal")


GOLDEN = HERE / "testdata"


class GoldenPackets(unittest.TestCase):
    """Four packets that Studio's own readFrame + buildRingPacket made from four consecutive REAL stored records (testdata/, kept equal to the reader by processing.ring_packet_golden).
    The RESULT mapping is therefore tested against the C++ builder, not against this tool's own reading of the layout."""

    def scenario(self):
        d = Path(tempfile.mkdtemp(prefix="rvs_golden_"))
        self.addCleanup(shutil.rmtree, d, True)
        (d / "ring").mkdir()
        for s in range(4):
            shutil.copy(GOLDEN / f"seq-{s}.mibr", d / "ring" / f"seq-{s}.mibr")
        dec, _ = R.P.load_decoder()
        recs = [dec.frame_header((GOLDEN / "records.bin").read_bytes()[i * R.REC_BYTES:i * R.REC_BYTES + 64]) for i in range(4)]
        self.ids = [r["frame_id"] for r in recs]
        status = {"frozen": True, "run_frozen": True, "invalid": False, "restore_needed": False, "epoch": recs[0]["epoch"], "capacity_frames": 4, "first_seq": 0, "last_seq": 3, "count": 4}
        (d / "status.json").write_text(json.dumps(status))
        (d / "runs.json").write_text(json.dumps([{"run_id": recs[0]["run_id"], "written": 4, "last_frame_id": self.ids[-1], "filter": 0, "tick_hz": 100_000_000}]))
        self.run_id = recs[0]["run_id"]
        return d

    def run_tool(self, d):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = R.main(["--ring", str(d / "ring"), "--status", str(d / "status.json"), "--run", str(GOLDEN / "records.bin"), "--runs", str(d / "runs.json"), "--run-id", str(self.run_id),
                         "--out", str(d / "out"), "--no-reread", "--guard", "0", "--delta-max", "0"])
        return rc, buf.getvalue()

    def test_studios_packets_equal_the_records(self):
        d = self.scenario()
        rc, out = self.run_tool(d)
        self.assertEqual(rc, 0, out)
        rep = json.loads((d / "out" / "ring-vs-ssd.json").read_text())
        self.assertEqual((rep["overlap"], rep["compared"]["tail"], rep["tail_flagged"]), (4, 4, 0))
        # two cells of 19 words per packet, nothing left over
        self.assertEqual([len(R.parse_packet((d / "ring" / f"seq-{s}.mibr").read_bytes()).cells) for s in range(4)], [2, 2, 2, 2])

    def test_a_changed_word_in_a_golden_cell_fails(self):
        for word in range(19):
            d = self.scenario()
            p = d / "ring" / "seq-2.mibr"
            b = bytearray(p.read_bytes())
            off = 48 + 512 * 96 + 512 * 96 // 8 + 19 * 4 + 4 * word          # the second cell
            b[off] ^= 0x01
            p.write_bytes(bytes(b))
            rc, out = self.run_tool(d)
            self.assertEqual(rc, 1, f"word {word}: {out}")
            self.assertIn("RESULT 1: words [%d] differ" % word, (d / "out" / "ring-vs-ssd.json").read_text().replace("\\n", " "))

    def test_a_changed_gray_or_mask_byte_in_a_golden_packet_fails(self):
        for off in (48 + 17, 48 + 512 * 96 + 5):
            d = self.scenario()
            p = d / "ring" / "seq-1.mibr"
            b = bytearray(p.read_bytes())
            b[off] ^= 0x80
            p.write_bytes(bytes(b))
            rc, out = self.run_tool(d)
            self.assertEqual(rc, 1, out)


REAL = Path("/mnt/hdd/shared/exports/export-slot/20261010T201841Z")


@unittest.skipUnless((REAL / "DL" / "run19.bin").exists(), "the export slot's downloads are on the HDD only")
class RealRecords(unittest.TestCase):
    """A smoke run on the export slot's real run 19: a ring is built from the newest 300 records (images, masks, flags, header from the decoded records) and compared with the whole
    download. It exercises the decoder path and the tail logic on real data; the RESULT words are derived with the tool's own function, so they are not an independent check."""

    def test_the_tail_of_run_19_compared_with_itself(self):
        dec, _ = R.P.load_decoder()
        bits = dec.abi().bits
        n = 300
        d = Path(tempfile.mkdtemp(prefix="rvs_real_"))
        self.addCleanup(shutil.rmtree, d, True)
        (d / "ring").mkdir()
        size = (REAL / "DL" / "run19.bin").stat().st_size // R.REC_BYTES
        with open(REAL / "DL" / "run19.bin", "rb") as f:
            f.seek((size - n) * R.REC_BYTES)
            recs = [dec.decode_record(f.read(R.REC_BYTES), i, strict=False, expected_run_id=19) for i in range(n)]
        for s, rec in enumerate(recs):
            fl = 1 if rec.mask is not None else 0
            packed = np.packbits(rec.mask, axis=1, bitorder="little").tobytes() if rec.mask is not None else bytes(6144)
            cells = R.expected_cell_words(rec, bits)
            head = R.HEADER.pack(b"MIBR", 1, 48, s, rec.frame_id, rec.ticks, 100_000_000, rec.header["flags"] & ~(1 << bits["frame_flags"]["STORED"]), 512, 96, len(cells), fl)
            (d / "ring" / f"seq-{s}.mibr").write_bytes(head + rec.image.tobytes() + packed + b"".join(struct.pack("<19I", *c) for c in cells))
        status = {"frozen": True, "run_frozen": True, "invalid": False, "restore_needed": False, "epoch": recs[0].epoch, "capacity_frames": n, "first_seq": 0, "last_seq": n - 1, "count": n}
        (d / "status.json").write_text(json.dumps(status))
        runs = REAL / "post.runs.json"
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = R.main(["--ring", str(d / "ring"), "--status", str(d / "status.json"), "--run", str(REAL / "DL" / "run19.bin"), "--runs", str(runs), "--run-id", "19",
                         "--out", str(d / "out"), "--no-reread", "--guard", "0", "--delta-max", "4"])
        self.assertEqual(rc, 0, buf.getvalue())
        rep = json.loads((d / "out" / "ring-vs-ssd.json").read_text())
        self.assertEqual(rep["overlap"], n)


if __name__ == "__main__":
    unittest.main()
