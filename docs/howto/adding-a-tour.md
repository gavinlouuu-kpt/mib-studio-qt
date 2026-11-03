# Adding a guided tour

MIB Studio ships guided tours built on [Driver.js](https://driverjs.com/) 1.3.1 (pinned
exactly; notice in [`desktop/THIRD_PARTY_NOTICES.md`](../../desktop/THIRD_PARTY_NOTICES.md)).
A tour walks the operator through the real views, explains a backend change with before and
after flowcharts, and keeps what was demonstrated, what is tested and what is still uncertain
apart. Vault note: [`Desktop-Shell`](../../knowledge_map/architecture/Desktop-Shell.md) ("Guided tours").

## The rule

**Every new user-facing feature, and every backend change the operator relies on, ships with a
tour, or with the reason it has none.** Put the reason in the PR body. Valid reasons:

- the change is invisible to the operator (refactor, test, CI, build);
- there is no production view yet (a UI probe, a placeholder, a JSON-only utility). For the
  fold-ins (E, I, F), the backend-before-view rule applies: tour only goals whose headless proof
  passes, and only on views that are not UI probes. F09 (comparison and fits) has no tour for
  this reason.

## Where things are

| Path | What |
|---|---|
| `desktop/src/tours/registry.ts` | Types (`TourEntry`, `TourStep`, `FlowSpec`, `TourEvidence`) and `validateEntries` |
| `desktop/src/tours/runner.ts` | The Driver.js wrapper: Back / Next / Skip / Restart, missing-target recovery, action steps, the feedback checkpoint |
| `desktop/src/tours/FlowchartView.tsx` | Before / after (and planned) SVG flowcharts and the evidence panel |
| `desktop/src/tours/WhatsNew.tsx`, `TourHost.tsx`, `feedback.ts` | The "what's new" notice, the Tours list (with feedback export), per-browser storage behind try/catch |
| `desktop/src/tours/mib/` | MIB Studio's host (`MibTours.tsx`), its view vocabulary (`views.ts`), the facts store (`facts.ts`), source roots (`sources.ts`) and the entries (`entries/*.ts`) |
| `scripts/tours/drive_tours.py` | The browser drive: plays every tour in headless Chromium against the real `yofo-studio-server` and saves screenshots |

The generic files (everything outside `mib/`) are shared with the ZU15EG Studio patch in
`zu15eg-linux` (`studio/ui-patches/0004-zu15eg-tours.patch`). Keep them free of MIB names, and
note any change to them in the PR so the two copies can be reconciled.

## Writing an entry

1. Add `desktop/src/tours/mib/entries/<name>.ts` exporting a `TourEntry` and list it in
   `entries/index.ts`. `introduced` is the date and the branch or PR; `source` lists every
   document or code path the text comes from (repository paths, or `<root>:<path>` roots defined
   in `mib/sources.ts`). Every step `cite`s its sources.
2. **Targets** are `data-tour` attributes you add to the real control
   (`{ ui: "recipe-create", view: workspaceView("experiment", "plan") }`), never CSS class chains.
   Canvas-native targets (plots, galleries, the camera image) use a named region in image
   coordinates (`canvasRegions` plus `{ canvas, region }`); never guess a selector. If a target
   is absent (a capability hid it, nothing is loaded), the runner says so and offers Skip or
   Restart: it never points at nothing.
3. **Views:** `view` only changes view state (workspace stage and sub-view, classic tab). It
   never sends a command, never writes the landing preference, and never navigates to a tab
   that can switch the camera mode.
4. **Action steps** ask the operator to use the real control; the tour sends nothing. The step
   names a condition from `mib/facts.ts` (`action.await`), a timeout and a recovery text. A
   component reports the fact only after the backend reply confirmed it
   (`reportFact(FACTS.recipeCreated, { ok: true, detail })` or the refusal). Add a new fact name
   there when a step needs one. Action steps are blocked while an experiment runs, something
   records or the camera acquires; `duringRun: true` exempts a step whose control belongs to the
   run itself (Stop), and `needsCamera: true` a step that only works on a running camera (a
   feature write needs an open camera session; a run or a recording still blocks it). The
   Driver.js overlay covers everything outside the highlighted element, so an action step must
   highlight everything the operator touches (the whole form, not only its button), and a step
   whose text asks for a press on a control that only reads or previews is marked
   `interactive: true`; without it the overlay swallows the press. The browser drive
   (`scripts/tours/drive_tours.py`) acts as the operator through the real overlay, so it fails
   on a step the operator cannot do.
5. **Backend tours** (`kind: "backend"`) have a `before` and an `after` flowchart and an
   evidence panel, and end where the outcome shows in the UI. Before is how Studio worked
   before the change (from the goals, plans and code history, cited with a `before@<sha>` root);
   after is the met goal. Something only planned is `planned`, never `after`.
6. **Evidence:** `demonstrated` is what was shown, with its date and its evidence class (mock
   camera, real backend, synthetic data, or the rig with its log); `tested` names the test or
   e2e step exactly; `uncertain` is what is not proven yet. Never claim hardware, the rig or a
   check nobody saw.
7. **No science in TypeScript.** Tours format what the backend returned and point at it; they
   compute nothing.

## Checks

```bash
cd desktop && npx tsc --noEmit && npm test     # validateEntries, sources exist, every ui target has a data-tour
npm run build
python3 scripts/tours/drive_tours.py --help    # the browser drive and its screenshots (see the script's docstring)
python3 scripts/check_docs.py
```

Screenshots of every step, light and dark, go to `docs/evidence/<date>-<job>/` with a manifest
and a contact sheet. They are evidence, not user-manual images: do not reference them as
`images/*.png` from `docs/manual/` (`scripts/check_screenshots.py` ties those to the Qt
screenshot harness).
