"""pzrec_to_h5: synthetic records built with the vendored ABI encoder (always run) and the fixed S2 reference records on the HDD (skipped without them).

  python3 -m pytest tools/pzrec_to_h5   (or: python3 tools/pzrec_to_h5/test_pzrec_to_h5.py)
"""
import hashlib
import http.server
import json
import shutil
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import h5py
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pzrec_to_h5 as P  # noqa: E402

from synthetic_records import ABI, DEC, FLAG_STORED, PROV, REC, build_record, cell, mib_abi, stream  # noqa: E402,F401

REF = Path("/mnt/hdd/developer-data/IMX426/s2-records")
RUNS = Path(__file__).resolve().parent / "testdata" / "s2-runs.json"


RUN = P.RunInfo(run_id=18, start_unix_ms=1791639269355, tick_hz=100_000_000, first_ticks=62_000_000_000, filter="all", wall_source=1, client_tag=1, written=6)


class Base(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp(prefix="pzh5_"))
        self.addCleanup(shutil.rmtree, self.d, True)

    def write(self, records, name="in.bin"):
        p = self.d / name
        p.write_bytes(b"".join(records))
        return p


class Synthetic(Base):
    def test_converts_and_layout(self):
        recs, imgs, masks = stream(6)
        rep = P.convert(self.write(recs), self.d / "o.h5", RUN, expected_records=6)
        self.assertTrue(rep.ok and rep.converted, rep)
        self.assertFalse((self.d / "o.h5.partial").exists())
        with h5py.File(self.d / "o.h5") as f:
            v, iv = f["valid_frames"], f["invalid_frames"]
            # routed per object, as Studio: frames 0 and 3 have a valid cell and a rejected one (reason AREA), frames 1 and 4 one valid cell, frames 2 and 5 no results
            self.assertEqual((v["metadata"].shape[0], iv["metadata"].shape[0]), (4, 4))
            self.assertEqual(v["images"].shape, (4, 96, 512))
            self.assertTrue(np.array_equal(v["images"][0], imgs[0]) and np.array_equal(iv["images"][0], imgs[0]))        # repeated on every row of the frame
            self.assertTrue(np.array_equal(v["masks"][0], masks[0] * 255) and v["masks"].dtype == np.uint8)
            self.assertTrue(np.array_equal(iv["images"][1], imgs[2]))
            m = v["metadata"][:]
            self.assertEqual(list(m["index"]), [1000, 1002, 1006, 1008])
            self.assertEqual(list(m["isValid"]), [1, 1, 1, 1])
            self.assertEqual(list(iv["metadata"][:]["isValid"]), [0, 0, 0, 0])                                           # the AREA-rejected cells, the no-result frames
            self.assertEqual(list(iv["metadata"][:]["index"]), [1000, 1004, 1006, 1010])
            self.assertAlmostEqual(m["area"][0], 630.0)
            self.assertAlmostEqual(m["youngsModulus"][0], 24.6, places=3)
            self.assertAlmostEqual(m["centroidX"][0], 185.1, places=3)         # unsigned q16.16, as the vendored profile
            self.assertEqual((m["pixelCount"][0], m["blemishCount"][0], m["objectCount"][0]), (385, 18, 1))
            self.assertEqual(m["bboxX"][0], 161)
            ssd = v["ssd_meta"][:]
            self.assertEqual(list(ssd["ssd_run_id"][:2]), [18, 18])
            self.assertEqual(list(ssd["frame_id"]), [1000, 1002, 1006, 1008])
            self.assertEqual(list(ssd["drops_before"]), [0, 1, 1, 1])
            self.assertEqual(list(ssd["record_index"]), [0, 1, 3, 4])
            # wall time = start_unix_ms x 1e6 + (ticks - first_ticks) / tick_hz
            self.assertEqual(int(m["timestampNs"][0]), 1791639269355 * 1_000_000)
            self.assertEqual(int(m["timestampNs"][1]), 1791639269355 * 1_000_000 + 40_000 * 10)
            info = f["experiment_info"].attrs
            self.assertEqual((info["total_valid_frames"], info["total_invalid_frames"], info["roi_w"], info["roi_h"]), (4, 4, 512, 96))
            cfg = json.loads(info["config_json"])
            self.assertEqual((cfg["ssd_run_id"], cfg["records_converted"], cfg["salvaged"]), (18, 6, False))
            self.assertEqual(cfg["input_sha256"], hashlib.sha256(b"".join(recs)).hexdigest())

    def assertQuarantined(self, rep, code):
        self.assertFalse(rep.ok and rep.converted)
        self.assertFalse((self.d / "o.h5").exists(), "no output of a failed input")
        self.assertFalse((self.d / "o.h5.partial").exists())
        report = json.loads((self.d / "quarantine" / "in.bin.report.json").read_text())
        found = [p["code"] for p in report["problems"]] + [b["code"] for b in report["bad_records"]]
        self.assertIn(code, found, report)

    def test_a_cell_of_an_empty_or_invalid_frame_is_not_valid_anywhere(self):
        # the frame flags EMPTY (bit 0) and INVALID (bit 2) beat a result that carries its own valid flag: the row lands in /invalid_frames and says isValid = inRange = 0
        recs = [build_record(1000 + 2 * k, 62_000_000_000 + 40_000 * k, objects=[cell(1)], frame_flags=fl, drops=1 if k else 0)[0] for k, fl in enumerate((0, 1, 4, 0))]
        rep = P.convert(self.write(recs), self.d / "o.h5", RUN, expected_records=4)
        self.assertTrue(rep.ok and rep.converted, rep)
        with h5py.File(self.d / "o.h5") as f:
            self.assertEqual(list(f["valid_frames/metadata"][:]["index"]), [1000, 1006])
            self.assertEqual(list(f["valid_frames/metadata"][:]["isValid"]), [1, 1])
            iv = f["invalid_frames/metadata"][:]
            self.assertEqual(list(iv["index"]), [1002, 1004])
            self.assertEqual(list(iv["isValid"]), [0, 0])
            self.assertEqual(list(iv["inRange"]), [0, 0])
            self.assertAlmostEqual(iv["area"][0], 630.0)                       # the measurement itself is kept

    def test_truncated_stream(self):
        recs, _, _ = stream(4)
        p = self.write(recs)
        p.write_bytes(p.read_bytes()[:-100])
        self.assertQuarantined(P.convert(p, self.d / "o.h5", RUN), "TRUNCATED_STREAM")

    def test_expected_length(self):
        recs, _, _ = stream(4)
        self.assertQuarantined(P.convert(self.write(recs), self.d / "o.h5", RUN, expected_records=5), "LENGTH")

    def test_corrupt_crc_inside_a_record(self):
        recs, _, _ = stream(5)
        bad = bytearray(recs[2])
        bad[20] ^= 0x01                                     # inside the FRAME wire record
        recs[2] = bytes(bad)
        rep = P.convert(self.write(recs), self.d / "o.h5", RUN)
        self.assertQuarantined(rep, "BAD_CRC")
        self.assertEqual([b["index"] for b in rep.bad_records], [2])

    def test_wrong_run_id(self):
        recs, _, _ = stream(3, run_id=19)
        self.assertQuarantined(P.convert(self.write(recs), self.d / "o.h5", RUN), "RUN_ID")

    def test_frame_order_and_drops(self):
        a, _, _ = build_record(1000, 62_000_000_000)
        b, _, _ = build_record(1001, 62_000_040_000, drops=0)
        c, _, _ = build_record(1004, 62_000_080_000, drops=3)       # a gap of 2 but 3 drops counted: impossible
        self.assertQuarantined(P.convert(self.write([a, b, c]), self.d / "o.h5", RUN), "DROPS_BEFORE")
        shutil.rmtree(self.d / "quarantine")
        c, _, _ = build_record(1004, 62_000_080_000, drops=1)       # a gap of 2 with 1 drop: fine by default (frames can be lost before the ring or skipped by a filter)...
        rep = P.convert(self.write([a, b, c]), self.d / "o.h5", RUN)
        self.assertTrue(rep.ok and rep.converted)
        (self.d / "o.h5").unlink()
        exact = P.RunInfo(18, RUN.start_unix_ms, first_ticks=RUN.first_ticks, filter="all", exact_drops=True)   # ...but wrong when the run table proves every gap frame was a drain drop
        self.assertQuarantined(P.convert(self.write([a, b, c]), self.d / "o.h5", exact), "DROPS_BEFORE")
        shutil.rmtree(self.d / "quarantine")
        d, _, _ = build_record(999, 62_000_090_000)
        self.assertQuarantined(P.convert(self.write([a, b, d]), self.d / "o.h5", RUN), "FRAME_ORDER")

    def test_exact_drops_comes_from_the_run_table(self):
        self.assertTrue(P.load_run(RUNS, 18).exact_drops)             # seen 152672 == 156928 - 4257 + 1, filter ALL
        self.assertTrue(P.load_run(RUNS, 19).exact_drops)
        row = dict(json.loads(RUNS.read_text())[0], seen=1)
        self.assertFalse(P.RunInfo.from_row(row).exact_drops)

    def test_salvage_keeps_the_intact_records_and_says_so(self):
        recs, _, _ = stream(5)
        bad = bytearray(recs[1])
        bad[20] ^= 0x01
        recs[1] = bytes(bad)
        rep = P.convert(self.write(recs), self.d / "o.h5", P.RunInfo(18, RUN.start_unix_ms, first_ticks=62_000_000_000, filter=None), salvage=True)
        self.assertTrue(rep.converted and rep.salvaged and not rep.ok)
        self.assertTrue((self.d / "quarantine" / "in.bin.report.json").exists())
        with h5py.File(self.d / "o.h5") as f:
            self.assertEqual(f["experiment_info"].attrs["salvaged"], 1)
            cfg = json.loads(f["experiment_info"].attrs["config_json"])
            self.assertEqual(cfg["skipped_records"], [1])
            frames = set(f["valid_frames/ssd_meta"]["frame_id"][:]) | set(f["invalid_frames/ssd_meta"]["frame_id"][:])
            self.assertNotIn(1002, frames)
            self.assertEqual(len(frames), 4)

    def test_unknown_profile_leaves_numbers_nan(self):
        b, _, _ = build_record(1000, 62_000_000_000, objects=[cell(1)], profile=1, version=2)
        rep = P.convert(self.write([b]), self.d / "o.h5", RUN)
        self.assertTrue(rep.converted and rep.warnings, rep)
        with h5py.File(self.d / "o.h5") as f:
            row = (f["valid_frames/metadata"][:] if f["valid_frames/metadata"].shape[0] else f["invalid_frames/metadata"][:])[0]
            self.assertTrue(np.isnan(row["youngsModulus"]) and row["bboxWidth"] == 46)

    def test_vendored_decoder_must_be_unmodified(self):
        root = self.d / "dec"
        shutil.copytree(P.DECODER_ROOT, root)
        (root / "tools/pzrec/pzrec_decode.py").write_text("# tampered\n")
        old = P.DECODER_ROOT
        P.DECODER_ROOT = root
        try:
            with self.assertRaises(P.ConvertError):
                P.load_decoder()
        finally:
            P.DECODER_ROOT = old


class Fetch(Base):
    """fetch against a stand-in for Studio's route."""

    def serve(self, handler):
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.shutdown)
        return f"http://127.0.0.1:{srv.server_address[1]}"

    def handler(self, body, *, status=200, cut=None, extra=None, count=None):
        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                self.send_response(status)
                n = count if count is not None else len(body) // REC
                if status == 200:
                    self.send_header("Content-Length", str(n * REC))
                    self.send_header("X-Record-Count", str(n))
                    self.send_header("X-Run-Id", "18")
                    self.send_header("X-Record-Bytes", str(REC))
                    self.send_header("X-First-Record", "0")
                    for k, v in (extra or {}).items():
                        self.send_header(k, v)
                else:
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body if cut is None else body[:cut])
                self.wfile.flush()
                self.close_connection = True
        return H

    def test_download_and_convert(self):
        recs, _, _ = stream(4)
        hdr = {"X-Run-Start-Unix-Ms": "1791639269355", "X-Run-Tick-Hz": "100000000", "X-Run-First-Ticks": "62000000000", "X-Run-Filter": "0", "X-Run-Wall-Source": "1", "X-Run-Written": "4"}
        url = self.serve(self.handler(b"".join(recs), extra=hdr))
        self.assertEqual(P.main(["fetch", url, "--run", "18", "--out-dir", str(self.d)]), 0)
        self.assertTrue((self.d / "run-18-records.h5").exists() and (self.d / "run-18-records.bin").exists())
        with h5py.File(self.d / "run-18-records.h5") as f:                       # no --runs: the run table came from the headers
            cfg = json.loads(f["experiment_info"].attrs["config_json"])
            self.assertEqual((cfg["start_unix_ms"], cfg["first_ticks"], cfg["filter"], cfg["run_records"]), (1791639269355, 62_000_000_000, "all", 4))

    def test_fetch_without_runs_against_the_headers_of_691(self):
        """The header block of the #691 route (testdata/route-headers-691.txt, run 5, 8 records) in front of 8 real-layout records: fetch needs no --runs."""
        text = (Path(__file__).resolve().parent / "testdata" / "route-headers-691.txt").read_text()
        route_headers = dict(line.split(": ", 1) for line in text.splitlines() if line and not line.startswith("#"))
        recs = [build_record(1 + k, 100 + 40_000 * k, run_id=5, objects=[cell(1)] if k % 2 else [])[0] for k in range(8)]       # frame ids 1..8, no drops: exact_drops holds
        body = b"".join(recs)
        assert len(body) == int(route_headers["content-length"])

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                self.send_response(200)
                for k, v in route_headers.items():
                    if k not in ("connection", "date"):
                        self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True
        url = self.serve(H)
        self.assertEqual(P.main(["fetch", url, "--run", "5", "--out-dir", str(self.d)]), 0)
        with h5py.File(self.d / "run-5-records.h5") as f:
            cfg = json.loads(f["experiment_info"].attrs["config_json"])
            self.assertEqual((cfg["ssd_run_id"], cfg["tick_hz"], cfg["filter"], cfg["run_records"], cfg["exact_drops_check"], cfg["records_converted"]), (5, 100_000_000, "all", 8, True, 8))
        # a run asked for but another delivered: refused before the body is read
        self.assertEqual(P.main(["fetch", url, "--run", "6", "--out-dir", str(self.d / "other")]), 2)
        self.assertTrue((self.d / "other" / "quarantine" / "run-6-records.bin.report.json").exists())

    def test_short_body_is_quarantined(self):
        recs, _, _ = stream(4)
        body = b"".join(recs)
        url = self.serve(self.handler(body, cut=len(body) - 5000))
        dest, headers, problem = P.fetch(url, None, 18, self.d)
        self.assertIsNotNone(problem)
        self.assertTrue((self.d / "quarantine" / "run-18-records.bin.part").exists())
        self.assertFalse((self.d / "run-18-records.bin").exists())
        self.assertTrue(dest.exists() and "DOWNLOAD" in dest.read_text())

    def test_refusal_is_reported_with_the_servers_reason(self):
        url = self.serve(self.handler(b'{"error":"the SSD drain is ARMED","code":"BUSY"}', status=503))
        _, _, problem = P.fetch(url, "t", 18, self.d)
        self.assertEqual(problem, "HTTP_503")
        self.assertIn("ARMED", (self.d / "quarantine" / "run-18-records.bin.report.json").read_text())

    def test_bad_stream_from_a_complete_download_is_moved_to_quarantine(self):
        recs, _, _ = stream(4)
        bad = bytearray(recs[1])
        bad[20] ^= 1
        recs[1] = bytes(bad)
        hdr = {"X-Run-Start-Unix-Ms": "1791639269355", "X-Run-Tick-Hz": "100000000", "X-Run-First-Ticks": "62000000000", "X-Run-Filter": "0"}
        url = self.serve(self.handler(b"".join(recs), extra=hdr))
        self.assertEqual(P.main(["fetch", url, "--run", "18", "--out-dir", str(self.d)]), 1)
        self.assertTrue((self.d / "quarantine" / "run-18-records.bin").exists())
        self.assertFalse((self.d / "run-18-records.h5").exists())


class Contract(Base):
    def test_headers_of_the_export_route_691(self):
        text = (Path(__file__).resolve().parent / "testdata" / "route-headers-691.txt").read_text()
        h = dict(line.split(": ", 1) for line in text.splitlines() if line and not line.startswith("#"))
        self.assertIsNone(P.check_download_headers(h, 5, 0))
        run = P.RunInfo.from_headers(h, 5)
        self.assertEqual((run.run_id, run.start_unix_ms, run.tick_hz, run.first_ticks, run.filter, run.wall_source, run.client_tag, run.written, run.reason, run.exact_drops),
                         (5, 1791530000000, 100_000_000, 0, "all", 1, 1, 8, 0, True))

    def test_garbled_headers_are_refused_not_crashes(self):
        good = dict(line.split(": ", 1) for line in (Path(__file__).resolve().parent / "testdata" / "route-headers-691.txt").read_text().splitlines() if line and not line.startswith("#"))
        for key, value in (("x-run-start-unix-ms", "abc"), ("x-run-tick-hz", "0"), ("x-run-tick-hz", "-5")):
            with self.assertRaises(P.ConvertError, msg=(key, value)):
                P.RunInfo.from_headers({**good, key: value}, 5)
        with self.assertRaises(P.ConvertError):
            P.RunInfo.from_headers({k: v for k, v in good.items() if k != "x-run-id"}, 5)
        with self.assertRaises(P.ConvertError):
            P.RunInfo.from_headers(good, 6)                                  # run 5 downloaded for --run 6
        self.assertIn("X-Run-Id", P.check_download_headers(good, 6, 0))
        self.assertIn("X-Record-Bytes", P.check_download_headers({**good, "x-record-bytes": "1024"}, 5, 0))
        self.assertIn("X-First-Record", P.check_download_headers(good, 5, 3))
        self.assertIn("garbled", P.check_download_headers({**good, "x-record-count": "many"}, 5, 0))
        zero = P.check_download_headers({**good, "x-record-count": "0", "content-length": "0"}, 5, 0)
        self.assertIn("holds no records", zero)
        self.assertNotIn("does not match", zero)
        nolen = P.check_download_headers({k: v for k, v in good.items() if k != "content-length"}, 5, 0)
        self.assertIn("no Content-Length", nolen)
        self.assertNotIn("KeyError", nolen)
        self.assertIn("does not match", P.check_download_headers({**good, "content-length": "123"}, 5, 0))

    def test_default_expected_count(self):
        run = P.RunInfo(18, written=27119)
        self.assertEqual((P.default_expected(run, 0, 0), P.default_expected(run, 100, 0), P.default_expected(run, 100, 20), P.default_expected(P.RunInfo(18), 0, 0)),
                         (27119, 27019, 20, None))

    def test_a_short_stream_of_a_longer_run_is_refused_by_default(self):
        recs, _, _ = stream(4)
        run = P.RunInfo(18, RUN.start_unix_ms, first_ticks=RUN.first_ticks, filter="all", written=27119)
        self.write(recs)
        self.assertEqual(P.main(["convert", str(self.d / "in.bin"), "--out", str(self.d / "o.h5"), "--run", "18", "--start-unix-ms", "1791639269355",
                                 "--first-ticks", "62000000000", "--expect-records", "4"]), 0)
        runs = self.d / "runs.json"
        runs.write_text(json.dumps([{"run_id": 18, "start_unix_ms": 1791639269355, "tick_hz": 100000000, "first_ticks": 62000000000, "filter": 0, "written": 27119}]))
        (self.d / "o.h5").unlink()
        self.assertEqual(P.main(["convert", str(self.d / "in.bin"), "--out", str(self.d / "o.h5"), "--run", "18", "--runs", str(runs)]), 1)
        self.assertFalse((self.d / "o.h5").exists())
        # a window says so
        self.assertEqual(P.main(["convert", str(self.d / "in.bin"), "--out", str(self.d / "o.h5"), "--run", "18", "--runs", str(runs), "--from", "100", "--count", "4"]), 0)

    def test_a_run_table_entry_that_does_not_belong_to_the_stream_is_refused(self):
        recs, _, _ = stream(3)
        late = P.RunInfo(18, RUN.start_unix_ms, first_ticks=62_000_000_000 + 1_000_000, filter="all")        # first_ticks after the records
        rep = P.convert(self.write(recs), self.d / "o.h5", late)
        self.assertFalse(rep.converted)
        self.assertTrue(all(b["code"] == "TIME_BEFORE_RUN" for b in rep.bad_records) and rep.bad_records)


@unittest.skipUnless((REF / "run19_from0_count5.bin").exists(), "the S2 reference records are on the HDD only")
class Reference(Base):
    def test_reference_set_converts_and_matches_the_decoder(self):
        for name, run_id, count in (("run18_from0_count3.bin", 18, 3), ("run18_from1000_count20.bin", 18, 20), ("run19_from0_count5.bin", 19, 5)):
            out = self.d / (name + ".h5")
            run = P.load_run(RUNS, run_id)
            # a window of a run: the first_ticks in the run table belong to record 0 of the run, wall time stays right
            rep = P.convert(REF / name, out, run, expected_records=count, quarantine_dir=self.d / "q")
            self.assertTrue(rep.ok and rep.converted, (name, rep.problems, rep.bad_records[:3]))
            rows = list(DEC.rows((REF / name).open("rb"), {"start_unix_ms": run.start_unix_ms, "first_ticks": run.first_ticks, "tick_hz": run.tick_hz}, filter=run.filter))
            with h5py.File(out) as f:
                seen = {}
                for g in ("valid_frames", "invalid_frames"):
                    ssd = f[g]["ssd_meta"][:]
                    for k in range(len(ssd)):
                        seen.setdefault(int(ssd["record_index"][k]), []).append((g, k))
                self.assertEqual(sorted(seen), list(range(count)))
                for i, r in enumerate(rows):
                    g, k = seen[i][0]
                    self.assertTrue(np.array_equal(f[g]["images"][k], r["image"]), (name, i))
                    self.assertTrue(np.array_equal(f[g]["masks"][k], r["mask"] * 255), (name, i))
                    ssd = f[g]["ssd_meta"][k]
                    self.assertEqual((int(ssd["frame_id"]), int(ssd["wall_unix_ns"]), int(ssd["drops_before"])), (r["frame_id"], r["wall_ns"], r["drops_before"]))
                    self.assertEqual(len(seen[i]), max(r["results"]["count"], 1))
                    for (gg, kk), obj in zip(seen[i], r["results"]["objects"] or [None]):
                        if obj is None:
                            continue
                        m = f[gg]["metadata"][kk]
                        self.assertEqual((m["bboxX"], m["bboxWidth"]), (obj["bbox"][0], obj["bbox"][2]))
                        self.assertEqual(bool(m["isValid"]), obj["valid"], (name, i))
                        self.assertAlmostEqual(m["area"], obj["fields"]["hull_area"], places=4)
                        if obj["validity"] >> 10 & 1:
                            self.assertAlmostEqual(m["youngsModulus"], obj["fields"]["emodulus_kpa"], places=4)
                        else:
                            self.assertTrue(np.isnan(m["youngsModulus"]))              # Studio's rule: a word flagged invalid is NaN, never 0

    def test_a_flipped_status_byte_in_the_set_area_is_caught(self):
        data = bytearray((REF / "run19_from0_count5.bin").read_bytes())
        data[REC * 2 + 6] ^= 0xFF                              # the FRAME's length field
        p = self.write([bytes(data)], "bad.bin")
        rep = P.convert(p, self.d / "o.h5", P.load_run(RUNS, 19), expected_records=5, quarantine_dir=self.d / "q")
        self.assertFalse(rep.converted)
        self.assertEqual([b["index"] for b in rep.bad_records], [2])


if __name__ == "__main__":
    unittest.main()
