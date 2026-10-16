---
category: Actions
---

# Toolbar

One row of actions directly under the stage tabs: sub-view buttons first (current one `variant="selected"`), then the view's actions (at most one `primary`). The view title sits at the right.

## Example

```tsx
<Toolbar title="Run summary">
  <Button>Plan</Button>
  <Button>Live</Button>
  <Button variant="selected">Run summary</Button>
  <Button variant="primary">Next sample</Button>
</Toolbar>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Toolbar row):

- U0 · UI language: page `1e1b66e2-283d-5c13-8083-7bc37a320cfd`, board `80ae0c94-466d-4f46-babd-b131d5fef13b`
- O5 · Run summary: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `eb7b196d-6e3e-47c2-bb65-183b19329704`
