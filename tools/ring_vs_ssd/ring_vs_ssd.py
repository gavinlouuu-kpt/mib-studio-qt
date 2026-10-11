#!/usr/bin/env python3
"""ring_vs_ssd: compare the frames Studio's playback served from the frame ring with the records of the same SSD run (M1 gate 5, #693).

Host only; nothing here touches the board. Inputs are files saved by the slot:

  python3 tools/ring_vs_ssd/ring_vs_ssd.py --ring OUT/ring --status OUT/ring-status-1.json \\
      --status2 OUT/ring-status-2.json --ring2 OUT/ring2 [--status3 OUT/ring-status-3.json] \\
      --run DL/run19.bin --runs OUT/post.runs.json --run-id 19 --out OUT/ring-vs-ssd

  --ring DIR     seq-<n>.mibr: the unmodified `fetch_ring_frame {seq}` packets of EVERY sequence first_seq..last_seq of the first read
  --status F     the `fetch_ring_status` JSON of that read (ABI 37: carries `epoch`)
  --ring2 DIR    packets re-read after the export (a subset of the sequences is fine) and --status2 F, the status after the export; --status3 F a later one (idle)
  --run F        the export of the SSD run (whole 59,392 B records, `GET /ssd/runs/{id}/records`), --runs F the run table JSON, --run-id N
  --out DIR      ring-vs-ssd.json and summary.txt go here

Exit 0 only when every check passes; 1 on any FAIL; 2 on unusable input. The comparison key is (epoch, frame_id): the epoch of the ring comes from the status (a frozen ring holds
one ARM; the MIBR packet carries none), the epoch of the SSD side from each record's FRAME header.

Criteria (the plan on #693, from pzrec_records.md "Frames in both the ring and the SSD"):
  a  every ring frame inside the SSD run's final consecutive tail exists on the SSD and is equal: MONO8 bytes, MASK1 bits, RESULT words (the 19 words of a MIBR cell), ticks, frame flags
     (the SSD-only STORED bit aside); every SSD record of the tail inside the ring's range is in the ring
  b  ring frames older than the tail are equal to the SSD record when it has them, or absent (counted, bounded by --guard + --delta-max)
  c  the ring is complete and consecutive: a packet for every sequence, header sequence = file sequence, frame_id - seq constant
  d  the overlap is at least ring frames - guard - delta (default 256 + 32) and the ring's newest frame is within delta of the run's last_frame_id
  e  re-reads after the export: the status is identical and the re-read packets are byte-equal to the first read; a later status (--status3) too
  f  frames flagged cut / mask incomplete / invalid are listed by name and excluded from the byte comparison (--strict-flags makes them failures); a flag the two sides disagree on fails
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "pzrec_to_h5"))
import pzrec_to_h5 as P  # noqa: E402  (the vendored decoder is loaded and verified through it; nothing new is vendored here)

REC_BYTES = P.REC_BYTES
HEADER = struct.Struct("<4sHHQQQIIHHHH")          # MIBR v1: magic, version, header bytes, seq, frame id, ticks, tick Hz, frame flags, width, height, cells, flags
CELL_WORDS = 19
FLAG_MASK_PRESENT, FLAG_RESULTS_TRUNCATED, FLAG_FRAME_INVALID, FLAG_CUT, FLAG_MASK_INCOMPLETE = 1, 2, 4, 8, 16
GUARD_DEFAULT, DELTA_DEFAULT = 256, 32


class InputError(Exception):
    pass


@dataclass
class RingPacket:
    seq: int
    frame_id: int
    ticks: int
    tick_hz: int
    frame_flags: int
    width: int
    height: int
    flags: int
    gray: bytes
    mask: bytes
    cells: list          # list of 19-word tuples


def parse_packet(buf: bytes) -> RingPacket:
    """A `fetch_ring_frame` packet ('MIBR' version 1, 48-byte header); ValueError when it is not one."""
    if len(buf) < HEADER.size:
        raise ValueError(f"{len(buf)} bytes is shorter than the 48-byte header")
    magic, version, hbytes, seq, frame_id, ticks, tick_hz, fflags, w, h, ncells, flags = HEADER.unpack_from(buf)
    if magic != b"MIBR":
        raise ValueError("not a MIBR packet")
    if version != 1 or hbytes != HEADER.size:
        raise ValueError(f"MIBR version {version}, header {hbytes} bytes: this tool reads version 1 with a 48-byte header")
    if w == 0 or h == 0 or w % 8:
        raise ValueError(f"bad geometry {w}x{h}")
    gray_n, mask_n = w * h, w * h // 8
    want = hbytes + gray_n + mask_n + ncells * CELL_WORDS * 4
    if len(buf) != want:
        raise ValueError(f"{len(buf)} bytes, a {w}x{h} frame with {ncells} cells is {want}")
    gray = bytes(buf[hbytes:hbytes + gray_n])
    mask = bytes(buf[hbytes + gray_n:hbytes + gray_n + mask_n])
    off = hbytes + gray_n + mask_n
    cells = [struct.unpack_from("<19I", buf, off + i * CELL_WORDS * 4) for i in range(ncells)]
    return RingPacket(seq, frame_id, ticks, tick_hz, fflags, w, h, flags, gray, mask, cells)


class Report:
    def __init__(self):
        self.checks: list[dict] = []
        self.ok = True
        self.data: dict = {}

    def check(self, crit: str, name: str, ok: bool, detail: str = ""):
        self.checks.append({"criterion": crit, "check": name, "ok": bool(ok), "detail": detail})
        self.ok &= bool(ok)
        print(("PASS " if ok else "FAIL ") + f"[{crit}] {name}" + (f": {detail}" if detail else ""), flush=True)


def load_json(path: Path, what: str) -> dict:
    try:
        v = json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        raise InputError(f"{what} {path}: {e}") from e
    if not isinstance(v, dict):
        raise InputError(f"{what} {path} is not a JSON object")
    return v


def load_packets(directory: Path) -> dict[int, bytes]:
    """seq -> raw packet bytes of every seq-<n>.mibr in the directory."""
    out: dict[int, bytes] = {}
    for p in Path(directory).glob("seq-*.mibr"):
        try:
            out[int(p.stem.split("-", 1)[1])] = p.read_bytes()
        except (ValueError, OSError) as e:
            raise InputError(f"{p}: {e}") from e
    return out


# ---- the SSD side -----------------------------------------------------------------------------------------------------------------------------------------

def scan_headers(path: Path, dec) -> tuple[list[int], list[int], list[int]]:
    """frame ids, epochs, ticks of every record (the 64-byte FRAME header of each, CRC checked)."""
    ids, epochs, ticks = [], [], []
    size = Path(path).stat().st_size
    if size % REC_BYTES:
        raise InputError(f"{path}: {size} bytes is not a whole number of {REC_BYTES} B records")
    with open(path, "rb") as f:
        for i in range(size // REC_BYTES):
            f.seek(i * REC_BYTES)
            try:
                h = dec.frame_header(f.read(64))
            except dec.DecodeError as e:
                raise InputError(f"{path}: record {i}: {e}") from e
            ids.append(h["frame_id"])
            epochs.append(h["epoch"])
            ticks.append(h["timestamp"])
    return ids, epochs, ticks


def expected_cell_words(rec, abi_bits) -> list[tuple]:
    """The 19 words of a MIBR cell, rebuilt from a decoded stored record (pzrec_records.md; PzFrameRing.cpp buildRingPacket)."""
    n = len(rec.results)
    valid_bit = 1 << abi_bits["result_flags"]["VALID"]
    out = []
    for r in rec.results:
        payload = list(r["payload"])[:15]
        payload += [0] * (15 - len(payload))
        words = [w & 0xFFFFFFFF for w in payload]
        words.append((r["bbox_x"] & 0xFFFF) | (r["bbox_y"] & 0xFFFF) << 16)
        words.append((r["bbox_w"] & 0xFFFF) | (r["bbox_h"] & 0xFFFF) << 16)
        words.append((n & 0xFF) << 24 | (r["result_index"] & 0xFF) << 16 | (1 if r["flags"] & valid_bit else 0))
        words.append(r["payload_validity"] & 0xFFFFFFFF)
        out.append(tuple(words))
    return out


def ssd_flags(rec, abi_bits) -> dict:
    """What the MIBR flags word says about a stored record: mask usable, results truncated, frame invalid, cut, mask incomplete."""
    fb, ib = abi_bits["frame_flags"], abi_bits["image_flags"]
    fl = rec.header["flags"]
    mask_blocks = [d for d in rec.images if d.get("format") == "MASK1"]
    mono_blocks = [d for d in rec.images if d.get("format") == "MONO8"]
    mask_no_result = any(d["flags"] & (1 << ib["NO_RESULT"]) for d in mask_blocks)
    mask_incomplete = any(d["flags"] & (1 << ib["INCOMPLETE"]) for d in mask_blocks)
    return {
        "mask_present": bool(mask_blocks) and not mask_no_result and not mask_incomplete and rec.mask is not None,
        "truncated": bool(fl & (1 << fb["RESULTS_TRUNCATED"] | 1 << fb["RESULTS_OVERFLOW"])),
        "invalid": bool(fl & (1 << fb["INVALID"] | 1 << fb["PARTIAL"])),
        "cut": any(d["flags"] & (1 << ib["INCOMPLETE"]) for d in mono_blocks),
        "mask_incomplete": mask_incomplete,
        "overflow": bool(fl & (1 << fb["RESULTS_OVERFLOW"])),
    }


def packet_flags(pk: RingPacket) -> dict:
    return {"mask_present": bool(pk.flags & FLAG_MASK_PRESENT), "truncated": bool(pk.flags & FLAG_RESULTS_TRUNCATED), "invalid": bool(pk.flags & FLAG_FRAME_INVALID),
            "cut": bool(pk.flags & FLAG_CUT), "mask_incomplete": bool(pk.flags & FLAG_MASK_INCOMPLETE)}


def compare_frame(pk: RingPacket, rec, abi_bits) -> tuple[list[str], dict]:
    """(differences, flags of the ring frame). Empty differences: equal in every part."""
    diffs: list[str] = []
    a_flags, s_flags = packet_flags(pk), ssd_flags(rec, abi_bits)
    for k in ("mask_present", "truncated", "invalid", "cut", "mask_incomplete"):
        if a_flags[k] != s_flags[k]:
            diffs.append(f"flag {k}: ring {a_flags[k]}, SSD {s_flags[k]}")
    flagged = a_flags["cut"] or a_flags["mask_incomplete"] or a_flags["invalid"] or s_flags["cut"] or s_flags["mask_incomplete"] or s_flags["invalid"]
    if flagged:
        return diffs, a_flags                              # listed by name and excluded from the byte comparison, only the flag agreement counts
    stored = 1 << abi_bits["frame_flags"]["STORED"]
    if (pk.frame_flags & ~stored) != (rec.header["flags"] & ~stored):
        diffs.append(f"frame flags: ring 0x{pk.frame_flags:x}, SSD 0x{rec.header['flags']:x} (the STORED bit aside)")
    if pk.ticks != rec.header["timestamp"]:
        diffs.append(f"ticks: ring {pk.ticks}, SSD {rec.header['timestamp']}")
    if rec.image is None or (pk.height, pk.width) != tuple(rec.image.shape):
        diffs.append(f"geometry: ring {pk.width}x{pk.height}, SSD {None if rec.image is None else rec.image.shape}")
    else:
        if pk.gray != rec.image.tobytes():
            n = int(np.count_nonzero(np.frombuffer(pk.gray, np.uint8) != rec.image.reshape(-1)))
            diffs.append(f"MONO8: {n} bytes differ")
        if a_flags["mask_present"] and s_flags["mask_present"]:
            ssd_mask = np.packbits(rec.mask, axis=1, bitorder="little").tobytes()
            if pk.mask != ssd_mask:
                n = int(np.count_nonzero(np.unpackbits(np.frombuffer(pk.mask, np.uint8) ^ np.frombuffer(ssd_mask, np.uint8))))
                diffs.append(f"MASK1: {n} bits differ")
    if not s_flags["overflow"]:
        want = expected_cell_words(rec, abi_bits)
        if len(want) != len(pk.cells):
            diffs.append(f"RESULT count: ring {len(pk.cells)}, SSD {len(want)}")
        else:
            for i, (g, w) in enumerate(zip(pk.cells, want)):
                if tuple(g) != w:
                    bad = [k for k in range(CELL_WORDS) if g[k] != w[k]]
                    diffs.append(f"RESULT {i}: words {bad} differ")
    return diffs, a_flags


# ---- the checks -------------------------------------------------------------------------------------------------------------------------------------------

def run(args) -> int:
    rep = Report()
    dec, prov = P.load_decoder()
    abi_bits = dec.abi().bits
    status = load_json(Path(args.status), "ring status")
    if "epoch" not in status:
        raise InputError("the ring status has no `epoch` (bridge ABI 37): this build cannot be compared by (epoch, frame_id)")
    epoch = int(status["epoch"])
    first_seq, last_seq = int(status["first_seq"]), int(status["last_seq"])
    count = int(status.get("count", last_seq - first_seq + 1))
    capacity = int(status.get("capacity_frames", count))
    rep.check("c", "ring status: frozen, held, valid", bool(status.get("frozen")) and bool(status.get("run_frozen")) and not status.get("invalid")
              and not status.get("restore_needed"), json.dumps({k: status.get(k) for k in ("frozen", "run_frozen", "invalid", "stop_incomplete", "restore_needed", "epoch", "first_seq", "last_seq", "count")}))
    rep.check("c", "ring status: count equals last_seq - first_seq + 1", count == last_seq - first_seq + 1 and count > 0, f"{count} frames, sequences {first_seq}..{last_seq}")

    # -- the ring packets (c)
    raw = load_packets(Path(args.ring))
    missing = [s for s in range(first_seq, last_seq + 1) if s not in raw]
    rep.check("c", "a packet for every sequence of the readable range", not missing, f"{len(missing)} missing" + (f", first {missing[:5]}" if missing else ""))
    ring: dict[int, RingPacket] = {}
    bad: list[str] = []
    for s in range(first_seq, last_seq + 1):
        if s not in raw:
            continue
        try:
            pk = parse_packet(raw[s])
        except ValueError as e:
            bad.append(f"seq {s}: {e}")
            continue
        if pk.seq != s:
            bad.append(f"seq {s}: the packet says sequence {pk.seq}")
        ring[s] = pk
    rep.check("c", "every packet parses (MIBR v1) and carries its own sequence", not bad, "; ".join(bad[:5]))
    consts = {pk.frame_id - s for s, pk in ring.items()}
    rep.check("c", "frame_id - sequence is constant: the ring is consecutive", len(consts) <= 1 and bool(ring), f"{len(consts)} distinct offsets" + (f" {sorted(consts)[:5]}" if len(consts) > 1 else ""))
    ring_by_id = {pk.frame_id: pk for pk in ring.values()}
    if not ring:
        rep.data = {"fatal": "no ring packets"}
        return finish(rep, args)
    ring_lo_id, ring_hi_id = min(ring_by_id), max(ring_by_id)

    # -- the SSD run
    ids, epochs, ticks = scan_headers(Path(args.run), dec)
    if args.runs:
        data = json.loads(Path(args.runs).read_text())
        rows = data["runs"] if isinstance(data, dict) else data
        row = next((r for r in rows if int(r.get("run_id", r.get("id", -1))) == args.run_id), None)
        if row is None:
            raise InputError(f"run {args.run_id} is not in {args.runs}")
        rep.check("a", "the SSD stream holds the run table's record count", len(ids) == int(row["written"]), f"{len(ids)} records, written {row['written']}")
        rep.check("a", "the SSD stream ends at the run table's last_frame_id", ids[-1] == int(row["last_frame_id"]), f"{ids[-1]} vs {row['last_frame_id']}")
        run_last = int(row["last_frame_id"])
    else:
        run_last = ids[-1]
    rep.check("a", "frame ids ascend strictly on the SSD side", all(b > a for a, b in zip(ids, ids[1:])))
    tail_idx = dec.consecutive_tail(ids)
    tail_start_id = ids[tail_idx]
    tail_epochs = set(epochs[tail_idx:])
    rep.check("a", "the SSD tail has one epoch and it is the ring's", tail_epochs == {epoch}, f"SSD tail epochs {sorted(tail_epochs)}, ring status epoch {epoch}")
    key_to_index = {(e, i): n for n, (e, i) in enumerate(zip(epochs, ids))}

    # -- (a) and (b): compare
    in_range = [pk for pk in ring_by_id.values() if tail_start_id <= pk.frame_id <= run_last]
    older = [pk for pk in ring_by_id.values() if pk.frame_id < tail_start_id]
    newer = [pk for pk in ring_by_id.values() if pk.frame_id > run_last]
    tail_ids_in_ring_range = [i for i in ids[tail_idx:] if ring_lo_id <= i <= ring_hi_id]
    absent_on_ring = [i for i in tail_ids_in_ring_range if i not in ring_by_id]
    rep.check("a", "every SSD tail record inside the ring's range is in the ring", not absent_on_ring, f"{len(absent_on_ring)} missing, first {absent_on_ring[:5]}")
    absent_on_ssd = [pk.frame_id for pk in in_range if (epoch, pk.frame_id) not in key_to_index]
    rep.check("a", "every ring frame inside the SSD tail is on the SSD (key epoch, frame_id)", not absent_on_ssd, f"{len(absent_on_ssd)} missing, first {absent_on_ssd[:5]}")

    flagged: list[dict] = []
    mismatches: list[dict] = []
    compared = {"tail": 0, "older": 0}
    with open(args.run, "rb") as f:
        def load(frame_id):
            n = key_to_index[(epoch, frame_id)]
            f.seek(n * REC_BYTES)
            rec = dec.decode_record(f.read(REC_BYTES), n, strict=False, expected_run_id=args.run_id)
            return n, rec

        for group, frames in (("tail", sorted(in_range, key=lambda p: p.frame_id)), ("older", sorted(older, key=lambda p: p.frame_id))):
            for pk in frames:
                if (epoch, pk.frame_id) not in key_to_index:
                    continue
                n, rec = load(pk.frame_id)
                if rec.problems:
                    mismatches.append({"group": group, "frame_id": pk.frame_id, "seq": pk.seq, "diffs": ["SSD record problems: " + "; ".join(rec.problems)]})
                    continue
                diffs, fl = compare_frame(pk, rec, abi_bits)
                compared[group] += 1
                if fl["cut"] or fl["mask_incomplete"] or fl["invalid"]:
                    flagged.append({"group": group, "frame_id": pk.frame_id, "seq": pk.seq, "flags": {k: v for k, v in fl.items() if v and k != "mask_present"}})
                if diffs:
                    mismatches.append({"group": group, "frame_id": pk.frame_id, "seq": pk.seq, "diffs": diffs})
    mt = [m for m in mismatches if m["group"] == "tail"]
    mo = [m for m in mismatches if m["group"] == "older"]
    rep.check("a", "ring frames in the SSD tail are equal: MONO8, MASK1, RESULT words, ticks, flags", not mt,
              f"{compared['tail']} compared, {len(mt)} differ" + (f", first: frame {mt[0]['frame_id']}: {'; '.join(mt[0]['diffs'])}" if mt else ""))
    absent_older = [pk.frame_id for pk in older if (epoch, pk.frame_id) not in key_to_index]
    rep.check("b", "older ring frames that the SSD has are equal", not mo, f"{compared['older']} compared, {len(mo)} differ" + (f", first: frame {mo[0]['frame_id']}: {'; '.join(mo[0]['diffs'])}" if mo else ""))
    rep.check("b", "older ring frames absent from the SSD stay within guard + delta", len(absent_older) <= args.guard + args.delta_max,
              f"{len(absent_older)} absent (bound {args.guard} + {args.delta_max}), {len(older)} older than the tail")

    # -- (d) the overlap
    overlap = len(in_range)
    min_overlap = max(0, len(ring_by_id) - args.guard - args.delta_max)
    rep.check("d", "the overlap is at least ring frames - guard - delta", overlap >= min_overlap and overlap <= len(ring_by_id),
              f"overlap {overlap} of {len(ring_by_id)} ring frames (minimum {min_overlap}, SSD tail {len(ids) - tail_idx} records from frame {tail_start_id})")
    ahead = ring_hi_id - run_last
    rep.check("d", "the ring's newest frame is within delta of the run's last_frame_id", abs(ahead) <= args.delta_max, f"ring newest {ring_hi_id}, run last {run_last}: ring is {ahead:+d} frames ahead")
    rep.check("d", "ring frames beyond the run's end are only the few received between the two STOPs", len(newer) <= args.delta_max, f"{len(newer)} frames")

    # -- (e) re-reads after the export
    if args.no_reread:
        rep.check("e", "re-read checks skipped (--no-reread)", True)
    else:
        if not (args.status2 and args.ring2):
            rep.check("e", "the re-read status and packets were given (--status2, --ring2)", False, "give them, or --no-reread to skip knowingly")
        else:
            s2 = load_json(Path(args.status2), "ring status 2")
            rep.check("e", "the ring status after the export equals the first", s2 == status, "" if s2 == status else
                      "; ".join(f"{k}: {status.get(k)!r} -> {s2.get(k)!r}" for k in sorted(set(status) | set(s2)) if status.get(k) != s2.get(k))[:300])
            again = load_packets(Path(args.ring2))
            rep.check("e", "re-read packets exist", bool(again), f"{len(again)} packets")
            diff = [s for s, b in again.items() if raw.get(s) != b]
            rep.check("e", "every re-read packet is byte-equal to the first read", not diff, f"{len(again)} re-read, {len(diff)} differ" + (f", first sequences {sorted(diff)[:5]}" if diff else ""))
        if args.status3:
            s3 = load_json(Path(args.status3), "ring status 3")
            rep.check("e", "the later (idle) ring status equals the first", s3 == status, "" if s3 == status else
                      "; ".join(f"{k}: {status.get(k)!r} -> {s3.get(k)!r}" for k in sorted(set(status) | set(s3)) if status.get(k) != s3.get(k))[:300])

    # -- (f) flagged frames
    names = [f"frame {x['frame_id']} (seq {x['seq']}, {','.join(sorted(x['flags']))}, {x['group']})" for x in flagged]
    in_tail_flagged = [x for x in flagged if x["group"] == "tail"]
    rep.check("f", "frames flagged cut / mask incomplete / invalid are listed and excluded from the byte comparison", not (args.strict_flags and flagged),
              f"{len(flagged)} flagged" + (": " + "; ".join(names[:10]) if names else "") + (" (--strict-flags)" if args.strict_flags and flagged else ""))
    rep.data = {
        "run_id": args.run_id, "epoch": epoch, "ring": {"first_seq": first_seq, "last_seq": last_seq, "count": count, "capacity": capacity, "first_frame_id": ring_lo_id, "last_frame_id": ring_hi_id},
        "ssd": {"records": len(ids), "tail_start_frame_id": tail_start_id, "tail_records": len(ids) - tail_idx, "last_frame_id": run_last},
        "overlap": overlap, "min_overlap": min_overlap, "ring_ahead_of_run_end": ahead, "older_than_tail": len(older), "older_absent": len(absent_older), "newer_than_run_end": len(newer),
        "compared": compared, "flagged": flagged, "mismatches": mismatches[:50], "tail_flagged": len(in_tail_flagged),
        "decoder_pz7035_commit": prov["commit"],
    }
    return finish(rep, args)


def finish(rep: Report, args) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "ring-vs-ssd.json").write_text(json.dumps({"ok": rep.ok, "checks": rep.checks, **rep.data}, indent=2))
    lines = [("PASS" if c["ok"] else "FAIL") + f" [{c['criterion']}] {c['check']}" + (f": {c['detail']}" if c["detail"] else "") for c in rep.checks]
    summary = "\n".join(lines) + f"\nOVERALL {'PASS' if rep.ok else 'FAIL'}\n"
    (out / "summary.txt").write_text(summary)
    print("OVERALL " + ("PASS" if rep.ok else "FAIL"))
    return 0 if rep.ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Compare Studio's ring playback frames with the same SSD run by (epoch, frame_id).")
    ap.add_argument("--ring", required=True, help="directory of seq-<n>.mibr packets (every sequence of the first read)")
    ap.add_argument("--status", required=True, help="fetch_ring_status JSON of the first read (with epoch)")
    ap.add_argument("--status2", help="fetch_ring_status JSON after the export")
    ap.add_argument("--ring2", help="directory of seq-<n>.mibr packets re-read after the export")
    ap.add_argument("--status3", help="a later fetch_ring_status JSON (idle for 10 min)")
    ap.add_argument("--run", required=True, help="the exported records of the SSD run (run<N>.bin)")
    ap.add_argument("--runs", help="the run table JSON (post.runs.json): record count and last_frame_id are checked")
    ap.add_argument("--run-id", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--guard", type=int, default=GUARD_DEFAULT, help="the feeder's overwrite guard in frames (default 256)")
    ap.add_argument("--delta-max", type=int, default=DELTA_DEFAULT, help="frames the ring and the SSD may end apart (the two STOPs), default 32")
    ap.add_argument("--no-reread", action="store_true", help="skip criterion e knowingly")
    ap.add_argument("--strict-flags", action="store_true", help="a cut / mask-incomplete / invalid frame is a failure, not just listed")
    args = ap.parse_args(argv)
    try:
        return run(args)
    except (InputError, P.ConvertError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
