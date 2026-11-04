# YOFO Review app: Penpot boards

Renders of the "Review app" page of the fold-in sandbox file (`2c3c14b3…`, page
`6dcbad3e…`), for the track B tasks (#717) and the outside pickup issues #801, #802 and #805.
Penpot access is not needed to implement from them. Board ids and their status are in
[`../egrabber-workspace/PENPOT.md`](../egrabber-workspace/PENPOT.md).

| Board | File | Status |
|---|---|---|
| R0 · Review app map (how it works / never does) | `design-R0-map.png` | reference (#512) |
| R1 · Open · experiment library | `design-R1-open.png` | built (B1, #746 #747 #753) |
| R2 · Overview · runs as recorded, per-sample statistics | `design-R2-overview.png` | **#801** |
| R3 · Inspect · scatter → exact record, image, mask | `design-R3-inspect.png` | built (B3a–d) |
| R6 · Export · per run, with context and provenance | `design-R6-export.png` | **#802** |

R4 Populations and R5 Compare are reference views only (#523, #314) and are not rendered here.
The shell computes no statistics: numbers come from `review_run_statistics` (review ABI 6) or the
#523 worker, or show as Unavailable.
