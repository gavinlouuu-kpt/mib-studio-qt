---
category: Data
---

# Stepper

Vertical lifecycle stepper for runs (Start requested → Confirmed closed), export jobs and worker runs. A step is `done` only when the backend confirmed it; `current` is the step in progress; `failed` stops the sequence.

## Example

```tsx
<Stepper steps={[
  { label: "Queued", state: "done" },
  { label: "Writing 3 of 7", state: "current" },
  { label: "Verifying checksums", state: "pending" },
  { label: "Done · 7 files · exports.json", state: "pending" },
]} />
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Lifecycle steppers):

- C · Stepper: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `000889c1-8ddb-4cca-bfa3-a7da06dbc598`
- O5 · Run summary: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `eb7b196d-6e3e-47c2-bb65-183b19329704`
- R6 · Export: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `1f19ee2f-b36b-481a-afad-b110567072cf`
