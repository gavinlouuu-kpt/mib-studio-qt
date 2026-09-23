#!/usr/bin/env python3
"""Validate the standalone embedded target manifest (epic #441, E0 #443).

Checks deploy/embedded/pz7035-target.json against
deploy/embedded/pz7035-target.schema.json with a small stdlib-only JSON
Schema subset (no `jsonschema` dependency, same policy as
scripts/test_export_hdf5_paths.py), then enforces the E0 contract rules the
schema cannot express:

1. The target is Cortex-A9 / ARMv7-A / 32-bit. No excluded-target token
   (aarch64, Ultra96, KU5P, ...) appears anywhere inside `target` or
   `dependency_allowlist` except in `target.excluded_targets` itself.
2. Every ownership row has exactly one owner (schema enum) and a non-empty
   failure policy and test; every `ctest:<name>` it cites is registered in
   tests/CMakeLists.txt; every unresolved row is backed by an unknown.
3. Every budget/quantity is either a number with a unit or explicitly
   `unknown` with a null value; nothing is silently zero.
4. Unknown ids are unique and each is referenced from the manifest text, so
   a stale unknown cannot linger unnoticed.
5. Forbidden appliance dependencies never appear in the ARM runtime allowlist.
6. Baseline commits are full 40-hex SHAs.

Exit 0 = valid, 1 = violations (each with a remediation hint).
Usage: python3 scripts/check_embedded_target.py [manifest.json] [schema.json]
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "deploy" / "embedded" / "pz7035-target.json"
DEFAULT_SCHEMA = REPO_ROOT / "deploy" / "embedded" / "pz7035-target.schema.json"
TESTS_CMAKE = REPO_ROOT / "tests" / "CMakeLists.txt"

TYPE_CHECKS = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


class SchemaValidator:
    """Minimal draft-2020-12 subset: $ref (local), type, required,
    additionalProperties, properties, items, enum, const, minimum, pattern."""

    def __init__(self, schema: dict):
        self.root = schema
        self.errors: list[str] = []

    def resolve(self, ref: str) -> dict:
        if not ref.startswith("#/"):
            raise ValueError(f"only local $ref supported, got {ref}")
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    def check(self, value, schema: dict, path: str) -> None:
        if "$ref" in schema:
            self.check(value, self.resolve(schema["$ref"]), path)
            return
        expected = schema.get("type")
        if expected is not None:
            types = expected if isinstance(expected, list) else [expected]
            if not any(TYPE_CHECKS[t](value) for t in types):
                self.errors.append(f"{path}: expected type {types}, got {type(value).__name__}")
                return
        if "const" in schema and value != schema["const"]:
            self.errors.append(f"{path}: must equal {schema['const']!r}, got {value!r}")
        if "enum" in schema and value not in schema["enum"]:
            self.errors.append(f"{path}: {value!r} not in {schema['enum']}")
        if "minimum" in schema and isinstance(value, (int, float)) and value < schema["minimum"]:
            self.errors.append(f"{path}: {value} < minimum {schema['minimum']}")
        if "pattern" in schema and isinstance(value, str) and not re.search(schema["pattern"], value):
            self.errors.append(f"{path}: {value!r} does not match /{schema['pattern']}/")
        if isinstance(value, dict):
            props = schema.get("properties", {})
            for key in schema.get("required", []):
                if key not in value:
                    self.errors.append(f"{path}: missing required key '{key}'")
            for key, sub in value.items():
                if key in props:
                    self.check(sub, props[key], f"{path}.{key}")
                else:
                    extra = schema.get("additionalProperties", True)
                    if extra is False:
                        self.errors.append(f"{path}: unexpected key '{key}'")
                    elif isinstance(extra, dict):
                        self.check(sub, extra, f"{path}.{key}")
        if isinstance(value, list) and "items" in schema:
            for i, item in enumerate(value):
                self.check(item, schema["items"], f"{path}[{i}]")


def walk_strings(node, path: str):
    if isinstance(node, str):
        yield path, node
    elif isinstance(node, dict):
        for k, v in node.items():
            yield from walk_strings(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk_strings(v, f"{path}[{i}]")


def registered_ctests() -> set[str]:
    if not TESTS_CMAKE.exists():
        return set()
    text = TESTS_CMAKE.read_text(encoding="utf-8")
    return set(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_.]+)", text))


def semantic_checks(m: dict, errors: list[str]) -> None:
    target = m["target"]
    ps = target["processing_system"]
    if (ps["cpu"], ps["isa"], ps["address_bits"]) != ("Cortex-A9", "armv7-a", 32):
        errors.append("target.processing_system must be Cortex-A9 / armv7-a / 32-bit (epic #441)")

    # 1. Excluded-target tokens must not leak into the target or allowlists.
    excluded = [t.lower() for t in target["excluded_targets"]]
    for section in ("target", "dependency_allowlist"):
        for path, text in walk_strings(m[section], section):
            if path.startswith("target.excluded_targets"):
                continue
            lowered = text.lower()
            for token in excluded:
                if re.search(rf"(?<![a-z0-9]){re.escape(token)}(?![a-z0-9])", lowered):
                    errors.append(
                        f"{path}: mentions excluded target '{token}'. Move non-XC7Z035 evidence to "
                        "reuse[].not_transferable; the target section must describe only the selected board.")

    # 2. Ownership rows: policies, tests, ctest references, unresolved backing.
    ctests = registered_ctests()
    unknown_ids = {u["id"] for u in m["unknowns"]}
    unknown_blocks = {b for u in m["unknowns"] for b in u["blocks"]}
    seen_ops: set[str] = set()
    for i, row in enumerate(m["ownership"]):
        p = f"ownership[{i}] ({row['operation']})"
        if row["operation"] in seen_ops:
            errors.append(f"{p}: duplicate operation; one owner per operation means one row per operation")
        seen_ops.add(row["operation"])
        for key in ("failure_policy", "test", "path", "data"):
            if not row[key].strip():
                errors.append(f"{p}: '{key}' is empty; record the policy or write 'unresolved' with an unknown id")
        for name in re.findall(r"ctest:([A-Za-z0-9_.]+)", row["test"]):
            if name not in ctests:
                errors.append(f"{p}: cites ctest:{name} which is not registered in tests/CMakeLists.txt")
        if row["status"] == "unresolved":
            key = f"ownership-row: {row['operation']}"
            if key not in unknown_blocks:
                errors.append(f"{p}: status unresolved but no unknown lists blocks=['{key}']")
        if row["status"] == "implemented" and "ctest:" not in row["test"]:
            errors.append(f"{p}: implemented rows must cite at least one registered ctest:<name>")

    # 3. Quantities and budgets: number+unit or explicit unknown, never a silent zero.
    def check_quantity(q: dict, path: str) -> None:
        if q["status"] == "unknown":
            if q["value"] is not None:
                errors.append(f"{path}: status unknown but value is {q['value']!r}; set value to null")
        else:
            if not isinstance(q["value"], (int, float)) or isinstance(q["value"], bool):
                errors.append(f"{path}: status {q['status']} requires a numeric value (or mark it unknown)")
            elif q["value"] <= 0:
                errors.append(f"{path}: {q['value']} is not a positive budget; a zero is not a measurement")
        if not q["unit"].strip():
            errors.append(f"{path}: unit is empty")

    for name, b in m["budgets"].items():
        check_quantity(b, f"budgets.{name}")
        if b["status"] != "unknown" and not b.get("derivation") and b["status"] != "frozen":
            errors.append(f"budgets.{name}: proposed/sizing-input values need a 'derivation'")
    for path, node in walk_quantities(target, "target"):
        check_quantity(node, path)

    # 4. Unknowns: unique ids, referenced somewhere outside the unknowns list.
    ids = [u["id"] for u in m["unknowns"]]
    for dup in {x for x in ids if ids.count(x) > 1}:
        errors.append(f"unknowns: duplicate id {dup}")
    referenced = " ".join(text for path, text in walk_strings(m, "$") if not path.startswith("$.unknowns"))
    for uid in unknown_ids:
        if not re.search(rf"(?<![A-Za-z0-9]){uid}(?![0-9])", referenced):
            errors.append(f"unknowns: {uid} is never referenced from the manifest body; cite it or delete it")
    for u in m["unknowns"]:
        if not u["owner"].strip():
            errors.append(f"unknowns.{u['id']}: owner is empty")

    # 5. Forbidden appliance dependencies stay out of the ARM allowlist.
    forbidden = [f.split(" (")[0].strip().lower() for f in m["dependency_allowlist"]["forbidden_in_arm_runtime"]]
    for dep in m["dependency_allowlist"]["arm_runtime"]:
        name = dep["name"].lower()
        for f in forbidden:
            head = f.split("/")[0].strip()
            if head and head in name:
                errors.append(f"dependency_allowlist.arm_runtime: '{dep['name']}' matches forbidden '{f}'")

    # 6. Boundaries keep the three protocols independent.
    b = m["boundaries"]
    if b["native_plugin_abi"]["version"] is None:
        errors.append("boundaries.native_plugin_abi.version must be the current MIB_PROCESSING_ENGINE_ABI_VERSION")
    if b["ps_pl_wire"]["status"] != "board-owned":
        errors.append("boundaries.ps_pl_wire must stay board-owned (pz7035-imx426 PZ2)")


def walk_quantities(node, path: str):
    if isinstance(node, dict):
        if {"value", "unit", "status"} <= set(node.keys()) and "evidence" not in node:
            yield path, node
        else:
            for k, v in node.items():
                yield from walk_quantities(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk_quantities(v, f"{path}[{i}]")


def main(argv: list[str]) -> int:
    manifest_path = Path(argv[1]) if len(argv) > 1 else DEFAULT_MANIFEST
    schema_path = Path(argv[2]) if len(argv) > 2 else DEFAULT_SCHEMA
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"check_embedded_target: cannot read inputs: {exc}", file=sys.stderr)
        return 1

    validator = SchemaValidator(schema)
    validator.check(manifest, schema, "$")
    errors = list(validator.errors)
    if not errors:
        semantic_checks(manifest, errors)

    if errors:
        print(f"check_embedded_target: {len(errors)} violation(s) in {manifest_path.relative_to(REPO_ROOT) if manifest_path.is_relative_to(REPO_ROOT) else manifest_path}:")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(
        f"check_embedded_target: OK ({manifest['manifest_id']}, reviewed {manifest['reviewed']}, "
        f"{len(manifest['ownership'])} ownership rows, {len(manifest['budgets'])} budgets, "
        f"{len(manifest['unknowns'])} explicit unknowns)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
