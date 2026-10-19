# UI Language Kit (`@mib/ui`)

> The shared component library of MIB Studio, YOFO Studio and YOFO Review: one set of tokens, chips, cards, shell and the area–deformability chart. The brand changes per product; components and words do not.

**Source:** `desktop/ui/` (package `@mib/ui`). The desktop app consumes it **from source** (see "Using it from the desktop app" below); `npm run build` from `desktop/ui`, with `desktop/node_modules/.bin` on `PATH`, still writes a standalone `dist/index.js`, `dist/style.css`, `dist/fonts.css` and `.d.ts` files for consumers outside `desktop/`. Tests live in `desktop/ui/src/ui.test.tsx` and run with the desktop `vitest`.

## Components

| Component | Role |
|---|---|
| `AppShell`, `StageTabs`, `Toolbar`, `UiRoot` | Shell: band, header with context, numbered stages, toolbar with the title at the right |
| `Card`, `FactList` | Card anatomy: title, subtitle, chips, facts, actions |
| `StateChip`, `ConditionSwatch` | Backend states (confirmed / warn / blocked / unknown / unsupported / info / neutral) and condition colours (slots 1–5) |
| `Button` | primary / secondary / selected / destructive / disabled |
| `OptionGroup`, `Notice` | Labelled choices that only change the draft (unavailable options stay visible with the reason); a status banner with a backend tone and at most two actions |
| `StatTile`, `Stepper`, `RecordIdentity` | Counts and Quick look deltas, lifecycle steps, record identity with evidence |
| `DataTable` | Rows of recorded facts: statistics, linked records, sample sheet, library, export preview. Numbers align right, a missing value shows as missing, the selected row is the inspected record |
| `CellImage`, `CellStrip` | A recorded cell crop with its recorded contour or mask, by record id; the live detected-cell strip with click-to-pin. A missing image or overlay is stated, never drawn |
| `AreaDeformChart` | Area vs deformability with LUT isoelastics, population polygon and inspected record. It never computes E |

Tokens are CSS custom properties named `--mib-*` (`desktop/ui/src/styles/tokens.css`). The rules (truth, vocabulary, colour) are in `desktop/ui/guides/ui-language.md`.

## Penpot and Claude Design

- **Penpot:** the visual source is the Penpot sandbox file holding the "UI language" (U0), "Operation app" (O0–O6) and "Review app" (R0–R6) pages, plus a "Components (@mib/ui)" page with one board per component ("C · <Name>"; StageTabs and UiRoot share "C · AppShell"). Each component lists its own board first. `desktop/ui/penpot.json` maps each component to its boards, and each `desktop/ui/docs/<Name>.md` ends with a "Penpot reference".
- **Claude Design:** `desktop/ui/.design-sync/` (config, notes, conventions, authored previews) syncs the kit to the Claude Design design-system project "MIB · YOFO UI language" (`14e57f39-1852-4a03-a868-550dc43f27b1`, first synced 2026-10-16), so designs made there use these components. Re-sync with `/design-sync` after kit changes.

## Using it from the desktop app

`desktop/src` imports the kit by its package name, exactly as the kit README shows:

```tsx
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import { AppShell, Card, StateChip } from "@mib/ui";
```

The names resolve to the kit's **source**, not to `dist/`: `desktop/vite.config.ts` has three exact
`resolve.alias` entries (`@mib/ui` → `ui/src/index.ts`, `@mib/ui/style.css` → the same entry, which
imports `tokens.css` and `components.css` itself, `@mib/ui/fonts.css` → `ui/src/styles/fonts.css`),
and `desktop/tsconfig.json` maps `@mib/ui` in `paths` for `tsc`. The dev server, Vitest, `tsc --noEmit`,
both production pages (`index.html`, `review.html`) and the Tauri build (its `beforeBuildCommand`
is `npm run build`) therefore need no kit build step. Why source and not `dist/`: nothing to
prebuild in CI or before `npm run dev`, no stale `dist/`, one React (the kit resolves `react` from
`desktop/node_modules`), kit edits hot-reload, and the kit is type-checked under the app's strict
options. The kit's CSS is namespaced (`.mib-*` classes, `--mib-*` custom properties, `@font-face`
"Inter"), so importing it does not restyle existing views. Guard: `desktop/src/uiKit.test.tsx`
renders a kit shell from `desktop/src` through the aliases.

## Status

New on `feat/ui-language-kit` (2026-10-16). Wired into the desktop app on 2026-10-19 (source
alias, above). First consumer: the opt-in workspace in `desktop/src/workspace/` (P1/P2, #713;
see [[../architecture/Desktop-Shell]], "Opt-in workspace"), which imports the stylesheets once
at its entry. O2 Align (#716 A3) uses `DataTable` (compact) for the quick controls, `Notice` for
the explicit mode switch and `StateChip` tones for write states. The kit has no number field yet:
the quick controls' X / Y / W / H inputs are app-local markup in `workspace.css` until one lands
(#715). O3 Experiment · Plan (#716 A5) uses `OptionGroup` (sample format, the queue inert with its
reason), a compact `DataTable` for the sheet's row as it will be recorded (with a "Not declared"
`missing`), `ConditionSwatch` slots 1–4 in sheet order, `FactList` for the run identity and
`Notice` for each reason Start is unavailable and for the outcome. Its text fields (and the notes
textarea) are app-local too, without `maxLength` (a long value is refused, never cut off). Two
local CSS rules wrap content the kit keeps on one line: an `OptionGroup` reason chip in a narrow
card and long values in the sheet's table cells.

Related: [[YofoReview]], [[../architecture/Desktop-Shell]].
