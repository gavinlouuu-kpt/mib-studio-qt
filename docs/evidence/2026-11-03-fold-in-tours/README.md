# Guided tours: browser drive (2026-11-03)

Every MIB Studio fold-in tour (`desktop/src/tours/mib/entries/`) played end to end in headless
Chromium 134 (Playwright, 1440x900), light and dark colour schemes, against the real
`yofo-studio-server` (debug build of this branch, bridge ABI 50) with the mock camera and the
production-writer fixtures `fold_in_e2e` uses (`review_fixture --population 6000`).

**Evidence classes:** mock camera (folder camera, one 512x96 frame looped, fake GenApi node
map; never hardware), real backend (the server process and its native authorities), synthetic
data (production-writer HDF5 fixture). No rig, no camera, no grabber.

```bash
cd desktop && npm run build && cd ..
python3 scripts/tours/drive_tours.py        # ~5.5 min; defaults: server, dist, fixtures, out = this folder
```

**Result:** 12/12 tour runs PASS (6 tours x 2 schemes) and 6/6 extra scenarios PASS
(`manifest.json` `all_passed: true`). Per run: [`drive-log.txt`](drive-log.txt).

What the drive asserts:
- each action step waits (>= 2 s with no input) and advances only after the backend reply to the
  operator's command (Refresh, Write, Start, Stop, Create recipe, Open recording, Analyze
  population, Save workspace); times are in the log and the manifest;
- between action steps the page sends only reads (`fetch_*`, status and frame pulls): the tour
  itself sends no command;
- missing-target recovery (a `data-tour` removed): the step says why and offers Skip and Restart;
- the idle gate: action steps are refused while the camera acquires or an experiment runs;
- the "what's new" notice and the Tours list (from the notice and from Help) never start a tour;
- the feedback checkpoint, exported from the Tours list (`feedback/`).

Notes: the app has no dark theme; the dark scheme darkens only the tour popovers and panels. With
the mock camera's 10 ms frames, the E04 "first frame" step read "Already done" (the separate wait
is covered by unit tests). `contact-sheet.html` and `contact-sheet-{light,dark}.png` show every
screenshot.
