"""Bounded CAD adapter. Does not change the frozen MIB dot-grid encoding."""

from __future__ import annotations
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
import ezdxf
from ezdxf import bbox
from ezdxf.math import Matrix44
import numpy as np
try:
    from .placement import Placement, selected_areas
    from .batched import MAX_NODES, supported, pattern_batches, spool_pattern, write_dxf_stream, write_gds_stream, dot_preview
    from .regions import regions, outside_regions
    from .cad_import import convert, flatten_regions
    from .domains import candidates, matching_groups
except ImportError:
    from placement import Placement, selected_areas
    from batched import MAX_NODES, supported, pattern_batches, spool_pattern, write_dxf_stream, write_gds_stream, dot_preview
    from regions import regions, outside_regions
    from cad_import import convert, flatten_regions
    from domains import candidates, matching_groups

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/dot_grid"))
from dotgrid import core_identity, CODEC_CONTRACT
from dotgrid.codebook import generate_codebook, Chip
from dotgrid.registry import Registry, Design, cross_check
from dotgrid.pattern import (
    load_design,
    chip_table,
    build_pattern,
    KeepOut,
    write_dxf,
    write_gds,
)

MAX_ENTITIES = 20000
MAX_EXTENT_UM = 150000
BUNDLED = ROOT / "resources/defaults/dot_grid/registry.json"


def digest(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def json_write(path, obj):
    Path(path).write_text(
        json.dumps(obj, indent=2, allow_nan=False) + "\n", encoding="utf-8"
    )


def inspect(source: Path, units: str, out: Path) -> dict:
    if units not in ("um", "mm", "inch"):
        raise ValueError("Choose drawing units: um, mm or inch")
    out.mkdir(parents=True, exist_ok=True)
    source_hash = digest(source)
    converted = source
    converter = None
    if source.suffix.lower() == ".dwg":
        converted, converter = convert(source, out)
    doc = ezdxf.readfile(converted)
    if doc.audit().has_errors:
        raise ValueError(
            "CAD contains structural errors; repair in the authoring application"
        )
    converted_regions = flatten_regions(doc)
    all_entities = list(doc.modelspace())
    annotations = [e for e in all_entities if e.dxftype() in {"TEXT", "MTEXT"} and e.dxf.layer.upper() != "NO_DOT"]
    entities = [e for e in all_entities if e not in annotations]
    if not entities or len(entities) > MAX_ENTITIES:
        raise ValueError(f"Drawing must contain 1–{MAX_ENTITIES} model-space entities")
    allowed = {"LINE", "ARC", "CIRCLE", "LWPOLYLINE"}
    unsupported = sorted({e.dxftype() for e in entities} - allowed)
    if unsupported:
        raise ValueError(
            "Unsupported CAD entities: "
            + ", ".join(unsupported)
            + ". Explode blocks/convert curves in CAD first."
        )
    for e in entities:
        if e.dxf.layer == "DOTGRID":
            raise ValueError(
                "Drawing already contains DOTGRID geometry; upload the original device design"
            )
        if e.dxftype() == "LWPOLYLINE":
            if len(e) < 2 or e.has_arc or e.has_width:
                raise ValueError(
                    "Polylines must have >=2 vertices, no bulges and no widths; flatten curves first"
                )
            if len(e) == 4 and not e.closed:
                raise ValueError(
                    "Open four-vertex polylines are unsupported by the current keep-out engine"
                )
            if float(e.dxf.elevation) != 0:
                raise ValueError("Only planar XY drawings are supported")
        if e.dxf.hasattr("extrusion") and tuple(e.dxf.extrusion) != (0, 0, 1):
            raise ValueError("Non-XY extrusion is unsupported")
        if e.dxf.hasattr("thickness") and float(e.dxf.thickness) != 0:
            raise ValueError("3D thickness is unsupported")
        for attr in ("start", "end", "center"):
            if e.dxf.hasattr(attr) and abs(e.dxf.get(attr).z) > 1e-7:
                raise ValueError("Only planar XY drawings are supported")
    ext = bbox.extents(entities)
    vals = [ext.extmin.x, ext.extmin.y, ext.extmax.x, ext.extmax.y]
    factor = {"um": 1.0, "mm": 1000.0, "inch": 25400.0}[units]
    if not all(math.isfinite(v) for v in vals):
        raise ValueError("Invalid drawing bounds")
    width, height = (vals[2] - vals[0]) * factor, (vals[3] - vals[1]) * factor
    if min(width, height) < 300 or max(width, height) > MAX_EXTENT_UM:
        raise ValueError("Drawing extent must be 300–150000 um on each axis")
    columns, rows = math.ceil(width / 30) + 1, math.ceil(height / 30) + 1
    # Translate only negative coordinates, explicitly recorded in the map transform.
    # Use a tight zero-based mask domain for all drawings; camera results use this frame.
    offset = [-vals[0] * factor, -vals[1] * factor]
    transform = Matrix44.scale(factor) @ Matrix44.translate(*offset, 0)
    for e in all_entities:
        e.transform(transform)
    doc.header["$INSUNITS"] = 13
    doc.header["$EXTMIN"] = (0, 0, 0)
    doc.header["$EXTMAX"] = (width, height, 0)
    areas = regions(doc) if any(e.dxf.layer.upper() == "NO_DOT" for e in entities) else []
    choices, open_paths = candidates(doc)
    open_fragments = len(open_paths)
    target = out / "normalized.dxf"
    doc.saveas(target)
    # Numeric preview paths only: never expose embedded CAD XML, scripts or URLs.
    paths = []
    no_dot_paths = []
    for e in entities:
        typ = e.dxftype()
        if typ == "LINE":
            p = [(e.dxf.start.x, e.dxf.start.y), (e.dxf.end.x, e.dxf.end.y)]
        elif typ == "LWPOLYLINE":
            p = list(e.get_points("xy"))
            if e.closed:
                p.append(p[0])
        else:
            start, end = (
                (0, 360) if typ == "CIRCLE" else (e.dxf.start_angle, e.dxf.end_angle)
            )
            if end <= start:
                end += 360
            angles = np.linspace(math.radians(start), math.radians(end), 129 if typ == "CIRCLE" and e.dxf.radius > 1000 else 17)
            p = [
                (
                    e.dxf.center.x + e.dxf.radius * math.cos(a),
                    e.dxf.center.y + e.dxf.radius * math.sin(a),
                )
                for a in angles
            ]
        path = [[round(float(x), 3), round(float(y), 3)] for x, y in p]
        paths.append(path)
        if e.dxf.layer.upper() == "NO_DOT":
            no_dot_paths.append(path)
    # Preview bounded separately from parser and source limits.
    if sum(len(p) for p in paths) > 200000:
        raise ValueError("Preview exceeds 200,000 vertices; simplify drawing")
    result = {
        "source_sha256": source_hash,
        "normalized_sha256": digest(target),
        "source_units": units,
        "transform": {"scale_to_um": factor, "translation_um": offset},
        "extent_um": [width, height],
        "entity_count": len(entities),
        "no_dot_regions": len(areas),
        "region_candidates": choices,
        "selected_regions": [],
        "open_fragments": open_fragments,
        "open_paths": open_paths,
        "matching_groups": matching_groups(choices),
        "converted_regions": converted_regions,
        "annotation_count": len(annotations),
        "generation_supported": supported(columns, rows),
        "no_dot_contract": 1,
        "placement_contract": 1,
        "chip_regions": [],
        "manual_chip_rectangles": [],
        "cut_clearance_um": 200,
        "converter": converter,
        "paths": paths,
        "no_dot_paths": no_dot_paths,
        "warnings": [
            "Preview uses mask coordinates in micrometres. Confirm orientation and dimensions.",
            "Filled NO_DOT areas exclude entire dots with 50 µm edge clearance. Confirm every channel, chamber and port is included; this is not fabrication approval.",
        ],
    }
    if open_fragments:
        result["warnings"].append(f"{open_fragments} open boundary fragments: no interiors were guessed. Check these before confirming coverage.")
    if choices:
        result["warnings"].append("Select closed no-dot areas in the preview. These are geometry candidates, not automatically identified fluid channels.")
    if not supported(columns, rows):
        result["warnings"].append("Full drawing imported for review; generation exceeds the current capacity. No mask is generated from this full wafer yet.")
    header_units = {13: "um", 4: "mm", 1: "inch"}.get(ezdxf.readfile(converted).units)
    if header_units and header_units != units:
        result["warnings"].append(f"Drawing header says {header_units}; you selected {units}. Verify a known physical dimension.")
    if len(json.dumps(result)) > 15 * 1024**2:
        raise ValueError("Preview metadata exceeds job limit; simplify drawing")
    json_write(out / "inspection.json", result)
    return result


def generate(source: Path, meta: dict, out: Path) -> dict:
    inspection = meta["inspection"]
    if digest(source) != inspection["normalized_sha256"]:
        raise ValueError("Normalized CAD checksum mismatch; inspect again")
    seed = meta["seed"]
    if type(seed) is not int or seed < 8:
        raise ValueError("Seed must be centrally allocated above bundled reservations")
    known = Registry.load(str(BUNDLED))
    for entry in meta["designs"]:
        known.add(Design.from_dict(entry))
    if known.find(meta["id"]) or any(d.seed == seed for d in known.designs):
        raise ValueError("Design identity or seed already registered")
    try:
        placement = Placement(inspection)
    except ValueError as exc:
        raise ValueError(f"CAD_PLACEMENT_INVALID: {exc}") from exc
    doc, pts, chips, marks, wafer = load_design(str(source))
    xmin,ymin,xmax,ymax = placement.bounds
    w,h = xmax-xmin,ymax-ymin
    cols, rows = math.ceil(w / 30) + 1, math.ceil(h / 30) + 1
    if not supported(cols, rows):
        raise ValueError("Job node limit exceeded")
    cb = generate_codebook(
        seed,
        cols,
        rows,
        pitch_um=30,
        dot_diameter_um=12,
        displacement_um=5,
        origin_um=(xmin, ymin),
        chips=[Chip(f"chip-{n+1}",*p.bounds) for n,p in enumerate(placement.polys)],
        design_name=meta["id"],
        design_scale=1.0,
    )
    ko = KeepOut(
        channel_um=50, dicing_um=200, wafer_edge_um=3000, existing_marks_um=300
    )
    areas = regions(doc) if inspection.get("no_dot_regions",0)>0 else []
    areas += selected_areas(inspection)
    if not areas: raise ValueError("Confirm fluid exclusions before generation")
    out.mkdir(parents=True, exist_ok=True)
    dots = spool_pattern(out / "dots.bin", pattern_batches(
        cb, pts, [], marks, None, ko, areas, (w,h),
        ko.channel_um + cb.dot_diameter_um/2 + (0.1 if inspection.get("selected_regions") else 0), placement, placement.bounds,
    ))
    if len(dots) < 100:
        raise ValueError("Too few usable dots after keep-outs")
    chip_counts = placement.counts(dots)
    if min(chip_counts) < 100:
        raise ValueError("CAD_CHIP_EMPTY: A selected chip has fewer than 100 dots after exclusions; review its boundary and fluid regions")
    design = Design.from_codebook(
        cb,
        meta["id"],
        meta["name"],
        codec_contract=CODEC_CONTRACT,
        encoder=core_identity(),
        source={
            "sha256": inspection["source_sha256"],
            "normalized_sha256": inspection["normalized_sha256"],
        },
        keepout=vars(ko),
        notes="Portal-generated; fabrication approval is separate. See manifest for source-to-mask transform.",
    )
    known.add(design)
    errors = cross_check(known, design.id, views=6)
    if errors:
        raise ValueError(
            "Cross-design discrimination failed; this allocated pattern cannot be registered"
        )
    out.mkdir(parents=True, exist_ok=True)
    # Review geometry is kept separate; source entities are never augmented with helper shapes.
    write_dxf_stream(doc, dots, cb, str(out / "design_with_dotgrid.dxf"))
    if not write_gds_stream(dots, cb, str(out / "dotgrid_layer.gds")):
        raise ValueError("GDS exporter unavailable")
    cb.save(str(out / "codebook.json"))
    Registry([design]).save(str(out / "registry.json"))
    manifest = {
        "version": 1,
        "design_id": design.id,
        "seed": seed,
        "codec_contract": CODEC_CONTRACT,
        "encoder": core_identity(),
        "source_sha256": inspection["source_sha256"],
        "normalized_sha256": inspection["normalized_sha256"],
        "source_to_mask": inspection["transform"],
        "mask_units": "um",
        "dot_count": len(dots),
        "placement_contract": 1,
        "chip_dot_counts": chip_counts,
        "cut_clearance_um": inspection.get("cut_clearance_um",200),
        "extent_um": [w, h],
        "fabrication_approved": False,
        "gds_contents": "dot-grid layer only, not device geometry",
        "cross_design_views": 6,
        "files": {
            n: digest(out / n)
            for n in [
                "design_with_dotgrid.dxf",
                "dotgrid_layer.gds",
                "codebook.json",
                "registry.json",
            ]
        },
    }
    json_write(out / "manifest.json", manifest)
    (out / "README.txt").write_text(
        "MIB Studio map package\nSet dot_grid.registry_path in config.json to registry.json.\nIf you already use a local registry, merge with duplicate-id/seed validation; do not overwrite it.\nMask coordinates are micrometres; source-to-mask transform is in manifest.json.\nThis enables localization; it does not add a CAD-overlay viewer.\nSynthetic checks are not real-wafer validation or fabrication approval.\n",
        encoding="utf-8",
    )
    with zipfile.ZipFile(out / "mib-map.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for n in ["registry.json", "codebook.json", "manifest.json", "README.txt"]:
            z.write(out / n, n)
    return {"entry": design.to_dict(), "manifest": manifest, "preview": {**dot_preview(dots, (w,h), [(p.centroid.x,p.centroid.y) for p in placement.polys], placement.bounds), "chip_dot_counts": chip_counts}}
