---
category: Data
---

# CellStrip

The live detected-cell strip on the Experiment · Live view: the crops the backend kept, newest first, each a `CellImage` with its record id. Clicking a crop pins it (`onPin` reports the record id); `pinnedId` draws the pinned crop in the inspect colour. Pinning never affects acquisition or the population.

The strip shows only crops the backend reported. It holds a bounded copy, so eviction is the backend's explicit decision, never a lost record. With no cells, `empty` says why ("No cells detected yet in this run.").

## Example

```tsx
<CellStrip
  cells={keptCrops /* newest first */}
  pinnedId="R-063002"
  onPin={(recordId) => pin(recordId)}
  empty="No cells detected yet in this run."
/>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (the detected-cell strip):

- C · CellImage: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `3dff16a8-610c-45d8-b069-fb300f3c056b`
- O4 · Experiment · Live: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `22f62c97-3b47-43f4-bb9c-96eb2be8c257`
