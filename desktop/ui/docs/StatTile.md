---
category: Data
---

# StatTile

A labelled number: accounting counts in the run summary, medians in Quick look. Values come formatted from the backend (thin-space thousands: "100 000"). `delta` shows the difference against the operator-chosen reference run; it is descriptive only, never a test result. `tone="confirmed"` for saved counts, `blocked` for losses. A value passed as `null`, `undefined` or `""` shows as an en dash titled "Not recorded", without a delta; a recorded 0 stays 0.

## Example

```tsx
<StatTile label="Valid objects" value="100 000" />
<StatTile label="Saved cell records" value="100 000" tone="confirmed" />
<StatTile label="median def." value="0.072" delta="+0.027" />
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Counts and Quick look numbers):

- C · StatTile: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `846e096c-0cc7-450d-82be-d4380b31719d`
- O5 · Run summary: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `eb7b196d-6e3e-47c2-bb65-183b19329704`
- O6 · Quick look: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `d8579b82-5140-4ee0-8205-d9f19862d237`
