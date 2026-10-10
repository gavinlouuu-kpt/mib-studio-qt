#!/usr/bin/env python3
"""Reference decoder of the stored records `pzrec read` writes (layout: tools/pzrec/pzrec_records.md, the authoritative description).

  from pzrec_decode import iter_records, decode_record, rows
  for rec in iter_records(open("run18.bin", "rb"), filter="all"):
      rec.header            # dict: the FRAME record (run_id, frame_id, timestamp, epoch, flags, ..., drops_before)
      rec.image             # numpy uint8 (96, 512): the MONO8 block
      rec.mask              # numpy uint8 (96, 512), 0/1: the MASK1 block unpacked (None without a MASK1 block)
      rec.results           # list of dicts: RESULT envelope (bbox, flags, validity) + "payload" words + "fields" decoded by the profile JSON
      rec.problems          # list of strings; empty = every check passed (iter_records(strict=True) raises DecodeError instead)
      rec.frame_id, rec.epoch, rec.ticks
  for row in rows(open("run18.bin", "rb"), run_table_entry, filter="all"):      # HDF5-ready dicts: frame_id, epoch, ticks, wall_ns, drops_before, image, mask, results, ...

  pzrec_decode.py FILE [--rec-bytes 59392] [--filter all|valid|any] [--exact-drops] [--run-id N] [--runs RUNS.json] [--limit N] [--json] [--npz OUT.npz]
    --run-id N: the stream must be run N; --runs RUNS.json (`pzrec runs`) with --run-id takes the filter and the exactness of the drops_before check from the run's entry;
    --npz keeps every record in its slot (missing blocks are zeros with has_image / has_mask False).

Every wire record in the set is validated by src/imx426/mib_abi.py (magic, length, CRC32 over the record with the crc field zeroed, type, version); on top of that this module checks the
store record structure (set area padding, IMAGE records vs the fixed block offsets, in-record sequence, STORED flag, result count, frame id agreement, zero tail) and, across a stream,
one run_id, ascending frame ids and timestamps, and drops_before against the frame id gap. Nothing here needs the board; the input is a file or any binary stream."""
from __future__ import annotations

import json
import os
import struct
import sys
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "src"))
from imx426 import mib_abi  # noqa: E402

REC_BYTES = 59392            # 116 sectors: set area 4096 + MONO8 49152 + MASK1 6144
SET_BYTES = 4096
PROFILE_DIR = os.path.join(HERE, "..", "..", "abi", "profiles")


class DecodeError(ValueError):
    def __init__(self, code: str, detail: str = "", index: int | None = None):
        super().__init__("%s%s%s" % (code, ": " + detail if detail else "", " (record %d)" % index if index is not None else ""))
        self.code, self.index = code, index


@dataclass
class StoredRecord:
    index: int
    header: dict
    images: list[dict]
    results: list[dict]
    events: list[dict]
    image: object = None          # numpy uint8 (height, width)
    mask: object = None           # numpy uint8 (height, width), 0/1
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)       # soft observations, never failures (e.g. OVERFLOW_FIXED_LAYOUT)

    @property
    def valid(self) -> bool:
        return not self.problems

    @property
    def frame_id(self) -> int:
        return self.header["frame_id"]

    @property
    def epoch(self) -> int:
        return self.header["epoch"]

    @property
    def ticks(self) -> int:
        return self.header["timestamp"]


_ABI = None
_PROFILES: dict = {}


def abi() -> mib_abi.Abi:
    global _ABI
    if _ABI is None:
        _ABI = mib_abi.Abi()
    return _ABI


def profile(science_profile: int, version: int):
    """The payload description of (science_profile, profile_version) from abi/profiles/*.json, or None."""
    key = (science_profile, version)
    if key not in _PROFILES:
        found = None
        for name in sorted(os.listdir(PROFILE_DIR)):
            if name.endswith(".json"):
                p = json.load(open(os.path.join(PROFILE_DIR, name)))
                if p.get("science_profile") == science_profile and p.get("profile_version") == version and "payload" in p:
                    found = p
        _PROFILES[key] = found
    return _PROFILES[key]


def _field_value(word: int, f: dict, enums: dict):
    lo, hi = f["bits"]
    raw = (word >> lo) & ((1 << (hi - lo + 1)) - 1)
    fmt = f["format"]
    if fmt in ("u8", "u16", "u24", "u32", "u4"):
        return raw
    if fmt == "bool":
        return bool(raw)
    if fmt in ("q16_16", "q0_16"):
        return raw / 65536.0
    if fmt in ("q24_8", "q8_8", "q0_8"):
        return raw / 256.0
    if fmt.startswith("enum:"):
        names = {v: k for k, v in enums.get(fmt[5:], {}).items()}
        return names.get(raw, raw)
    return raw


def decode_payload(science_profile: int, version: int, words: list[int]) -> dict:
    """The named fields of one RESULT payload (profile JSON), {} for a profile this module has no description of. Fixed point formats are returned as floats."""
    p = profile(science_profile, version)
    if p is None:
        return {}
    return {f["name"]: _field_value(words[f["word"]], f, p.get("enums", {})) for f in p["payload"] if f["word"] < len(words)}


def _record_dict(r: mib_abi.Record, raw: bytes) -> dict:
    crc = struct.unpack_from("<I", raw, 12)[0]
    d = {"type": r.type, "version": r.version, "length": r.length, "sequence": r.sequence, "crc32": crc}
    d.update({k: v for k, v in r.fields.items() if k not in ("payload",)})
    return d


def decode_record(buf: bytes, index: int = 0, *, set_bytes: int = SET_BYTES, strict: bool = True, expected_run_id: int | None = None) -> StoredRecord:
    """Decode one stored record (len(buf) = rec_bytes). strict: any violated check raises DecodeError; otherwise it is listed in .problems (an undecodable wire record still raises).
    expected_run_id: the run the stream was read from; another FRAME.run_id is a violation. A frame with RESULTS_OVERFLOW has only its FRAME in the set (the PL keeps no IMAGE or RESULT
    records then); its blocks are still at the fixed offsets and are taken from there (a note, not a problem)."""
    a = abi()
    problems: list[str] = []
    notes: list[str] = []

    def bad(code: str, detail: str = ""):
        if strict:
            raise DecodeError(code, detail, index)
        problems.append("%s%s" % (code, ": " + detail if detail else ""))

    if len(buf) < set_bytes or len(buf) % 512:
        raise DecodeError("BAD_LENGTH", "%d bytes" % len(buf), index)
    off, recs = 0, []
    while off + a.header_bytes <= set_bytes and struct.unpack_from("<I", buf, off)[0] == a.record_magic:
        length = struct.unpack_from("<H", buf, off + 6)[0]
        if length < a.header_bytes or off + length > set_bytes:
            raise DecodeError("BAD_LENGTH", "wire record at %d has length %d" % (off, length), index)
        try:
            rec = mib_abi.decode_record(a, buf[off:off + length])
        except mib_abi.AbiError as e:
            raise DecodeError(e.code, "wire record at set offset %d: %s" % (off, e), index) from e
        recs.append((rec, buf[off:off + length]))
        off += length
    if not recs or recs[0][0].type != "FRAME":
        raise DecodeError("BAD_MAGIC", "the record set does not start with a FRAME record", index)
    if any(buf[off:set_bytes]):
        bad("SET_PADDING", "non-zero bytes after the record set (set ends at %d)" % off)
    frame_rec, frame_raw = recs[0]
    header = _record_dict(frame_rec, frame_raw)
    header["drops_before"] = header["reserved"][0]
    fl = header["flags"]
    if not fl & (1 << a.bits["frame_flags"]["STORED"]):
        bad("NOT_STORED", "FRAME flags 0x%x lack STORED" % fl)
    if expected_run_id is not None and header["run_id"] != expected_run_id:
        bad("RUN_ID", "FRAME.run_id is %d, the stream was expected to be run %d" % (header["run_id"], expected_run_id))
    images, results, events = [], [], []
    for n, (rec, raw) in enumerate(recs):
        if rec.sequence != n:
            bad("SEQUENCE", "wire record %d (%s) has sequence %d, the set is numbered from 0" % (n, rec.type, rec.sequence))
        if n and rec.type == "FRAME":
            bad("SECOND_FRAME", "a second FRAME record in the set")
        if n == 0:
            continue
        d = _record_dict(rec, raw)
        if "frame_id" in d and d["frame_id"] != header["frame_id"]:
            bad("FRAME_ID", "%s record carries frame id %d, FRAME has %d" % (rec.type, d["frame_id"], header["frame_id"]))
        if rec.type == "IMAGE":
            d["format"] = {v: k for k, v in a.enums["pixel_format"].items()}.get(d["pixel_format"], d["pixel_format"])
            d["flags_named"] = [k for k, b in a.bits["image_flags"].items() if d["flags"] & (1 << b)]
            images.append(d)
        elif rec.type == "RESULT":
            payload = rec.fields["payload"]
            d["payload"] = payload
            d["fields"] = decode_payload(d["science_profile"], d["profile_version"], payload)
            d["flags_named"] = [k for k, b in a.bits["result_flags"].items() if d["flags"] & (1 << b)]
            results.append(d)
        elif rec.type == "EVENT":
            events.append(d)
        else:
            bad("UNEXPECTED_TYPE", rec.type)
    if len(results) != header["result_count"] and not fl & (1 << a.bits["frame_flags"]["RESULTS_OVERFLOW"]):
        bad("RESULT_COUNT", "FRAME says %d results, the set holds %d" % (header["result_count"], len(results)))
    overflow = bool(fl & (1 << a.bits["frame_flags"]["RESULTS_OVERFLOW"]))
    if not images:
        if overflow and header["width"] and header["height"] and header["pixel_format"] == a.enums["pixel_format"]["MONO8"]:
            w, h = header["width"], header["height"]
            for fmt, pf, o, ln, stride in (("MONO8", a.enums["pixel_format"]["MONO8"], set_bytes, w * h, w), ("MASK1", a.enums["pixel_format"]["MASK1"], set_bytes + w * h, w * h // 8, w // 8)):
                if o + ln <= len(buf):
                    images.append({"type": "IMAGE", "format": fmt, "pixel_format": pf, "byte_offset": o, "byte_length": ln, "width": w, "height": h, "stride": stride, "flags": 0, "flags_named": [], "synthesized": True})
            notes.append("OVERFLOW_FIXED_LAYOUT: RESULTS_OVERFLOW, the set holds only the FRAME; MONO8/MASK1 taken from the fixed offsets %s" % [(d["format"], d["byte_offset"]) for d in images])
        else:
            bad("NO_IMAGE_RECORDS", "the set holds no IMAGE record")
    image = mask = None
    covered = []
    for d in images:
        o, n = d["byte_offset"], d["byte_length"]
        if o < set_bytes or o % 8 or o + n > len(buf):
            bad("IMAGE_RANGE", "%s block at %d length %d is outside the record" % (d["format"], o, n))
            continue
        if d["stride"] * d["height"] != n:
            bad("IMAGE_SIZE", "%s: stride %d x height %d != length %d" % (d["format"], d["stride"], d["height"], n))
            continue
        covered.append((o, o + n))
        block = buf[o:o + n]
        import numpy as np
        if d["format"] == "MONO8":
            if d["stride"] != d["width"]:
                bad("IMAGE_STRIDE", "MONO8 stride %d != width %d" % (d["stride"], d["width"]))
                continue
            image = np.frombuffer(block, dtype=np.uint8).reshape(d["height"], d["width"]).copy()
        elif d["format"] == "MASK1":
            if d["stride"] * 8 != d["width"]:
                bad("IMAGE_STRIDE", "MASK1 stride %d x 8 != width %d" % (d["stride"], d["width"]))
                continue
            if "NO_RESULT" in d["flags_named"]:
                notes.append("NO_MASK: the MASK1 block was not produced (IMAGE flag NO_RESULT); mask is None")
                continue                                                      # an all-zero block would look like an empty mask
            packed = np.frombuffer(block, dtype=np.uint8).reshape(d["height"], d["stride"])
            mask = np.unpackbits(packed, axis=1, bitorder="little")          # packed LSB-first: pixel x is bit x % 8 of byte x // 8
    covered.sort()
    if any(covered[i][1] > covered[i + 1][0] for i in range(len(covered) - 1)):
        bad("IMAGE_OVERLAP", str(covered))
    end = max([c[1] for c in covered] + [set_bytes])
    if any(buf[end:]):
        bad("TAIL", "non-zero bytes after the last image block (ends at %d)" % end)
    return StoredRecord(index, header, images, results, events, image, mask, problems, notes)


def frame_header(buf: bytes) -> dict:
    """The FRAME header of a stored record (its first 64 bytes), CRC checked; for index scans that do not need the blocks. DecodeError on a bad record."""
    a = abi()
    if len(buf) < 64:
        raise DecodeError("TRUNCATED", "%d bytes" % len(buf))
    try:
        rec = mib_abi.decode_record(a, bytes(buf[:64]))
    except mib_abi.AbiError as e:
        raise DecodeError(e.code, str(e)) from e
    if rec.type != "FRAME":
        raise DecodeError("BAD_MAGIC", "the record does not start with a FRAME record")
    h = _record_dict(rec, bytes(buf[:64]))
    h["drops_before"] = h["reserved"][0]
    return h


def check_gap(ph: dict, h: dict, flt: str | None, exact: bool = False) -> list[str]:
    """Stream checks between the FRAME headers of two consecutive stored records.
    drops_before counts the passing records the drain dropped; frame_id is the sensor's counter, so a gap can also hold frames lost before the ring (acquisition FRAMES_LOST) or skipped by
    a filter, which never reach drops_before. Therefore the default rule is `drops_before <= min(gap, 255)`. `exact` (only meaningful for filter "all") asserts that every gap frame was a
    drain drop (the run table has seen == the frame id span, see drops_exact()) and then requires `drops_before == min(gap, 255)`. flt None: no drops_before check."""
    out = []
    if h["run_id"] != ph["run_id"]:
        out.append("RUN_ID: %d after %d" % (h["run_id"], ph["run_id"]))
    if h["frame_id"] <= ph["frame_id"]:
        out.append("FRAME_ORDER: frame id %d after %d" % (h["frame_id"], ph["frame_id"]))
    if h["timestamp"] <= ph["timestamp"]:
        out.append("TIME_ORDER: timestamp %d after %d" % (h["timestamp"], ph["timestamp"]))
    gap = h["frame_id"] - ph["frame_id"] - 1
    d = h["drops_before"]
    if flt is not None and gap >= 0:
        if flt == "all" and exact and d != min(gap, 255):
            out.append("DROPS_BEFORE: %d for a frame id gap of %d (every gap frame was a drain drop: must be min(gap, 255))" % (d, gap))
        elif d > min(gap, 255):
            out.append("DROPS_BEFORE: %d for a frame id gap of %d (at most min(gap, 255))" % (d, gap))
    return out


def drops_exact(run: dict) -> bool:
    """True when the run table proves that every frame-id gap of a filter-ALL run was a drain drop: the run's filter is ALL (0) and `seen` equals the frame id span
    (last_frame_id - first_frame_id + 1), so no frame was lost before the ring. Pass it as exact_drops= to iter_records()."""
    return int(run.get("filter", -1)) == 0 and int(run["seen"]) == int(run["last_frame_id"]) - int(run["first_frame_id"]) + 1


def iter_records(stream, *, rec_bytes: int = REC_BYTES, set_bytes: int = SET_BYTES, filter: str | None = None, strict: bool = True, exact_drops: bool = False, expected_run_id: int | None = None):
    """Records of a `pzrec read` stream (a binary file object or bytes). filter = the run's filter (all|valid|any) enables the drops_before check (None skips it); exact_drops: see check_gap
    and drops_exact(); expected_run_id: the run the stream was read from (refuses a stream of another run)."""
    if isinstance(stream, (bytes, bytearray, memoryview)):
        import io
        stream = io.BytesIO(bytes(stream))
    prev, n = None, 0
    while True:
        buf = stream.read(rec_bytes)
        while buf and len(buf) < rec_bytes:
            more = stream.read(rec_bytes - len(buf))
            if not more:
                break
            buf += more
        if not buf:
            return
        if len(buf) < rec_bytes:
            raise DecodeError("TRUNCATED_STREAM", "%d bytes after %d whole records" % (len(buf), n), n)
        rec = decode_record(buf, n, set_bytes=set_bytes, strict=strict, expected_run_id=expected_run_id)
        if prev is not None:
            for p in check_gap(prev.header, rec.header, filter, exact_drops):
                if strict:
                    raise DecodeError(p.split(":")[0], p, n)
                rec.problems.append(p)
        yield rec
        prev, n = rec, n + 1


def wall_ns(ticks: int, run: dict) -> int:
    """Wall clock (ns since the Unix epoch) of a tick count: the run table's start_unix_ms plus the ticks since the run's first record. Accurate to the Start-to-first-frame delay (ms)."""
    return int(run["start_unix_ms"]) * 1_000_000 + (ticks - int(run["first_ticks"])) * 1_000_000_000 // int(run["tick_hz"])


def rows(stream, run: dict | None = None, *, rec_bytes: int = REC_BYTES, filter: str | None = None, strict: bool = True, exact_drops: bool | None = None):
    """HDF5-ready rows of a `pzrec read` stream, one dict per stored record, in stream order:
         frame_id, epoch, run_id, ticks, wall_ns (None without `run`), drops_before, image (uint8 [96, 512]), mask (uint8 [96, 512] 0/1 or None),
         results {"count", "truncated", "objects": [{"index", "bbox": (x, y, w, h), "target", "valid", "flags", "validity", "payload": [u32], "fields": {...profile fields...}}]},
         flags (FRAME flags names), problems (empty when strict).
       `run` = the run table entry (`pzrec runs`): needs start_unix_ms, first_ticks and tick_hz for wall_ns, and (with its run_id, seen, first/last_frame_id, filter) it makes the stream
       check stricter: the records must belong to that run, and drops_before is compared exactly when drops_exact(run) holds (override with exact_drops). filter = the run's filter name for
       the drops_before check. `mask` is None when the PL did not produce it (IMAGE flag NO_RESULT)."""
    a = abi()
    if exact_drops is None:
        exact_drops = bool(run) and "seen" in run and drops_exact(run)
    for rec in iter_records(stream, rec_bytes=rec_bytes, filter=filter, strict=strict, exact_drops=exact_drops, expected_run_id=int(run["run_id"]) if run and "run_id" in run else None):
        h = rec.header
        yield {
            "frame_id": h["frame_id"], "epoch": h["epoch"], "run_id": h["run_id"], "ticks": h["timestamp"],
            "wall_ns": wall_ns(h["timestamp"], run) if run else None,
            "drops_before": h["drops_before"], "image": rec.image, "mask": rec.mask, "flags": h["flags_named"],
            "results": {
                "count": len(rec.results), "truncated": bool(h["flags"] & ((1 << a.bits["frame_flags"]["RESULTS_TRUNCATED"]) | (1 << a.bits["frame_flags"]["RESULTS_OVERFLOW"]))),
                "objects": [{"index": r["result_index"], "bbox": (r["bbox_x"], r["bbox_y"], r["bbox_w"], r["bbox_h"]), "target": bool(r["flags"] & (1 << a.bits["result_flags"]["TARGET"])),
                            "valid": bool(r["flags"] & (1 << a.bits["result_flags"]["VALID"])), "flags": r["flags_named"], "validity": r["payload_validity"], "payload": r["payload"], "fields": r["fields"]}
                           for r in rec.results]},
            "problems": rec.problems, "notes": rec.notes,
        }


def consecutive_tail(frame_ids) -> int:
    """Index of the first record of the final run of consecutive frame ids (len(frame_ids) for an empty list). After a graceful Stop the end of a run's records is the newest
    ring_records - GUARD frames of the ring without a gap (pzrec_records.md, 'Frames in both the ring and the SSD')."""
    k = len(frame_ids)
    if not k:
        return 0
    k -= 1
    while k > 0 and frame_ids[k] - frame_ids[k - 1] == 1:
        k -= 1
    return k


def _jsonable(o):
    if isinstance(o, dict):
        return {k: _jsonable(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_jsonable(v) for v in o]
    return o


def main(argv=None):
    a = list(sys.argv[1:] if argv is None else argv)
    if not a or a[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    path = a[0]
    opt = lambda name, dflt=None: a[a.index(name) + 1] if name in a else dflt
    limit = int(opt("--limit", 0)) or None
    flt = opt("--filter")
    rb = int(opt("--rec-bytes", REC_BYTES))
    npz = opt("--npz")
    as_json = "--json" in a
    run_id = int(opt("--run-id")) if opt("--run-id") else None
    exact = "--exact-drops" in a
    if opt("--runs"):                                       # the run table (`pzrec runs` JSON): run id, filter and exactness come from the entry
        entries = {r["run_id"]: r for r in json.load(open(opt("--runs")))}
        if run_id is None or run_id not in entries:
            print("--runs needs --run-id N naming an entry of the file", file=sys.stderr)
            return 2
        e = entries[run_id]
        flt = flt or {0: "all", 1: "any", 2: "valid"}.get(e.get("filter"))
        exact = exact or ("seen" in e and drops_exact(e))
    n, bad_n = 0, 0
    images, masks, headers, has_img, has_msk = [], [], [], [], []
    try:
        import numpy as np
        with open(path, "rb") as f:
            for rec in iter_records(f, rec_bytes=rb, filter=flt, strict=False, exact_drops=exact, expected_run_id=run_id):
                n += 1
                bad_n += bool(rec.problems)
                h = rec.header
                if as_json:
                    print(json.dumps(_jsonable({"index": rec.index, "header": {k: v for k, v in h.items() if k != "reserved"}, "results": [{k: v for k, v in r.items()} for r in rec.results], "problems": rec.problems, "notes": rec.notes})))
                else:
                    print("%6d frame %d ts %d run %d drops_before %d results %d%s%s" % (rec.index, h["frame_id"], h["timestamp"], h["run_id"], h["drops_before"], len(rec.results),
                                                                                      "  PROBLEMS: " + "; ".join(rec.problems) if rec.problems else "", "  NOTES: " + "; ".join(n_.split(":")[0] for n_ in rec.notes) if rec.notes else ""))
                if npz:                                    # every record keeps its slot: a missing block is zeros plus has_image / has_mask False (the arrays stay index-aligned)
                    shape = (h["height"], h["width"])
                    images.append(rec.image if rec.image is not None else np.zeros(shape, np.uint8)); has_img.append(rec.image is not None)
                    masks.append(rec.mask if rec.mask is not None else np.zeros(shape, np.uint8)); has_msk.append(rec.mask is not None)
                    headers.append((h["frame_id"], h["timestamp"], h["run_id"], h["drops_before"], h["epoch"]))
                if limit and n >= limit:
                    break
    except DecodeError as e:
        print("DECODE ERROR:", e, file=sys.stderr)
        return 1
    if npz and images:
        np.savez_compressed(npz, images=np.stack(images), masks=np.stack(masks), has_image=np.array(has_img), has_mask=np.array(has_msk),
                            frame_id=np.array([x[0] for x in headers], dtype=np.uint64), timestamp=np.array([x[1] for x in headers], dtype=np.uint64),
                            run_id=np.array([x[2] for x in headers], dtype=np.uint64), drops_before=np.array([x[3] for x in headers], dtype=np.uint8), epoch=np.array([x[4] for x in headers], dtype=np.uint32))
    print("records=%d with_problems=%d" % (n, bad_n), file=sys.stderr)
    return 1 if bad_n else 0


if __name__ == "__main__":
    sys.exit(main())
