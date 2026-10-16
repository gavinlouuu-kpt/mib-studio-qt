# UI language rules

Shared by MIB Studio, YOFO Studio (the operation app) and YOFO Review (the review app). The brand changes; components and words do not.

## Two products

- **Operation app**: stages Prepare, Align, Experiment, Review. It records truthfully and shows what the backend reports. Quick look (Experiment) is descriptive only: per-run numbers and deltas against an operator-chosen reference run. No tests, fits or conclusions on the instrument.
- **Review app**: stages Open, Overview, Inspect, Populations, Compare, Export. It owns analysis (populations, statistics, comparison) through the analysis worker and never edits a recording.

## Truth rules

- State comes from the backend. `StateChip` tones: confirmed, warn, blocked, unknown, unsupported, info, neutral. Unknown is never shown as Pass; Unsupported never blocks.
- Facts never guess. A missing source is written out ("Unknown · not reported").
- Navigation never actuates hardware. Only an explicit button press acts.
- Requested values and readbacks are shown side by side; a value is current only after readback.

## Vocabulary

| Word | Meaning |
|---|---|
| Experiment | a set of runs planned from one sample sheet |
| Run | one acquisition = one HDF5 file |
| Sample | a sample-sheet row: condition, unit, concentration |
| Record | one measured object: record · frame · object (ids are strings) |
| Population | polygon AND E range in the shell; richer gates live in the worker |
| Flag | a qualifier on a number: loss, low n, E outside LUT, legacy |
| Method | approved acquisition settings; overrides are marked |
| Calibration record | applied pixel → µm with age and identity binding |

## Colour

State colours and condition colours never mix. Conditions use slots 1–5 in sample-sheet order (`ConditionSwatch`, `conditionColor`); slot 5 is calibration / reference. Interface accents: teal = confirmed / selected, blue = inspected record, dark = the primary action.

## Layout

`AppShell` → `Toolbar` (sub-views, then actions, title at right) → a grid of `Card`s. One primary action per view.
