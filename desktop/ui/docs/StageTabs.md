---
category: Layout
---

# StageTabs

Numbered stage tabs ("01 Prepare"). The workflow order is the navigation. `AppShell` already renders them; use `StageTabs` alone only inside a custom header. Selecting a stage navigates; it never starts, homes or lights anything.

## Example

```tsx
<StageTabs stages={["Open", "Overview", "Inspect", "Populations", "Compare", "Export"]} active={2} />
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (Stage tabs):

- C · AppShell: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `14277a26-6172-4921-84c3-513b5659ccfd`
- O0 · Operation app map: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `7ad763ec-7b0a-408f-bef9-ef9f3d25dcf1`
- R0 · Review app map: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `643f5337-5992-46b6-9854-4b5ccf638c76`
