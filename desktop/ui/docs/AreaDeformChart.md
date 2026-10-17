---
category: Data
---

# AreaDeformChart

The one chart component: area (µm²) on X, deformability on Y, isoelastic E curves from the E-LUT (dashed, labelled in kPa at the right), an optional population polygon (teal dotted with handles, labelled "Population A: N of M (x %)") and the inspected record (blue ring). The UI never computes E, statistics or gates: pass curve points and records as the backend gives them. Colour points by `condition` slot; omit it for the grey all-records cloud.

## Example

```tsx
<AreaDeformChart
  points={records}
  isoelastics={lutCurves}
  polygon={populationA}
  populationLabel="Population A: 3 212 of 100 000 (3.2 %)"
  inspected={{ area: 186.8, deformability: 0.0612, label: "R-024210" }}
/>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (The chart):

- C · AreaDeformChart: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `c78afa89-5643-4e25-b9ed-7d845eb6e483`
- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- R3 · Inspect: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `dcdbaa6b-9790-4e69-bea4-936747f588b1`
- O6 · Quick look: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `d8579b82-5140-4ee0-8205-d9f19862d237`
