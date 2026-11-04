---
category: States
---

# StateChip

A state chip shows one backend state. Pick the tone from what the backend reported:

| Tone | Meaning |
|---|---|
| `confirmed` | reported and good: Pass, Connected, Saved |
| `warn` | needs attention, does not block: "Warn · 31 h", "low n" |
| `blocked` | blocks the action or data was lost: "Fail · origin unknown", "loss 0.7 %" |
| `unknown` | not read yet; never shown as pass |
| `unsupported` | capability absent (no sensor fitted); never blocks |
| `info` | informational: reference run, inspected record |
| `neutral` | plain label: "queued", "closed" |

Write the label as `State · detail` ("Pass · 0.91", "Unknown · not reported"). Use `size="small"` in table flag columns.

## Example

```tsx
<StateChip tone="confirmed">Connected</StateChip>
<StateChip tone="warn">Calibration record 31 h old · warn</StateChip>
<StateChip tone="blocked">Focus: motor origin unknown</StateChip>
<StateChip tone="unknown">Unknown · not reported</StateChip>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (State chips):

- C · StateChip: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `9b91ee6f-72fc-4957-aca5-cc30f19aef3e`
- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- O1 · Prepare · readiness: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `e4c8fe07-f21c-45cb-9f8d-0045e350da41`
