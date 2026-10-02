# Processing-core lines, PR 1 (T1.1b, internal) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Status: active (2026-10-02). Branch `feat/t1.1b-core-lines` from `develop`.

**Goal:** Build, name, version, audit and gold-check both processing-core lines (`subtract-ring`, Contract 1; `absdiff-laplacian`, Contract 2) in CI, without signing or publishing anything new.

**Architecture:** Each line gets its own version source, artifact name `mib_processing_core-<line>-<version>-<os>_<arch>`, release tag prefix `mib-processing-<line>-v`, and a sidecar that names its `algorithm`. The native release jobs build, stage, export-audit and gold-check both lines, and upload one artifact per line. Signing and publishing stay subtract-ring only, with the names and tag prefix updated. The absdiff line's signing, the registry subtree and the promote workflow are PR 2.

**Tech Stack:** CMake 3.24 (`file(GENERATE)`), MSVC/GCC, GitHub Actions (`python-wheel.yml`), Python 3.10+ (`ctypes`, `numpy`), Inno Setup.

**Spec:** `docs/exec-plans/active/2026-09-25-contract2-safe-rollout.md` (T1.1, T1.1b, Phase 1 exit gate), `docs/decisions/0007-one-contract-per-shipped-core.md` (points 6–7, Consequences).

## Decisions (Gavin, 2026-10-02)

- Rename both lines: Contract 1 becomes `subtract-ring` in artifact names and tags from its next release. Releases `mib-processing-v0.1.0`…`v0.2.1` stay as they are (immutable).
- Independent per-line versions. subtract-ring keeps the pyproject version, which the wheel still shares. absdiff-laplacian gets its own version file and starts at `0.1.0`.
- Registry: a new per-line subtree (PR 2). Existing registry keys are untouched.
- PR 1 is internal: nothing is signed or published for absdiff-laplacian. PR 2 follows for review.

## Global Constraints

- ADR 0007: one shipped core = one contract. subtract-ring exports exactly `mib_processing_get_api`; absdiff-laplacian exports exactly `mib_processing_get_api_v2`.
- The subtract-ring binary's science must not change. `processing.science_golden`, `processing.core_fixture_matrix` and the Contract-1 real reference must still pass unchanged.
- The app treats `releaseTag` and artifact filenames as opaque (`ProcessingCoreCache` only requires a safe filename), so renaming does not break shipped apps. Do not add filename-pattern checks to the app.
- The Windows release lane links OpenCV statically, and the audit forbids `opencv*` imports. Keep that rule for both lines.
- Line names are exactly `subtract-ring` and `absdiff-laplacian`. Tag prefixes are exactly `mib-processing-subtract-ring-v` and `mib-processing-absdiff-laplacian-v`.
- Vault maintenance is mandatory (`knowledge_map/Vault-Maintenance.md`). Run `scripts/check_docs.py` before each commit that touches markdown.

## Review Focus

1. **A subtract-ring release tag pushed between PR 1 and PR 2.** It must still build, sign and publish subtract-ring under the new names. Task 4 covers the tag-prefix, glob and asset-set edits, and Task 3 tests the publisher prefix.
2. **Both DLLs land in one `native-dist` directory.** The sign job's "exactly one DLL" glob would then fail. Each line must stage to its own artifact (Task 4 Step 3).
3. **Version drift.** The absdiff plugin descriptor's `core_version` must equal its sidecar `version` and the version in its tag. Task 2's sidecar test checks this.
4. **A real Contract-2 frame where the native core and the reference disagree** because of a config-mapping slip (for example `bg_subtract_threshold` vs `difference_threshold`) rather than science. Task 5's harness must fail loudly with the per-field diff, not be loosened.
5. **The installer keeps packing stale core DLLs** from old installs. Task 6 adds `[InstallDelete]` for `{app}\mib_processing_core*.dll` and a test that pins it.

---

### Task 1: Per-line version sources and plugin identity

**Files:**
- Create: `processing-cores/absdiff-laplacian.version` (one line: `0.1.0`)
- Modify: `src/backend/CMakeLists.txt` (version block at the top; `mib_add_processing_core_plugin`)
- Modify: `tests/CMakeLists.txt` (equivalence and v2 plugin test targets get `MIB_ABSDIFF_LAPLACIAN_VERSION`)
- Modify: `tests/processing/processing_core_contract2_equivalence_test.cpp:100,107`
- Test: `tests/release/test_core_line_versions.py`

**Interfaces:**
- Produces CMake variables `MIB_PROCESSING_CORE_VERSION` (subtract-ring, unchanged source: pyproject) and `MIB_ABSDIFF_LAPLACIAN_VERSION`.
- Changes `mib_add_processing_core_plugin(target contract)` to `mib_add_processing_core_plugin(target contract line version)`. It defines `MIB_PROCESSING_CORE_VERSION="<version>"` on the target (`line` is used by Task 2).

- [ ] **Step 1: Write the failing test**

```python
# tests/release/test_core_line_versions.py
"""Each processing-core line has exactly one version source (ADR 0007)."""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")


class CoreLineVersions(unittest.TestCase):
    def test_absdiff_laplacian_version_file(self) -> None:
        text = (REPO / "processing-cores" / "absdiff-laplacian.version").read_text(encoding="utf-8")
        self.assertEqual(text.count("\n"), 1, "one line, newline-terminated")
        self.assertRegex(text.strip(), SEMVER)

    def test_cmake_reads_each_line_version(self) -> None:
        cmake = (REPO / "src" / "backend" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("processing-cores/absdiff-laplacian.version", cmake)
        self.assertRegex(
            cmake, r"mib_add_processing_core_plugin\(mib_processing_core 1 subtract-ring \$\{MIB_PROCESSING_CORE_VERSION\}\)")
        self.assertRegex(
            cmake,
            r"mib_add_processing_core_plugin\(mib_processing_core_absdiff_laplacian 2 absdiff-laplacian "
            r"\$\{MIB_ABSDIFF_LAPLACIAN_VERSION\}\)")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `python tests/release/test_core_line_versions.py -v`
Expected: FAIL with `FileNotFoundError` on `absdiff-laplacian.version`.

- [ ] **Step 3: Implement**

Create `processing-cores/absdiff-laplacian.version` containing `0.1.0` followed by a newline.

Add this to `src/backend/CMakeLists.txt` after the pyproject block (around line 15):

```cmake
# absdiff-laplacian (Contract 2) is versioned independently of subtract-ring
# and the wheel (ADR 0007: releasing one line never rebuilds the other).
set(_mib_absdiff_version_file "${PROJECT_SOURCE_DIR}/processing-cores/absdiff-laplacian.version")
file(STRINGS "${_mib_absdiff_version_file}" MIB_ABSDIFF_LAPLACIAN_VERSION LIMIT_COUNT 1)
string(STRIP "${MIB_ABSDIFF_LAPLACIAN_VERSION}" MIB_ABSDIFF_LAPLACIAN_VERSION)
if(NOT MIB_ABSDIFF_LAPLACIAN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "Invalid absdiff-laplacian version in ${_mib_absdiff_version_file}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mib_absdiff_version_file}")
```

Change the function signature and compile definitions:

```cmake
function(mib_add_processing_core_plugin target contract line version)
    # ...sources unchanged...
    target_compile_definitions(${target} PRIVATE
        MIB_PROCESSING_CORE_PLUGIN_EXPORTS=1
        MIB_PROCESSING_CORE_VERSION="${version}"
        MIB_PROCESSING_BUNDLED_CONTRACT=${contract}
    )
```

and the calls:

```cmake
mib_add_processing_core_plugin(mib_processing_core 1 subtract-ring ${MIB_PROCESSING_CORE_VERSION})
mib_add_processing_core_plugin(mib_processing_core_absdiff_laplacian 2 absdiff-laplacian ${MIB_ABSDIFF_LAPLACIAN_VERSION})
```

In `tests/CMakeLists.txt`, after the `processing_core_contract2_equivalence_test` and `processing_core_v2_plugin_test` targets are created:

```cmake
target_compile_definitions(processing_core_contract2_equivalence_test PRIVATE
    MIB_ABSDIFF_LAPLACIAN_VERSION="${MIB_ABSDIFF_LAPLACIAN_VERSION}")
```

In `processing_core_contract2_equivalence_test.cpp`:

```cpp
    requirements.expectedVersion = MIB_ABSDIFF_LAPLACIAN_VERSION;
    // ...
    requirements.releaseTag = std::string("mib-processing-absdiff-laplacian-v") + MIB_ABSDIFF_LAPLACIAN_VERSION;
```

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `python tests/release/test_core_line_versions.py -v`. Expected: 2 passed.
Run (PowerShell, rig PC): `ctest --test-dir build-ninja -L processing --output-on-failure`. Expected: all pass. `processing.core_contract2_equivalence` now expects the plugin at `0.1.0`.

- [ ] **Step 5: Commit**

```bash
git add processing-cores/absdiff-laplacian.version src/backend/CMakeLists.txt tests/CMakeLists.txt \
  tests/processing/processing_core_contract2_equivalence_test.cpp tests/release/test_core_line_versions.py
git commit -m "build(processing): independent version per core line"
```

### Task 2: Per-line artifact names and sidecars

**Files:**
- Modify: `src/backend/CMakeLists.txt:218-264` (replace the two inline `file(GENERATE)` blocks with one function called per line)
- Modify: `tests/release/test_contract_version_consistency.py:56-61,83-90` (read the subtract-ring sidecar from the function arguments, not the first regex hit)
- Create: `scripts/check_core_line_sidecars.py`
- Modify: `tests/CMakeLists.txt` (ctest `release.core_line_sidecars`)

**Interfaces:**
- Consumes: `mib_add_processing_core_plugin(... line version)` from Task 1.
- Produces: `mib_set_processing_core_release_identity(target line version contract abi entrypoint)`. Its outputs are `<config dir>/mib_processing_core-<line>-<version>-<os>_<arch>.{dll|so}` and a matching `.json` sidecar with keys `schema_version, algorithm, version, filename, os, arch, engine_abi_version, contract_version, entrypoint, runtime_fingerprint, app_min_version, app_max_version, signing`.

- [ ] **Step 1: Write the failing check script**

```python
# scripts/check_core_line_sidecars.py
"""Validate the built processing-core artifacts and sidecars of every line.

Usage: check_core_line_sidecars.py <build config dir> <os> <arch>
"""
import json
import sys
from pathlib import Path

LINES = {
    "subtract-ring": {"contract_version": 1, "engine_abi_version": 1, "entrypoint": "mib_processing_get_api"},
    "absdiff-laplacian": {"contract_version": 2, "engine_abi_version": 2, "entrypoint": "mib_processing_get_api_v2"},
}


def main(argv: list[str]) -> int:
    root, os_name, arch = Path(argv[1]), argv[2], argv[3]
    suffix = ".dll" if os_name == "windows" else ".so"
    failures: list[str] = []
    for line, expected in LINES.items():
        sidecars = sorted(root.glob(f"mib_processing_core-{line}-*-{os_name}_{arch}.json"))
        if len(sidecars) != 1:
            failures.append(f"{line}: expected one sidecar, found {[p.name for p in sidecars]}")
            continue
        meta = json.loads(sidecars[0].read_text(encoding="utf-8"))
        stem = f"mib_processing_core-{line}-{meta['version']}-{os_name}_{arch}"
        checks = {
            "algorithm": line,
            "filename": stem + suffix,
            "os": os_name,
            "arch": arch,
            **expected,
        }
        for key, want in checks.items():
            if meta.get(key) != want:
                failures.append(f"{line}: {key}={meta.get(key)!r}, expected {want!r}")
        if sidecars[0].name != stem + ".json":
            failures.append(f"{line}: sidecar name {sidecars[0].name} does not match {stem}.json")
        if not (root / (stem + suffix)).is_file():
            failures.append(f"{line}: artifact {stem + suffix} missing")
    for failure in failures:
        print(f"FAIL {failure}")
    print("core line sidecars OK" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

In `tests/CMakeLists.txt` (inside the existing `if(Python3_EXECUTABLE)` scripts block):

```cmake
if(WIN32)
    set(_mib_core_os windows)
    set(_mib_core_arch x86_64)
else()
    set(_mib_core_os linux)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_mib_core_arch aarch64)
    else()
        set(_mib_core_arch x86_64)
    endif()
endif()
add_test(NAME release.core_line_sidecars
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_core_line_sidecars.py
            $<TARGET_FILE_DIR:mib_processing_core> ${_mib_core_os} ${_mib_core_arch})
set_tests_properties(release.core_line_sidecars PROPERTIES LABELS "release;processing")
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `cmake --build --preset windows-ninja-build` then `ctest --test-dir build-ninja -R ^release\.core_line_sidecars$ --output-on-failure`.
Expected: FAIL with `subtract-ring: expected one sidecar, found []` and the same for absdiff-laplacian.

- [ ] **Step 3: Implement the per-line identity function**

Replace `src/backend/CMakeLists.txt:218-264` (from `if(WIN32)` through the matching `endif()`) with:

```cmake
# Release identity of one core line: artifact name and signing sidecar.
# Names: mib_processing_core-<line>-<version>-<os>_<arch> (rollout plan T1.1).
function(mib_set_processing_core_release_identity target line version contract abi entrypoint)
    if(WIN32)
        set(_os windows)
        set(_arch x86_64)
        set(_scheme authenticode)
        if(MSVC)
            set(_fingerprint "windows-x86_64-msvc${MSVC_VERSION}-md-cxx17")
        else()
            set(_fingerprint "windows-x86_64-unknown-cxx17")
        endif()
    elseif(UNIX AND NOT APPLE)
        set(_os linux)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            set(_arch x86_64)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(_arch aarch64)
        else()
            set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        # The build cannot self-sign: release CI appends the Ed25519
        # public_key_spki_base64/signature_base64/public_key_spki_sha256 fields.
        set(_scheme ed25519)
        # Must match runtimeFingerprint() in BundledProcessingKernel.cpp.
        string(REGEX MATCH "^[0-9]+" _cxx_major "${CMAKE_CXX_COMPILER_VERSION}")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            set(_fingerprint "linux-${_arch}-clang${_cxx_major}-cxx17")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(_fingerprint "linux-${_arch}-gcc${_cxx_major}-cxx17")
        else()
            set(_fingerprint "unknown-platform-cxx17")
        endif()
    else()
        return()
    endif()
    set(_basename "mib_processing_core-${line}-${version}-${_os}_${_arch}")
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${_basename}")
    file(GENERATE
        OUTPUT "${PROJECT_BINARY_DIR}/$<CONFIG>/${_basename}.json"
        CONTENT "{\n  \"schema_version\": 1,\n  \"algorithm\": \"${line}\",\n  \"version\": \"${version}\",\n  \"filename\": \"$<TARGET_FILE_NAME:${target}>\",\n  \"os\": \"${_os}\",\n  \"arch\": \"${_arch}\",\n  \"engine_abi_version\": ${abi},\n  \"contract_version\": ${contract},\n  \"entrypoint\": \"${entrypoint}\",\n  \"runtime_fingerprint\": \"${_fingerprint}\",\n  \"app_min_version\": \"${MIB_PROCESSING_CORE_APP_MIN_VERSION}\",\n  \"app_max_version\": \"${MIB_PROCESSING_CORE_APP_MAX_VERSION}\",\n  \"signing\": {\"required\": true, \"scheme\": \"${_scheme}\"}\n}\n")
endfunction()

mib_set_processing_core_release_identity(mib_processing_core
    subtract-ring ${MIB_PROCESSING_CORE_VERSION} 1 1 mib_processing_get_api)
mib_set_processing_core_release_identity(mib_processing_core_absdiff_laplacian
    absdiff-laplacian ${MIB_ABSDIFF_LAPLACIAN_VERSION} 2 2 mib_processing_get_api_v2)
```

The sidecar's `contract_version` and `engine_abi_version` now come from CMake variables, so `tests/release/test_contract_version_consistency.py` can no longer regex a literal. Change both of its CMake reads (`:56-61`, `:83-90`) to parse the subtract-ring call:

```python
        match = re.search(
            r"mib_set_processing_core_release_identity\(mib_processing_core\s+subtract-ring\s+\S+\s+(\d+)\s+(\d+)\s+mib_processing_get_api\)",
            backend_cmake,
        )
        self.assertIsNotNone(match, "subtract-ring release identity not found in backend CMake")
        declarations["src/backend/CMakeLists.txt subtract-ring sidecar"] = int(match.group(1))
```

(the engine-ABI test uses `int(match.group(2))`).

- [ ] **Step 4: Run the tests and confirm they pass**

Run: rebuild, then `ctest --test-dir build-ninja -R "^release\.core_line_sidecars$|^processing\." --output-on-failure` and `python tests/release/test_contract_version_consistency.py -v`.
Expected: `core line sidecars OK`; all processing tests pass (the loader tests use `$<TARGET_FILE:...>`, so they follow the rename); consistency test passes.

- [ ] **Step 5: Commit**

```bash
git add src/backend/CMakeLists.txt scripts/check_core_line_sidecars.py tests/CMakeLists.txt tests/release/test_contract_version_consistency.py
git commit -m "build(processing): per-line artifact names and sidecars with algorithm"
```

### Task 3: Per-line version bump, tag prefixes and the publisher prefix

**Files:**
- Modify: `scripts/bump_mib_processing_version.py`
- Modify: `scripts/test_bump_mib_processing_version.py`
- Modify: `scripts/release/publish-processing-core.py:66,130-132,965`
- Test: the bump script tests plus a new publisher prefix test in `scripts/release/` (follow the existing test file there; if none exists, add `tests/release/test_publisher_tag_prefix.py`).

**Interfaces:**
- Produces `LINES = {"subtract-ring": ..., "absdiff-laplacian": ...}` in the bump script and the CLI `bump_mib_processing_version.py --line <name> <version> [--create-tag]`. `--line` defaults to `subtract-ring` so existing callers keep working.
- Produces publisher `_TAG_PREFIX = "mib-processing-subtract-ring-v"`. PR 2 makes the prefix per line.

- [ ] **Step 1: Write the failing tests** (add to `scripts/test_bump_mib_processing_version.py`)

```python
    def test_absdiff_line_bumps_only_its_version_file(self) -> None:
        root = self.make_repo()  # existing helper: writes pyproject + __init__
        version_file = root / "processing-cores" / "absdiff-laplacian.version"
        version_file.parent.mkdir(parents=True, exist_ok=True)
        version_file.write_text("0.1.0\n", encoding="utf-8")
        updates, current = bump.plan_updates(root, "0.2.0", line="absdiff-laplacian")
        self.assertEqual(current, "0.1.0")
        self.assertEqual(set(updates), {version_file})
        self.assertEqual(updates[version_file], "0.2.0\n")

    def test_tag_prefix_per_line(self) -> None:
        self.assertEqual(bump.tag_for("subtract-ring", "0.3.3"), "mib-processing-subtract-ring-v0.3.3")
        self.assertEqual(bump.tag_for("absdiff-laplacian", "0.1.0"), "mib-processing-absdiff-laplacian-v0.1.0")
        with self.assertRaises(ValueError):
            bump.tag_for("ring", "0.1.0")
```

If the test file has no `make_repo` helper, write the same fixture inline: a temp dir with `bindings/python/pyproject.toml` (`version = "0.3.2"`) and `bindings/python/python/mib_processing/__init__.py` (`__version__ = "0.3.2"`).

- [ ] **Step 2: Run them and confirm they fail**

Run: `python scripts/test_bump_mib_processing_version.py -v`
Expected: FAIL. `plan_updates()` does not accept `line`, and `tag_for` is undefined.

- [ ] **Step 3: Implement**

```python
LINES = {
    "subtract-ring": "pyproject",              # shares the wheel version
    "absdiff-laplacian": Path("processing-cores/absdiff-laplacian.version"),
}
_LINE_VERSION = re.compile(r"^([0-9][A-Za-z0-9._+!-]*)\n$")


def tag_for(line: str, version: str) -> str:
    if line not in LINES:
        raise ValueError(f"Unknown processing-core line: {line!r}")
    return f"mib-processing-{line}-v{validate_version(version)}"


def plan_updates(repo_root: Path, version: str, line: str = "subtract-ring") -> tuple[dict[Path, str], str]:
    version = validate_version(version)
    if line not in LINES:
        raise ValueError(f"Unknown processing-core line: {line!r}")
    if LINES[line] != "pyproject":
        path = repo_root / LINES[line]
        match = _LINE_VERSION.fullmatch(path.read_text(encoding="utf-8"))
        if match is None:
            raise ValueError(f"{LINES[line]} must hold exactly one version line")
        return {path: f"{version}\n"}, match.group(1)
    # ...existing pyproject + __init__ body, unchanged...
```

Make `create_committed_tag(repo_root, version, message, line="subtract-ring")` use `tag_for(line, version)`. It checks HEAD for the line's own file: pyproject plus `__init__` for subtract-ring, the `.version` file for absdiff-laplacian. Delete the module-level `TAG_PREFIX` and replace every use with `tag_for`. Add `parser.add_argument("--line", choices=sorted(LINES), default="subtract-ring")` and pass it through.

Publisher: `_TAG_PREFIX = "mib-processing-subtract-ring-v"`. Lines 130-132 and 965 already use the constant, so no other edit is needed there.

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `python scripts/test_bump_mib_processing_version.py -v` (all pass) and `python -c "import importlib.util as u; s=u.spec_from_file_location('p','scripts/release/publish-processing-core.py'); m=u.module_from_spec(s); s.loader.exec_module(m); assert m._TAG_PREFIX=='mib-processing-subtract-ring-v'"`.

- [ ] **Step 5: Commit**

```bash
git add scripts/bump_mib_processing_version.py scripts/test_bump_mib_processing_version.py scripts/release/publish-processing-core.py
git commit -m "release(processing): per-line version bump and subtract-ring tag prefix"
```

### Task 4: Release workflow builds, audits and uploads both lines

**Files:**
- Modify: `.github/workflows/python-wheel.yml`. Tag trigger (`:6`), `validate-source-version` (`:90-123`), the `-DMIB_REQUIRE_PROCESSING_CORE_APP_COMPAT` ref check (`:361` and the Linux twin), both native build jobs (`:290-670`), the sign jobs' download step and globs (`:672-891`), and the publish job's `expected_native` sets and `removeprefix` (`:943-950`, `:1008-1016`).

**Interfaces:**
- Consumes: Task 2 artifact names, Task 3 tag prefix.
- Produces: per-line artifacts `mib_processing-native-<os>-<arch>-<line>-unsigned` (4 artifacts). The sign and publish jobs download only the `subtract-ring` ones.

- [ ] **Step 1: Tag trigger and identity**

```yaml
on:
  push:
    tags: ['mib-processing-subtract-ring-v*']
```

In `validate-source-version`, replace the `refs/tags/mib-processing-v` prefix with `refs/tags/mib-processing-subtract-ring-v`. Do the same in the two `MIB_REQUIRE_PROCESSING_CORE_APP_COMPAT` expressions and in both publish-job `removeprefix(...)` calls. (A pushed tag with the old `mib-processing-v` prefix now triggers nothing. The old releases are immutable and stay published.)

- [ ] **Step 2: Build both lines**

Windows (`:364-370`) and Linux (`:529-535`) build targets gain `mib_processing_core_absdiff_laplacian` and `processing_core_v2_plugin_test`. The verify step's `ctest -R` becomes `processing\.core_(abi_c|abi_v2_c|loader|cache|fixture_matrix|v2_plugin)`. Also add `processing_core_abi_v2_c_test` to the build targets.

- [ ] **Step 3: Stage each line into its own directory**

Replace "Locate and validate release assets" (Windows `:412-437`) with a loop over the lines. Each line is read from `build\Release\*.json`, so there is no hard-coded version:

```powershell
          $lines = @{
            'subtract-ring'     = @{ contract = 1; abi = 1; entry = 'mib_processing_get_api' }
            'absdiff-laplacian' = @{ contract = 2; abi = 2; entry = 'mib_processing_get_api_v2' }
          }
          $nativeDist = Join-Path $env:RUNNER_TEMP 'mib-processing-native-dist'
          Remove-Item -LiteralPath $nativeDist -Recurse -Force -ErrorAction SilentlyContinue
          foreach ($line in $lines.Keys) {
            $sidecars = @(Get-ChildItem build\Release -Filter "mib_processing_core-$line-*-windows_x86_64.json")
            if ($sidecars.Count -ne 1) { throw "$line: expected one sidecar, found $($sidecars.Count)" }
            $meta = Get-Content $sidecars[0].FullName -Raw | ConvertFrom-Json
            $want = $lines[$line]
            if ($meta.algorithm -ne $line -or $meta.contract_version -ne $want.contract -or
                $meta.engine_abi_version -ne $want.abi -or $meta.entrypoint -ne $want.entry) {
              throw "$line: sidecar identity mismatch: $($sidecars[0].Name)"
            }
            $dll = Get-Item -LiteralPath (Join-Path build\Release $meta.filename)
            $dir = Join-Path $nativeDist $line
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
            Copy-Item $dll.FullName, $sidecars[0].FullName -Destination $dir
            "$($line -replace '-','_')_dll=$(Join-Path $dir $dll.Name)" >> $env:GITHUB_OUTPUT
          }
          # subtract-ring line version must equal pyproject (wheel identity).
          $pyproject = Get-Content bindings/python/pyproject.toml -Raw
          if ($pyproject -notmatch '(?m)^version\s*=\s*"([^"]+)"\s*$') { throw 'pyproject version missing' }
          $sr = Get-Content (Get-ChildItem "$nativeDist\subtract-ring\*.json").FullName -Raw | ConvertFrom-Json
          if ($sr.version -ne $matches[1]) { throw "subtract-ring $($sr.version) != pyproject $($matches[1])" }
```

The Linux job (`:540-578`) gets the same loop in bash, using `jq` (present on `ubuntu-latest`) to read the sidecar fields and writing `subtract_ring_so=` and `absdiff_laplacian_so=` outputs.

- [ ] **Step 4: Export audit per line**

Wrap the existing dumpbin and `nm` audits in a loop over the two staged binaries, each with its expected single export (`mib_processing_get_api` or `mib_processing_get_api_v2`). Keep the forbidden-import rules exactly as they are. The Linux Ed25519 rehearsal and publisher validation run per line with `expected_contract_version=1` or `2` and `release_tag=f"mib-processing-{line}-v{version}"`. For absdiff-laplacian, call `build_native_plugin_entries` directly, which takes `expected_contract_version` as an argument; the prefix check only applies to `--release-tag` parsing.

- [ ] **Step 5: Upload one artifact per line**

```yaml
      - name: Upload unsigned subtract-ring plugin
        uses: actions/upload-artifact@v6
        with:
          name: mib_processing-native-windows-x86_64-subtract-ring-unsigned
          path: ${{ runner.temp }}/mib-processing-native-dist/subtract-ring/*
          if-no-files-found: error
      - name: Upload unsigned absdiff-laplacian plugin
        uses: actions/upload-artifact@v6
        with:
          name: mib_processing-native-windows-x86_64-absdiff-laplacian-unsigned
          path: ${{ runner.temp }}/mib-processing-native-dist/absdiff-laplacian/*
          if-no-files-found: error
```

(The Linux job does the same with `linux-x86_64`.) In both sign jobs and the publish job, change `download-artifact` names to the `-subtract-ring-unsigned` artifacts. Change the Windows sign glob to `mib_processing_core-subtract-ring-*-windows_x86_64.dll` and the Linux glob to `mib_processing_core-subtract-ring-*-linux_x86_64.so`. Change the publish job's `expected_native` sets to `f"mib_processing_core-subtract-ring-{version}-windows_x86_64.dll"` and the other three equivalents.

- [ ] **Step 6: Verify**

Run: `python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/python-wheel.yml'))"` (parses). Then push the branch, open the PR and confirm in CI that **Build native processing core (Windows x64)** and **(Linux x86_64)** pass and list four uploaded `*-unsigned` artifacts. Sign and publish run only on tags and cannot run on the PR. Review their diff by hand against Review Focus item 1.

- [ ] **Step 7: Commit**

```bash
git add .github/workflows/python-wheel.yml
git commit -m "ci(processing): build, audit and upload both core lines"
```

### Task 5: Native Contract-2 gold check against the built binary

**Files:**
- Create: `scripts/run_native_core_conformance.py`
- Modify: `tests/CMakeLists.txt` (ctest `processing.native_core_contract2_gold`, Python + numpy required)
- Modify: `.github/workflows/python-wheel.yml` (one step per native job, after staging)

**Interfaces:**
- Consumes: the absdiff-laplacian binary path (staged by Task 4, `$<TARGET_FILE:mib_processing_core_absdiff_laplacian>` locally), `scripts/conformance/focus-50v-real.npz` and `focus-50v-real-contract2.json`.
- Produces the CLI `run_native_core_conformance.py --core <path> --frames-npz <npz> --reference <json>`. It exits 0 on a full match, 1 on mismatch and 2 on load or ABI error.

The existing `compare_metrics.run_comparison` requires host-only fields (`track_*`, `mask_sha256`, `series_images_sha256`) that the core does not produce. The harness therefore compares the core-owned per-object fields itself, at the same tolerance (`compare_metrics.DEFAULT_NUMERIC_TOLERANCE`).

- [ ] **Step 1: Write the harness** (complete file)

```python
#!/usr/bin/env python3
"""Run a built Contract-2 processing core (DLL/.so) over the real-frame
fixture through engine ABI v2 and compare its per-object metrics with the
Contract-2 gold reference (rollout plan Phase 1 exit gate)."""
from __future__ import annotations

import argparse
import ctypes as C
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_metrics  # noqa: E402

ABI2 = 2
CONTRACT2 = 2
FIELDS = {  # reference key -> metrics attribute
    "object_id": "object_id", "object_count": "object_count", "is_valid": "is_valid",
    "touches_border": "touches_border", "is_target_group": "is_target_group", "in_range": "in_range",
    "area": "area", "deformability": "deformability", "area_ratio": "area_ratio",
    "laplacian_variance": "laplacian_variance", "youngs_modulus": "youngs_modulus",
    "brightness_q1": "brightness_q1", "brightness_q2": "brightness_q2",
    "brightness_q3": "brightness_q3", "brightness_q4": "brightness_q4",
}


class ImageView(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("width", C.c_uint32), ("height", C.c_uint32),
                ("stride_bytes", C.c_uint64), ("data", C.c_void_p), ("data_size_bytes", C.c_uint64)]


class Roi(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("x", C.c_int32), ("y", C.c_int32),
                ("width", C.c_int32), ("height", C.c_int32)]


class ConfigV2(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("gaussian_blur_size", C.c_int32),
                ("difference_threshold", C.c_int32), ("morphology_kernel_size", C.c_int32),
                ("morphology_iterations", C.c_int32), ("empty_frame_pixel_threshold", C.c_int32),
                ("laplacian_kernel_size", C.c_int32), ("flags", C.c_uint32),
                ("filters", C.c_void_p), ("science_config_json", C.c_char_p),
                ("science_config_json_size", C.c_uint64), ("reserved_u32", C.c_uint32 * 16)]


class Metrics(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("object_id", C.c_int32), ("object_count", C.c_int32),
                ("is_valid", C.c_int32), ("touches_border", C.c_int32), ("is_target_group", C.c_int32),
                ("track_id", C.c_int32)] + [(n, C.c_double) for n in (
                    "area", "deformability", "area_ratio", "laplacian_variance", "youngs_modulus",
                    "centroid_x", "centroid_y", "bbox_x", "bbox_y", "bbox_width", "bbox_height",
                    "brightness_q1", "brightness_q2", "brightness_q3", "brightness_q4")] + [
                ("in_range", C.c_int32), ("in_channel", C.c_int32), ("reserved_u32", C.c_uint32 * 8)]


class ObjectBuffer(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("capacity", C.c_uint32), ("count", C.c_uint32),
                ("required", C.c_uint32), ("objects", C.POINTER(Metrics))]


class Descriptor(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("engine_abi_version", C.c_uint32),
                ("contract_version", C.c_uint32), ("capabilities", C.c_uint32),
                ("core_version", C.c_char_p), ("build_id", C.c_char_p),
                ("runtime_fingerprint", C.c_char_p), ("reserved_u64", C.c_uint64 * 8)]


ERR = C.c_char_p
CTX = C.c_void_p
DescriptorFn = C.CFUNCTYPE(C.POINTER(Descriptor))
CreateFn = C.CFUNCTYPE(C.c_int, C.POINTER(CTX), ERR, C.c_size_t)
DestroyFn = C.CFUNCTYPE(None, CTX)
ProcessObjectsFn = C.CFUNCTYPE(C.c_int, CTX, C.POINTER(ImageView), C.POINTER(ImageView),
                               C.POINTER(ConfigV2), C.POINTER(Roi), C.c_double, C.c_void_p,
                               C.POINTER(ObjectBuffer), ERR, C.c_size_t)


class ApiV2(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("engine_abi_version", C.c_uint32),
                ("descriptor", DescriptorFn), ("create_context", CreateFn),
                ("destroy_context", DestroyFn), ("reset_context", C.c_void_p),
                ("process_mask", C.c_void_p), ("is_empty", C.c_void_p),
                ("process_objects", ProcessObjectsFn), ("self_test", C.c_void_p),
                ("reserved", C.c_void_p * 8)]


def view(image: np.ndarray) -> ImageView:
    image = np.ascontiguousarray(image, dtype=np.uint8)
    return ImageView(C.sizeof(ImageView), image.shape[1], image.shape[0], image.strides[0],
                     image.ctypes.data, image.nbytes)


def load_api(path: Path) -> ApiV2:
    lib = C.CDLL(str(path))
    get_api = lib.mib_processing_get_api_v2
    get_api.restype = C.c_int
    get_api.argtypes = [C.c_uint32, C.c_uint32, C.POINTER(ApiV2), ERR, C.c_size_t]
    api = ApiV2()
    err = C.create_string_buffer(512)
    status = get_api(ABI2, C.sizeof(ApiV2), C.byref(api), err, len(err))
    if status != 0:
        raise RuntimeError(f"get_api_v2 failed ({status}): {err.value.decode()}")
    desc = api.descriptor().contents
    if desc.engine_abi_version != ABI2 or desc.contract_version != CONTRACT2:
        raise RuntimeError(f"not an ABI-2/Contract-2 core: abi={desc.engine_abi_version} "
                           f"contract={desc.contract_version}")
    api._lib = lib  # keep the library loaded
    return api


def kernel_config(config: dict, science_json: bytes) -> ConfigV2:
    cfg = ConfigV2()
    cfg.struct_size = C.sizeof(ConfigV2)
    cfg.gaussian_blur_size = int(config["gaussian_blur_size"])
    cfg.difference_threshold = int(config["difference_threshold"])
    cfg.morphology_kernel_size = int(config["morph_kernel_size"])
    cfg.morphology_iterations = int(config["morph_iterations"])
    cfg.empty_frame_pixel_threshold = int(config["empty_frame_pixel_threshold"])
    cfg.laplacian_kernel_size = int(config.get("laplacian_kernel_size", 3))
    cfg.science_config_json = science_json
    cfg.science_config_json_size = len(science_json)
    return cfg


def run(core: Path, npz: Path, reference: Path) -> int:
    api = load_api(core)
    ref = json.loads(reference.read_text(encoding="utf-8"))
    with np.load(npz, allow_pickle=False) as archive:
        frames, backgrounds = archive["frames"], archive["backgrounds"]
        owner = archive["frame_background"]
        config = json.loads(str(archive["config_json"]))
        pixel = float(archive["pixel_to_micron"])
    config["processing_contract_version"] = 2
    if "bg_subtract_threshold" in config:
        config["difference_threshold"] = config.pop("bg_subtract_threshold")
    science = json.dumps(config).encode("utf-8")
    cfg = kernel_config(config, science)
    ctx = CTX()
    err = C.create_string_buffer(512)
    if api.create_context(C.byref(ctx), err, len(err)) != 0:
        raise RuntimeError(err.value.decode())
    candidate: dict[tuple[int, int], dict] = {}
    try:
        for index in range(frames.shape[0]):
            frame, background = frames[index], backgrounds[owner[index]]
            roi = Roi(C.sizeof(Roi), 0, 0, frame.shape[1], frame.shape[0])
            slots = (Metrics * 256)()
            buffer = ObjectBuffer(C.sizeof(ObjectBuffer), 256, 0, 0, slots)
            inp, bg = view(frame), view(background)
            status = api.process_objects(ctx, C.byref(inp), C.byref(bg), C.byref(cfg), C.byref(roi),
                                         pixel, None, C.byref(buffer), err, len(err))
            if status != 0:
                raise RuntimeError(f"frame {index}: process_objects {status}: {err.value.decode()}")
            for i in range(buffer.count):
                m = slots[i]
                candidate[(index, m.object_id)] = {k: getattr(m, a) for k, a in FIELDS.items()}
    finally:
        api.destroy_context(ctx)

    tol = compare_metrics.DEFAULT_NUMERIC_TOLERANCE
    failures = []
    for record in ref["frames"]:
        key = (int(record["index"]), int(record["object_id"]))
        got = candidate.pop(key, None)
        if got is None:
            failures.append(f"{key}: missing in native output")
            continue
        for field in FIELDS:
            if field not in record:
                continue
            want, have = record[field], got[field]
            if isinstance(want, bool) or field in ("object_id", "object_count"):
                ok = int(want) == int(have)
            elif want is None or (isinstance(have, float) and math.isnan(have)):
                ok = want is None and (have is None or math.isnan(have))
            else:
                ok = abs(float(have) - float(want)) <= tol * max(1.0, abs(float(want)))
            if not ok:
                failures.append(f"{key} {field}: native {have!r} != gold {want!r}")
    failures += [f"{key}: native-only object" for key in candidate]
    for failure in failures[:50]:
        print("FAIL", failure)
    print(f"native Contract-2 gold: {len(ref['frames'])} reference objects, {len(failures)} failure(s)")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--frames-npz", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    try:
        return run(args.core, args.frames_npz, args.reference)
    except (OSError, RuntimeError, KeyError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
```

`ConfigV2` deliberately declares `reserved_u32[16]`, the layout on `develop`. When #461 lands, the slot becomes `precomputed_mask` overlaying the first two words. The size and offsets are identical, so this declaration stays valid.

- [ ] **Step 2: Run it against the local build and inspect**

Run: `python scripts/run_native_core_conformance.py --core build-ninja/Release/mib_processing_core-absdiff-laplacian-0.1.0-windows_x86_64.dll --frames-npz scripts/conformance/focus-50v-real.npz --reference scripts/conformance/focus-50v-real-contract2.json`
Expected: `407 reference objects, 0 failure(s)`, exit 0.
If it fails, the cause is a mapping difference between the wheel record and the core output, not a reason to loosen the tolerance. Check, in order: the config key names the plugin's `ProcessingConfigJson` parser reads (fix the `kernel_config` keys to match), the ROI (use the config's ROI if `process_batch` applies one), the per-frame object identity (`object_id` one-based in both), and `in_range` vs `is_valid`. Record what was needed in the step's commit message.

- [ ] **Step 3: Register the ctest and the CI step**

```cmake
add_test(NAME processing.native_core_contract2_gold
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run_native_core_conformance.py
            --core $<TARGET_FILE:mib_processing_core_absdiff_laplacian>
            --frames-npz ${PROJECT_SOURCE_DIR}/scripts/conformance/focus-50v-real.npz
            --reference ${PROJECT_SOURCE_DIR}/scripts/conformance/focus-50v-real-contract2.json)
set_tests_properties(processing.native_core_contract2_gold PROPERTIES
    LABELS "processing;release" TIMEOUT 120)
```

In each native job, after staging, run the same script on the staged absdiff binary (`steps.native-assets.outputs.absdiff_laplacian_dll` or `_so`). Install numpy first: `python -m pip install numpy`.

- [ ] **Step 4: Verify**

Run: `ctest --test-dir build-ninja -R ^processing\.native_core_contract2_gold$ --output-on-failure` on the rig PC. Expected: pass. Then CI on the PR: both native jobs pass the new step.

- [ ] **Step 5: Commit**

```bash
git add scripts/run_native_core_conformance.py tests/CMakeLists.txt .github/workflows/python-wheel.yml
git commit -m "test(processing): Contract-2 gold against the built absdiff-laplacian binary"
```

### Task 6: The desktop installer stops shipping core DLLs

**Files:**
- Modify: `resources/installers/mib-studio-qt.iss:84` and its `[InstallDelete]` section (create one if absent)
- Test: `tests/release/test_installer_excludes_cores.py`

Cores reach rigs only as signed downloads (ADR 0007). The installer currently packs `{BuildDir}\*.dll`, which includes the unsigned dev cores. The installed v1.1.2 carries `mib_processing_core-0.2.0` and `-0.2.1`.

- [ ] **Step 1: Write the failing test**

```python
# tests/release/test_installer_excludes_cores.py
import re
import unittest
from pathlib import Path

ISS = Path(__file__).resolve().parents[2] / "resources" / "installers" / "mib-studio-qt.iss"


class InstallerExcludesCores(unittest.TestCase):
    def test_wildcard_dll_line_excludes_processing_cores(self) -> None:
        text = ISS.read_text(encoding="utf-8")
        line = next(l for l in text.splitlines() if re.search(r'\\\*\.dll"', l) and l.startswith("Source:"))
        self.assertIn('Excludes: "mib_processing_core*.dll"', line)

    def test_old_cores_are_removed_on_upgrade(self) -> None:
        text = ISS.read_text(encoding="utf-8")
        self.assertRegex(text, r'(?m)^\[InstallDelete\]')
        self.assertRegex(text, r'Type: files; Name: "\{app\}\\mib_processing_core\*\.dll"')


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it and confirm it fails.** Run `python tests/release/test_installer_excludes_cores.py -v`. Expected: 2 failures.

- [ ] **Step 3: Implement**

```
Source: "{#SourceDir}{#BuildDir}\*.dll"; DestDir: "{app}"; Excludes: "mib_processing_core*.dll"; Flags: ignoreversion
```

and, in `[InstallDelete]`:

```
Type: files; Name: "{app}\mib_processing_core*.dll"
```

- [ ] **Step 4: Run it and confirm it passes.** Expected: 2 passed. Also confirm the app never loaded cores from `{app}`: `git grep -n applicationDirPath -- src/backend/processing src/frontend/dialogs/ProcessingCoreDialog.cpp src/frontend/utils/ProcessingCore*` returns nothing.

- [ ] **Step 5: Commit**

```bash
git add resources/installers/mib-studio-qt.iss tests/release/test_installer_excludes_cores.py
git commit -m "installer: stop packing unsigned processing-core DLLs; remove old ones"
```

### Task 7: Documentation, ADR and vault

**Files:**
- Modify: `docs/exec-plans/active/2026-09-25-contract2-safe-rollout.md` (status header: T1.1b PR 1 done, PR 2 next)
- Modify: `docs/decisions/0007-one-contract-per-shipped-core.md` (point 7: the names now in effect, tags and artifacts; Status unchanged unless Gavin promotes it)
- Modify: `docs/portable-processing-sync.md` (line names, per-line versions, the version file)
- Modify: `knowledge_map/services/ProcessingService.md`, `knowledge_map/current-state/Recent-Work.md`, `knowledge_map/Vault-Maintenance.md` (map `processing-cores/*.version` and `scripts/run_native_core_conformance.py`)

- [ ] **Step 1:** Edit each file with the facts from Tasks 1–6: names, prefixes, version files, four CI artifacts, the native gold step and the installer change. Write nothing about PR 2 beyond "next".
- [ ] **Step 2:** Run `python scripts/check_docs.py`. Expected: `check_docs: knowledge base OK`.
- [ ] **Step 3: Commit**

```bash
git add docs knowledge_map
git commit -m "docs(processing): core lines, per-line versions and native gold (T1.1b PR 1)"
```

### Task 8: Rig verification and PR

- [ ] **Step 1:** On the rig PC (PowerShell): full build, `ctest --preset windows-ninja-test`, then `ctest --test-dir build-ninja -L "processing|release" --output-on-failure`. Expected: everything passes except the known TD-20 pair.
- [ ] **Step 2:** Push `feat/t1.1b-core-lines` and open a PR against `develop` titled "T1.1b PR 1: build, name, version and gold-check both processing-core lines". The body covers the decisions, what is not signed or published, the sign and publish edits for subtract-ring (Review Focus 1), and the CI artifact list.
- [ ] **Step 3:** Watch the PR checks. Native jobs: four `*-unsigned` artifacts, both export audits and the native gold step pass.
