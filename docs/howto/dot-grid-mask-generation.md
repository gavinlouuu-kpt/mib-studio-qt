# How-to: generate a dot-grid mask layer and test the decoder

Tooling lives in `scripts/dot_grid/` (Python 3, numpy, opencv-python; `ezdxf`
+ `scipy` for pattern generation, `gdstk` for GDS output). Design background:
[architecture/dot-grid-localization.md](../architecture/dot-grid-localization.md).

```bash
pip install numpy opencv-python-headless ezdxf scipy gdstk
cd scripts/dot_grid
```

## 1. Generate the layer from a design DXF

The generator reads the channel-layer DXF (units µm), samples every LINE /
ARC / CIRCLE / LWPOLYLINE, fills reservoir circles, finds the chip outlines
(large closed 4-point polylines) and the existing alignment marks (small
polylines), then keeps every lattice node that clears the keep-outs.

```bash
python3 dotgrid_cli.py generate Wafer_soRT.dxf --seed 7 --out-dir out/ \
    --pitch 30 --dot 12 --shift 5 --keepout-channel 50 --design-scale 1.015 \
    --formats gds,csv
```

Outputs in `out/`:

- `codebook.json` — the pattern definition **and** the chip table (`R<row>C<col>`
  from the inner dicing outlines). Ship it with the app (`dot_grid.codebook_path`)
  or copy it to `resources/defaults/dot_grid/`.
- `dotgrid_layer.gds` — layer 10, one `DOT` cell reference per dot. Merge it
  with the channel layer in your layout tool, or add `merged` to `--formats`
  for a DXF that contains both (large).
- `dots.csv` — `i, j, x_um, y_um` for every dot (audit / other tools).

A DWG must be converted first (`dwg2dxf` from LibreDWG ≥ 0.14 reads AutoCAD
2018 files).

The app and the mask must agree on `seed`, `pitch`, `dot`, `shift` and origin;
`columns`/`rows` only need to cover the wafer (a larger lattice is a superset).

## 2. Render synthetic camera views

```bash
python3 dotgrid_cli.py render out/codebook.json --x 40000 --y 52000 --theta 12 \
    --um-per-px 0.293 --mirror --out view.png
python3 dotgrid_cli.py decode out/codebook.json view.png --um-per-px 0.3
```

`--mirror` models viewing the bonded chip through the glass. The decode
prints the pose as JSON (`centre_um`, `theta_deg`, `um_per_px`, `mirrored`,
`chip`, votes, agreement).

## 3. Drive MIB Studio with mock frames

```bash
python3 dotgrid_cli.py mockdir out/codebook.json data/mock_frames/dotgrid \
    --x 40000 --y 52000 --theta 5 --um-per-px 0.293 --mirror --count 30
MIB_CAMERA_MODE=mock MIB_MOCK_CAMERA_DIR=data/mock_frames/dotgrid ./mib_studio_qt
```

Set `dot_grid.enabled: true` in `config.json` (or press **Wafer Grid** on the
Preview page). `truth.json` next to the frames records the rendered poses.

## 4. Tests

- `python3 scripts/dot_grid/test_dotgrid.py` (also registered as CTest
  `scripts.dot_grid_reference`; exits 77 = skipped when numpy/OpenCV are
  missing).
- `ctest --preset linux-backend-only-test -R dot_grid` runs the C++ codebook
  golden, decoder and service tests.
