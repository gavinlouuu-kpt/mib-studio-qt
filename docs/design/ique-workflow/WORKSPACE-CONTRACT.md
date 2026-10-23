# I-workspace/1 proposal

Logical proposal posted on #808; cross-track coordinator subsequently confirmed
I owns shared metadata persistence and F owns scientific payloads. Not an ABI
reservation or deployed API. F owns plan/result/
comparison meaning; I consumes immutable references and owns recipe/finding UX.
The worker-private terminal JOB journal is not user workspace persistence.

## Single authority and ownership

Implement against #393/#395's metadata database and operator/workspace model,
not a second per-feature JSON store. The portable manifest below is a logical
export shape, not a decision to replace SQLite or its migration authority.
Existing ProfileRegistryService remains the sole method/apply authority.
Unknown operator attribution stays unknown; UI names are not authentication.

Proposed isolated I paths: `include/backend/workspace/`, `src/backend/workspace/`,
`tests/workspace/`, `tools/ique_acceptance/`. F owns analysis paths and scientific
artifacts. Shared AppBackend, bridge, ReviewSession and build integration require
explicit ownership agreement. The isolated core in #815 is implemented under
confirmed I ownership; no shared host integration is claimed.

## Minimum logical envelope

`schema_version:1`, `workspace_id`, authoritative opaque `revision`, documents
with `{kind,id,revision,payload}`, immutable source links/fingerprints and
content-addressed artifact references. Kinds: analysis-plan, recipe, finding,
comparison, annotation-display, qc-policy, qc-exclusion, run-journal. Scientific
payload validation remains F-owned; acquisition journal facts remain E-owned.
The envelope must not replace either journal or invent operator provenance.

Recipe v1 associates `{source:local|registry,id,revision}` method and opaque
`{id,revision}` analysis plan, title/question and optional immutable source
recipe/finding references. A copied draft has a fresh ID and null revision;
only a successful authoritative commit gives it a revision. Finding pins result
ID/revision and complete input key; interpretation is separate from evidence.

## Operations and failure semantics

- Read exact workspace revision or latest; missing is distinct from empty.
- Commit workspace ID, expected revision (null=create-only), stable command ID,
  and mutations atomically. No partial update of recipe and findings.
- Same command ID and same payload returns the original result after a lost
  reply; different payload rejects. Unknown outcome is queried, not retried
  under a new identity.
- Replies: success/new revision, conflict/current revision, not-found, invalid,
  I/O error, unknown. Conflict never silently overwrites current work.
- History persists; restore creates a new revision referencing the old one.
- SQLite transaction failure retains the previous committed state. Fault tests
  cover reopen, failed writes, rollback, conflict and command replay.
- Export pins a workspace revision. Relink verifies fingerprints. Export jobs
  retain queryable terminal state; never replace an earlier output silently.
- No source HDF5 mutations, browser persistence or hardware operations.

## Consumer status

`recipeModel.ts` is a typed consumer proposal with strict persisted-input checks,
copy/lineage and exact compatibility key matching. `RecipePairing.tsx` is isolated
presentation. Neither claims durable storage, scientific compatibility computation
or live Apply integration. Existing `registry_plan_apply` / explicit apply remain
required for eventual operational binding. The fixture demo is not that binding.

## Implemented lifecycle extension (2026-10-23)

The historical isolated/shared-file gates above are superseded on the I branch.
`workspace_read` accepts `list {after?,limit<=100}`, `export_status {workspace_id,job_id}`,
`verify_source {workspace_id,revision,source_id}`, plus read/history.
`workspace_write` additionally accepts:

- `export {workspace_id,revision,job_id}`: durable completed materialization, exactly
  replayable/queryable across restart. Payload excludes external source bytes.
- `import {workspace_id,command_id,manifest}`: validates canonical payload SHA256
  and format, then create-only commit. Existing workspaces cannot be overwritten.
- `attach_source` / `relink` with workspace_id, expected_revision, command_id,
  source_id and path: regular-file SHA256/size; relink refuses mismatched bytes.

Manifest: `{payload:{format:"I-workspace/1",workspace_id,revision,document},sha256}`.
SHA256 covers native canonical JSON payload bytes; it detects corruption, not author
identity. Imported file paths are untrusted until explicitly reverified. Export job
completion means bytes retained in the DB, not delivery to an external filesystem.
Source verification limits: regular files <=2 GiB, hashing deadline 10 seconds.
This bounded operation runs only inside the facade's idle acquisition/recording guard.
