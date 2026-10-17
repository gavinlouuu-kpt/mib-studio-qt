---
category: Layout
---

# FactList

Label/value rows inside a card: readbacks ("requested 12.0 µm · read 12.0 µm"), provenance, export options. Give a row a `tone` only when the value is a backend state (`confirmed`, `warn`, `blocked`, `unknown`). Values that are not reported say so ("Unknown · no sensor fitted") instead of being left out.

## Example

```tsx
<FactList facts={[
  { label: "Coarse motor", value: "Found · origin unknown", tone: "blocked" },
  { label: "Piezo", value: "Connected · midrange", tone: "confirmed" },
  { label: "Reservoir level", value: "Unknown · no sensor fitted", tone: "unknown" },
]} />
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Card anatomy (facts)):

- C · FactList: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `e1be673c-9e1a-45d6-aa06-b1f8648c0be6`
- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- O2 · Align: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `7832aa3b-faed-4e2c-ae5c-0b72f1e4fecb`
