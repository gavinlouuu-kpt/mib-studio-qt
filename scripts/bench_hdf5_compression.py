#!/usr/bin/env python3
"""Measure lossless compression headroom for HDF5 recordings (ADR 0006, PR 0).

Usage:
    python3 scripts/bench_hdf5_compression.py [--frames-dir DIR | --h5 FILE [--dataset PATH]]
                                              [--levels 1,4,6] [--threads 1,2,4]
                                              [--chunk-frames 10,50] [--write-dir DIR]
                                              [--spikes] [--json OUT.json]

Input is real frames. Use one of:
  * --frames-dir: a directory of 8-bit TIFFs (default: $MIB_MOCK_CAMERA_DIR, else the
    provisioned build/vendor/assets/datasets/512x96stream-mock-frames).
  * --h5: an existing recording/experiment file (--dataset defaults to
    /recorded_frames/images, else /valid_frames/images).

For every (level, threads, chunk frames) the script reports:
  * compression ratio;
  * zlib compress MB/s on a thread pool, the way the planned writer compresses
    chunks (zlib releases the GIL, so threads scale like the C++ pool);
  * the fps that rate sustains at the input frame size;
  * inflate time per chunk (review random-access cost).
With --write-dir it also writes a deflate dataset with H5Dwrite_chunk
(h5py write_direct_chunk) to that directory, so disk throughput is included.
--spikes re-runs spikes S1 (tail-rewrite file growth) and S2 (mixed
compressed/raw chunks read back byte-identical) on this machine's h5py/HDF5.
--seconds N keeps each (level, threads, chunk) configuration compressing for N
seconds: the sustained load run_compression_headroom_e2e.py puts beside a live run.
--emit-mixed FILE writes a small mixed compressed/raw file (every third chunk
raw) at /recorded_frames/images, for opening in HDFView or MATLAB.

The process lowers its own priority (the planned pool runs below normal). To
measure headroom under real contention, run it on the rig while the app runs a
live experiment. The procedure is in docs/howto/hdf5-compression-measurement.md.

Needs numpy and h5py, plus tifffile, opencv-python or Pillow for TIFF input.
"""

import argparse
import json
import os
import platform
import sys
import tempfile
import time
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    import numpy as np
    import h5py
except ImportError as exc:  # pragma: no cover - environment guard
    sys.exit(f"bench_hdf5_compression: needs numpy and h5py ({exc}); pip install numpy h5py")

REPO = Path(__file__).resolve().parent.parent
DEFAULT_FRAMES = REPO / "build" / "vendor" / "assets" / "datasets" / "512x96stream-mock-frames"


def lower_priority():
    try:
        if os.name == "nt":
            import ctypes
            below_normal = 0x00004000
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), below_normal)
        else:
            os.nice(5)
        return True
    except Exception:
        return False


def read_tiff(path):
    try:
        import tifffile
        return tifffile.imread(str(path))
    except ImportError:
        pass
    try:
        import cv2
        return cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    except ImportError:
        pass
    try:
        from PIL import Image
        return np.asarray(Image.open(path))
    except ImportError:
        sys.exit("bench_hdf5_compression: install tifffile, opencv-python or Pillow to read TIFFs")


def load_frames(args):
    if args.h5:
        with h5py.File(args.h5, "r") as f:
            ds_path = args.dataset or next(
                (p for p in ("/recorded_frames/images", "/valid_frames/images") if p in f), None)
            if ds_path is None:
                sys.exit(f"{args.h5}: no image dataset; pass --dataset")
            ds = f[ds_path]
            n = min(len(ds), args.max_frames)
            frames = ds[:n]
        source = f"{args.h5}:{ds_path}"
    else:
        folder = Path(args.frames_dir or os.environ.get("MIB_MOCK_CAMERA_DIR") or DEFAULT_FRAMES)
        files = sorted(p for p in folder.glob("*") if p.suffix.lower() in (".tif", ".tiff"))[: args.max_frames]
        if not files:
            sys.exit(f"no TIFF frames in {folder}; run: python3 scripts/provision-assets.py "
                     "--asset 512x96stream-mock-frames --count 1000")
        frames = np.stack([read_tiff(p) for p in files])
        source = str(folder)
    if frames.ndim == 4:  # (N, H, W, C) -> keep first channel, the recorder stores mono
        frames = frames[..., 0]
    if frames.dtype != np.uint8:
        sys.exit(f"expected 8-bit frames, got {frames.dtype}")
    return np.ascontiguousarray(frames), source


def chunks_of(frames, c):
    n = (len(frames) // c) * c
    return [frames[i:i + c].tobytes() for i in range(0, n, c)]


def bench_codec(chunks, level, threads, repeat, seconds=0.0):
    t0 = time.perf_counter()
    with ThreadPoolExecutor(threads) as ex:
        if seconds > 0:  # sustained load: keep the pool busy for `seconds`
            raw = stored = 0
            out = []
            while time.perf_counter() - t0 < seconds:
                out = list(ex.map(lambda b: zlib.compress(b, level), chunks))
                raw += sum(map(len, chunks))
                stored += sum(map(len, out))
        else:
            raw = sum(map(len, chunks)) * repeat
            out = list(ex.map(lambda b: zlib.compress(b, level), chunks * repeat))
            stored = sum(map(len, out))
    dt = time.perf_counter() - t0
    t1 = time.perf_counter()
    for z in out[: len(chunks)]:
        zlib.decompress(z)
    inflate_ms = (time.perf_counter() - t1) / len(chunks) * 1e3
    return raw / 1e6 / dt, raw / stored, inflate_ms


def bench_write(chunks, c, shape, level, threads, write_dir):
    h, w = shape
    Path(write_dir).mkdir(parents=True, exist_ok=True)
    path = Path(write_dir) / f"bench_{os.getpid()}.h5"
    t0 = time.perf_counter()
    with ThreadPoolExecutor(threads) as ex, h5py.File(path, "w") as f:
        ds = f.create_dataset("x", shape=(len(chunks) * c, h, w), maxshape=(None, h, w),
                              chunks=(c, h, w), dtype="u1", compression="gzip", compression_opts=level)
        for i, z in enumerate(ex.map(lambda b: zlib.compress(b, level), chunks)):
            ds.id.write_direct_chunk((i * c, 0, 0), z, filter_mask=0)
        f.flush()
    dt = time.perf_counter() - t0
    size = path.stat().st_size
    path.unlink()
    return sum(map(len, chunks)) / 1e6 / dt, size


def run_spikes(frames, c):
    n = (len(frames) // c) * c
    frames = frames[:n]
    h, w = frames.shape[1:]
    chunk_bytes = c * h * w
    results = {}
    with tempfile.TemporaryDirectory(prefix="mib_bench_") as tmp:
        def build(name, tail, raw_every=0):
            p = Path(tmp) / name
            with h5py.File(p, "w") as f:
                ds = f.create_dataset("x", shape=(0, h, w), maxshape=(None, h, w), chunks=(c, h, w),
                                      dtype="u1", compression="gzip", compression_opts=1)
                for k in range(n // c):
                    full = frames[k * c:(k + 1) * c]
                    if tail:
                        for j in range(1, c):
                            buf = np.zeros((c, h, w), np.uint8)
                            buf[:j] = full[:j]
                            ds.resize(k * c + j, axis=0)
                            ds.id.write_direct_chunk((k * c, 0, 0), buf.tobytes(), filter_mask=1)
                    ds.resize((k + 1) * c, axis=0)
                    if raw_every and k % raw_every == 0:
                        ds.id.write_direct_chunk((k * c, 0, 0), full.tobytes(), filter_mask=1)
                    else:
                        ds.id.write_direct_chunk((k * c, 0, 0), zlib.compress(full.tobytes(), 1), filter_mask=0)
            with h5py.File(p, "r") as f:
                identical = bool(np.array_equal(f["x"][:], frames))
            return p.stat().st_size, identical
        direct, ok_d = build("direct.h5", False)
        tail, ok_t = build("tail.h5", True)
        mixed, ok_m = build("mixed.h5", False, raw_every=3)
    results["S1"] = {"direct_bytes": direct, "tail_bytes": tail,
                     "growth_pct": round((tail / direct - 1) * 100, 2),
                     "pass": ok_d and ok_t and tail < direct * 1.05, "chunk_bytes": chunk_bytes}
    results["S2"] = {"mixed_bytes": mixed, "byte_identical": ok_m, "pass": ok_m}
    return results


def emit_mixed(frames, c, path):
    n = (len(frames) // c) * c
    h, w = frames.shape[1:]
    with h5py.File(path, "w") as f:
        ds = f.create_dataset("recorded_frames/images", shape=(n, h, w), maxshape=(None, h, w),
                              chunks=(c, h, w), dtype="u1", compression="gzip", compression_opts=1)
        for k in range(n // c):
            block = frames[k * c:(k + 1) * c].tobytes()
            if k % 3 == 0:
                ds.id.write_direct_chunk((k * c, 0, 0), block, filter_mask=1)
            else:
                ds.id.write_direct_chunk((k * c, 0, 0), zlib.compress(block, 1), filter_mask=0)
        ds.attrs["bench_note"] = "every third chunk stored raw (filter mask 0x1); frames must match the source"
    print(f"mixed file written to {path} ({n} frames, chunk {c})")


def parse_ints(text):
    return [int(x) for x in text.split(",") if x]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames-dir")
    ap.add_argument("--h5")
    ap.add_argument("--dataset")
    ap.add_argument("--max-frames", type=int, default=1000)
    ap.add_argument("--levels", default="1,4,6")
    ap.add_argument("--threads", default=",".join(str(t) for t in sorted({1, 2, 4, os.cpu_count() or 1})))
    ap.add_argument("--chunk-frames", default="10,50")
    ap.add_argument("--repeat", type=int, default=4, help="passes over the frames per measurement")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="sustained mode: compress continuously for this long per configuration "
                         "(used as a load generator alongside a live run)")
    ap.add_argument("--target-fps", default="1000,5000")
    ap.add_argument("--write-dir", help="also time H5Dwrite_chunk writes to this directory (the recording drive)")
    ap.add_argument("--spikes", action="store_true", help="re-run spikes S1/S2 on this h5py/HDF5")
    ap.add_argument("--emit-mixed", metavar="FILE", help="write a mixed compressed/raw file for reader checks")
    ap.add_argument("--no-nice", action="store_true", help="keep normal priority")
    ap.add_argument("--json", help="write the full report here")
    args = ap.parse_args()

    niced = False if args.no_nice else lower_priority()
    frames, source = load_frames(args)
    frame_bytes = frames.shape[1] * frames.shape[2]
    targets = parse_ints(args.target_fps)
    report = {
        "host": {"platform": platform.platform(), "processor": platform.processor(),
                 "cpu_count": os.cpu_count(), "python": platform.python_version(),
                 "h5py": h5py.version.version, "hdf5": h5py.version.hdf5_version,
                 "zlib": zlib.ZLIB_RUNTIME_VERSION, "below_normal_priority": niced},
        "input": {"source": source, "frames": int(len(frames)), "height": int(frames.shape[1]),
                  "width": int(frames.shape[2]), "mean": round(float(frames.mean()), 1),
                  "std": round(float(frames.std()), 1)},
        "demand_mb_s": {str(t): round(t * frame_bytes / 1e6, 1) for t in targets},
        "codec": [], "write": [],
    }
    print(f"input {source}: {len(frames)} x {frames.shape[1]}x{frames.shape[2]} u8, "
          f"mean {report['input']['mean']} std {report['input']['std']}")
    print(f"HDF5 {report['host']['hdf5']} (h5py {report['host']['h5py']}), zlib {report['host']['zlib']}, "
          f"{os.cpu_count()} CPUs, below-normal priority: {niced}")
    print("demand: " + ", ".join(f"{t} fps = {v} MB/s" for t, v in report["demand_mb_s"].items()))
    if args.write_dir:
        Path(args.write_dir).mkdir(parents=True, exist_ok=True)
        # FILE_ATTRIBUTE_COMPRESSED: Windows compresses every write synchronously, so the
        # write-to-disk numbers would measure NTFS compression, not the drive.
        report["write_dir_ntfs_compressed"] = bool(getattr(os.stat(args.write_dir), "st_file_attributes", 0) & 0x800)
        if report["write_dir_ntfs_compressed"]:
            print(f"WARNING: {args.write_dir} is NTFS-compressed; write-to-disk rows measure NTFS "
                  "compression, not the drive. Use an uncompressed folder (compact /u).")
    print(f"\n{'level':>5} {'chunk':>5} {'thr':>3} {'ratio':>6} {'MB/s':>7} {'fps':>8} {'inflate ms':>10}  "
          + " ".join(f"{t}fps" for t in targets))
    for c in parse_ints(args.chunk_frames):
        chunks = chunks_of(frames, c)
        if not chunks:
            continue
        for level in parse_ints(args.levels):
            for threads in parse_ints(args.threads):
                mbs, ratio, inflate_ms = bench_codec(chunks, level, threads, args.repeat, args.seconds)
                fps = mbs * 1e6 / frame_bytes
                row = {"level": level, "chunk_frames": c, "threads": threads, "ratio": round(ratio, 3),
                       "compress_mb_s": round(mbs, 1), "fps_capacity": round(fps),
                       "inflate_ms_per_chunk": round(inflate_ms, 2)}
                report["codec"].append(row)
                verdict = " ".join(f"{'ok' if fps >= t else 'NO':>{len(str(t)) + 3}}" for t in targets)
                print(f"{level:>5} {c:>5} {threads:>3} {ratio:>6.2f} {mbs:>7.0f} {fps:>8.0f} {inflate_ms:>10.2f}  {verdict}")
            if args.write_dir:
                for threads in parse_ints(args.threads):
                    mbs, size = bench_write(chunks, c, frames.shape[1:], level, threads, args.write_dir)
                    report["write"].append({"level": level, "chunk_frames": c, "threads": threads,
                                            "end_to_end_mb_s": round(mbs, 1), "file_bytes": size})
                    print(f"      write-to-disk level {level} chunk {c} threads {threads}: {mbs:.0f} MB/s")
    if args.spikes:
        report["spikes"] = run_spikes(frames, parse_ints(args.chunk_frames)[0])
        s1, s2 = report["spikes"]["S1"], report["spikes"]["S2"]
        print(f"\nS1 tail rewrites: growth {s1['growth_pct']} % -> {'PASS' if s1['pass'] else 'FAIL'}")
        print(f"S2 mixed chunks byte-identical: {'PASS' if s2['pass'] else 'FAIL'}")
    if args.emit_mixed:
        emit_mixed(frames, parse_ints(args.chunk_frames)[0], args.emit_mixed)
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))
        print(f"\nreport written to {args.json}")
    return 0 if all(s.get("pass", True) for s in report.get("spikes", {}).values()) else 1


if __name__ == "__main__":
    sys.exit(main())
