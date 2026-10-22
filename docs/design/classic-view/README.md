# Classic view, summary-first: approved designs

Approved by Gavin on 2026-10-22. Tracking issue: #799 (tasks #796, #797, #798). These files are
the design authority for those tasks; Penpot access is not needed to implement them.

| Board | File | Penpot id (sandbox file `2c3c14b3…`, page Operation app) |
|---|---|---|
| Classic · Hardware Preflight · summary-first (#796) | `design-classic1.png` | `da067aaa-13de-4ab2-b92a-7ae43b73cf1b` |
| Classic · redesigned left column, idle + running variant (#797) | `design-classic2.png` | `17bcbb85-0932-4b7d-a8d2-e5b706a0ca6a` |
| Classic · Hardware setup page (#798) | `design-classic3.png` | `a53600ab-7a56-4285-bcdf-f2d3b8f9bdfd` |
| O1b · Prepare · summary-first, the approved content: all ready / needs attention / expanded | `design-O1b-ready.png`, `design-O1b-attention.png`, `design-O1b-expanded.png` | `6e4c1ed3…`, `9f185ab4…`, `e4994c8a…` |

- `words/classic_states.py` and `words/states.py`: the generators that produced the boards; every
  label, chip and sentence on the boards is in them. Use them for the exact wording.
- `prototype-O1b.html`: a self-contained prototype of the O1b content built with the real `@mib/ui`
  kit (the kit as of `feat/egrabber-workspace` b1b6aff7, exposed as the global `MibUI`). Open it in a
  browser; Confirm, "Show all" and the locked-open rows behave as designed.
- `prepare-structure-notes.md`: how Prepare's data is built today (checklist ids, gate groupings,
  verdict words, confirm, calibration auto-expand, the disclosure test rule).

## Decisions on record (2026-10-22)

1. Gates a running camera or Align resolves (`camera.session`, `camera.deliveryMode`,
   `camera.geometry`, `telemetry.transportLoss`, `processing.roi`, `processing.background`) never
   count in Prepare; they show as one neutral "Next · Align" line. Start still refuses until they
   pass; the backend is unchanged.
2. The mock camera chosen on purpose reads "Hardware ready · simulated camera"; it is not a warning.
3. Confirm hardware preflight stays enabled whenever the checklist passes; a one-line Start-refusal
   note sits beside it.
4. `storage.output` is never an issue.
5. No configuration on the first page; "Set up…" leads to the Hardware setup page, also reachable
   from Settings. "Enter Service mode…" on board 3 is the existing Mode switch.
6. Kit gaps (disclosure row, inputs, gated-actions tile, stage tab with status, status-bar cell,
   telemetry column) are styled locally in these tasks and filed as follow-ups to #715.

The shared summary model is already on `feat/prepare-summary-first`
(`desktop/src/workspace/prepareSummaryModel.ts`, `DisclosureRow.tsx`).
