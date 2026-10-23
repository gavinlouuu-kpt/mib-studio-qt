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
