#!/usr/bin/env python3
"""Command line for the dot-grid tools.

Design registry (one entry per chip design; the app decodes against all of them):

  python3 dotgrid_cli.py register  design.dxf --id my-chip --name "My chip" --out-dir out/   # new design + mask
  python3 dotgrid_cli.py mask      my-chip design.dxf --out-dir out/                         # regenerate its mask
  python3 dotgrid_cli.py list                                                                # registered designs
  python3 dotgrid_cli.py check     [--cross-check]                                           # validate the registry
  python3 dotgrid_cli.py gold      [--write]                    # check this core against the codec gold reference

Pattern tools (CODEBOOK = a codebook.json path or a registered design id):

  python3 dotgrid_cli.py generate  design.dxf --seed 7 --out-dir out/      # ad-hoc codebook + layer, not registered
  python3 dotgrid_cli.py render    CODEBOOK --x 20000 --y 30000 --theta 12 --out view.png
  python3 dotgrid_cli.py decode    frame.png --um-per-px 0.293             # which design, which chip, where
  python3 dotgrid_cli.py mockdir   CODEBOOK out_dir --count 20             # frames for MIB Studio mock camera

The registry defaults to resources/defaults/dot_grid/registry.json (bundled
into the app); --registry points every command at another file.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dotgrid import CODEC_CONTRACT, Codebook, core_identity, render_view  # noqa: E402
from dotgrid.registry import Design, Registry, cross_check, decode_registry, sha256_file, today  # noqa: E402
from dotgrid.render import ViewPose  # noqa: E402

DEFAULT_REGISTRY = os.path.normpath(os.path.join(HERE, "..", "..", "resources", "defaults", "dot_grid", "registry.json"))


def _log(msg: str) -> None:
    print(msg, file=sys.stderr)


def _load_registry(path: str) -> Registry:
    return Registry.load(path) if os.path.exists(path) else Registry()


def _load_codebook(spec: str, registry_path: str) -> Codebook:
    """A codebook.json path, or the id of a registered design."""
    if os.path.exists(spec):
        return Codebook.load(spec)
    return _load_registry(registry_path).codebook(spec)


def _write_mask(cb, dots, doc, out_dir: str, formats: str) -> None:
    from dotgrid.pattern import write_dxf, write_dots_only_dxf, write_gds
    os.makedirs(out_dir, exist_ok=True)
    cb.save(os.path.join(out_dir, "codebook.json"))
    wanted = {f.strip() for f in formats.split(",") if f.strip()}
    if "csv" in wanted:
        np.savetxt(os.path.join(out_dir, "dots.csv"), dots, fmt="%d,%d,%.3f,%.3f", header="i,j,x_um,y_um", comments="")
    if "dxf" in wanted:
        write_dots_only_dxf(dots, cb, os.path.join(out_dir, "dotgrid_layer.dxf"))
    if "merged" in wanted:
        write_dxf(doc, dots, cb, os.path.join(out_dir, "design_with_dotgrid.dxf"))
    if "gds" in wanted:
        if write_gds(dots, cb, os.path.join(out_dir, "dotgrid_layer.gds")):
            _log("wrote GDS")
        else:
            _log("gdstk not installed: GDS skipped")


def _keepout(a):
    from dotgrid.pattern import KeepOut
    return KeepOut(channel_um=a.keepout_channel, dicing_um=a.keepout_dicing, wafer_edge_um=a.keepout_edge,
                   existing_marks_um=a.keepout_marks)


# ---------------------------------------------------------------- registry commands
def cmd_register(a):
    from dotgrid.pattern import generate_for_design
    reg = _load_registry(a.registry)
    # A local registry is merged on top of the bundled one by the app, so it must
    # not reuse the bundled ids or seeds either.
    known = Registry(reg.designs)
    if os.path.abspath(a.registry) != DEFAULT_REGISTRY and os.path.exists(DEFAULT_REGISTRY):
        known = Registry(reg.designs + [x for x in Registry.load(DEFAULT_REGISTRY).designs
                                        if not any(y.id == x.id and y.seed == x.seed for y in reg.designs)])
    if known.find(a.id):
        sys.exit(f"error: design id '{a.id}' is already registered (use a new id, or `mask` to regenerate it)")
    digest = sha256_file(a.dxf)
    clash = known.find_source(digest)
    if clash and not a.allow_same_source:
        sys.exit(f"error: this DXF is already registered as '{clash.id}'. Use `mask {clash.id} ...` to "
                 "regenerate its mask, or --allow-same-source for a deliberate second pattern.")
    seed = a.seed if a.seed is not None else known.next_seed()
    if any(x.seed == seed for x in known.designs):
        sys.exit(f"error: seed {seed} is already used by '{next(x.id for x in known.designs if x.seed == seed)}'")
    geometries = {(x.dot_diameter_um, round(x.displacement_um / x.pitch_um, 9)) for x in known.designs}
    if geometries and (a.dot, round(a.shift / a.pitch, 9)) not in geometries:
        _log(f"warning: dot {a.dot} um / shift-to-pitch {a.shift / a.pitch:.3f} is a new dot geometry. "
             "Every distinct geometry adds one dot-detection pass to each live decode (~2x per frame); "
             "keep the registered geometry unless the process needs otherwise.")
    ko = _keepout(a)
    cb, dots, doc = generate_for_design(a.dxf, seed, pitch_um=a.pitch, dot_diameter_um=a.dot,
                                        displacement_um=a.shift, keepout=ko, design_name=a.id,
                                        design_scale=a.design_scale, progress=_log)
    design = Design.from_codebook(
        cb, a.id, a.name or a.id, revision=a.revision or today(), status="active",
        codec_contract=CODEC_CONTRACT, encoder=core_identity(),
        keepout={"channel_um": ko.channel_um, "dicing_um": ko.dicing_um, "wafer_edge_um": ko.wafer_edge_um,
                 "existing_marks_um": ko.existing_marks_um},
        source={"file": os.path.basename(a.dxf), "sha256": digest}, author=a.author,
        registered=today(), notes=a.notes)
    try:
        reg.add(design)
    except ValueError as e:
        sys.exit(f"error: {e}")
    if not a.skip_cross_check:
        problems = cross_check(Registry(known.designs + [design]), a.id, views=a.views, progress=_log)
        if problems:
            sys.exit("error: cross-design check failed, registry NOT updated:\n  " + "\n  ".join(problems))
    _write_mask(cb, dots, doc, a.out_dir, a.formats)
    reg.save(a.registry)
    print(json.dumps({"registered": a.id, "seed": seed, "dots": int(len(dots)), "columns": cb.columns,
                      "rows": cb.rows, "chips": [c.name for c in cb.chips], "registry": a.registry,
                      "mask_dir": a.out_dir}, indent=1))
    _log(f"\nNext: send {a.out_dir}/dotgrid_layer.gds to mask fabrication and commit {a.registry} "
         "(the app picks the design up from the bundled registry, or from dot_grid.registry_path).")


def cmd_mask(a):
    from dotgrid.pattern import KeepOut, generate_for_design
    reg = _load_registry(a.registry)
    design = reg.find(a.id)
    if design is None:
        sys.exit(f"error: design '{a.id}' is not registered in {a.registry}")
    if not design.supported:
        sys.exit(f"error: '{a.id}' was made with codec contract {design.codec_contract}; this generator "
                 f"implements contract {CODEC_CONTRACT}. Use the codec core of that contract.")
    digest = sha256_file(a.dxf)
    expected = design.source.get("sha256")
    if expected and expected != digest and not a.force:
        sys.exit(f"error: {a.dxf} is not the DXF '{a.id}' was registered from (sha256 differs). A changed "
                 "layout needs a new revision: register it under a new id, or pass --force.")
    ko = KeepOut(**design.keepout) if design.keepout else KeepOut()
    cb, dots, doc = generate_for_design(a.dxf, design.seed, pitch_um=design.pitch_um,
                                        dot_diameter_um=design.dot_diameter_um,
                                        displacement_um=design.displacement_um, keepout=ko,
                                        design_name=design.id, design_scale=design.design_scale, progress=_log)
    if (cb.columns, cb.rows) != (design.columns, design.rows):
        sys.exit(f"error: lattice {cb.columns}x{cb.rows} differs from the registered {design.columns}x{design.rows}")
    _write_mask(cb, dots, doc, a.out_dir, a.formats)
    print(json.dumps({"design": design.id, "seed": design.seed, "dots": int(len(dots)), "mask_dir": a.out_dir}))


def cmd_list(a):
    reg = _load_registry(a.registry)
    if a.json:
        print(json.dumps([{"id": x.id, "name": x.name, "revision": x.revision, "status": x.status,
                           "codec_contract": x.codec_contract, "supported": x.supported,
                           "seed": x.seed, "pitch_um": x.pitch_um, "chips": len(x.chips)} for x in reg.designs], indent=1))
        return
    print(f"{'id':<28} {'seed':>5} {'codec':>5} {'rev':<11} {'status':<8} {'pitch':>6} {'chips':>5}  name")
    for x in sorted(reg.designs, key=lambda d: d.seed):
        codec = str(x.codec_contract) + ("" if x.supported else "!")
        print(f"{x.id:<28} {x.seed:>5} {codec:>5} {x.revision:<11} {x.status:<8} {x.pitch_um:>6g} {len(x.chips):>5}  {x.name}")


def cmd_check(a):
    reg = Registry.load(a.registry)  # raises on duplicate ids/seeds or bad geometry
    for x in reg.designs:
        if x.supported:
            reg.codebook(x.id)  # generation itself checks delta-window uniqueness
        else:
            _log(f"note: '{x.id}' uses codec contract {x.codec_contract}, not implemented by this tool")
    _log(f"{a.registry}: {len(reg.designs)} design(s), ids and seeds unique, codebooks regenerate")
    if a.cross_check:
        problems = []
        for x in reg.designs:
            problems += cross_check(reg, x.id, views=a.views, progress=_log)
        if problems:
            sys.exit("cross-design check failed:\n  " + "\n  ".join(problems))
        _log("cross-design check passed")


def cmd_gold(a):
    from dotgrid import gold
    path = a.path or os.path.join(HERE, "gold", f"codec-contract{CODEC_CONTRACT}.json")
    if a.write:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(gold.build_reference(), f, indent=1)
            f.write("\n")
        _log(f"wrote {path} - commit it only in a PR labelled gold-reference-change")
    with open(path, encoding="utf-8") as f:
        ref = json.load(f)
    if ref["codec_contract"] != CODEC_CONTRACT:
        sys.exit(f"error: {path} is contract {ref['codec_contract']}; this core implements {CODEC_CONTRACT}")
    from dotgrid.codebook import generate_codebook
    problems = gold.check_encode(ref, lambda s, c, r: generate_codebook(s, c, r, pitch_um=30.0,
                                                                        dot_diameter_um=12.0, displacement_um=5.0))
    problems += gold.check_reference_decode(ref, progress=_log if a.verbose else None)
    if problems:
        sys.exit("gold reference check FAILED:\n  " + "\n  ".join(problems))
    print(f"gold reference {os.path.basename(path)}: encode exact, "
          f"{len(ref['decode']['cases'])} decode cases met")


# ---------------------------------------------------------------- pattern tools
def cmd_generate(a):
    from dotgrid.pattern import generate_for_design
    cb, dots, doc = generate_for_design(a.dxf, a.seed, pitch_um=a.pitch, dot_diameter_um=a.dot,
                                        displacement_um=a.shift, keepout=_keepout(a),
                                        design_name=os.path.basename(a.dxf), design_scale=a.design_scale,
                                        progress=_log)
    _write_mask(cb, dots, doc, a.out_dir, a.formats)
    print(json.dumps({"dots": int(len(dots)), "columns": cb.columns, "rows": cb.rows, "chips": [c.name for c in cb.chips]}))


def _pose(a):
    return ViewPose(centre_um=(a.x, a.y), theta_deg=a.theta, um_per_px=a.um_per_px, mirrored=a.mirror,
                    width=a.width, height=a.height)


def cmd_render(a):
    import cv2
    cb = _load_codebook(a.codebook, a.registry)
    img = render_view(cb, _pose(a), seed=a.seed)
    cv2.imwrite(a.out, img)
    print(a.out)


def cmd_decode(a):
    import cv2
    img = cv2.imread(a.image, cv2.IMREAD_GRAYSCALE)
    if a.codebook:
        from dotgrid import decode_image
        r, design = decode_image(_load_codebook(a.codebook, a.registry), img, a.um_per_px), None
    else:
        r, design = decode_registry(Registry.load(a.registry), img, a.um_per_px)
    out = {k: v for k, v in r.__dict__.items() if k not in ("pixel_to_wafer", "lattice_px", "debug")}
    out["design"] = design
    if r.pixel_to_wafer is not None:
        out["pixel_to_wafer"] = r.pixel_to_wafer.tolist()
    print(json.dumps(out, indent=1))


def cmd_mockdir(a):
    import cv2
    cb = _load_codebook(a.codebook, a.registry)
    os.makedirs(a.out_dir, exist_ok=True)
    rng = np.random.default_rng(a.seed)
    truth = []
    for k in range(a.count):
        pose = ViewPose(centre_um=(a.x + float(rng.uniform(-a.jitter, a.jitter)), a.y + float(rng.uniform(-a.jitter, a.jitter))),
                        theta_deg=a.theta + float(rng.uniform(-2, 2)), um_per_px=a.um_per_px, mirrored=a.mirror,
                        width=a.width, height=a.height)
        img = render_view(cb, pose, seed=k)
        name = f"frame_{k:04d}.png"
        cv2.imwrite(os.path.join(a.out_dir, name), img)
        truth.append({"file": name, "design": cb.design_name, "centre_um": pose.centre_um,
                      "theta_deg": pose.theta_deg, "mirrored": pose.mirrored})
    with open(os.path.join(a.out_dir, "truth.json"), "w") as f:
        json.dump(truth, f, indent=1)
    print(a.out_dir)


def _add_mask_args(g):
    g.add_argument("--pitch", type=float, default=30.0)
    g.add_argument("--dot", type=float, default=12.0)
    g.add_argument("--shift", type=float, default=5.0)
    g.add_argument("--keepout-channel", type=float, default=50.0)
    g.add_argument("--keepout-dicing", type=float, default=200.0)
    g.add_argument("--keepout-edge", type=float, default=3000.0)
    g.add_argument("--keepout-marks", type=float, default=300.0)
    g.add_argument("--design-scale", type=float, default=1.0,
                   help="pre-enlargement already applied to the DXF (e.g. 1.015 for PDMS shrinkage); recorded only")


def _add_formats(g):
    g.add_argument("--formats", default="gds,dxf,csv",
                   help="comma list of outputs: gds, dxf (dots only), merged (design + dots DXF), csv")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--registry", default=DEFAULT_REGISTRY, help="registry.json (default: the bundled one)")
    sp = p.add_subparsers(dest="cmd", required=True)

    g = sp.add_parser("register", help="add a design to the registry and generate its mask layer")
    g.add_argument("dxf"); g.add_argument("--id", required=True, help="lowercase slug, e.g. sorter-v2")
    g.add_argument("--name", default=""); g.add_argument("--revision", default="", help="default: today")
    g.add_argument("--author", default=""); g.add_argument("--notes", default="")
    g.add_argument("--seed", type=int, default=None, help="default: next free seed (never reuse one)")
    g.add_argument("--out-dir", required=True)
    g.add_argument("--views", type=int, default=6, help="synthetic views per design in the cross-check")
    g.add_argument("--skip-cross-check", action="store_true")
    g.add_argument("--allow-same-source", action="store_true")
    _add_mask_args(g); _add_formats(g); g.set_defaults(func=cmd_register)

    m = sp.add_parser("mask", help="regenerate the mask layer of a registered design")
    m.add_argument("id"); m.add_argument("dxf"); m.add_argument("--out-dir", required=True)
    m.add_argument("--force", action="store_true", help="accept a DXF whose sha256 differs from the registered one")
    _add_formats(m); m.set_defaults(func=cmd_mask)

    ls = sp.add_parser("list", help="list registered designs")
    ls.add_argument("--json", action="store_true"); ls.set_defaults(func=cmd_list)

    c = sp.add_parser("check", help="validate the registry (unique ids/seeds, codebooks regenerate)")
    c.add_argument("--cross-check", action="store_true", help="also run the synthetic discrimination check")
    c.add_argument("--views", type=int, default=4); c.set_defaults(func=cmd_check)

    gd = sp.add_parser("gold", help="check this core against its codec contract's frozen gold reference")
    gd.add_argument("--path", default="", help="default: gold/codec-contract<N>.json next to this tool")
    gd.add_argument("--write", action="store_true", help="regenerate the reference (gold-reference-change PRs only)")
    gd.add_argument("-v", "--verbose", action="store_true"); gd.set_defaults(func=cmd_gold)

    g = sp.add_parser("generate", help="ad-hoc codebook + layer for an explicit seed (not registered)")
    g.add_argument("dxf"); g.add_argument("--seed", type=int, required=True); g.add_argument("--out-dir", required=True)
    _add_mask_args(g); _add_formats(g); g.set_defaults(func=cmd_generate)

    for name, fn in (("render", cmd_render), ("mockdir", cmd_mockdir)):
        r = sp.add_parser(name); r.add_argument("codebook", help="codebook.json or registered design id")
        if name == "render":
            r.add_argument("--out", required=True)
        else:
            r.add_argument("out_dir"); r.add_argument("--count", type=int, default=20); r.add_argument("--jitter", type=float, default=200.0)
        r.add_argument("--x", type=float, required=True); r.add_argument("--y", type=float, required=True)
        r.add_argument("--theta", type=float, default=0.0); r.add_argument("--um-per-px", type=float, default=0.293)
        r.add_argument("--mirror", action="store_true"); r.add_argument("--width", type=int, default=1920)
        r.add_argument("--height", type=int, default=1200); r.add_argument("--seed", type=int, default=0)
        r.set_defaults(func=fn)
    d = sp.add_parser("decode", help="decode a frame against the whole registry (or one codebook)")
    d.add_argument("image"); d.add_argument("--codebook", default="", help="restrict to one codebook.json / design id")
    d.add_argument("--um-per-px", type=float, default=0.293); d.set_defaults(func=cmd_decode)
    a = p.parse_args(argv)
    a.func(a)


if __name__ == "__main__":
    main()
