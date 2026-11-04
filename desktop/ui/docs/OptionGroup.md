---
category: Actions
---

# OptionGroup

Labelled choices shown together instead of hidden in a dropdown: the sample format and per-run auto-stop on Experiment · Plan, and what to export on Review · Export. `type="single"` (the default) uses radio buttons; `type="multiple"` uses checkboxes and reports the chosen values in option order.

Choosing an option only changes the draft. The view's primary `Button` applies it, so an option never actuates hardware or starts a job by itself.

An option the instrument or the build cannot offer keeps its place: give it `unavailable` with the reason ("Unsupported · no autosampler", "Deferred · chapters"). It shows as inert with a grey chip. Never hide or remove it.

Use `variant="cards"` (default) for choices with a description, and `variant="list"` for short choices inside a card.

## Example

```tsx
<OptionGroup
  legend="Sample format"
  value={draft.format}
  onChange={(format) => setDraft({ ...draft, format })}
  options={[
    { value: "single", label: "Single sample", description: "One run, one file" },
    { value: "queue", label: "Sample queue", description: "Manual tube swaps · one file per sample" },
    { value: "rack", label: "Tube rack / carousel", unavailable: "Unsupported · no autosampler" },
  ]}
/>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (sample format, auto-stop, what to export):

- C · OptionGroup: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `9aa8331e-8eb6-4f93-84dd-0a0d58cb90cd`
- O3 · Experiment · Plan: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `14db68fb-c9d2-4e21-ae5a-bd4291fc6273`
- R6 · Export: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `1f19ee2f-b36b-481a-afad-b110567072cf`
