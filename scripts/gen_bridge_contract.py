#!/usr/bin/env python3
"""Generate (or verify) the TypeScript bridge-contract mirror.

Source of truth: crates/mib-bridge/contract/bridge-contract.json (BE-1,
issue #271, ADR 0004). This script renders it to desktop/src/bridgeContract.ts
so the TypeScript layer cannot drift silently from the C++/Rust contract.

Usage:
    python3 scripts/gen_bridge_contract.py            # (re)write the TS file
    python3 scripts/gen_bridge_contract.py --check    # CI drift gate: exit 1
                                                      # if the TS file differs

The C++ side is pinned by static_asserts in crates/mib-bridge/src/shim.cpp and
the Rust side by crates/mib-bridge/tests/contract.rs — both against the same
JSON values.

YOFO Review has its own contract, crates/mib-bridge/contract/review-contract.json
(ADR 0014): it renders to desktop/src/review/reviewContract.ts and
desktop/src-tauri/src/review_packet_contract.rs, and never touches the bridge
contract above.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTRACT_JSON = REPO_ROOT / "crates/mib-bridge/contract/bridge-contract.json"
OUTPUT_TS = REPO_ROOT / "desktop/src/bridgeContract.ts"
OUTPUT_RS = REPO_ROOT / "crates/mib-app-commands/src/frame_packet_contract.rs"
REVIEW_CONTRACT_JSON = REPO_ROOT / "crates/mib-bridge/contract/review-contract.json"
REVIEW_OUTPUT_TS = REPO_ROOT / "desktop/src/review/reviewContract.ts"
REVIEW_OUTPUT_RS = REPO_ROOT / "desktop/src-tauri/src/review_packet_contract.rs"
REVIEW_ENUM_GROUPS = [
    ("overlay_modes", "OVERLAY_MODES"),
    ("review_operation_kinds", "REVIEW_OPERATION_KINDS"),
    ("review_regenerate_sources", "REVIEW_REGENERATE_SOURCES"),
]

ENUM_GROUPS = [
    ("event_kinds", "EVENT_KINDS"),
    ("frame_ready_sources", "FRAME_READY_SOURCES"),
    ("command_types", "COMMAND_TYPES"),
    ("camera_types", "CAMERA_TYPES"),
    ("camera_selection_modes", "CAMERA_SELECTION_MODES"),
    ("discovery_device_kinds", "DISCOVERY_DEVICE_KINDS"),
    ("discovery_job_states", "DISCOVERY_JOB_STATES"),
    ("discovery_identity_strengths", "DISCOVERY_IDENTITY_STRENGTHS"),
    ("discovery_identification_statuses", "DISCOVERY_IDENTIFICATION_STATUSES"),
    ("discovery_error_kinds", "DISCOVERY_ERROR_KINDS"),
    ("registry_session_states", "REGISTRY_SESSION_STATES"),
    ("registry_connectivity", "REGISTRY_CONNECTIVITY"),
    ("registry_job_kinds", "REGISTRY_JOB_KINDS"),
    ("registry_job_states", "REGISTRY_JOB_STATES"),
    ("registry_central_states", "REGISTRY_CENTRAL_STATES"),
    ("review_image_datasets", "REVIEW_IMAGE_DATASETS"),
    ("pump_ids", "PUMP_IDS"),
    ("pump_run_states", "PUMP_RUN_STATES"),
    ("pump_directions", "PUMP_DIRECTIONS"),
    ("pump_models", "PUMP_MODELS"),
    ("experiment_states", "EXPERIMENT_STATES"),
    ("experiment_command_actions", "EXPERIMENT_COMMAND_ACTIONS"),
    ("experiment_start_outcomes", "EXPERIMENT_START_OUTCOMES"),
    ("experiment_stop_outcomes", "EXPERIMENT_STOP_OUTCOMES"),
    ("run_completion_states", "RUN_COMPLETION_STATES"),
    ("readiness_gate_statuses", "READINESS_GATE_STATUSES"),
    ("error_sources", "ERROR_SOURCES"),
    ("operation_kinds", "OPERATION_KINDS"),
    ("operation_states", "OPERATION_STATES"),
    ("camera_states", "CAMERA_STATES"),
    ("recording_states", "RECORDING_STATES"),
]


def render(contract: dict) -> str:
    lines = [
        "// GENERATED FILE — do not edit by hand.",
        "// Source of truth: crates/mib-bridge/contract/bridge-contract.json",
        "// Regenerate with: python3 scripts/gen_bridge_contract.py",
        "// CI verifies this file with: python3 scripts/gen_bridge_contract.py --check",
        "",
        f"export const BRIDGE_ABI_VERSION = {contract['abi_version']};",
        "",
    ]
    for json_key, ts_name in ENUM_GROUPS:
        entries = contract[json_key]
        lines.append(f"export const {ts_name} = {{")
        for name, value in entries.items():
            lines.append(f"  {name}: {value},")
        lines.append("} as const;")
        lines.append("")
    lines.append("export const FRAME_PACKET = " + json.dumps(contract["frame_packet"], indent=2) + " as const;")
    lines.append("")
    for key, name in [("json_transport", "JSON_TRANSPORT"), ("event_payloads", "EVENT_PAYLOADS")]:
        lines.append("export const " + name + " = " + json.dumps(contract[key], indent=2) + " as const;")
        lines.append("")
    # Reverse lookup for event kinds: numeric value -> stable name.
    lines.append("export const EVENT_KIND_NAMES: Readonly<Record<number, string>> = {")
    for name, value in contract["event_kinds"].items():
        lines.append(f"  {value}: \"{name}\",")
    lines.append("};")
    lines.append("")
    lines.append("export type BridgeEventKindName = keyof typeof EVENT_KINDS;")
    lines.append("")
    return "\n".join(lines)


def render_rust(contract: dict) -> str:
    c = contract["frame_packet"]
    fields = [("VERSION", "u16", "version"), ("HEADER_BYTES", "usize", "header_bytes"),
              ("MAX_PAYLOAD_BYTES", "u64", "max_payload_bytes"),
              ("MAX_PIXELS", "u64", "max_pixels"), ("MAX_DIMENSION", "u64", "max_dimension")]
    lines = ["// GENERATED by scripts/gen_bridge_contract.py; do not edit."]
    lines += [f"pub const {name}: {kind} = {c[key]};" for name, kind, key in fields]
    lines += [f"pub const JSON_TRANSPORT_VERSION: u32 = {contract['json_transport']['version']};",
              f"pub const MAX_EVENT_TEXT_BYTES: usize = {contract['json_transport']['max_event_text_bytes']};"]
    return "\n".join(lines) + "\n"


def render_review(review: dict) -> str:
    """desktop/src/review/reviewContract.ts: YOFO Review's contract (ADR 0014)."""
    lines = [
        "// GENERATED by scripts/gen_bridge_contract.py from",
        "// crates/mib-bridge/contract/review-contract.json — do not edit by hand.",
        "// YOFO Review's own contract (ADR 0014); the MIB Studio bridge contract is",
        "// ../bridgeContract.ts.",
        "",
        f"export const REVIEW_ABI_VERSION = {review['review_abi_version']};",
        "",
    ]
    for json_key, name in REVIEW_ENUM_GROUPS:
        lines.append(f"export const {name} = {{")
        for key, value in review[json_key].items():
            lines.append(f"  {key}: {value},")
        lines.append("} as const;")
        lines.append("")
    for json_key, name in [("review_density", "REVIEW_DENSITY"), ("review_pixel_formats", "REVIEW_PIXEL_FORMATS"),
                           ("frame_packet", "REVIEW_FRAME_PACKET")]:
        lines.append(f"export const {name} = " + json.dumps(review[json_key], indent=2, ensure_ascii=False) + " as const;")
        lines.append("")
    return "\n".join(lines)


def render_review_rust(review: dict) -> str:
    c = review["frame_packet"]
    lines = ["// GENERATED by scripts/gen_bridge_contract.py from review-contract.json; do not edit."]
    lines += [f"pub const PIXEL_FORMAT_MONO8_LEGACY: u64 = {c['pixel_formats']['mono8_legacy']};",
              f"pub const PIXEL_FORMAT_MONO8: u64 = {c['pixel_formats']['mono8']};",
              f"pub const PIXEL_FORMAT_RGB8: u64 = {c['pixel_formats']['rgb8']};"]
    lines += [f"pub const PULL_KIND_{k.upper()}: u32 = {v};" for k, v in c["pull_kinds"].items()]
    return "\n".join(lines) + "\n"


def check_review(review: dict) -> str | None:
    """Structural checks on review-contract.json; an error message or None."""
    for json_key, _ in REVIEW_ENUM_GROUPS:
        values = sorted(review[json_key].values())
        if values != list(range(len(values))):
            return f"review {json_key} values must be contiguous from 0 (got {values})"
    # review_density.ramp_rgb must equal the C++ ramp stops
    # (MonitoringDensity.h densityRampColor) so both shells colour levels alike.
    header = (REPO_ROOT / "include/backend/processing/MonitoringDensity.h").read_text(encoding="utf-8")
    block = header.split("kStops[] = {", 1)[1].split("};", 1)[0]
    stops = [[int(v, 16) for v in m] for m in re.findall(
        r"\{0x([0-9a-fA-F]{2}), 0x([0-9a-fA-F]{2}), 0x([0-9a-fA-F]{2})\}", block)]
    if stops != review["review_density"]["ramp_rgb"]:
        return "review_density.ramp_rgb differs from MonitoringDensity.h kStops"
    return None


def main() -> int:
    check = "--check" in sys.argv[1:]
    contract = json.loads(CONTRACT_JSON.read_text(encoding="utf-8"))

    # Structural sanity: values inside a group must be unique and contiguous
    # from 0 (the contract is additive-append-only).
    for json_key, _ in ENUM_GROUPS:
        values = sorted(contract[json_key].values())
        if values != list(range(len(values))):
            print(
                f"gen_bridge_contract: {json_key} values must be contiguous "
                f"from 0 (got {values}) — the contract is append-only",
                file=sys.stderr,
            )
            return 1

    review = json.loads(REVIEW_CONTRACT_JSON.read_text(encoding="utf-8"))
    problem = check_review(review)
    if problem:
        print(f"gen_bridge_contract: {problem}", file=sys.stderr)
        return 1

    outputs = {OUTPUT_TS: render(contract), OUTPUT_RS: render_rust(contract),
               REVIEW_OUTPUT_TS: render_review(review), REVIEW_OUTPUT_RS: render_review_rust(review)}
    for path, rendered in outputs.items():
        if check:
            current = path.read_text(encoding="utf-8") if path.exists() else ""
            if current != rendered:
                print(f"gen_bridge_contract: drift detected in {path.relative_to(REPO_ROOT)}", file=sys.stderr)
                return 1
        else:
            path.write_text(rendered, encoding="utf-8")
    print("gen_bridge_contract: TypeScript and Rust packet contracts " + ("in sync" if check else "generated"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
