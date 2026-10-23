# I continuation requirement / evidence ledger

Own branch `feat/807-ique-workspace-core`, starting `5e9ec3c5`. No other branch or
runtime is edited. Existing SQLite metadata, CAS/receipt transaction, native SHA256,
production Plan UI, method registry and F/E scientific authorities are reused.
No new persistence service, dependency, or fixture replacement UI is needed.

| Requirement | Implementation / evidence | Status |
|---|---|---|
| List/create/open/rename/archive/unarchive workspaces | Existing snapshots + paginated native listing; production library controls | Implemented; native/API tests pass |
| Complete document round-trip | Preserve definitions/findings/display/source/journal payloads, not just sheet projection | Implemented; native/API tests pass |
| Immutable history/CAS restore | Existing native store, production controls; extend pagination and detached restore | Implemented; UI tests pass |
| Pending-save recovery | Pre-dispatch journal; exact explicit replay, never auto-start/apply | Existing, extending all mutations |
| Export and terminal recovery | SQLite-pinned manifest/materialized export job, queryable after restart; browser download separate | Implemented; native/API tests pass |
| Import without overwrite | Version/digest validation, create-only CAS, exact retry receipt | Implemented; native/API tests pass |
| Fingerprint-checked relink | Native bounded SHA256, size, read-only stable file checks; mismatch preserves original | Implemented; native/API tests pass |
| Scientific resolution/evaluation | E a26d26ba run archive; F a6158e2b graph; exact provider contract requested directly | Integration pending |
| Owned isolated real runtime | Distinct loopback listener/data; no takeover of current QA controller | Verified (see evidence below) |
| Failures and recovery | Corrupt manifest, mismatch/missing file, rollback, CAS, replay, reload/restart, viewer refusal | Verified (see evidence below) |

Export completion means manifest materialized durably in SQLite, not proof that the
browser saved it to an external filesystem. Source bytes remain external and read-only.
A link fingerprint is checked again before scientific use, not a permanent promise
that a path still contains those bytes. No inferred operator authentication or hardware proof.

## Observed lifecycle evidence

- 563 workspace tests, 29 files passed after lifecycle/annotation changes.
- Production native library/server builds; TypeScript check and Vite isolated build pass.
- Host SQLite harness: import digest rejection/create-only refusal, export identity collision,
  full document transfer, list cursor, moved-source receipt replay, matching relink,
  mismatched changed bytes and immutable export retrieval after database reopen.
- `workspace_ws.mjs` ran through actual Studio WebSocket to isolated SQLite data on
  loopback :18089. Create/archive/CAS/history/restore/export/import/attach/relink checks pass.
  Restarted only that owned process and reran `recover`: exact export/import/relink receipts
  and head survived. The original :18088 process/controller was not restarted or claimed.
- Isolated UI assets now use `data/qa-807-recovery/dist`; existing :18088 keeps UI pinned
  to 5e9ec3c5. A distinct tailnet-only :18489 Serve route now points at isolated :18089; all pre-existing Serve JSON entries were verified preserved. HTTPS 200 and WSS recovery both pass.
- Browser tool refused local navigation (`browser navigation blocked by policy`). Rendered
  click-through is unverified; native/API and mocked UI evidence are reported separately.
- TSan remains host startup-blocked, not a pass. Scientific dependency integration remains
  open; no acceptance claimed from syntax, fingerprints, or the metadata-only test findings.

- Read-only `resolve_recipe` uses exact local profile ID/hash or verified registry-cache
  method/revision pairs, never syntax alone. Actual WebSocket test creates a synthetic
  local profile, resolves it, modifies its bytes and observes unresolved. Compatibility
  remains unknown; no apply command is sent.
- Host lifecycle harness passes ASan/UBSan with leak detection enabled.

## Pinned scientific/lifecycle integration (2026-10-23)

This section supersedes the pending provider rows above. Provider code is pinned from
F `0f5588df` (relative to its `0a58e201` base, retaining I's store) and E `65406f15`
run-fence delta. No main/develop merge and no edits to either provider worktree.

- `workspace_read.evaluate_recipe` requires exact workspace revision, linked source ID,
  source kind (`stored` or `run-archive`) and local profile base. It verifies file bytes,
  resolves the exact saved method, reads/evaluates the exact persisted F strategy, then
  rechecks file/method. Wrong/missing revisions, changed methods/files and a different
  open Review source refuse; there is no fallback to latest or frontend arithmetic.
- Production Plan opens a linked source using the existing Review path, explicitly
  evaluates, shows provider counts/statistics/missingness, and persists immutable findings
  with interpretation separate from scientific evidence. Saved evidence can be inspected
  after reopening. No method apply, gate deployment, acquisition or automatic retry.
- Exports include exact referenced F definitions from recipe and findings. Import inserts
  all missing identities/revisions and the workspace/receipt in one SQLite transaction.
  Conflicting existing identities refuse, injected failure rolls everything back. Multiple
  referenced revisions of a newly imported identity are supported; source bytes and method
  configurations are external, and unresolved analysis refs are reported, not invented.
- Explicit workspace saves now record current client queue/start/reference observations.
  They are historical observations, never live state or automatic queue resumption.
  Loading a sheet still uses a fresh experiment identity and clears active queue/reference.

### Verified evidence

- 597 tests across 34 workspace/provider/adapter test files passed after final integration.
  Additional focused tests cover observation-only persistence and malformed/late evaluation
  responses; final command logs are in the isolated QA data/host temporary logs.
- Native production library and server, TypeScript and isolated Vite bundle build.
- Five native CTest cases pass: workspace store/host, F analysis host, E live evidence,
  Review statistics. Three standalone population/graph/statistics cases pass.
- 16 server Rust tests pass. Workspace host passes ASan/UBSan with leak detection,
  including atomic dependency rollback, conflicting definitions and multi-revision import.
- `recipe_ws.mjs` against the real app: stored 10-selected gate gives mean 104.6 and
  median 104.5; saving strategy revision 2 leaves recipe revision 1 unchanged; empty
  selection, missing revision, changed method/file and wrong open source are checked.
- The actual E processing/HDF producer fixture passes the same workspace recipe path,
  with keyed measurements preserved and unknown denominator retained after processing
  failures. This is synthetic processing evidence, not camera/hardware qualification.
- Both stored and E archive results/findings/exports/receipts reproduce after restarting
  only the owned :18089 process, through secure tailnet WebSocket :18489. A separately
  owned fresh real-app database imported the exported strategy and reproduced evaluation;
  the unexported newer revision was absent.
- `viewer_ws.mjs` confirms viewer reads and denial of workspace/analysis writes without
  taking control. Existing :18088 and its controller remain untouched.

### Concrete remaining acceptance boundaries

This closes the requested metadata lifecycle and pinned native scientific integration,
not all historical I01–I12 product acceptance. Browser rendered interaction is blocked
by browser policy; native Tauri packaging and hardware acquisition are unverified.
TSan cannot start on this host. F's pinned API does not supply qualified dose-response
fits, compensation/embedding, comparison criteria qualification or scientific QC trends;
those cannot be fabricated by I. Acquisition-compatible method validation is distinct
from exact lookup and remains `unknown`; hardware readiness is `not_evaluated`.
Run observations are inspectable historical records, not a backend restart-resumption
contract. No queue auto-resume or inferred biological replication is claimed.

### Independent E-supplied fixture acceptance

Consumed a read-only copy of E's `/tmp/e806-production-fixtures/production-run.h5`
and paired JSON in I's isolated app, using committed `recipe_ws.mjs` with
`MIB_QA_SOURCE_KIND=run-archive`. HDF SHA256:
`7cd635abaf289a14ddaab2d0f65a232551b6b93fb4f5fe79488b1b0e125e1bbc`.
Native result: 8 selected/measured, area mean/median 117 pixel², denominator null,
`populationComplete=false`. Coverage remains half-open batch sequence [0,23),
not 23 events. Exact strategy persistence, evaluation, saved finding, export,
relink and source/method refusal checks passed. E originals were not modified;
no hardware or complete-population claim follows from these observations.

E's independent clean second-run fixture also passes the same real-app probe.
HDF SHA256 `dd9e96ed7c59b5de03ce9d7abc7dbfea5790f0af6a2d1e613cb489e4f065eb1c`:
3 selected/measured, mean 146.66666666666666 pixel², median 206 pixel²,
denominator `"3"`, `populationComplete=true` for that retained source snapshot.
The probe now pairs each `.h5` with its own `.json`; use
`MIB_QA_SOURCE_KIND=run-archive MIB_QA_EXPECT_COMPLETE=1` for the clean fixture.
The fault fixture was rerun and still returns null denominator/incomplete.
Both explicitly assert `compatibility=unknown`, `hardwareReadiness=not_evaluated`,
and `applied=false`. Complete event coverage is not calibration/LUT authentication,
scientific QC acceptance, method compatibility or biological independence.
No producer mismatch was observed in either supplied fixture.

F handoff pin `d0c6cb510ae23e6ee43f4bfdfa9067a600544fe2` was compared with
`0f5588df`: production API/native evaluator are unchanged. Its only functional
delta removes public visibility from three Tauri command wrappers; adopted
branch-locally. Thumbnail whitespace cleanup was already present. Existing
scientific evidence applies to the identical evaluator; this source alignment
does not upgrade the unverified native Tauri packaging claim.
