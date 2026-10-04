# Processing Core dialog: core lines and contract filter (T1.1c) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Status: active (2026-10-04). Branch `feat/t1.1c-dialog-core-lines`, stacked on `feat/t1.1b-core-lines-pr2` (#481).

**Goal:** The desktop Processing Core dialog lists signed cores from both registry trees: the Contract-1 tree `{channel}/processing-core/` and the absdiff-laplacian line `{channel}/processing-core/absdiff-laplacian/`. It shows each core's line and contract, offers only cores matching the active profile's processing contract (others are shown with the reason), and can download, verify, cache, activate and restore an ABI-v2 Contract-2 core.

**Architecture:** All registry semantics stay in the Qt-Core-only `ProcessingCoreCatalog` (unit-tested without a GUI). The catalog learns the line schemas, ties `entrypoint` to the engine ABI, and gains a pure `buildCoreOptions` that merges trees and decides offerability. `ProcessingCoreDialog` only fetches the trees, renders the options and runs the existing download/verify/activate path; that path already passes ABI and contract from the plugin entry to the loader, which accepts ABI 2 / Contract 2. The selection persists the line.

**Tech Stack:** C++17, Qt 6 (Core, Network, Widgets), the existing `support/assert.h` test harness.

**Spec:** `docs/exec-plans/active/2026-09-25-contract2-safe-rollout.md` T1.1 ("The Processing Core dialog shows the contract and only offers cores that match the active profile's contract") and Phase 3 T3.1 ("A mismatch between profile and core is explained, never silently fixed"); `docs/portable-processing-sync.md` (absdiff-laplacian registry line, schema in `scripts/release/publish-processing-core-line.py`).

## Global Constraints

- A core is offered only if it has a plugin for this OS/arch, the app version is in its range, its ABI/contract/entrypoint pair is one this host loads ((1, 1, `mib_processing_get_api`) or (2, 2, `mib_processing_get_api_v2`)), its runtime fingerprint equals the host's, and its contract equals the active profile's `processing_contract_version`.
- A Contract-1-only app must keep working against today's registry: the absdiff tree is optional, and a 404 means "no cores on that line yet", not an error.
- No change to signing trust: same compiled SPKI pins, same verifier.
- The administrator pin (`MIB_STUDIO_PROCESSING_CORE_VERSION`) keeps overriding.
- Vault maintenance plus `scripts/check_docs.py`.

## Ruling (made before execution)

- A core whose contract differs from the profile's is **listed but not selectable**, with "requires a Contract-N profile (active profile: Contract M)". The spec's "only offers" is met (it cannot be activated), and T3.1's "explained, never silently fixed" is met (it is visible with the reason). Cost if wrong: one filter change to hide the rows instead.

## Review Focus

1. **Today's registry, where the absdiff tree 404s on stable.** The dialog lists the Contract-1 history exactly as before, with no error banner (Task 3).
2. **A Contract-2 profile with only Contract-1 cores published.** Every row shows the contract reason; Prepare stays disabled (Task 2 `buildCoreOptions`).
3. **A line manifest whose `line` disagrees with the tree it was fetched from**, or an ABI-2 plugin with the v1 entrypoint. Refused at parse time (Task 1).
4. **Restore after restart of a persisted ABI-2 core.** Uses the persisted ABI and contract, as today; the line is persisted for display (Task 3).
5. **The same version string on both lines** (for example both trees publish 0.1.0). Rows and selection keep them apart by line (Task 2 test).

---

### Task 1: Catalog parses the line schemas; entrypoint follows the ABI

**Files:** `include/frontend/utils/ProcessingCoreCatalog.h`, `src/frontend/utils/ProcessingCoreCatalog.cpp`, `tests/frontend/processing_core_catalog_test.cpp`.

**Interfaces (produces):**
- `VersionEntry::line` (`QString`, "subtract-ring" for the legacy schemas) and `VersionEntry::engineAbiVersion` (`int`; 1 for legacy, else the line manifest's).
- `ParseResult::line`.
- `parseIndex`: accepts `processing_core_index_schema_version` 1 (legacy, line "subtract-ring") or `processing_core_line_index_schema_version` 1 (requires non-empty `line`).
- `parseVersionManifest`: accepts `processing_core_manifest_schema_version` 2 (legacy, release tag under `wheel`) or `processing_core_line_manifest_schema_version` 1 (`line`, `release_tag`, `release_url`, `engine_abi_version` at the top level).
- Native plugin validity: `entrypoint` must be `mib_processing_get_api` when `engine_abi_version` is 1 and `mib_processing_get_api_v2` when it is 2.
- `samePublishedVersion` also compares `line`.

- [ ] **Step 1: Failing tests.** Append to `processing_core_catalog_test.cpp` before `return mib::test::exitCode();`:

```cpp
    // ---- absdiff-laplacian line (native-only, ABI 2, Contract 2) ----------------
    const QByteArray linePlugin = R"({"filename":"mib_processing_core-absdiff-laplacian-0.1.0-windows_x86_64.dll",
       "os":"windows","arch":"x86_64","url":"https://example/c2.dll",
       "sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       "size_bytes":7,"engine_abi_version":2,"contract_version":2,
       "runtime_fingerprint":"windows-x86_64-msvc1942-md-cxx17","entrypoint":"mib_processing_get_api_v2",
       "app_min_version":"1.0.0","app_max_version":null,"signing":{"scheme":"authenticode","required":true}})";
    const QByteArray lineIndex = R"({"processing_core_line_index_schema_version":1,"line":"absdiff-laplacian",
       "channel":"beta","active_version":"0.1.0","versions":[{"version":"0.1.0","contract_version":2,
       "engine_abi_version":2,"published_at":"2026-10-04T00:00:00Z",
       "release_tag":"mib-processing-absdiff-laplacian-v0.1.0","release_url":"https://example/r",
       "manifest_url":"https://updates.example/beta/processing-core/absdiff-laplacian/versions/0.1.0.json",
       "native_plugins":[)" + linePlugin + R"(]}]})";
    const auto lineParsed = frontend::processingcorecatalog::parseIndex(lineIndex);
    MIB_REQUIRE(lineParsed.ok, lineParsed.error.toStdString());
    MIB_EXPECT(lineParsed.line == "absdiff-laplacian" && lineParsed.versions.front().line == "absdiff-laplacian" &&
                   lineParsed.versions.front().engineAbiVersion == 2,
               "line index carries its line and ABI");
    MIB_EXPECT(parsed.line == "subtract-ring" && parsed.versions.front().line == "subtract-ring" &&
                   parsed.versions.front().engineAbiVersion == 1,
               "legacy index is the subtract-ring line at ABI 1");
    const QByteArray lineManifest = R"({"processing_core_line_manifest_schema_version":1,"line":"absdiff-laplacian",
       "channel":"beta","version":"0.1.0","published_at":"2026-10-04T00:00:00Z","contract_version":2,
       "engine_abi_version":2,"release_tag":"mib-processing-absdiff-laplacian-v0.1.0","release_url":"https://example/r",
       "manifest_url":"https://updates.example/beta/processing-core/absdiff-laplacian/versions/0.1.0.json",
       "native_plugins":[)" + linePlugin + R"(]})";
    const auto lineLatest = frontend::processingcorecatalog::parseVersionManifest(lineManifest);
    MIB_REQUIRE(lineLatest.ok, lineLatest.error.toStdString());
    MIB_EXPECT(lineLatest.version.releaseTag == "mib-processing-absdiff-laplacian-v0.1.0" &&
                   lineLatest.version.line == "absdiff-laplacian",
               "line manifest identity parsed from top-level fields");
    const auto lineActive = frontend::processingcorecatalog::validateCanonicalActive(lineParsed, lineLatest);
    MIB_EXPECT(lineActive.ok && lineActive.version == "0.1.0", "line latest.json validates against its index");
    QByteArray wrongEntry = lineIndex;
    wrongEntry.replace("\"entrypoint\":\"mib_processing_get_api_v2\"", "\"entrypoint\":\"mib_processing_get_api\"");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(wrongEntry).ok,
               "an ABI-2 plugin with the v1 entrypoint is refused");
    QByteArray noLine = lineIndex;
    noLine.replace("\"line\":\"absdiff-laplacian\",", "");
    MIB_EXPECT(!frontend::processingcorecatalog::parseIndex(noLine).ok, "a line index without its line is refused");
```

- [ ] **Step 2: Build and run it to confirm it fails.** `ctest --test-dir build-ninja -R ^frontend\.processing_core_catalog$` (PowerShell). Expected: compile error (`line` is not a member) or failures.

- [ ] **Step 3: Implement.**
  - Add the fields to the header.
  - In `parseNativePlugin`, replace `plugin.entrypoint == QStringLiteral("mib_processing_get_api")` with the ABI-to-entrypoint rule: `(abi == 1 && ep == "mib_processing_get_api") || (abi == 2 && ep == "mib_processing_get_api_v2")`.
  - In `parseIndex`, read either schema key, set `result.line` (default "subtract-ring"; the line schema requires a non-empty `line`), and give each entry that line plus its `engine_abi_version` (default 1).
  - In `parseVersionManifest`, branch on the schema: legacy (unchanged, line "subtract-ring", ABI 1) or line (top-level `release_tag`/`release_url`, `line`, `engine_abi_version`).
  - Add `line` to `samePublishedVersion`.

- [ ] **Step 4: Run it and confirm it passes**, and that the existing assertions are unchanged.
- [ ] **Step 5: Commit** `feat(processing-core): catalog reads the absdiff-laplacian registry line`.

### Task 2: `buildCoreOptions`, the offerability decision

**Files:** the same three.

**Interfaces (produces):**

```cpp
struct HostIdentity {
    QString os, arch, appVersion, runtimeFingerprint;
    int profileContractVersion{0};  // 0 = unknown/unset: contract not enforced (legacy)
};
struct CoreOption {
    VersionEntry version;
    const NativePluginEntry* plugin{nullptr};  // into version.nativePlugins (stable while options live)
    bool channelActive{false};
    QString disabledReason;  // empty = offerable
};
QVector<CoreOption> buildCoreOptions(const QVector<ParseResult>& trees, const HostIdentity& host);
bool isLoadableAbi(const NativePluginEntry& plugin);  // (1,1,get_api) or (2,2,get_api_v2)
```

Rows are ordered by tree in the order given (subtract-ring first), each tree in its index order. `channelActive` is `version == tree.activeVersion`. The reason is the first that applies, in this order:
1. "not published for <os>/<arch>"
2. "requires app <min>–<max>"
3. "engine ABI <n> / contract <c> is not loadable by this app"
4. "built for another runtime (<fp>)"
5. "requires a Contract-<c> profile (active profile: Contract <p>)" — only when `profileContractVersion > 0`

- [ ] **Step 1: Failing tests** (append):

```cpp
    namespace pc = frontend::processingcorecatalog;
    auto legacy = parsed;           // subtract-ring tree (2.0.0 has a windows plugin, 1.0.0 none)
    legacy.activeVersion = "2.0.0";
    auto line = lineParsed;         // absdiff-laplacian tree
    line.activeVersion = "0.1.0";
    pc::HostIdentity host{"windows", "x86_64", "1.2.3", "windows-x86_64-msvc1942-md-cxx17", 1};
    auto options = pc::buildCoreOptions({legacy, line}, host);
    MIB_REQUIRE(options.size() == 3, "both trees merged");
    MIB_EXPECT(options[0].version.line == "subtract-ring" && options[0].disabledReason.isEmpty() &&
                   options[0].channelActive, "matching Contract-1 core offered on a Contract-1 profile");
    MIB_EXPECT(options[1].disabledReason.contains("not published"), "missing platform explained");
    MIB_EXPECT(options[2].version.line == "absdiff-laplacian" &&
                   options[2].disabledReason == "requires a Contract-2 profile (active profile: Contract 1)",
               "contract mismatch listed with its reason, not offered");
    host.profileContractVersion = 2;
    options = pc::buildCoreOptions({legacy, line}, host);
    MIB_EXPECT(options[0].disabledReason.contains("Contract-1 profile") && options[2].disabledReason.isEmpty(),
               "Contract-2 profile offers only the Contract-2 core");
    host.runtimeFingerprint = "windows-x86_64-msvc1999-md-cxx17";
    MIB_EXPECT(pc::buildCoreOptions({line}, host).front().disabledReason.contains("another runtime"),
               "runtime fingerprint mismatch explained");
    host.runtimeFingerprint = "windows-x86_64-msvc1942-md-cxx17";
    host.profileContractVersion = 0;
    MIB_EXPECT(pc::buildCoreOptions({legacy, line}, host)[2].disabledReason.isEmpty(),
               "unknown profile contract does not block (legacy profiles)");
    auto sameVersion = line;
    sameVersion.versions.front().version = "2.0.0";
    options = pc::buildCoreOptions({legacy, sameVersion}, host);
    MIB_EXPECT(options[0].version.line != options[2].version.line &&
                   options[0].version.version == options[2].version.version,
               "same version string on two lines stays two distinct rows");
```

- [ ] **Step 2: Run it and confirm it fails** (no `buildCoreOptions`).
- [ ] **Step 3: Implement** in the catalog (pure; uses `findNativePlugin`, `isAppCompatible`).
- [ ] **Step 4: Run it and confirm it passes.** **Step 5: Commit** `feat(processing-core): one offerability decision across core lines`.

### Task 3: Dialog fetches both trees and renders the options

**Files:** `include/frontend/dialogs/ProcessingCoreDialog.h`, `src/frontend/dialogs/ProcessingCoreDialog.cpp`, `src/frontend/utils/ProcessingCoreSettings.{h,cpp}` (persist `line`), `tests/frontend/processing_core_settings_test.cpp`.

- [ ] **Step 1: Failing settings test.** A selection round-trip keeps `line` ("absdiff-laplacian"), and a selection persisted before this change reads back `line` "subtract-ring".
- [ ] **Step 2:** Run it and confirm it fails.
- [ ] **Step 3: Implement.**
  - `ProcessingCoreSelection::line`, persisted as `ProcessingCore/Line`, default "subtract-ring".
  - Dialog state: `QVector<processingcorecatalog::ParseResult> trees_` and `QVector<processingcorecatalog::CoreOption> options_`, replacing `catalog_`.
  - `reload()` fetches the trees in sequence: `{channel}/processing-core` (required; errors as today), then `{channel}/processing-core/absdiff-laplacian` (optional: HTTP 404 on either `index.json` or `latest.json` means the tree is skipped silently; any other error shows a status line but keeps the Contract-1 rows). Each tree's `latest.json` is validated with `validateCanonicalActive` exactly as today.
  - `populate()` builds `HostIdentity` (platform, app version, bundled runtime fingerprint, and `backend_.processing().getProcessingConfig().processing_contract_version`) and renders one row per option: "<line> <version> · Contract <c> · ABI <a>", plus "— channel active", "— selected" and "— <reason>". Rows with a reason, or excluded by the administrator pin, are not selectable.
  - `selectedVersionIndex()` indexes `options_`. `prepareAndActivateSelected()` re-checks `disabledReason` and uses `options_[i].version` and `.plugin`. The manifest re-fetch compares with the parsed line manifest (Task 1 makes `parseVersionManifest` accept it).
  - `isHostCompatible()` is replaced by `processingcorecatalog::isLoadableAbi(plugin) && plugin.runtimeFingerprint == host fingerprint`.
  - Persist `selection.line = version.line`.
  - `updateActiveCoreLabel()` adds the persisted line: "Active core: absdiff-laplacian 0.1.0 · contract 2 · ABI 2 · signed".
- [ ] **Step 4:** Settings test passes. Run `frontend.processing_core_dialog` and `frontend.processing_core_settings` from PowerShell (`QT_QPA_PLATFORM=windows` handled by the test).
- [ ] **Step 5: Commit** `feat(processing-core): dialog lists both core lines and filters by profile contract`.

### Task 4: Docs and vault

- `docs/portable-processing-sync.md` "Desktop native selection": both trees, the offer rule, the reasons, the optional line tree.
- `knowledge_map/frontend/` note for the Processing Core dialog (find the existing one via `knowledge_map/Vault-Maintenance.md`), `knowledge_map/current-state/Recent-Work.md`, and the rollout plan status (T1.1c done; Phase 1 exit awaits the first absdiff tag).
- [ ] Run `python scripts/check_docs.py` (expected OK) and commit.

### Task 5: Verification and PR

- [ ] **Rig PC:** full build; `ctest --preset windows-ninja-test`; `-L "frontend|processing|release|scripts"`. Known flakes are named, not hidden.
- [ ] **Manual smoke with a local registry fixture** (no production R2):
  1. Serve two trees from a temp directory over HTTPS. The dialog refuses http, so use `python -m http.server` behind a self-signed cert only if trivial; otherwise rely on the dialog test plus the catalog tests and record that in the PR.
  2. Open the dialog in the dev build. Check the Contract-1 rows render as before with the stable channel, which has no absdiff tree.
- [ ] Push and open the PR against `develop`, stacked on #481.
