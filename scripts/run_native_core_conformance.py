#!/usr/bin/env python3
"""Run a built Contract-2 processing core (DLL/.so) over the real-frame
fixture through engine ABI v2 and compare its per-object metrics with the
Contract-2 gold reference (rollout plan Phase 1 exit gate).

The reference was produced by the wheel, which adds host-side fields
(tracking, mask and series hashes) that a core does not compute, so this
compares the core-owned per-object fields itself at the same tolerance as
compare_metrics.

Exit codes: 0 full match, 1 mismatch, 2 load/ABI/input error, 77 numpy missing
(only with --skip-if-missing-numpy).
"""
from __future__ import annotations

import argparse
import ctypes as C
import json
import math
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:  # handled in main() (--skip-if-missing-numpy)
    np = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_metrics  # noqa: E402

ABI2 = 2
CONTRACT2 = 2
MAX_OBJECTS = 256
FIELDS = {  # reference key -> metrics attribute
    "object_id": "object_id", "object_count": "object_count", "is_valid": "is_valid",
    "touches_border": "touches_border", "is_target_group": "is_target_group", "in_range": "in_range",
    "area": "area", "deformability": "deformability", "area_ratio": "area_ratio",
    "laplacian_variance": "laplacian_variance", "youngs_modulus": "youngs_modulus",
    "brightness_q1": "brightness_q1", "brightness_q2": "brightness_q2",
    "brightness_q3": "brightness_q3", "brightness_q4": "brightness_q4",
}
INTEGER_FIELDS = {"object_id", "object_count", "is_valid", "touches_border", "is_target_group", "in_range"}


class ImageView(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("width", C.c_uint32), ("height", C.c_uint32),
                ("stride_bytes", C.c_uint64), ("data", C.c_void_p), ("data_size_bytes", C.c_uint64)]


class Roi(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("x", C.c_int32), ("y", C.c_int32),
                ("width", C.c_int32), ("height", C.c_int32)]


class ConfigV2(C.Structure):
    # reserved_u32[16] as on develop; #461's precomputed_mask overlays the
    # first two words without changing size or offsets.
    _fields_ = [("struct_size", C.c_uint32), ("gaussian_blur_size", C.c_int32),
                ("difference_threshold", C.c_int32), ("morphology_kernel_size", C.c_int32),
                ("morphology_iterations", C.c_int32), ("empty_frame_pixel_threshold", C.c_int32),
                ("laplacian_kernel_size", C.c_int32), ("flags", C.c_uint32),
                ("filters", C.c_void_p), ("science_config_json", C.c_char_p),
                ("science_config_json_size", C.c_uint64), ("reserved_u32", C.c_uint32 * 16)]


class Metrics(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("object_id", C.c_int32), ("object_count", C.c_int32),
                ("is_valid", C.c_int32), ("touches_border", C.c_int32), ("is_target_group", C.c_int32),
                ("track_id", C.c_int32)] + [(name, C.c_double) for name in (
                    "area", "deformability", "area_ratio", "laplacian_variance", "youngs_modulus",
                    "centroid_x", "centroid_y", "bbox_x", "bbox_y", "bbox_width", "bbox_height",
                    "brightness_q1", "brightness_q2", "brightness_q3", "brightness_q4")] + [
                ("in_range", C.c_int32), ("in_channel", C.c_int32), ("reserved_u32", C.c_uint32 * 8)]


class ObjectBuffer(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("capacity", C.c_uint32), ("count", C.c_uint32),
                ("required", C.c_uint32), ("objects", C.POINTER(Metrics))]


class Descriptor(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("engine_abi_version", C.c_uint32),
                ("contract_version", C.c_uint32), ("capabilities", C.c_uint32),
                ("core_version", C.c_char_p), ("build_id", C.c_char_p),
                ("runtime_fingerprint", C.c_char_p), ("reserved_u64", C.c_uint64 * 8)]


CTX = C.c_void_p
DescriptorFn = C.CFUNCTYPE(C.POINTER(Descriptor))
CreateFn = C.CFUNCTYPE(C.c_int, C.POINTER(CTX), C.c_char_p, C.c_size_t)
DestroyFn = C.CFUNCTYPE(None, CTX)
ProcessObjectsFn = C.CFUNCTYPE(C.c_int, CTX, C.POINTER(ImageView), C.POINTER(ImageView),
                               C.POINTER(ConfigV2), C.POINTER(Roi), C.c_double, C.c_void_p,
                               C.POINTER(ObjectBuffer), C.c_char_p, C.c_size_t)


class ApiV2(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("engine_abi_version", C.c_uint32),
                ("descriptor", DescriptorFn), ("create_context", CreateFn),
                ("destroy_context", DestroyFn), ("reset_context", C.c_void_p),
                ("process_mask", C.c_void_p), ("is_empty", C.c_void_p),
                ("process_objects", ProcessObjectsFn), ("self_test", C.c_void_p),
                ("reserved", C.c_void_p * 8)]


def image_view(image: np.ndarray) -> tuple[ImageView, np.ndarray]:
    image = np.ascontiguousarray(image, dtype=np.uint8)
    view = ImageView(C.sizeof(ImageView), image.shape[1], image.shape[0], image.strides[0],
                     image.ctypes.data, image.nbytes)
    return view, image  # keep the array alive with the view


def load_api(path: Path) -> tuple[C.CDLL, ApiV2]:
    library = C.CDLL(str(path))
    get_api = library.mib_processing_get_api_v2
    get_api.restype = C.c_int
    get_api.argtypes = [C.c_uint32, C.c_uint32, C.POINTER(ApiV2), C.c_char_p, C.c_size_t]
    api = ApiV2()
    error = C.create_string_buffer(512)
    status = get_api(ABI2, C.sizeof(ApiV2), C.byref(api), error, len(error))
    if status != 0:
        raise RuntimeError(f"get_api_v2 failed ({status}): {error.value.decode()}")
    descriptor = api.descriptor().contents
    if descriptor.engine_abi_version != ABI2 or descriptor.contract_version != CONTRACT2:
        raise RuntimeError(f"not an ABI-2/Contract-2 core: abi={descriptor.engine_abi_version} "
                           f"contract={descriptor.contract_version}")
    return library, api


def contract2_config(config: dict) -> dict:
    """The fixture's recorded config under Contract 2 (same mapping as
    run_processing_conformance.config_for_contract)."""
    config = dict(config)
    config["processing_contract_version"] = CONTRACT2
    if "bg_subtract_threshold" in config:
        config["difference_threshold"] = config.pop("bg_subtract_threshold")
    return config


_TOP_LEVEL_KEYS = (
    "gaussian_blur_size", "morph_kernel_size", "morph_iterations", "area_threshold_min",
    "area_threshold_max", "deformability_threshold_min", "deformability_threshold_max",
    "area_ratio_threshold_max", "ring_ratio_min", "ring_ratio_max", "empty_frame_pixel_threshold",
    "auto_background_enabled", "auto_background_empty_frames", "auto_background_cooldown_frames",
    "auto_roi_from_background", "auto_roi_wall_gradient_ratio", "auto_roi_wall_margin",
)
_FILTER_KEYS = (
    "enable_border_check", "enable_area_range_check", "enable_deformability_range_check",
    "enable_area_ratio_check", "enable_ring_ratio_check", "require_single_inner_contour",
)
_TARGET_GROUP_KEYS = {  # science key -> flat wheel key
    "enabled": "enable_target_group", "area_min": "target_group_area_min",
    "area_max": "target_group_area_max", "deformability_min": "target_group_deformability_min",
    "deformability_max": "target_group_deformability_max",
    "emodulus_enabled": "enable_target_group_emodulus", "emodulus_min": "target_group_emodulus_min",
    "emodulus_max": "target_group_emodulus_max",
}
_ABI_V2_KEYS = (
    "processing_contract_version", "enable_laplacian_variance_check", "laplacian_variance_min",
    "laplacian_variance_max", "channel_band_y", "channel_band_h",
)


def science_json(config: dict) -> bytes:
    """The flat wheel config in the layout a core parses
    (config_json::toScienceJson: the persisted image_processing object plus
    "abi_v2"). Keys the core would silently ignore must not be left flat."""
    science: dict = {key: config[key] for key in _TOP_LEVEL_KEYS if key in config}
    # ProcessingConfig keeps the Contract-1 field name for the difference threshold.
    science["bg_subtract_threshold"] = config["difference_threshold"]
    science["filters"] = {key: config[key] for key in _FILTER_KEYS if key in config}
    science["target_group"] = {
        key: config[flat] for key, flat in _TARGET_GROUP_KEYS.items() if flat in config
    }
    science["multi_image"] = {
        key: config[flat] for key, flat in (("enabled", "multi_image_enabled"),
                                            ("count", "multi_image_count")) if flat in config
    }
    science["abi_v2"] = {key: config[key] for key in _ABI_V2_KEYS if key in config}
    return json.dumps(science).encode("utf-8")


def kernel_config(config: dict, science_json: bytes) -> ConfigV2:
    cfg = ConfigV2()
    cfg.struct_size = C.sizeof(ConfigV2)
    cfg.gaussian_blur_size = int(config["gaussian_blur_size"])
    cfg.difference_threshold = int(config["difference_threshold"])
    cfg.morphology_kernel_size = int(config["morph_kernel_size"])
    cfg.morphology_iterations = int(config["morph_iterations"])
    cfg.empty_frame_pixel_threshold = int(config["empty_frame_pixel_threshold"])
    cfg.laplacian_kernel_size = int(config.get("laplacian_kernel_size", 3))
    cfg.science_config_json = science_json
    cfg.science_config_json_size = len(science_json)
    return cfg


def native_objects(api: ApiV2, frames: np.ndarray, backgrounds: np.ndarray, owner: np.ndarray,
                   cfg: ConfigV2, pixel_to_micron: float) -> dict[tuple[int, int], dict]:
    context = CTX()
    error = C.create_string_buffer(512)
    if api.create_context(C.byref(context), error, len(error)) != 0:
        raise RuntimeError(f"create_context: {error.value.decode()}")
    objects: dict[tuple[int, int], dict] = {}
    try:
        for index in range(frames.shape[0]):
            frame_view, _frame = image_view(frames[index])
            background_view, _background = image_view(backgrounds[int(owner[index])])
            roi = Roi(C.sizeof(Roi), 0, 0, frames.shape[2], frames.shape[1])
            slots = (Metrics * MAX_OBJECTS)()
            buffer = ObjectBuffer(C.sizeof(ObjectBuffer), MAX_OBJECTS, 0, 0, slots)
            status = api.process_objects(context, C.byref(frame_view), C.byref(background_view),
                                         C.byref(cfg), C.byref(roi), pixel_to_micron, None,
                                         C.byref(buffer), error, len(error))
            if status != 0:
                raise RuntimeError(f"frame {index}: process_objects {status}: {error.value.decode()}")
            for slot in range(buffer.count):
                metrics = slots[slot]
                objects[(index, metrics.object_id)] = {
                    key: getattr(metrics, attribute) for key, attribute in FIELDS.items()
                }
    finally:
        api.destroy_context(context)
    return objects


def field_matches(field: str, want, have, tolerance: float) -> bool:
    if field in INTEGER_FIELDS or isinstance(want, bool):
        return int(want) == int(have)
    if want is None:
        return have is None or (isinstance(have, float) and math.isnan(have))
    return abs(float(have) - float(want)) <= tolerance * max(1.0, abs(float(want)))


def compare(reference: dict, native: dict[tuple[int, int], dict]) -> tuple[list[str], int]:
    """Every reference object must come out of the core with identical
    core-owned fields. The reference records every invalid object but only the
    valid objects the host tracker assigned to a track (track_id >= 0), so a
    core-only object is a failure unless it is valid; its frame must still
    report the reference's object_count. Returns (failures, tracker-skipped)."""
    tolerance = compare_metrics.DEFAULT_NUMERIC_TOLERANCE
    remaining = dict(native)
    failures = []
    reference_counts: dict[int, int] = {}
    for record in reference["frames"]:
        key = (int(record["index"]), int(record["object_id"]))
        reference_counts[key[0]] = int(record["object_count"])
        got = remaining.pop(key, None)
        if got is None:
            failures.append(f"{key}: missing in native output")
            continue
        for field in FIELDS:
            if field in record and not field_matches(field, record[field], got[field], tolerance):
                failures.append(f"{key} {field}: native {got[field]!r} != gold {record[field]!r}")
    skipped = 0
    for key, got in remaining.items():
        if not got["is_valid"]:
            failures.append(f"{key}: native-only invalid object (the reference records every invalid object)")
        elif key[0] in reference_counts and got["object_count"] != reference_counts[key[0]]:
            failures.append(f"{key}: object_count {got['object_count']} != reference {reference_counts[key[0]]}")
        else:
            skipped += 1
    return failures, skipped


def run(core: Path, npz: Path, reference_path: Path) -> int:
    _library, api = load_api(core)
    reference = json.loads(reference_path.read_text(encoding="utf-8"))
    if int(reference.get("contract_version", 0)) != CONTRACT2:
        raise ValueError(f"{reference_path} is not a Contract-2 reference")
    with np.load(npz, allow_pickle=False) as archive:
        frames = np.asarray(archive["frames"])
        backgrounds = np.asarray(archive["backgrounds"])
        owner = np.asarray(archive["frame_background"])
        config = contract2_config(json.loads(str(archive["config_json"])))
        pixel_to_micron = float(archive["pixel_to_micron"])
    science = science_json(config)
    native = native_objects(api, frames, backgrounds, owner, kernel_config(config, science),
                            pixel_to_micron)
    failures, skipped = compare(reference, native)
    for failure in failures[:50]:
        print("FAIL", failure)
    print(f"native Contract-2 gold: {len(reference['frames'])} reference objects matched against "
          f"{len(native)} native objects ({skipped} valid objects not recorded by the host "
          f"tracker), {len(failures)} failure(s)")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--frames-npz", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--skip-if-missing-numpy", action="store_true",
                        help="exit 77 (ctest skip) instead of failing when numpy is not installed")
    args = parser.parse_args()
    if np is None:
        print("numpy is required", file=sys.stderr)
        return 77 if args.skip_if_missing_numpy else 2
    try:
        return run(args.core, args.frames_npz, args.reference)
    except (OSError, RuntimeError, KeyError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
