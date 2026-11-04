---
category: Data
---

# RecordIdentity

How one measured object is identified everywhere: record id, frame, object, run, sample, then a provenance line (experiment id, method revision, E-LUT, calibration record). All ids are strings (u64 ids travel as strings end to end). `evidence` rows report what is stored for the record: Saved · Pending · Missing (reason).

## Example

```tsx
<RecordIdentity
  recordId="R-024210" frame="82 140" object="02" run="3" sample="03"
  provenance="experiment 7c1e… · method v3 rev 8f2a · lut-v2 · cal-0042"
  evidence={[{ label: "Image", value: "Saved", tone: "confirmed" }, { label: "Mask / contour", value: "Missing (not recorded)", tone: "unknown" }]}
/>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Record identity):

- C · RecordIdentity: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `7431ea12-2873-4f29-97ce-9649334d2691`
- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- R3 · Inspect: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `dcdbaa6b-9790-4e69-bea4-936747f588b1`
