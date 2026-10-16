---
category: Layout
---

# AppShell

The shell shared by both products. Operation app (MIB Studio / YOFO Studio): stages Prepare, Align, Experiment, Review. Review app (YOFO Review): Open, Overview, Inspect, Populations, Compare, Export. Only `brand` and `tag` change between products; components and words stay the same. Header `context` holds at most two items (Source, Method, or Analysis plan). `status` holds at most two `StateChip`s describing what the stage is doing. Put a `Toolbar` first in the children, then a grid of `Card`s.

## Example

```tsx
<AppShell
  brand="MIB Studio"
  tag="Imaging + cytometry"
  context={[{ label: "Source", value: "Camera 01 / EGrabber" }, { label: "Method", value: "Cell mechanics / v3" }]}
  stages={["Prepare", "Align", "Experiment", "Review"]}
  activeStage={2}
  status={<StateChip tone="confirmed">Run closed · Exp-0142_03 · 4:21</StateChip>}
  headerActions={<Button>Settings</Button>}
>
  <Toolbar title="Run summary"><Button variant="primary">Next sample</Button></Toolbar>
</AppShell>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Shared shell):

- O0 · Operation app map: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `7ad763ec-7b0a-408f-bef9-ef9f3d25dcf1`
- R0 · Review app map: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `643f5337-5992-46b6-9854-4b5ccf638c76`
