## 2026-10-19 — Opt-in workspace with O1 Prepare; Unknown/Unsupported preflight states (#713)

Step A1 of track A (#716), the rest of #713 after the probe cache (#719) and the kit wiring (#723).

- **Opt-in workspace.** "Workspace preview" in the menu row opens a Prepare / Align / Experiment /
  Review shell on `@mib/ui` (`desktop/src/workspace/`). Session state only: every start and
  reload opens the classic view. The classic view stays mounted (hidden) and its tab never
  changes, because classic tab changes switch camera modes (and the PZ7035 LED). Only Prepare
  has content; the other stages say so and offer the way back.
- **Prepare** shows the hardware checklist by subsystem and the backend readiness gates read
  without a path (`fetchExperimentReadiness("")`, every 2 s while shown, paused during a run): the
  backend reports `storage.output` Unavailable and writes no probe file; Start re-evaluates with
  the chosen file. Tones follow the backend status; the destination gate is never called ready.
- **Checklist states** (`preflight.ts`): not read yet is `unknown` (was `failed` or
  `not-required`), an absent capability is `unsupported`, unreported storage is `unknown` (was
  `not-required`). `checkGateStatus` maps them to Pass / Warn / Fail / Unavailable / NotRequired.
  A required unknown blocks the confirmation; a required warning blocks only when its backend
  gate says Fail (`CHECK_GATES`), so e.g. an unverified PL build no longer blocks on its own.
- **Confirmation signature** (`confirmationSignature.ts`) now binds the PL build and weights,
  pump, Z stage and nanopositioner identities, profile id and revision and the reported
  destination, besides camera and core. Existing confirmations are invalidated once.
- **Gate:** `workspace/navigation.test.tsx` (whole App, mocked transport, desktop and PZ7035):
  pure navigation adds only the path-less readiness read and nothing that acts.

See [[architecture/Desktop-Shell]] ("Opt-in workspace") and [[frontend/UI-Language-Kit]].
