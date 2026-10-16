---
category: Layout
---

# Card

The card anatomy every view uses: title, one-line subtitle (what, from where, how fresh), state chips, facts, then actions. `variant="tile"` is the grey inner tile for items inside a card (map entries, queue rows); `variant="selected"` highlights the chosen item (current run, selected option). Facts never guess: a missing source shows Unknown.

## Example

```tsx
<Card
  title="Calibration record"
  subtitle="Rigid size beads · ADR 0015 item 7"
  status={<StateChip tone="warn">Applied · 31 h old</StateChip>}
  actions={<><Button variant="selected">Run calibration (≈ 2 min)</Button><Button>Records</Button></>}
>
  <FactList facts={[{ label: "Record", value: "cal-0042 · 15.2 µm beads" }, { label: "Pixel → µm", value: "0.4886 (applied)" }]} />
</Card>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Card anatomy):

- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- O1 · Prepare · readiness: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `e4c8fe07-f21c-45cb-9f8d-0045e350da41`
