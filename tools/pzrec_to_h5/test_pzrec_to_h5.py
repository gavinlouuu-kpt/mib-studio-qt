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

DEC, PROV = P.load_decoder()
ABI = DEC.abi()
mib_abi = sys.modules["imx426.mib_abi"]
REC = P.REC_BYTES
REF = Path("/mnt/hdd/developer-data/IMX426/s2-records")
RUNS = Path(__file__).resolve().parent / "testdata" / "s2-runs.json"
FLAG_STORED = 1 << 8


def build_record(frame_id, ticks, *, run_id=18, drops=0, epoch=5, objects=(), seed=0, profile=2, version=3):
    """One stored record exactly as the layout doc says: set area (FRAME, IMAGE x2, RESULT x n), MONO8 at 4096, MASK1 at 53248."""
    rng = np.random.default_rng(seed + frame_id)
    img = rng.integers(0, 256, (96, 512), dtype=np.uint8)
    mask = (rng.random((96, 512)) < 0.05).astype(np.uint8)
    packed = np.packbits(mask, axis=1, bitorder="little")
    frame = mib_abi.encode_record(ABI, "FRAME", 0, dict(run_id=run_id, frame_id=frame_id, timestamp=ticks, epoch=epoch, flags=FLAG_STORED, result_count=len(objects),
                                                            result_limit=64, science_profile=profile, profile_version=version, width=512, height=96, pixel_format=1,
                                                            reserved=[drops, 0, 0]))
    frame = bytearray(frame)
    struct.pack_into("<I", frame, 12, mib_abi.crc32_record(bytes(frame)))
    recs = [bytes(frame)]
    recs.append(mib_abi.encode_record(ABI, "IMAGE", 1, dict(frame_id=frame_id, timestamp=ticks, byte_offset=4096, byte_length=49152, pixel_format=1, width=512, height=96, stride=512, epoch=epoch)))
    recs.append(mib_abi.encode_record(ABI, "IMAGE", 2, dict(frame_id=frame_id, timestamp=ticks, byte_offset=53248, byte_length=6144, pixel_format=4, width=512, height=96, stride=64, epoch=epoch)))
    for k, o in enumerate(objects):
        words = o["payload"]
        recs.append(mib_abi.encode_record(ABI, "RESULT", 3 + k, dict(frame_id=frame_id, result_index=k, flags=o.get("flags", 8), science_profile=profile, profile_version=version,
                                                                    bbox_x=o["bbox"][0], bbox_y=o["bbox"][1], bbox_w=o["bbox"][2], bbox_h=o["bbox"][3],
                                                                    payload_validity=o.get("validity", 0x7FFF), payload_words=len(words)), words))
    setarea = b"".join(recs)
    buf = bytearray(REC)
    buf[:len(setarea)] = setarea
    buf[4096:4096 + 49152] = img.tobytes()
    buf[53248:53248 + 6144] = packed.tobytes()
    return bytes(buf), img, mask


def cell(object_id=1, reason=0, *, hull_area=630.0, deform=0.19, emod=24.6, cx=185.1, cy=24.6, cells=1):
    q = lambda x: int(round(x * 65536))
    words = [object_id | reason << 16, q(600.0), q(hull_area), q(100.0), q(1.5), int(deform * 65536) | cells << 24, q(120.0), q(cx), q(cy), q(50.0), q(emod), int(300.5 * 256),
             0, 385 | 18 << 16, int(25.25 * 256)]
    return {"bbox": (161, 13, 46, 20), "payload": words, "flags": 8 if reason == 0 else 0}


def stream(n=6, **kw):
    out, imgs, masks = [], [], []
    for k in range(n):
        objs = [cell(1), cell(2, reason=3)] if k % 3 == 0 else ([cell(1)] if k % 3 == 1 else [])
        b, i, m = build_record(1000 + 2 * k, 62_000_000_000 + 40_000 * k, objects=objs, drops=1 if k else 0, **kw)
        out.append(b)
        imgs.append(i)
        masks.append(m)
    return out, imgs, masks


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
            # frames 0 (2 objects), 1 (1), 3 (2), 4 (1) have a valid cell; frames 2 and 5 have no results: invalid, one row each
            self.assertEqual((v["metadata"].shape[0], iv["metadata"].shape[0]), (6, 2))
            self.assertEqual(v["images"].shape, (6, 96, 512))
            self.assertTrue(np.array_equal(v["images"][0], imgs[0]) and np.array_equal(v["images"][1], imgs[0]))        # repeated on every row of the frame
            self.assertTrue(np.array_equal(v["masks"][0], masks[0] * 255) and v["masks"].dtype == np.uint8)
            self.assertTrue(np.array_equal(iv["images"][0], imgs[2]))
            m = v["metadata"][:]
            self.assertEqual(list(m["index"][:3]), [1000, 1000, 1002])
            self.assertEqual(list(m["isValid"][:3]), [1, 0, 1])                                                          # reason AREA is not a valid cell
            self.assertAlmostEqual(m["area"][0], 630.0)
            self.assertAlmostEqual(m["youngsModulus"][0], 24.6, places=3)
            self.assertAlmostEqual(m["centroidX"][0], 185.1, places=3)
            self.assertEqual((m["pixelCount"][0], m["blemishCount"][0], m["objectCount"][0]), (385, 18, 1))
            self.assertEqual(m["bboxX"][0], 161)
            ssd = v["ssd_meta"][:]
            self.assertEqual(list(ssd["ssd_run_id"][:2]), [18, 18])
            self.assertEqual(list(ssd["frame_id"][:3]), [1000, 1000, 1002])
            self.assertEqual(list(ssd["drops_before"][:3]), [0, 0, 1])
            self.assertEqual(list(ssd["record_index"][:3]), [0, 0, 1])
            # wall time = start_unix_ms x 1e6 + (ticks - first_ticks) / tick_hz
            self.assertEqual(int(m["timestampNs"][0]), 1791639269355 * 1_000_000)
            self.assertEqual(int(m["timestampNs"][2]), 1791639269355 * 1_000_000 + 40_000 * 10)
            info = f["experiment_info"].attrs
            self.assertEqual((info["total_valid_frames"], info["total_invalid_frames"], info["roi_w"], info["roi_h"]), (6, 2, 512, 96))
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
        hdr = {"X-Start-Unix-Ms": "1791639269355", "X-Tick-Hz": "100000000", "X-First-Ticks": "62000000000", "X-Filter": "0", "X-Wall-Source": "1"}
        url = self.serve(self.handler(b"".join(recs), extra=hdr))
        self.assertEqual(P.main(["fetch", url, "--run", "18", "--out-dir", str(self.d)]), 0)
        self.assertTrue((self.d / "run-18-records.h5").exists() and (self.d / "run-18-records.bin").exists())

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
        hdr = {"X-Start-Unix-Ms": "1791639269355", "X-Tick-Hz": "100000000", "X-First-Ticks": "62000000000", "X-Filter": "0"}
        url = self.serve(self.handler(b"".join(recs), extra=hdr))
        self.assertEqual(P.main(["fetch", url, "--run", "18", "--out-dir", str(self.d)]), 1)
        self.assertTrue((self.d / "quarantine" / "run-18-records.bin").exists())
        self.assertFalse((self.d / "run-18-records.h5").exists())


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
