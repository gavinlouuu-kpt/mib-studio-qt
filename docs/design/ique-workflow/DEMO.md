# I-A1 annotation projection demonstration

This is a bounded I01 milestone, not the complete iQue fold-in. It runs inside
the existing MIB React workspace; no alternate application or scientific engine.

## Reproduce

From the repository root:

```sh
cd desktop
npm ci --no-audit --no-fund
npm test -- src/workspace/annotationModel.test.ts src/workspace/AnnotationGroups.test.tsx src/workspace/PlanView.test.tsx src/workspace/QueuePlan.test.tsx
npm run build
npm run dev
```

Open the development workspace, choose Experiment → Plan, and choose a sample
queue. This demo needs no camera and must not press Start or apply hardware.

1. Add three sample rows. Set S1/S3 to Vehicle, S2 to Drug. Declare units D1,
   D2, D1 respectively. Set concentrations with explicit units where declared.
2. In each sample editor add `batch` in Key/values: A, B, A. Under Annotation
   groups select `Annotation · batch`. The groups must contain S1/S3 and S2.
3. Switch to Condition. Its swatches match the sample sheet. D1 stays one
   declared label, not two inferred independent units. Empty units are counted
   explicitly as samples without a unit. Run order does not change.
4. Show annotation JSON. It uses schema `mib.planned-annotations`, version 1;
   sample annotations match `buildStartRequest`'s production serialization.
   Copy the JSON if an annotation-only export is wanted. No result, run index
   or new acquisition identity is manufactured.
5. Remove a concentration unit while retaining its value: export becomes
   unavailable and the invalid sheet issue is visible. Restore it to recover.
6. Rename/remove a selected custom key: selection falls back to Condition
   when that layer no longer exists. Nothing sends an instrument command.

## Evidence levels and limits

The reusable version-1 fixture is
`desktop/src/workspace/fixtures/ique-annotations-v1.json`. Its repeated D1 label
intentionally exercises the no-replicate-inference rule; it is not the full
three-independent-unit operational acceptance scenario.

- Model tests: three samples/two conditions, custom keys, nulls, zero
  concentration, exact Start annotation parity, duplicate keys/refs,
  UTF-8 bounds, invalid units and JSON round-trip. These are **synthetic UI
  metadata**, not independent scientific fixtures or HDF5 acquisition proof.
- React interaction tests: real component, select-layer/export/edit/fallback,
  escaped markup and immutable input; jsdom, not native-webview acceptance.
- Existing single/queue tests exercise the real controller over mocked bridge
  calls and verify its existing locking/start behavior remains intact.
- TypeScript and Vite build check both existing application entry points.
- No backend/writer/thread changes: no new TSan or hardware claims. Live
  acquisition, native-webview appearance, shared analysis parity, workspace
  save/reopen and the complete three-sample answer loop are **not verified**.
- Group selection is session-local; export is a projection for copying, not a
  saved experiment and not an import/reopen mechanism. The existing four
  condition colour slots are reused; additional categories remain labelled
  without invented swatches. Stable persisted colour registry is outstanding.

See [the ledger](README.md) for every remaining ID and dependency owner.
