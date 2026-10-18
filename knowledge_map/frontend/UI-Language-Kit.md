# UI Language Kit (`@mib/ui`)

> The shared component library of MIB Studio, YOFO Studio and YOFO Review: one set of tokens, chips, cards, shell and the area–deformability chart. The brand changes per product; components and words do not.

**Source:** `desktop/ui/` (package `@mib/ui`). Built with `npm run build` from `desktop/ui`, with `desktop/node_modules/.bin` on `PATH`. The build writes `dist/index.js`, `dist/style.css`, `dist/fonts.css` and `.d.ts` files. Tests live in `desktop/ui/src/ui.test.tsx` and run with the desktop `vitest`.

## Components

| Component | Role |
|---|---|
| `AppShell`, `StageTabs`, `Toolbar`, `UiRoot` | Shell: band, header with context, numbered stages, toolbar with the title at the right |
| `Card`, `FactList` | Card anatomy: title, subtitle, chips, facts, actions |
| `StateChip`, `ConditionSwatch` | Backend states (confirmed / warn / blocked / unknown / unsupported / info / neutral) and condition colours (slots 1–5) |
| `Button` | primary / secondary / selected / destructive / disabled |
| `StatTile`, `Stepper`, `RecordIdentity` | Counts and Quick look deltas, lifecycle steps, record identity with evidence |
| `DataTable` | Rows of recorded facts: statistics, linked records, sample sheet, library, export preview. Numbers align right, a missing value shows as missing, the selected row is the inspected record |
| `CellImage`, `CellStrip` | A recorded cell crop with its recorded contour or mask, by record id; the live detected-cell strip with click-to-pin. A missing image or overlay is stated, never drawn |
| `AreaDeformChart` | Area vs deformability with LUT isoelastics, population polygon and inspected record. It never computes E |

Tokens are CSS custom properties named `--mib-*` (`desktop/ui/src/styles/tokens.css`). The rules (truth, vocabulary, colour) are in `desktop/ui/guides/ui-language.md`.

## Penpot and Claude Design

- **Penpot:** the visual source is the Penpot sandbox file holding the "UI language" (U0), "Operation app" (O0–O6) and "Review app" (R0–R6) pages, plus a "Components (@mib/ui)" page with one board per component ("C · <Name>"; StageTabs and UiRoot share "C · AppShell"). Each component lists its own board first. `desktop/ui/penpot.json` maps each component to its boards, and each `desktop/ui/docs/<Name>.md` ends with a "Penpot reference".
- **Claude Design:** `desktop/ui/.design-sync/` (config, notes, conventions, authored previews) syncs the kit to the Claude Design design-system project "MIB · YOFO UI language" (`14e57f39-1852-4a03-a868-550dc43f27b1`, first synced 2026-10-16), so designs made there use these components. Re-sync with `/design-sync` after kit changes.

## Status

New on `feat/ui-language-kit` (2026-10-16). Not yet mounted by `desktop/src`. Adopting it is the P1 shell work of the eGrabber workspace plan.

Related: [[YofoReview]], [[../architecture/Desktop-Shell]].
