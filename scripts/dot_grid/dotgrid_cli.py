#!/usr/bin/env python3
"""Command line for the dot-grid tools.

  python3 dotgrid_cli.py generate  design.dxf --seed 7 --out-dir out/      # codebook + DOTGRID layer
  python3 dotgrid_cli.py render    codebook.json --x 20000 --y 30000 --theta 12 --out view.png
  python3 dotgrid_cli.py decode    codebook.json frame.png --um-per-px 0.293
  python3 dotgrid_cli.py mockdir   codebook.json out_dir --count 20            # frames for MIB Studio mock camera
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dotgrid import Codebook, decode_image, generate_codebook, render_view  # noqa: E402
from dotgrid.render import ViewPose  # noqa: E402


def cmd_generate(a):
    from dotgrid.pattern import KeepOut, generate_for_design, write_dxf, write_dots_only_dxf, write_gds
    ko = KeepOut(channel_um=a.keepout_channel, dicing_um=a.keepout_dicing, wafer_edge_um=a.keepout_edge)
    cb, dots, doc = generate_for_design(a.dxf, a.seed, pitch_um=a.pitch, dot_diameter_um=a.dot,
                                        displacement_um=a.shift, keepout=ko,
                                        design_name=os.path.basename(a.dxf), design_scale=a.design_scale,
                                        progress=lambda m: print(m, file=sys.stderr))
    os.makedirs(a.out_dir, exist_ok=True)
    cb.save(os.path.join(a.out_dir, "codebook.json"))
    formats = {f.strip() for f in a.formats.split(",") if f.strip()}
    if "csv" in formats:
        np.savetxt(os.path.join(a.out_dir, "dots.csv"), dots, fmt="%d,%d,%.3f,%.3f", header="i,j,x_um,y_um", comments="")
    if "dxf" in formats:
        write_dots_only_dxf(dots, cb, os.path.join(a.out_dir, "dotgrid_layer.dxf"))
    if "merged" in formats:
        write_dxf(doc, dots, cb, os.path.join(a.out_dir, "design_with_dotgrid.dxf"))
    if "gds" in formats:
        if write_gds(dots, cb, os.path.join(a.out_dir, "dotgrid_layer.gds")):
            print("wrote GDS", file=sys.stderr)
        else:
            print("gdstk not installed: GDS skipped", file=sys.stderr)
    print(json.dumps({"dots": int(len(dots)), "columns": cb.columns, "rows": cb.rows, "chips": [c.name for c in cb.chips]}))


def _pose(a, cb):
    return ViewPose(centre_um=(a.x, a.y), theta_deg=a.theta, um_per_px=a.um_per_px, mirrored=a.mirror,
                    width=a.width, height=a.height)


def cmd_render(a):
    import cv2
    cb = Codebook.load(a.codebook)
    img = render_view(cb, _pose(a, cb), seed=a.seed)
    cv2.imwrite(a.out, img)
    print(a.out)


def cmd_decode(a):
    import cv2
    cb = Codebook.load(a.codebook)
    img = cv2.imread(a.image, cv2.IMREAD_GRAYSCALE)
    r = decode_image(cb, img, a.um_per_px)
    out = {k: v for k, v in r.__dict__.items() if k not in ("pixel_to_wafer", "lattice_px", "debug")}
    if r.pixel_to_wafer is not None:
        out["pixel_to_wafer"] = r.pixel_to_wafer.tolist()
    print(json.dumps(out, indent=1))


def cmd_mockdir(a):
    import cv2
    cb = Codebook.load(a.codebook)
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
        truth.append({"file": name, "centre_um": pose.centre_um, "theta_deg": pose.theta_deg, "mirrored": pose.mirrored})
    with open(os.path.join(a.out_dir, "truth.json"), "w") as f:
        json.dump(truth, f, indent=1)
    print(a.out_dir)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("generate"); g.add_argument("dxf"); g.add_argument("--seed", type=int, required=True)
    g.add_argument("--out-dir", required=True); g.add_argument("--pitch", type=float, default=50.0)
    g.add_argument("--dot", type=float, default=12.0); g.add_argument("--shift", type=float, default=8.0)
    g.add_argument("--keepout-channel", type=float, default=60.0); g.add_argument("--keepout-dicing", type=float, default=200.0)
    g.add_argument("--keepout-edge", type=float, default=3000.0); g.add_argument("--design-scale", type=float, default=1.0)
    g.add_argument("--formats", default="gds,dxf,csv",
                   help="comma list of outputs: gds, dxf (dots only), merged (design + dots DXF), csv")
    g.set_defaults(func=cmd_generate)
    for name, fn in (("render", cmd_render), ("mockdir", cmd_mockdir)):
        r = sp.add_parser(name); r.add_argument("codebook")
        if name == "render":
            r.add_argument("--out", required=True)
        else:
            r.add_argument("out_dir"); r.add_argument("--count", type=int, default=20); r.add_argument("--jitter", type=float, default=200.0)
        r.add_argument("--x", type=float, required=True); r.add_argument("--y", type=float, required=True)
        r.add_argument("--theta", type=float, default=0.0); r.add_argument("--um-per-px", type=float, default=0.293)
        r.add_argument("--mirror", action="store_true"); r.add_argument("--width", type=int, default=1920)
        r.add_argument("--height", type=int, default=1200); r.add_argument("--seed", type=int, default=0)
        r.set_defaults(func=fn)
    d = sp.add_parser("decode"); d.add_argument("codebook"); d.add_argument("image")
    d.add_argument("--um-per-px", type=float, default=0.293); d.set_defaults(func=cmd_decode)
    a = p.parse_args(argv)
    a.func(a)


if __name__ == "__main__":
    main()
