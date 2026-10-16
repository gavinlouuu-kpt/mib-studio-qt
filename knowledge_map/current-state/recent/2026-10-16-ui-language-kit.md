## 2026-10-16 — UI language kit `@mib/ui`, wired to Penpot and Claude Design

New package `desktop/ui/` (`@mib/ui`) turns the Penpot "UI language" board into real React components: `AppShell`, `StageTabs`, `Toolbar`, `UiRoot`, `Card`, `FactList`, `StateChip`, `ConditionSwatch`, `Button`, `StatTile`, `Stepper`, `RecordIdentity` and `AreaDeformChart`. It ships `--mib-*` tokens and an Inter (OFL) Latin subset.

- Every component doc cites its Penpot boards, and `desktop/ui/penpot.json` maps components to boards.
- `desktop/ui/.design-sync/` holds the Claude Design sync inputs: config, conventions header, notes and 13 authored previews, all graded good. Uploaded the same day to the Claude Design project "MIB · YOFO UI language".
- The kit is not yet used by `desktop/src`; adopting it is the P1 shell work.

See [[frontend/UI-Language-Kit]].
