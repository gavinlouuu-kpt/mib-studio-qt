# Isolated workspace persistence qualification

Partial I12 evidence, not production host binding or full workspace acceptance.
The native core uses a caller-owned SQLite connection. It does not open a separate
DB, alter host pragmas/user_version, claim operator authority, or wire the facade.
`SqliteService` at this baseline only logs; it is not a functioning store to call.
The existing ProfileCache transaction/statement idiom was reused, but method-cache
tables are untouched. No new library: existing SQLite and nlohmann JSON suffice.

## Reproduce

From the repo root, with existing system dependencies:

```sh
cmake -S tools/ique_acceptance -B build/ique-workspace-core -G Ninja
cmake --build build/ique-workspace-core
build/ique-workspace-core/ique_workspace_test build/ique-workspace-core/fresh-test.sqlite
```

Use a fresh path each run; the harness refuses to overwrite any existing DB.
On this host, exclude the unrelated Homebrew prefix using both CMake
`CMAKE_IGNORE_PREFIX_PATH` and `CMAKE_SYSTEM_IGNORE_PREFIX_PATH` when configuring.

## Observed checks

The standalone C++17 executable passes with warnings-as-errors:

- actual SQLite commit/read and close/reopen;
- immutable old revisions and fresh-revision restore;
- create-only and stale expected-revision conflicts;
- successful command replay after later edits and restart;
- same command ID with different content rejects;
- injected command-receipt INSERT failure rolls back the earlier revision INSERT;
- failed command can be retried after the transaction has rolled back;
- missing workspace and missing restore target are distinct;
- workspace isolation, input size/version/identity checks, bounded history pages;
- nested transaction refusal leaves the host transaction untouched;
- two separate connections compete for the same revision: exactly one success,
  one conflict, and one new history entry (30-second watchdog, no fixed sleeps);
- unsupported schema initialization fails closed and preserves data.

ASan+UBSan build and execution passed (same harness, `-fsanitize=address,undefined
-fno-omit-frame-pointer -g`). This does not instrument the system SQLite library.
TSan compiled, but could not start on this host: `FATAL: ThreadSanitizer: unexpected
memory mapping`, exit 66. A non-PIE build had the same startup failure. **No TSan
pass is claimed.** Shared CI sanitizer integration remains required before acceptance.
No hardware, timing performance or production workspace reopen claim.

## Limits and integration handoff

- Core schema v1 stores complete logical JSON snapshots (max 1 MiB, nesting <=64),
  opaque scientific payloads, and successful command receipts in one transaction.
  Revisions cross the API as canonical decimal strings. History pages max 1000.
- This is not an acquisition journal or scientific job/artifact store. There is no
  new background writer. Caller serializes a connection on its metadata worker.
- Host owns DB lifecycle, authorization/operator/workspace resolution, migrations,
  durability pragmas, connection recovery, and mapping errors to a typed facade.
- Caller must initialize successfully before reads/writes. Unknown schema fails
  closed. An external migration must not run while this connection is in use.
- Scientific payload validation belongs to F; recipe/findings schema validation
  must happen at the host boundary. JSON validity is not scientific validity.
- Successful retries reuse exact command+expected revision+canonical payload.
  Failed transactions do not consume command IDs. An unknown COMMIT outcome is
  reconciled by replaying the same command; never mint a replacement command ID.
- Export jobs, fingerprint-checked relink, operator attribution, full journal,
  document-level schema/lineage validation and frontend provider binding are not
  implemented here. No I02/I12 complete acceptance.
- No shared build, AppBackend, ReviewSession, bridge/ABI or facade files changed.
  This describes the original core-only checkpoint. The later own-branch allocation
  supersedes that gate; see PRODUCTION-INTEGRATION.md for current host/facade integration.
