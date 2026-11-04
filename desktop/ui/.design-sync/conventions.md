# Building with @mib/ui (MIB Studio · YOFO Studio · YOFO Review)

These are instrument and analysis apps for deformability cytometry. Screens show what the backend reports, never values the UI made up.

## Setup

Wrap every screen in `AppShell` (it includes the root styles). Wrap anything outside a shell, such as a dialog or a standalone panel, in `UiRoot`. Without one of them, text falls back to the browser font.

```tsx
<AppShell brand="MIB Studio" tag="Imaging + cytometry"
  context={[{ label: "Source", value: "Camera 01 / EGrabber" }, { label: "Method", value: "Cell mechanics / v3" }]}
  stages={["Prepare", "Align", "Experiment", "Review"]} activeStage={2}
  status={<StateChip tone="confirmed">Run closed · Exp-0142_03 · 4:21</StateChip>}
  headerActions={<Button>Settings</Button>}>
  <Toolbar title="Run summary">
    <Button variant="selected">Run summary</Button>
    <Button variant="primary">Next sample</Button>
  </Toolbar>
  <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 1fr)", gap: "var(--mib-space-6)", padding: "0 var(--mib-space-6) var(--mib-space-6)" }}>
    <Card title="Outcome" subtitle="Backend-confirmed" status={<StateChip tone="confirmed">Writer closed · verified</StateChip>}>
      <FactList facts={[{ label: "Duration", value: "4 min 21 s" }, { label: "File", value: "Exp-0142_03_D1_cytoD_1.h5" }]} />
    </Card>
  </div>
</AppShell>
```

## The two products

- **Operation app** (brand "MIB Studio" or "YOFO Studio"): stages Prepare, Align, Experiment, Review. Quick look shows numbers and deltas only (`StatTile delta`). It never runs tests, fits or interpretations.
- **Review app** (brand "YOFO Review"): stages Open, Overview, Inspect, Populations, Compare, Export. It owns the analysis and never edits a recording.

## Styling idiom

Components carry their own styles. There are no utility classes. For your own layout glue, use plain CSS or inline styles with these tokens:

- **Surfaces:** `--mib-color-bg`, `--mib-color-band`, `--mib-color-card`, `--mib-color-tile`, `--mib-color-line`
- **Text:** `--mib-color-ink`, `--mib-color-sub`, `--mib-color-mute`
- **Accents:** `--mib-color-dark` (the primary action), `--mib-color-accent` and `--mib-color-accent-bg` (confirmed or selected), `--mib-color-inspect` and `--mib-color-inspect-bg` (the inspected record)
- **States:** `--mib-color-{confirmed,warn,blocked,unknown}` and `-bg`
- **Conditions:** `--mib-condition-1` … `--mib-condition-5`, or `conditionColor(n)`
- **Spacing:** `--mib-space-1` … `--mib-space-6` (4/8/12/16/18/24 px)
- **Shape and type:** `--mib-radius-sm`, `--mib-radius-md`, `--mib-font-family`, `--mib-font-size-{xs,sm,md,base,lg,title,brand,stat}`

## Rules the components assume

- Show backend states only through the kit's state tones: a `StateChip` (`confirmed`, `warn`, `blocked`, `unknown`, `unsupported`, `info`, `neutral`), or the `tone` of `FactList`, `StatTile`, `Stepper` and `Notice`, which use the same colours and meanings. Never colour a state by hand. Unknown is never shown as Pass. Write labels as "State · detail".
- Build a card as title, a one-line subtitle, chips, a `FactList`, then actions. Write missing values out ("Unknown · not reported").
- Use one `primary` Button per view. Clicking a stage only navigates.
- Show choices with `OptionGroup` (not a dropdown); choosing changes the draft and the primary Button applies it. Keep unavailable options visible with their reason.
- Put a view-wide backend state that needs attention in a `Notice` at the top of the view, with at most two actions.
- Condition colours never stand for states, and state colours never stand for conditions.
- Show ids (record, frame, run) as strings, and identify a record with `RecordIdentity`.
- Show rows of facts (statistics, records, sample sheets) in a `DataTable`. Pass missing values as `null`; the table shows them as missing.
- Show a cell crop with `CellImage` (or the live strip with `CellStrip`), passing only recorded images, contours and masks. Never draw a stand-in outline.
- Use `AreaDeformChart` for every area-vs-deformability plot. Pass E curves and records as data; the UI never computes E or statistics.

## Where the truth lives

- `styles.css` and its imports hold the tokens and component CSS.
- Each component has `<Name>.prompt.md` and `<Name>.d.ts`.
- `guidelines/ui-language.md` holds the vocabulary and the truth rules.
