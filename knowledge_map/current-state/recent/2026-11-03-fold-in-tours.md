## 2026-11-03 — Guided tours (Driver.js) for the fold-in features

MIB Studio gains a Driver.js 1.3.1 guided-tour module (`desktop/src/tours/`, shared with
the ZU15EG Studio patch) and six tours, one per proven fold-in goal group: E01 discovery,
E02 camera features, E04/E06 runs and accounting, I02 recipes, F04/F05/F08 population
analysis and I12/F12 workspace save. Each has a click-through on the real views, before
and after flowcharts and an evidence panel (demonstrated / tested / uncertain). Opened
from Help ▸ "Tours and what's new" or the workspace "Tours" button; never auto-started;
the tour sends no command. A demonstration layer: it advances no fold-in goal. How-to:
`docs/howto/adding-a-tour.md`; screenshots `docs/evidence/2026-11-03-fold-in-tours/`.
See [[architecture/Desktop-Shell]].

The browser drive (`scripts/tours/drive_tours.py`) acts as the operator through the real Driver.js
overlay and found three tour defects, fixed in the same change: the E02 write step was blocked by the
camera acquiring although a feature write needs a running camera (`needsCamera` on the action); five
steps whose text asks for a press on a read or preview control had that control disabled by the overlay
(`interactive` on the step); and the I02 create step highlighted only its button, not the form (a new
`recipe-profile` step and a `recipe-form` target). See [[architecture/Desktop-Shell]].
