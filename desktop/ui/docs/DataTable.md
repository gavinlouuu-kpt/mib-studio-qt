---
category: Data
---

# DataTable

Rows of recorded facts: per-run statistics, linked cell records, the sample sheet, the experiment library and export previews. Ids are strings (`rowKey`), including u64 record ids. Mark counts and measurements `numeric` so they align right with tabular figures, and the identifying column `emphasis`. Put flags in a column as small `StateChip`s on the row they qualify.

The table never computes or fills in a value. A cell whose value is `null`, `undefined` or `""` shows as missing (an en dash, titled "Not recorded"); a recorded `0` stays `0`, and an empty list (no flags) renders as an empty cell. With no rows, `empty` says why ("No runs recorded yet.") instead of showing a blank grid.

Rows are selectable only when `onSelect` is given (click, Enter or Space). The selected row (`selectedKey`) is the inspected record: it uses the inspect colour, as `RecordIdentity` does, and is marked `aria-current`. Clicks and Enter or Space on a control inside a cell (a button, link or input) stay with that control and do not select the row. Use `density="compact"` for sheets and lists inside cards.

## Example

```tsx
<DataTable
  columns={[
    { key: "run", header: "Run", emphasis: true },
    { key: "condition", header: "Condition", cell: (r) => <ConditionSwatch condition={r.slot}>{r.condition}</ConditionSwatch> },
    { key: "n", header: "n valid", numeric: true },
    { key: "medianE", header: "Median E (kPa)", numeric: true },
    { key: "flags", header: "Flags", cell: (r) => r.flags.map((f) => <StateChip key={f} tone="blocked" size="small">{f}</StateChip>) },
  ]}
  rows={runs}
  rowKey={(r) => r.run}
  empty="No runs recorded in this experiment yet."
/>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (tables on the app pages):

- C · DataTable: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `5cac1104-ac28-4c1e-9b66-f5abc5b361b1`
- R2 · Overview: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `f1003b66-4b1c-48f7-96eb-ed08b68e3ec4`
- R3 · Inspect: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `dcdbaa6b-9790-4e69-bea4-936747f588b1`
- O3 · Experiment · Plan: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `14db68fb-c9d2-4e21-ae5a-bd4291fc6273`
