---
category: States
---

# ConditionSwatch

Colour key for an experimental condition. Conditions take slots 1–5 in sample-sheet order (slot 5 = calibration / reference) and keep the same colour in every view, chart and legend. Use `conditionColor(slot)` for custom marks. Never use a condition colour for a state, or a state colour for a condition.

## Example

```tsx
<ConditionSwatch condition={1}>Vehicle</ConditionSwatch>
<ConditionSwatch condition={2}>CytoD 0.1 µM</ConditionSwatch>
<ConditionSwatch condition={3}>CytoD 1 µM</ConditionSwatch>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Colour):

- C · ConditionSwatch: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `dc97d71b-0eca-47de-aaf9-a7a99cea5b45`
- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- O6 · Quick look: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `d8579b82-5140-4ee0-8205-d9f19862d237`
