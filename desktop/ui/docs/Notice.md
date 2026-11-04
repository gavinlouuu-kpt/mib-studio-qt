---
category: States
---

# Notice

A banner across a view for a backend state that needs attention: settings that differ from the method (Align), a blocked Start (Prepare), a reading that is not available, a closed run. It has a `title` in "State · detail" form, one line of detail with the facts, and at most two `actions` (Buttons, the main one last).

The `tone` comes from the backend, with the same meaning as `StateChip`: `warn` needs attention but does not block, `blocked` blocks or reports lost data, `unknown` means not reported or no sensor, `confirmed` is an outcome the backend confirmed, `info` is neutral. The notice reports; it never acts on its own and never upgrades Unknown to a pass.

It renders with `role="status"`, so screen readers announce changes without interrupting.

## Example

```tsx
<Notice
  tone="warn"
  title="2 settings differ from method Cell mechanics / v3"
  actions={<><Button>Save to profile</Button><Button>Revert</Button></>}
>
  Exposure 2.0 µs (method 3.0) · Frame rate 5 000 fps (method 2 000). Recorded in every run header.
</Notice>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (override banner and blocked readiness):

- C · Notice: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `f9c56ff4-f56d-4206-81bc-63731e5bfd68`
- O2 · Align: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `7832aa3b-faed-4e2c-ae5c-0b72f1e4fecb`
- O1 · Prepare: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `e4c8fe07-f21c-45cb-9f8d-0045e350da41`
