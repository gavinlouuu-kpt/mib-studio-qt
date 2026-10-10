#!/usr/bin/env python3
"""Stored SSD records (a `pzrec read` stream) -> a MIB Studio HDF5 experiment that YOFO Review opens (#667).

  pzrec_to_h5.py convert run18.bin --runs runs.json --run 18 --out run18.h5
  pzrec_to_h5.py fetch http://HOST:8427 --token T --run 18 --out-dir exports/ [--from F --count N]

The input is exactly what Studio's `GET /ssd/runs/{id}/records` streams: whole 59,392 B records, back to back (layout: third_party/pz7035-decoder/tools/pzrec/pzrec_records.md, decoded
by the vendored reference decoder). The output layout is the one Studio's own experiments have (src/backend/recording/Hdf5Service.cpp): /valid_frames and /invalid_frames with
{images, masks, metadata} and /experiment_info. Images are the raw MONO8 window, masks are 0/255 (the MASK1 bit, 1 = cell), one metadata row per RESULT (the image and mask are repeated on every
row of a frame, as in Studio); a frame without a valid cell, an empty frame or an invalid frame goes to /invalid_frames.

Nothing is trusted: the stream length (a whole number of records, and the expected count when it is known), every wire record's CRC and the record structure, one run id, ascending frame ids and
timestamps, and drops_before against the frame id gap are verified while converting. The HDF5 file appears (renamed from `.partial`) only when all of that passed; otherwise no output is
left, the input is reported and quarantined (`quarantine/<name>.report.json`; a downloaded file is moved there, a file the user gave is left where it is). `--salvage` converts the intact
records of a damaged stream instead, says so in /experiment_info (salvaged, skipped_records) and still writes the report.

Time: wall ns of a record = start_unix_ms x 1e6 + (timestamp - first_ticks) / tick_hz, from the run table entry (`pzrec runs`, or the X-* headers of the download). The MONO8/MASK1 blocks are
not covered by any CRC (pzrec_records.md); this tool checks their structure, not their pixels.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

CONVERTER_VERSION = "1"
HERE = Path(__file__).resolve().parent
DECODER_ROOT = HERE.parent.parent / "third_party" / "pz7035-decoder"
REC_BYTES = 59392
NAN = float("nan")

# Studio's ProcessedFrameMetadataRecord (Hdf5Service.cpp), by name; Review reads the fields it knows by name.
METADATA = np.dtype([
    ("index", "<u8"), ("timestampNs", "<u8"), ("deformability", "<f8"), ("area", "<f8"), ("areaRatio", "<f8"), ("ringRatio", "<f8"),
    ("laplacianVariance", "<f8"), ("isValid", "u1"), ("touchesBorder", "u1"), ("hasSingleInnerContour", "u1"), ("inRange", "u1"),
    ("innerContourCount", "<i4"), ("brightness_q1", "<f8"), ("brightness_q2", "<f8"), ("brightness_q3", "<f8"), ("brightness_q4", "<f8"),
    ("youngsModulus", "<f8"), ("isTargetGroup", "u1"), ("brightness_mean", "<f8"), ("brightness_variance", "<f8"), ("contourArea", "<f8"),
    ("pixelCount", "<i4"), ("blemishCount", "<i4"), ("degenerateContour", "u1"), ("objectId", "<i4"), ("objectCount", "<i4"),
    ("trackId", "<i4"), ("trackFirstFrame", "<u8"), ("trackLastFrame", "<u8"), ("trackObservationCount", "<i4"),
    ("bboxX", "<f8"), ("bboxY", "<f8"), ("bboxWidth", "<f8"), ("bboxHeight", "<f8"), ("centroidX", "<f8"), ("centroidY", "<f8"),
])

# One row per metadata row, same order: where the row came from on the SSD.
SSD_META = np.dtype([
    ("ssd_run_id", "<u8"), ("frame_id", "<u8"), ("record_index", "<u8"), ("ticks", "<u8"), ("wall_unix_ns", "<u8"),
    ("drops_before", "u1"), ("epoch", "<u4"), ("frame_flags", "<u4"), ("result_index", "<i2"), ("result_count", "<u2"),
])

FRAME_EMPTY, FRAME_INVALID = 1 << 0, 1 << 2
RESULT_VALID = 1 << 3
REASON_NONE, REASON_NO_CONTOUR = 0, 1
UNET_PROFILE, UNET_VERSION = 2, 3
FILTER_NAMES = {0: "all", 1: "any", 2: "valid"}


class ConvertError(Exception):
    pass


# ---- the vendored decoder ---------------------------------------------------------------------------------------------------------------------------------

def load_decoder(verify: bool = True):
    """Import third_party/pz7035-decoder/tools/pzrec/pzrec_decode.py after checking its files against PROVENANCE.json."""
    prov = json.loads((DECODER_ROOT / "PROVENANCE.json").read_text())
    if verify:
        for rel, want in prov["files"].items():
            got = hashlib.sha256((DECODER_ROOT / rel).read_bytes()).hexdigest()
            if got != want:
                raise ConvertError(f"the vendored decoder file {rel} differs from PROVENANCE.json (pz7035 {prov['commit'][:8]}): refusing to decode with a modified copy")
    sys.path.insert(0, str(DECODER_ROOT / "tools" / "pzrec"))
    import pzrec_decode  # noqa: E402
    return pzrec_decode, prov


# ---- the run table entry ----------------------------------------------------------------------------------------------------------------------------------

def _drops_exact(row: dict) -> bool:
    """pzrec_decode.drops_exact: filter ALL and seen == the frame id span, so every frame id gap was a drain drop."""
    try:
        return int(row.get("filter", -1)) == 0 and int(row["seen"]) == int(row["last_frame_id"]) - int(row["first_frame_id"]) + 1
    except (KeyError, TypeError, ValueError):
        return False


@dataclass
class RunInfo:
    run_id: int
    start_unix_ms: int = 0
    tick_hz: int = 100_000_000
    first_ticks: int = 0
    filter: str | None = None            # all | any | valid, enables the drops_before check
    wall_source: int | None = None       # 1 = the client's clock at Start (0: not synced, the date may be wrong)
    client_tag: int | None = None
    written: int | None = None          # the run table's record count (expected stream length when the whole run is read)
    reason: int | None = None
    exact_drops: bool = False            # filter ALL and seen == the frame id span: every frame id gap was a drain drop, so drops_before must equal min(gap, 255)
    extra: dict = field(default_factory=dict)

    @staticmethod
    def from_row(row: dict) -> "RunInfo":
        rid = row.get("run_id", row.get("id"))
        flt = row.get("filter")
        return RunInfo(
            run_id=int(rid), start_unix_ms=int(row.get("start_unix_ms") or 0), tick_hz=int(row.get("tick_hz") or 100_000_000),
            first_ticks=int(row.get("first_ticks") or 0), filter=FILTER_NAMES.get(flt) if isinstance(flt, int) else flt,
            wall_source=row.get("wall_source"), client_tag=row.get("client_tag"), written=row.get("written"), reason=row.get("reason"),
            exact_drops=_drops_exact(row))

    @staticmethod
    def from_headers(h: dict) -> "RunInfo | None":
        """The X-* headers of the download (a self-describing download); None when the server does not send the run table values."""
        g = {k.lower(): v for k, v in h.items()}
        if "x-start-unix-ms" not in g or "x-tick-hz" not in g:
            return None
        flt = g.get("x-filter")
        return RunInfo(
            run_id=int(g["x-run-id"]), start_unix_ms=int(g["x-start-unix-ms"]), tick_hz=int(g["x-tick-hz"]), first_ticks=int(g.get("x-first-ticks", 0)),
            filter=FILTER_NAMES.get(int(flt)) if flt and flt.isdigit() else flt, wall_source=int(g["x-wall-source"]) if "x-wall-source" in g else None,
            client_tag=int(g["x-client-tag"]) if "x-client-tag" in g else None, written=int(g["x-run-records"]) if "x-run-records" in g else None,
            reason=int(g["x-end-reason"]) if "x-end-reason" in g else None, exact_drops=g.get("x-drops-exact") == "1")


def load_run(runs_path: Path, run_id: int) -> RunInfo:
    data = json.loads(Path(runs_path).read_text())
    rows = data["runs"] if isinstance(data, dict) else data
    for row in rows:
        if int(row.get("run_id", row.get("id", -1))) == run_id:
            return RunInfo.from_row(row)
    raise ConvertError(f"run {run_id} is not in {runs_path}")


def wall_ns(run: RunInfo, ticks: int) -> int:
    return run.start_unix_ms * 1_000_000 + max(ticks - run.first_ticks, 0) * 1_000_000_000 // run.tick_hz


# ---- the report -------------------------------------------------------------------------------------------------------------------------------------------

@dataclass
class Report:
    input: str
    run_id: int
    rec_bytes: int = REC_BYTES
    input_bytes: int = 0
    records_in_stream: int = 0
    records_ok: int = 0
    expected_records: int | None = None
    problems: list = field(default_factory=list)        # stream-level
    bad_records: list = field(default_factory=list)      # [{index, code, detail}]
    warnings: list = field(default_factory=list)
    sha256: str = ""
    converted: bool = False
    salvaged: bool = False
    output: str | None = None
    seconds: float = 0.0

    @property
    def ok(self) -> bool:
        return not self.problems and not self.bad_records

    def to_json(self) -> dict:
        d = dict(self.__dict__)
        d["ok"] = self.ok
        d["bad_records"] = self.bad_records[:200]
        d["bad_record_count"] = len(self.bad_records)
        return d


# ---- metadata rows ----------------------------------------------------------------------------------------------------------------------------------------

def _finite(x: float, default: float = 0.0) -> float:
    return x if math.isfinite(x) else default


def decode_cell(res: dict) -> dict | None:
    """The U-Net cell fields of one RESULT, as Studio's decodeUnetCellsV2 (PzRecords.cpp): words flagged invalid in payload_validity are NaN (areas 0); centroids are signed q16.16."""
    if res["science_profile"] != UNET_PROFILE or res["profile_version"] != UNET_VERSION or len(res["payload"]) < 15:
        return None
    w, v = res["payload"], res["payload_validity"]
    ok = lambda i: bool(v >> i & 1)
    q16 = lambda i: w[i] / 65536.0 if ok(i) else NAN
    s32 = lambda i: ((w[i] ^ 0x80000000) - 0x80000000) / 65536.0 if ok(i) else NAN
    q8 = lambda i: w[i] / 256.0 if ok(i) else NAN
    return dict(
        object_id=w[0] & 0xFFFF, reason=w[0] >> 16 & 15, cut_off=bool(w[0] >> 20 & 1), target=bool(w[0] >> 24 & 1),
        contour_area=w[1] / 65536.0 if ok(1) else 0.0, hull_area=w[2] / 65536.0 if ok(2) else 0.0, area_ratio=q16(4),
        deformability=(w[5] & 0xFFFF) / 65536.0 if ok(5) else NAN, cell_count=w[5] >> 24, brightness_mean=q16(6), centroid_x=s32(7), centroid_y=s32(8),
        emodulus_kpa=q16(10), laplacian_variance=q8(11), pixel_count=w[13] & 0xFFFF, blemish_count=w[13] >> 16, brightness_variance=q8(14))


def metadata_row(frame_id: int, wall: int, res: dict | None, n_results: int) -> tuple[np.ndarray, bool]:
    """(row, this result is a valid cell). A frame without results gets one row that is not valid."""
    row = np.zeros((), METADATA)
    row["index"], row["timestampNs"], row["trackId"] = frame_id, wall, -1
    for k in ("deformability", "ringRatio", "laplacianVariance", "brightness_q1", "brightness_q2", "brightness_q3", "brightness_q4", "youngsModulus",
              "brightness_mean", "brightness_variance", "centroidX", "centroidY"):
        row[k] = NAN
    if res is None:
        return row, False
    row["bboxX"], row["bboxY"], row["bboxWidth"], row["bboxHeight"] = res["bbox_x"], res["bbox_y"], res["bbox_w"], res["bbox_h"]
    row["objectCount"] = n_results
    c = decode_cell(res)
    valid = bool(res["flags"] & RESULT_VALID)
    if c is None:                                     # another profile: the envelope is all there is, never invented numbers
        row["isValid"] = row["inRange"] = int(valid)
        return row, valid
    valid = c["reason"] == REASON_NONE
    row["objectId"], row["objectCount"] = c["object_id"], c["cell_count"]
    row["isValid"] = row["inRange"] = int(valid)
    row["touchesBorder"], row["isTargetGroup"], row["degenerateContour"] = int(c["cut_off"]), int(c["target"]), int(c["reason"] == REASON_NO_CONTOUR)
    row["area"], row["areaRatio"], row["deformability"] = c["hull_area"], _finite(c["area_ratio"]), _finite(c["deformability"])
    row["youngsModulus"], row["laplacianVariance"] = c["emodulus_kpa"], c["laplacian_variance"]
    row["brightness_mean"], row["brightness_variance"], row["contourArea"] = c["brightness_mean"], c["brightness_variance"], c["contour_area"]
    row["pixelCount"], row["blemishCount"] = c["pixel_count"], c["blemish_count"]
    row["centroidX"], row["centroidY"] = c["centroid_x"], c["centroid_y"]
    return row, valid


# ---- HDF5 writing -----------------------------------------------------------------------------------------------------------------------------------------

class Group:
    """One of /valid_frames, /invalid_frames: resizable datasets appended in batches."""

    BATCH = 128

    def __init__(self, h5, name: str, shape: tuple[int, int], gzip: bool):
        self.name, self.shape, self.n = name, shape, 0
        g = h5.require_group(name)
        kw = dict(compression="gzip", compression_opts=4, shuffle=True) if gzip else {}
        h, w = shape
        chunk = (16, h, w)
        self.images = g.create_dataset("images", shape=(0, h, w), maxshape=(None, h, w), dtype="u1", chunks=chunk, **kw)
        self.masks = g.create_dataset("masks", shape=(0, h, w), maxshape=(None, h, w), dtype="u1", chunks=chunk, **kw)
        self.meta = g.create_dataset("metadata", shape=(0,), maxshape=(None,), dtype=METADATA, chunks=(1024,))
        self.ssd = g.create_dataset("ssd_meta", shape=(0,), maxshape=(None,), dtype=SSD_META, chunks=(1024,))
        self._buf: list = []

    def add(self, image, mask, meta, ssd):
        self._buf.append((image, mask, meta, ssd))
        if len(self._buf) >= self.BATCH:
            self.flush()

    def flush(self):
        if not self._buf:
            return
        n = len(self._buf)
        for ds, i in ((self.images, 0), (self.masks, 1)):
            ds.resize(self.n + n, axis=0)
            ds[self.n:self.n + n] = np.stack([b[i] for b in self._buf])
        for ds, i, dt in ((self.meta, 2, METADATA), (self.ssd, 3, SSD_META)):
            ds.resize(self.n + n, axis=0)
            ds[self.n:self.n + n] = np.array([b[i] for b in self._buf], dtype=dt)
        self.n += n
        self._buf = []


# ---- scan + convert (one pass) ----------------------------------------------------------------------------------------------------------------------------

def convert(input_path: str | Path, out_path: str | Path, run: RunInfo, *, expected_records: int | None = None, rec_bytes: int = REC_BYTES,
            salvage: bool = False, gzip: bool = False, quarantine_dir: str | Path | None = None, verify_decoder: bool = True, source: dict | None = None) -> Report:
    """Verify and convert. Returns the report (report.converted tells whether the HDF5 exists). Never leaves a partial output behind."""
    import h5py
    dec, prov = load_decoder(verify_decoder)
    t0 = time.time()
    input_path, out_path = Path(input_path), Path(out_path)
    size = input_path.stat().st_size
    rep = Report(input=str(input_path), run_id=run.run_id, rec_bytes=rec_bytes, input_bytes=size, records_in_stream=size // rec_bytes, expected_records=expected_records)
    if size % rec_bytes:
        rep.problems.append({"code": "TRUNCATED_STREAM", "detail": f"{size} bytes = {size // rec_bytes} whole records and {size % rec_bytes} bytes of a cut record"})
    if size == 0:
        rep.problems.append({"code": "EMPTY", "detail": "no data"})
    if expected_records is not None and size != expected_records * rec_bytes:
        rep.problems.append({"code": "LENGTH", "detail": f"{size} bytes, expected {expected_records} records x {rec_bytes} = {expected_records * rec_bytes}"})
    partial = out_path.with_name(out_path.name + ".partial")
    sha = hashlib.sha256()
    groups = {}
    first_wall = last_wall = None
    prev_header, prev_index, skipped = None, -1, 0
    shape = None
    h5 = None
    drops_total = 0
    unknown_profile = 0
    notes_no_mask = 0
    try:
        if rep.ok or salvage:
            h5 = h5py.File(partial, "w")
        with open(input_path, "rb") as f:
            for i in range(rep.records_in_stream):
                buf = f.read(rec_bytes)
                sha.update(buf)
                try:
                    rec = dec.decode_record(buf, i, strict=False, expected_run_id=run.run_id)
                except dec.DecodeError as e:
                    rep.bad_records.append({"index": i, "code": e.code, "detail": str(e)})
                    continue
                problems = list(rec.problems)
                h = rec.header
                if prev_header is not None and prev_index == i - 1:
                    problems += dec.check_gap(prev_header, h, run.filter, run.exact_drops)
                if rec.image is None:
                    problems.append("NO_IMAGE: the record lacks a MONO8 block")
                no_mask = rec.mask is None                                  # NO_RESULT: the mask block was not produced (zeros)
                notes_no_mask += no_mask
                if problems:
                    rep.bad_records.append({"index": i, "code": problems[0].split(":")[0], "detail": "; ".join(problems), "frame_id": h["frame_id"]})
                    continue
                prev_header, prev_index = h, i
                rep.records_ok += 1
                if h5 is None:
                    continue                                           # a bad stream without --salvage: keep verifying, write nothing
                if not rep.ok and not salvage:
                    continue
                if shape is None:
                    shape = rec.image.shape
                elif shape != rec.image.shape:
                    rep.bad_records.append({"index": i, "code": "SHAPE", "detail": f"{rec.image.shape} after {shape}"})
                    continue
                wall = wall_ns(run, h["timestamp"])
                first_wall = wall if first_wall is None else first_wall
                last_wall = wall
                drops_total += h["drops_before"]
                results = rec.results
                mask255 = np.zeros(rec.image.shape, np.uint8) if no_mask else (rec.mask * np.uint8(255)).astype(np.uint8)
                rows, any_valid = [], False
                for r in (results or [None]):
                    row, valid = metadata_row(h["frame_id"], wall, r, len(results))
                    unknown_profile += r is not None and decode_cell(r) is None
                    any_valid |= valid
                    rows.append((row, r))
                is_valid_frame = any_valid and not h["flags"] & (FRAME_INVALID | FRAME_EMPTY)
                gname = "/valid_frames" if is_valid_frame else "/invalid_frames"
                if gname not in groups:
                    groups[gname] = Group(h5, gname, shape, gzip)
                for k, (row, r) in enumerate(rows):
                    groups[gname].add(rec.image, mask255, row, (run.run_id, h["frame_id"], i, h["timestamp"], wall, h["drops_before"], h["epoch"], h["flags"],
                                                                r["result_index"] if r else -1, len(results)))
            tail = f.read()
            sha.update(tail)
        rep.sha256 = sha.hexdigest()
        if unknown_profile:
            rep.warnings.append(f"{unknown_profile} results of a profile this converter has no payload decoder for: envelope only, numbers left NaN")
        if notes_no_mask:
            rep.warnings.append(f"{notes_no_mask} records have no mask block (NO_RESULT): written as an empty mask")
        if not rep.ok and not salvage:
            raise ConvertError("the input failed verification")
        if h5 is None or shape is None:
            raise ConvertError("nothing to write: no intact record")
        for g in groups.values():
            g.flush()
        for name in ("/valid_frames", "/invalid_frames"):
            if name not in groups:
                groups[name] = Group(h5, name, shape, gzip)
        rep.salvaged = not rep.ok
        info = h5.require_group("/experiment_info")
        nv, ni = groups["/valid_frames"].n, groups["/invalid_frames"].n
        info.attrs["start_time_ns"] = np.uint64(first_wall or 0)
        info.attrs["end_time_ns"] = np.uint64(last_wall or 0)
        info.attrs["total_valid_frames"] = np.uint64(nv)
        info.attrs["total_invalid_frames"] = np.uint64(ni)
        for k, val in (("roi_x", 0), ("roi_y", 0), ("roi_w", shape[1]), ("roi_h", shape[0])):
            info.attrs[k] = np.int32(val)
        info.attrs["processing_core_source"] = "pz7035-fpga"
        info.attrs["ssd_run_id"] = np.uint64(run.run_id)
        cfg = {
            "source": "pz7035-ssd-record", "converter": "pzrec_to_h5", "converter_version": CONVERTER_VERSION, "decoder_pz7035_commit": prov["commit"],
            "ssd_run_id": run.run_id, "start_unix_ms": run.start_unix_ms, "tick_hz": run.tick_hz, "first_ticks": run.first_ticks, "filter": run.filter,
            "wall_source": run.wall_source, "wall_clock_synced": bool(run.wall_source), "client_tag": run.client_tag, "end_reason": run.reason,
            "run_records": run.written, "exact_drops_check": run.exact_drops, "records_converted": rep.records_ok, "drops_before_total": drops_total, "input_sha256": rep.sha256,
            "input_bytes": size, "salvaged": rep.salvaged, "skipped_records": [b["index"] for b in rep.bad_records][:1000],
            "image": "MONO8 raw sensor window", "mask": "MASK1 (0/255)", "metadata_index": "frame_id", "timestampNs": "wall unix ns", **(source or {})}
        info.attrs["config_json"] = json.dumps(cfg)
        info.attrs["salvaged"] = np.uint8(rep.salvaged)
        h5.close()
        h5 = None
        os.replace(partial, out_path)
        rep.converted, rep.output = True, str(out_path)
    except ConvertError:
        pass
    finally:
        if h5 is not None:
            h5.close()
        if partial.exists():
            partial.unlink()
        rep.seconds = time.time() - t0
    if not rep.ok:
        write_report(rep, quarantine_dir or out_path.parent / "quarantine", input_path)
    return rep


def write_report(rep: Report, qdir: str | Path, input_path: Path, move: bool = False) -> Path:
    qdir = Path(qdir)
    qdir.mkdir(parents=True, exist_ok=True)
    if move:
        shutil.move(str(input_path), qdir / input_path.name)
        rep.input = str(qdir / input_path.name)
    path = qdir / (input_path.name + ".report.json")
    path.write_text(json.dumps(rep.to_json(), indent=2))
    return path


# ---- download ---------------------------------------------------------------------------------------------------------------------------------------------

def fetch(base_url: str, token: str | None, run_id: int, out_dir: str | Path, *, first: int = 0, count: int = 0, timeout: float = 120.0) -> tuple[Path, dict, str | None]:
    """Download run records to out_dir/run-N-records.bin (or `-from-F-count-N`). Verifies status, Content-Length, X-Record-Count and the bytes received.
    Returns (file, headers, problem): a problem leaves the file in out_dir/quarantine with a report instead of raising."""
    import http.client
    import urllib.error
    import urllib.parse
    import urllib.request
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    q = {k: v for k, v in (("token", token), ("from", first or None), ("count", count or None)) if v}
    url = f"{base_url.rstrip('/')}/ssd/runs/{run_id}/records" + (("?" + urllib.parse.urlencode(q)) if q else "")
    name = f"run-{run_id}-records" + (f"-from-{first}-count-{count}" if first or count else "") + ".bin"
    dest = out_dir / name
    part = out_dir / (name + ".part")
    headers: dict = {}
    try:
        with urllib.request.urlopen(urllib.request.Request(url), timeout=timeout) as resp:
            headers = dict(resp.headers.items())
            declared = int(headers.get("Content-Length", -1))
            records = int(headers.get("X-Record-Count", -1))
            with open(part, "wb") as out:
                got = 0
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
                    got += len(chunk)
            if declared != records * REC_BYTES or got != declared:
                return _quarantine_download(part, out_dir, name, run_id, headers, f"Content-Length {declared}, X-Record-Count {records} x {REC_BYTES}, received {got}"), headers, "LENGTH"
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")[:2000]
        return _quarantine_download(None, out_dir, name, run_id, dict(e.headers.items()), f"HTTP {e.code}: {body}"), dict(e.headers.items()), f"HTTP_{e.code}"
    except (http.client.IncompleteRead, http.client.HTTPException, OSError) as e:
        return _quarantine_download(part if part.exists() else None, out_dir, name, run_id, headers, f"the transfer failed: {e!r}"), headers, "TRANSFER"
    os.replace(part, dest)
    return dest, headers, None


def _quarantine_download(part: Path | None, out_dir: Path, name: str, run_id: int, headers: dict, why: str) -> Path:
    rep = Report(input=name, run_id=run_id, problems=[{"code": "DOWNLOAD", "detail": why}])
    rep.input_bytes = part.stat().st_size if part is not None and part.exists() else 0
    qdir = out_dir / "quarantine"
    qdir.mkdir(parents=True, exist_ok=True)
    if part is not None and part.exists():
        shutil.move(str(part), qdir / (name + ".part"))
    (qdir / (name + ".report.json")).write_text(json.dumps({**rep.to_json(), "headers": headers}, indent=2))
    return qdir / (name + ".report.json")


# ---- command line -----------------------------------------------------------------------------------------------------------------------------------------

def _run_from_args(a, headers: dict | None = None) -> RunInfo:
    run = None
    if a.runs:
        run = load_run(a.runs, a.run)
    elif headers:
        run = RunInfo.from_headers(headers)
    if run is None:
        if a.start_unix_ms is None:
            raise ConvertError("the run table entry is needed (--runs runs.json --run N, or --start-unix-ms/--tick-hz/--first-ticks)")
        run = RunInfo(run_id=a.run, start_unix_ms=a.start_unix_ms, tick_hz=a.tick_hz, first_ticks=a.first_ticks or 0)
    if a.filter:
        run.filter = a.filter
    return run


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--runs", help="pzrec runs JSON (or the Studio runs listing) holding the run table entry")
        p.add_argument("--run", type=int, required=True)
        p.add_argument("--filter", choices=["all", "any", "valid"], help="the run's filter (enables the drops_before check); default from the run table")
        p.add_argument("--start-unix-ms", type=int)
        p.add_argument("--tick-hz", type=int, default=100_000_000)
        p.add_argument("--first-ticks", type=int)
        p.add_argument("--expect-records", type=int, help="records the stream must hold (default: the run table's count when the whole run is read)")
        p.add_argument("--salvage", action="store_true", help="convert the intact records of a damaged stream (flagged in /experiment_info)")
        p.add_argument("--gzip", action="store_true")
        p.add_argument("--no-verify-decoder", action="store_true")

    c = sub.add_parser("convert")
    c.add_argument("input")
    c.add_argument("--out", required=True)
    c.add_argument("--quarantine-dir")
    common(c)
    f = sub.add_parser("fetch")
    f.add_argument("url", help="Studio base URL, e.g. http://192.168.137.2:8427")
    f.add_argument("--token")
    f.add_argument("--out-dir", required=True)
    f.add_argument("--from", dest="first", type=int, default=0)
    f.add_argument("--count", type=int, default=0)
    common(f)
    a = ap.parse_args(argv)

    try:
        if a.cmd == "convert":
            run = _run_from_args(a)
            expect = a.expect_records
            rep = convert(a.input, a.out, run, expected_records=expect, salvage=a.salvage, gzip=a.gzip, quarantine_dir=a.quarantine_dir, verify_decoder=not a.no_verify_decoder)
        else:
            dest, headers, problem = fetch(a.url, a.token, a.run, a.out_dir, first=a.first, count=a.count)
            if problem:
                print(f"download failed ({problem}); quarantined: {dest}", file=sys.stderr)
                return 2
            run = _run_from_args(a, headers)
            expect = a.expect_records if a.expect_records is not None else int(headers["X-Record-Count"])
            out = Path(a.out_dir) / (dest.stem + ".h5")
            rep = convert(dest, out, run, expected_records=expect, salvage=a.salvage, gzip=a.gzip, quarantine_dir=Path(a.out_dir) / "quarantine",
                          verify_decoder=not a.no_verify_decoder, source={"download_url": a.url})
            if not rep.ok:
                qdir = Path(a.out_dir) / "quarantine"
                if (qdir / (dest.name + ".report.json")).exists():
                    shutil.move(str(dest), qdir / dest.name)         # a downloaded file that failed verification is moved aside with its report
    except ConvertError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if rep.converted:
        print(f"wrote {rep.output}: {rep.records_ok} records{' (SALVAGED, %d bad)' % len(rep.bad_records) if rep.salvaged else ''} in {rep.seconds:.1f} s")
    if not rep.ok:
        print(f"input FAILED verification: {len(rep.problems)} stream problems, {len(rep.bad_records)} bad records; first: "
              f"{(rep.problems or rep.bad_records)[0]}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
