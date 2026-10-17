---
category: Actions
---

# Button

Action button. Variants: `primary` (dark) for the one main action of a view, `secondary` (outlined) for everything else, `selected` for the active option of a segmented choice or the current sub-view in a toolbar, `destructive` for stop/discard. Set `disabled` for unavailable actions; it renders greyed and does not fire. Labels are verbs or nouns of the target view ("Next sample", "Run calibration (≈ 2 min)"). Navigation never actuates hardware; actions only fire on click.

## Example

```tsx
<Button variant="primary">Next sample</Button>
<Button>Repeat this sample</Button>
<Button variant="selected">Quick look</Button>
<Button variant="destructive">End queue</Button>
<Button disabled>Run preset</Button>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Toolbar row (primary / secondary / selected / destructive / unavailable)):

- C · Button: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `8ec89520-b6ba-4d9b-9c32-5fbc3cf7b8d2`
- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
