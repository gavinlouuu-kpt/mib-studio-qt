"""Host/ARM reference decoder for the PZ-MIB PS-PL wire ABI.

Standard library only. Layouts come from abi/pz_mib_abi.json so the decoder
never duplicates register or record definitions. RESULT payload words are
opaque here: their meaning belongs to the science profile, not the platform. Stream context (sequence, epoch,
generation) is tracked by StreamDecoder; single records are decoded by
decode_record.
"""
from __future__ import annotations

import json
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "abi" / "pz_mib_abi.json"

STRUCT = {"u8": "B", "u16": "H", "u32": "I", "u64": "Q"}
SIZES = {"u8": 1, "u16": 2, "u32": 4, "u64": 8}


class AbiError(ValueError):
    def __init__(self, code: str, detail: str = ""):
        super().__init__(f"{code}: {detail}" if detail else code)
        self.code = code


def _parse_type(t: str) -> tuple[str, int]:
    if "[" in t:
        base, count = t[:-1].split("[")
        return base, int(count)
    return t, 1


def _layout(fields: list[dict]) -> tuple[list[dict], int]:
    offset, out = 0, []
    for f in fields:
        base, count = _parse_type(f["type"])
        out.append({**f, "base": base, "count": count, "offset": offset})
        offset += SIZES[base] * count
    return out, offset


class Abi:
    """Parsed schema: enums, bitmasks, register offsets and record layouts."""

    def __init__(self, schema: dict | None = None):
        self.s = schema or json.loads(SCHEMA_PATH.read_text())
        a = self.s["abi"]
        self.major, self.minor = a["major"], a["minor"]
        self.identity_magic = int(a["identity_magic"], 16)
        self.record_magic = int(a["record_magic"], 16)
        self.board_id = int(a["board_id"], 16)
        self.constants = self.s["constants"]
        self.enums = {k: dict(v) for k, v in self.s["enums"].items()}
        self.enum_names = {k: {v2: k2 for k2, v2 in v.items()} for k, v in self.enums.items()}
        self.bits = self.s["bitmasks"]
        self.registers = {r["name"]: int(r["offset"], 16) for r in self.s["registers"]["map"]}
        self.counters = list(self.s["counters"])
        self.header, self.header_bytes = _layout(self.s["records"]["header"]["fields"])
        self.types: dict[int, dict] = {}
        for name, rec in self.s["records"]["types"].items():
            fields, size = _layout(rec["fields"])
            element = None
            if "element" in rec:
                element, _ = _layout(rec["element"])
            self.types[self.enums["record_type"][name]] = {"name": name, "fields": fields, "payload_bytes": size, "version": rec["version"], "element": element, "variable": rec.get("variable")}

    def bit(self, mask: str, name: str) -> int:
        return 1 << self.bits[mask][name]

    def required_capabilities(self, names: list[str]) -> int:
        return sum(self.bit("capabilities", n) for n in names)


@dataclass
class Record:
    type: str
    version: int
    length: int
    sequence: int
    fields: dict[str, Any] = field(default_factory=dict)

    def __getattr__(self, name):
        try:
            return self.fields[name]
        except KeyError as exc:
            raise AttributeError(name) from exc


def crc32_record(raw: bytes) -> int:
    return zlib.crc32(raw[:12] + b"\0\0\0\0" + raw[16:]) & 0xFFFFFFFF


def decode_record(abi: Abi, raw: bytes, *, result_limit: int | None = None) -> Record:
    """Decode one record. Raises AbiError with a decode_error code on any violation."""
    hb = abi.header_bytes
    if len(raw) < hb:
        raise AbiError("TRUNCATED", f"{len(raw)} < header")
    magic, rtype, version, length, sequence, crc = struct.unpack_from("<IBBHII", raw, 0)
    if magic != abi.record_magic:
        raise AbiError("BAD_MAGIC", f"0x{magic:08x}")
    if length > abi.constants["MAX_RECORD_BYTES"]:
        raise AbiError("OVERSIZED", f"length {length}")
    if length < hb or length % abi.constants["RECORD_ALIGN_BYTES"]:
        raise AbiError("BAD_LENGTH", f"length {length}")
    if len(raw) < length:
        raise AbiError("TRUNCATED", f"{len(raw)} < {length}")
    raw = raw[:length]
    if crc32_record(raw) != crc:
        raise AbiError("BAD_CRC")
    if rtype not in abi.types:
        raise AbiError("UNKNOWN_TYPE", str(rtype))
    spec = abi.types[rtype]
    if version > spec["version"]:
        raise AbiError("BAD_VERSION", f"{version} > {spec['version']}")
    if length < hb + spec["payload_bytes"]:
        raise AbiError("BAD_LENGTH", f"{length} below minimum for {spec['name']}")
    payload = raw[hb:]
    values: dict[str, Any] = {}
    for f in spec["fields"]:
        fmt = "<" + STRUCT[f["base"]] * f["count"]
        got = struct.unpack_from(fmt, payload, f["offset"])
        values[f["name"]] = list(got) if f["count"] > 1 else got[0]
    rec = Record(spec["name"], version, length, sequence)
    name = spec["name"]
    if name == "FRAME":
        if result_limit is None:
            result_limit = values["result_limit"] or abi.constants["MAX_RESULTS_PER_FRAME"]
        if values["result_count"] > result_limit:
            raise AbiError("RESULT_LIMIT", f"{values['result_count']} > {result_limit}")
        values["flags_named"] = [k for k, b in abi.bits["frame_flags"].items() if values["flags"] & (1 << b)]
        values["science_profile_name"] = abi.enum_names["science_profile"].get(values["science_profile"])
        values["pixel_format_name"] = abi.enum_names["pixel_format"].get(values["pixel_format"])
    if name == "RESULT":
        var = spec["variable"]
        count = values[var["field"]]
        if count > abi.constants[var["max"]]:
            raise AbiError("OVERSIZED", f"{count} payload words")
        need = hb + spec["payload_bytes"] + count * var["element_bytes"]
        if length < need:
            raise AbiError("BAD_LENGTH", f"{count} payload words need {need} bytes, have {length}")
        base = spec["payload_bytes"]
        # Payload words are opaque to the platform; the processing track owns their meaning.
        values["payload"] = [struct.unpack_from("<I", payload, base + i * 4)[0] for i in range(count)]
        values["flags_named"] = [k for k, b in abi.bits["result_flags"].items() if values["flags"] & (1 << b)]
        values["science_profile_name"] = abi.enum_names["science_profile"].get(values["science_profile"])
    if name == "EVENT":
        values["decision_name"] = abi.enum_names["event_decision"].get(values["decision"])
        if values["decision_name"] is None:
            raise AbiError("BAD_STATE", f"decision {values['decision']}")
        if values["decision_name"] != "ISSUED":
            values["output_timestamp"] = None
    if name == "COUNTERS":
        values["named"] = dict(zip(abi.counters, values["counters"]))
    if name == "PREVIEW":
        values["pixel_format_name"] = abi.enum_names["pixel_format"].get(values["pixel_format"])
    if name == "DESCRIPTOR":
        values["state_name"] = abi.enum_names["buffer_state"].get(values["state"])
        values["kind_name"] = abi.enum_names["buffer_kind"].get(values["kind"])
        if values["state_name"] is None or values["kind_name"] is None:
            raise AbiError("BAD_STATE", f"state {values['state']} kind {values['kind']}")
    rec.fields = values
    return rec


def encode_record(abi: Abi, type_name: str, sequence: int, values: dict[str, Any], payload_words: list[int] | None = None) -> bytes:
    """Encode one record exactly as the PL bridge writes it (8-byte aligned, CRC over length)."""
    rtype = abi.enums["record_type"][type_name]
    spec = abi.types[rtype]
    body = bytearray(spec["payload_bytes"])
    for f in spec["fields"]:
        v = values.get(f["name"], 0)
        if f["count"] > 1:
            seq = list(v) if v else []
            seq += [0] * (f["count"] - len(seq))
            struct.pack_into("<" + STRUCT[f["base"]] * f["count"], body, f["offset"], *seq)
        else:
            struct.pack_into("<" + STRUCT[f["base"]], body, f["offset"], v)
    for w in payload_words or []:
        body += struct.pack("<I", w)
    body += b"\0" * ((-(abi.header_bytes + len(body))) % abi.constants["RECORD_ALIGN_BYTES"])
    length = abi.header_bytes + len(body)
    raw = bytearray(struct.pack("<IBBHII", abi.record_magic, rtype, spec["version"], length, sequence & 0xFFFFFFFF, 0) + body)
    struct.pack_into("<I", raw, 12, crc32_record(bytes(raw)))
    return bytes(raw)


def split_records(abi: Abi, blob: bytes) -> list[bytes]:
    """Split a ring/DMA buffer into record byte strings using the length field."""
    out, pos = [], 0
    while pos + abi.header_bytes <= len(blob):
        length = struct.unpack_from("<H", blob, pos + 6)[0]
        if length < abi.header_bytes:
            break
        out.append(blob[pos:pos + length])
        pos += length
    return out


@dataclass
class StreamDecoder:
    """Tracks sequence, epoch and generation for a result stream."""

    abi: Abi
    epoch: int
    generation: int
    last_sequence: int | None = None
    gaps: int = 0

    def feed(self, raw: bytes) -> Record:
        rec = decode_record(self.abi, raw)
        if self.last_sequence is not None:
            expected = (self.last_sequence + 1) & 0xFFFFFFFF
            if rec.sequence == self.last_sequence:
                raise AbiError("DUPLICATE_SEQUENCE", str(rec.sequence))
            if rec.sequence != expected:
                self.gaps += 1
                self.last_sequence = rec.sequence
                raise AbiError("SEQUENCE_GAP", f"expected {expected} got {rec.sequence}")
        self.last_sequence = rec.sequence
        if "epoch" in rec.fields and rec.fields["epoch"] != self.epoch:
            if rec.fields["epoch"] < self.epoch:
                raise AbiError("STALE_EPOCH", f"{rec.fields['epoch']} < {self.epoch}")
            if rec.type == "FRAME" and rec.fields["flags"] & self.abi.bit("frame_flags", "FIRST_OF_EPOCH"):
                self.epoch = rec.fields["epoch"]
        if rec.type == "DESCRIPTOR" and rec.fields["generation"] != (self.generation & 0xFFFF):
            raise AbiError("STALE_GENERATION", f"{rec.fields['generation']} != {self.generation & 0xFFFF}")
        return rec


@dataclass
class DeviceIdentity:
    identity: int
    abi_major: int
    abi_minor: int
    board_id: int
    capabilities: int
    science_profile: int
    profile_id: bytes = b""
    build_id: bytes = b""


def check_compatible(abi: Abi, ident: DeviceIdentity, required_capabilities: list[str], expected_profile: str | None = None) -> list[str]:
    """Return reasons the device must be rejected before arm. Empty list means compatible."""
    reasons = []
    if ident.identity != abi.identity_magic:
        reasons.append(f"identity 0x{ident.identity:08x} is not a PZ-MIB image")
    if ident.board_id != abi.board_id:
        reasons.append(f"board id 0x{ident.board_id:04x} != 0x{abi.board_id:04x}")
    if ident.abi_major != abi.major:
        reasons.append(f"abi major {ident.abi_major} != {abi.major}")
    elif ident.abi_minor < abi.minor:
        reasons.append(f"abi minor {ident.abi_minor} < {abi.minor}")
    need = abi.required_capabilities(required_capabilities)
    missing = need & ~ident.capabilities
    if missing:
        names = [k for k, b in abi.bits["capabilities"].items() if missing & (1 << b)]
        reasons.append(f"missing capabilities {names}")
    if expected_profile is not None:
        want = abi.enums["science_profile"][expected_profile]
        have = ident.science_profile & 0xFFFF
        if have != want:
            reasons.append(f"science profile {abi.enum_names['science_profile'].get(have, have)} != {expected_profile}")
    return reasons


def identity_from_registers(abi: Abi, read32) -> DeviceIdentity:
    """Build a DeviceIdentity from a register reader callable(offset) -> u32."""
    r = abi.registers
    version = read32(r["ABI_VERSION"])
    profile_id = b"".join(read32(r[f"PROFILE_ID{i}"]).to_bytes(4, "little") for i in range(4))
    build = b"".join(read32(r[f"BUILD_ID{i}"]).to_bytes(4, "little") for i in range(4))
    return DeviceIdentity(read32(r["IDENTITY"]), version >> 16, version & 0xFFFF, read32(r["BOARD_ID"]), read32(r["CAPABILITIES"]), read32(r["SCIENCE_PROFILE"]), profile_id, build)
