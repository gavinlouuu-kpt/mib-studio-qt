"""Synthetic stored records for the pzrec_to_h5 and ring_vs_ssd tests: one record exactly as pzrec_records.md lays it out (set area FRAME, IMAGE x2, RESULT x n; MONO8 at 4096, MASK1
at 53248), encoded with the vendored ABI encoder. Needs numpy only (no h5py), so the ring_vs_ssd tests run wherever numpy does."""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pzrec_to_h5 as P  # noqa: E402

DEC, PROV = P.load_decoder()
ABI = DEC.abi()
mib_abi = sys.modules["imx426.mib_abi"]
REC = P.REC_BYTES
FLAG_STORED = 1 << 8


def build_record(frame_id, ticks, *, run_id=18, drops=0, epoch=5, objects=(), seed=0, profile=2, version=3, frame_flags=0):
    """One stored record exactly as the layout doc says: set area (FRAME, IMAGE x2, RESULT x n), MONO8 at 4096, MASK1 at 53248."""
    rng = np.random.default_rng(seed + frame_id)
    img = rng.integers(0, 256, (96, 512), dtype=np.uint8)
    mask = (rng.random((96, 512)) < 0.05).astype(np.uint8)
    packed = np.packbits(mask, axis=1, bitorder="little")
    frame = mib_abi.encode_record(ABI, "FRAME", 0, dict(run_id=run_id, frame_id=frame_id, timestamp=ticks, epoch=epoch, flags=FLAG_STORED | frame_flags, result_count=len(objects),
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
