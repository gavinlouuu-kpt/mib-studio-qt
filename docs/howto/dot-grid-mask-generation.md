# How-to: register a chip design, generate its dot-grid mask, test the decoder

Tooling lives in `scripts/dot_grid/` (Python 3, numpy, opencv-python; `ezdxf`
+ `scipy` for pattern generation, `gdstk` for GDS output). Design background:
[architecture/dot-grid-localization.md](../architecture/dot-grid-localization.md)
(see *Design registry*); decision: [ADR 0007](../decisions/0007-dot-grid-design-registry.md).

```bash
pip install numpy opencv-python-headless ezdxf scipy gdstk
cd scripts/dot_grid
```

Every chip design that carries a dot grid is registered once in
`resources/defaults/dot_grid/registry.json`. The registry gives each design a
unique seed, and the app decodes every frame against all registered designs,
so it reports **which design** is under the objective as well as the chip
(die) and the position. Commands below use the bundled registry; add
`--registry other.json` (before the command) to use another file.

## 1. Register a new design (developer workflow)

The input is the channel-layer DXF (units µm). A DWG must be converted first
(`dwg2dxf` from LibreDWG ≥ 0.14 reads AutoCAD 2018 files).

```bash
python3 dotgrid_cli.py register MyChip_v1.dxf --id mychip-v1 \
    --name "MyChip sorter, 25 um channels" --author "<you>" --out-dir out/mychip-v1 \
    --design-scale 1.015
```

What it does:

1. Refuses an `--id` that exists (ids are lowercase slugs: `[a-z0-9._-]`) and a
   DXF whose sha256 is already registered (use `mask` for that design, or
   `--allow-same-source` for a deliberate second pattern).
2. Allocates the next free seed (highest registered + 1; seeds are never
   reused, retired designs keep theirs).
3. Reads the DXF: samples every LINE / ARC / CIRCLE / LWPOLYLINE, fills
   reservoir circles, finds the chip outlines (large closed 4-point
   polylines, named `R<row>C<col>`) and the existing alignment marks, and
   keeps every lattice node that clears the keep-outs (`--keepout-channel 50`,
   `--keepout-dicing 200`, `--keepout-marks 300`, `--keepout-edge 3000` µm).
4. **Cross-check**: renders synthetic views of the new design and of every
   registered design, decodes them against the whole registry, and requires
   each view to be attributed to the right design (`--views`, default 6 per
   design). On failure the registry is left untouched.
5. Writes the mask outputs to `--out-dir` and appends the entry (geometry,
   chip table, keep-outs, DXF name + sha256, author, dates) to the registry.

Defaults are the validated Wafer_soRT geometry: `--pitch 30 --dot 12 --shift 5`
(µm). Keep them unless the process needs otherwise: designs sharing a dot
geometry cost nothing extra to decode, each new geometry adds a detection pass
to every live frame (`register` warns).

Outputs in `out/mychip-v1/`:

- `dotgrid_layer.gds` — layer 10, one `DOT` cell reference per dot (cells
  `DOT` → `DOTGRID`). Merge it with the channel layer in your layout tool, or
  add `merged` to `--formats` for a DXF that contains both (large). Send it
  for a chrome mask.
- `dotgrid_layer.dxf` — the dots only; `dots.csv` — `i, j, x_um, y_um` per dot.
- `codebook.json` — the full codebook (for archives and other tools; the app
  does not need it).

Then **commit `resources/defaults/dot_grid/registry.json` and open a PR**.
`python3 scripts/check_docs.py` is unaffected, but CTest
`processing.dot_grid_registry` and `scripts.dot_grid_reference` load the
bundled registry and fail on duplicates or malformed entries. Keep the bulky
mask outputs out of git (shared drive).

### Before a build ships: local registry on the bench PC

To try a design before a new build is installed, register it into a separate
file and point the app at it:

```bash
python3 dotgrid_cli.py --registry ~/dotgrid/registry.local.json register MyChip_v1.dxf --id mychip-v1 ...
```

and set `"dot_grid": {"registry_path": "<path>/registry.local.json"}` in
`config.json` (relative paths resolve against the config directory). The
app merges it with the bundled registry; an entry whose id or seed clashes
with a bundled design is skipped with a warning in the log. `register`
into a local file also checks the bundled registry, so it never hands out a
bundled id or seed. When the design later moves into the bundled registry,
copy its entry unchanged (same id and seed): the identical local entry is
then ignored.

## 2. Inspect, validate, regenerate

```bash
python3 dotgrid_cli.py list                       # id, seed, revision, status, pitch, chips
python3 dotgrid_cli.py check --cross-check        # unique ids/seeds, codebooks regenerate, discrimination
python3 dotgrid_cli.py mask mychip-v1 MyChip_v1.dxf --out-dir out/mychip-v1   # identical layer again
```

`mask` reuses the registered seed, geometry and keep-outs, and refuses a DXF
whose sha256 differs from the registered one: a changed layout is a new
revision, so register it under a new id (`mychip-v2`) and set the old
entry's `status` to `retired` in the JSON (retired designs still decode,
because their wafers still exist).

`generate DESIGN.dxf --seed N --out-dir out/` still produces an ad-hoc,
unregistered pattern for experiments; never fabricate one of those.

## 3. Render synthetic camera views and decode them

`render` and `mockdir` take a registered design id or a `codebook.json` path.

```bash
python3 dotgrid_cli.py render wafer-sort-rt --x 40000 --y 52000 --theta 12 \
    --um-per-px 0.293 --mirror --out view.png
python3 dotgrid_cli.py decode view.png --um-per-px 0.3
```

`--mirror` models viewing the bonded chip through the glass. `decode` tries
every registered design (or only `--codebook ID|PATH`) and prints the pose as
JSON (`design`, `chip`, `centre_um`, `theta_deg`, `um_per_px`, `mirrored`,
votes).

## 4. Drive MIB Studio with mock frames

```bash
python3 dotgrid_cli.py mockdir wafer-sort-rt data/mock_frames/dotgrid \
    --x 40000 --y 52000 --theta 5 --um-per-px 0.293 --mirror --count 30
MIB_CAMERA_MODE=mock MIB_MOCK_CAMERA_DIR=data/mock_frames/dotgrid ./mib_studio_qt
```

Set `dot_grid.enabled: true` in `config.json` (or press **Wafer Grid** on the
Preview page). The overlay shows the design name and chip next to the pose.
`truth.json` next to the frames records the rendered poses and design.

## 5. Tests

- `python3 scripts/dot_grid/test_dotgrid.py` (also CTest
  `scripts.dot_grid_reference`; exits 77 = skipped when numpy/OpenCV are
  missing; the end-to-end `register` test skips without ezdxf/scipy).
- `ctest --preset linux-backend-only-test -R dot_grid` runs the C++ codebook
  golden, decoder, registry and service tests.
