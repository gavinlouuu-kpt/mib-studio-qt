## 2026-10-18 — Operation app and Review app split into two tracks

The eGrabber workspace work is divided by product (`docs/design/egrabber-workspace/WORK-SPLIT.md`):

- **Track A, Operation app** (#716): MIB Studio / YOFO Studio, `desktop/src/**` except the review
  folder, and the run-writing backend. Slices S0, S2–S5 and views O1–O6.
- **Track B, Review app** (#717): YOFO Review and the review module Studio mounts,
  `desktop/src/review/**`. #512, #269 and S6, views R1–R3 and R6.
- **Kit** (#715): `@mib/ui` gets DataTable, CellImage, OptionGroup and Notice, which both tracks need.

The tracks meet only at the run file (snapshot v3, #712), the quick-look reconciliation test,
the review module's single entry point, and the kit. Each view goes Penpot board, then a
Claude Design prototype with the real kit, then implementation. The note lists one prototype
brief per view.

Documentation only: no code, ABI or schema changes.
