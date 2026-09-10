# Processing Core Dialog

> Settings UI for enumerating, preparing, and activating native
> deformability-cytometry processing-core versions.

**Source:** `src/frontend/dialogs/ProcessingCoreDialog.cpp`,
`include/frontend/dialogs/ProcessingCoreDialog.h`,
`src/frontend/utils/ProcessingCoreCatalog.cpp`,
`include/frontend/utils/ProcessingCoreCatalog.h`
**Related:** [[MainWindow]], [[Dialogs]],
[[../services/ProcessingService]], [[../architecture/Threading-Model]],
[[../build-and-run/Build]]

## User flow

Open **Settings → Processing Core…**, choose the stable or beta channel, then
select **Prepare & Activate**. The list marks the channel-active version from
the independently fetched `latest.json` pointer, the
currently selected version, and entries incompatible with the current OS,
architecture, or app-version range. The status bar always shows the active
core version and contract. That identity is rendered from the already-loaded
local core before any registry request begins, so an offline, malformed, or
untrusted registry cannot make the active selection appear blank; registry
errors remain visible separately in the status label.

Activation is a between-operation change. The dialog prepares and validates a
candidate, but `ProcessingService` refuses the final swap while realtime,
capture-backed experiment work, async batch work, or an offline synchronous
batch owns the current core. Stop those operations and retry.

Moving to an older semantic version uses a distinct, default-No downgrade
confirmation. Publication remains advisory: neither a new `latest.json` nor a
profile changes the active core. Profile metadata may declare an optional
processing contract; ConfigTabs marks a mismatch incompatible. Regenerating a
recorded HDF5 file with a different active identity warns the operator and
records the newly leased identity without silently switching.

## Resolution and trust chain

1. Fetch `{base}/{channel}/processing-core/index.json` over HTTPS with a
   20-second timeout and parse the schema-v1 history index defensively.
2. Fetch and validate `{base}/{channel}/processing-core/latest.json`
   independently. Its manifest version is the sole channel-active pointer;
   `index.active_version` is advisory and a disagreement produces a partial-
   publication warning rather than leading the selector.
3. Fetch the selected version's immutable schema-v2 manifest over HTTPS,
   hash its raw bytes, and cross-check its identity and native artifact fields
   against the mutable index.
4. Download the OS/architecture-matched shared library as bounded 64 KiB
   chunks over HTTPS with a 120-second absolute deadline; verify its declared
   byte size and SHA-256.
5. Materialize it atomically at
   `<cache>/<version>/<sha256>/<filename>`, guarded by a directory lock with
   stale-lock recovery. `.ready.json` is written last.
6. Immediately before module load, verify SHA-256 again and apply the persisted
   mandatory platform trust scheme. Windows validates Authenticode, requires
   the signer public key's DER SPKI SHA-256 compiled into the application, and
   uses restricted DLL dependency search. Linux validates the manifest's
   detached Ed25519 signature over the artifact bytes and requires the signer
   key's DER SPKI SHA-256 to match the compiled
   `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` pin (see
   [Linux signing](../../docs/architecture/processing-core-linux-signing.md)).
   Other platforms reject activation until their named verifier exists. Then
   negotiate ABI/contract/runtime identity and run the plugin self-test.
7. At a quiescent boundary, synchronize the complete verified identity/path in
   `QSettings` through `ProcessingService`'s locked pre-commit callback, then
   swap the live kernel. A settings error restores the prior logical selection
   and leaves the prior kernel usable; there is no activate-then-block window.
   Startup restores the cached artifact through the same digest/trust/ABI
   checks before capture begins.

The desktop establishes the stable `MIB Studio` / `MIB Studio Qt` settings
identity before any preference is read. On the first upgraded startup it copies
every missing key from Qt's former `Unknown Organization` namespace, preserves
all values already written in the stable namespace, leaves the legacy store
untouched, and records completion only after successful synchronization.
Startup fails closed if that migration cannot be persisted, rather than losing
an explicit core selection or unrelated application preferences silently.

The C ABI, registry, cache, and loader are cross-platform; Linux CI loads
real `.so` fixtures and an Ed25519-signed core through `dlopen`. The Windows
Authenticode and Linux Ed25519 detached-signature verifiers are both
implemented behind the injected trust seam; macOS and unknown schemes fail
closed. A13/#245 keeps the *live* signed `.so` publication gate open: no
production Linux keypair or repository pin exists yet, so an unpinned
production Linux build still refuses activation. Debug builds alone may set
`MIB_STUDIO_ALLOW_UNSIGNED_PROCESSING_CORE=1` for local loader fixtures, or
`MIB_STUDIO_PROCESSING_CORE_ED25519_SPKI_SHA256` to pin a test signer.

## Configuration

- `MIB_STUDIO_PROCESSING_CORE_BASE_URL` — registry base; default
  `https://updates.yofo.bio`.
- `MIB_STUDIO_PROCESSING_CORE_CACHE_DIR` — absolute persistent cache root;
  default is the app-local data directory's `processing-cores` folder.
- `MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256` — CMake cache value compiled into
  the production app as the approved Authenticode signer public-key hash.
  Official stable/beta builds enable
  `MIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI`, so CMake rejects an absent or
  malformed repository pin before packaging. Generic local/fork builds leave
  that requirement off and fail closed if native activation is attempted
  without a pin. Debug builds alone can use
  `MIB_STUDIO_PROCESSING_CORE_SIGNER_SPKI_SHA256` as a local override.
- `MIB_STUDIO_PROCESSING_CORE_VERSION` — administrator hard pin. Other
  versions are disabled and experiment start fails closed until the pinned
  core is active.

## Known boundary

The engine ABI v1 selects mask generation and empty-frame classification.
Metrics, contour filtering, tracking, target decisions, and orchestration
remain host-owned and consume the selected mask. A version that changes those
semantics is not fully hot-swappable yet; GitHub issue #242 (A7) remains open.
Independent Windows ABI-fixture auditing (#239/A8), A→B→A/TSan stress
(#241/A10), and profile/review/downgrade UI policy (#243/A11) also remain
explicit follow-ups. Deterministic settings-write fault injection now covers
the A10 persistence rollback boundary; the live A→B→A and concurrency evidence
are still required before closing that issue.
Release paths now require and validate the repository SPKI and compare it with
the DLL's actual Authenticode signer, but provisioning the real certificate,
pin, R2 publication, and an on-hardware Windows exercise remain live-environment
gates tracked under A12.

## Compatibility diagnostics (#394)

Catalog metadata eligibility is evaluated once by `evaluateCompatibility` for
both list presentation and activation. `CompatibilityResult` contains a typed
reason, diagnostic, required value and actual value. Failure precedence is OS,
architecture, ABI, processing contract, valid application bounds, minimum,
maximum, runtime fingerprint and administrator pin. Rejected items expose the
diagnostic in their label and tooltip and the reason in `Qt::UserRole`.

This preserves exact ABI/contract/runtime matching and inclusive app bounds.
Metadata compatibility is not artifact trust: download integrity, signature
verification, descriptor validation and loader self-test still run unchanged.
Future capability negotiation and structured loader failures remain separate
work; this catalog model describes only the current host's eligibility checks.
