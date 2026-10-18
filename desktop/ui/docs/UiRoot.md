---
category: Layout
---

# UiRoot

Root wrapper that applies the UI language: Inter, the base ink colour, white background and border-box sizing. `AppShell` already includes it. Wrap anything rendered outside an `AppShell` (a dialog, an embedded panel, a standalone card) in `UiRoot`, otherwise text falls back to the browser font.

## Example

```tsx
<UiRoot>
  <Card title="Ready to operate?" subtitle="A single authoritative preflight decision" />
</UiRoot>
```

## Penpot reference

The whole "UI language" page (board U0) assumes this root.
