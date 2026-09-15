# Portable Processing Sync

How a **non-Qt consumer** (e.g. Biowork's `services/mib-processing`) pulls
the same processing config, Young's-modulus LUT, and processing engine the
desktop app uses, so results stay in sync. This is the "publish" side of the
[Biowork portability epic](https://github.com/KPT1020/mib-studio-qt/issues/220);
`bindings/python/` (issue #223) is the engine artifact these manifests point
a consumer at, and `docs/gold_standard_metrics.md` ("Portable Processing
Contract") is the shape the engine's output must match.

All endpoints below are plain HTTPS `GET` on public JSON — no auth, no Qt,
no app dependency. `verify-emodulus-lut-manifest.py` and
`verify-processing-core-manifest.py` demonstrate this with nothing but
Python's stdlib `urllib`.

## One channel, versioned processing-core registry

Everything is published per **channel** (`stable` by default) to
`https://updates.yofo.bio/`, via `publish-profiles.py`,
`publish-emodulus-lut.py`, and `publish-processing-core.py`
(`scripts/s3_upload.py` does the actual R2 upload; all three scripts support
`--dry-run` to generate the manifest locally without uploading).

| Manifest | URL | Published by | Consumed for |
|---|---|---|---|
| Profile catalog | `{base}/profiles/{channel}/catalog.json` | `publish-profiles.py` | `ProcessingConfig` values (camera + processing profiles) |
| Emodulus LUT | `{base}/{channel}/emodulus-lut/latest.json` | `publish-emodulus-lut.py` | Young's-modulus lookup table |
| Processing core active pointer | `{base}/{channel}/processing-core/latest.json` | `publish-processing-core.py` | Full manifest for the channel-active version; legacy-compatible entry point |
| Processing core immutable version | `{base}/{channel}/processing-core/versions/<version>.json` | `publish-processing-core.py` | Exact, long-cache wheel/native-core pin |
| Processing core catalog | `{base}/{channel}/processing-core/index.json` | `publish-processing-core.py` | Enumerable version history and `active_version` for selectors |
| `mib-processing` package page | `{base}/{channel}/processing-core/simple/mib-processing/index.html` | `publish-processing-core.py` | PEP 503 links with `#sha256=` fragments for baked dependency pins |
| Pip project route | `{base}/{channel}/processing-core/simple/mib-processing/` | `publish-processing-core.py` | Same package HTML at the exact trailing-slash object key requested by pip |

For pip/uv configuration, the **index base URL** is the parent directory:
`https://updates.yofo.bio/{channel}/processing-core/simple/` (not the package
page itself). For example, uv uses that value as an index URL and then requests
`mib-processing/` according to PEP 503 normalization. R2 does not synthesize
`index.html` for directory requests, so the publisher deliberately uploads the
same bytes under both the human-readable `index.html` key and that route key.

### Profile catalog (`catalog.json`)

```jsonc
{
  "catalog_schema_version": 1,
  "channel": "stable",
  "published_at": "2026-07-13T00:00:00Z",
  "profiles": [
    {
      "profile_id": "default",
      "display_name": "Default",
      "revision": "2026-07-13T00:00:00Z",
      "profile_meta_url": "...profile.meta.json",
      "config_url": "...config.json",           // ProcessingConfig JSON (see below)
      "camera_script_url": "...egrabberConfig.js", // optional; camera-hardware only, not relevant to a headless consumer
      "config_sha256": "<64-hex>",
      "camera_script_sha256": "<64-hex or null>",
      "app_min_version": "0.8.0",
      "app_max_version": null,
      "processing_contract_version": 1
    }
  ]
}
```

`config_url` resolves to a JSON document with `config_schema_version: 1` and
the same `image_processing` (nested) structure as
`resources/defaults/config.json`. **Flatten it** before calling
`mib_processing.process_batch`/`compute_processed_frame` — see
`docs/gold_standard_metrics.md` ("ProcessingConfig contract") for the
field-name mapping and the note that the Python bindings' dict shape is
flat, not nested.

`processing_contract_version` is optional. It declares which processing
contract the profile was validated against; it never selects or activates a
core. MIB Studio marks a present, mismatched value incompatible and requires
the operator to choose a compatible core separately.

### Emodulus LUT manifest (`latest.json`)

```jsonc
{
  "manifest_schema_version": 1,
  "lut_id": "scaled_isoelastic_data_LUT_6.16-4.24",
  "display_name": "Scaled Isoelastic Data Lut 6.16-4.24",
  "revision": "2026.07.13-1",
  "download_url": "...scaled_isoelastic_data_LUT_6.16-4.24.txt",
  "sha256": "<64-hex>",
  "size_bytes": 12345,
  "published_at": "2026-07-13T00:00:00Z",
  "app_min_version": "0.8.0",
  "app_max_version": null
}
```

`download_url` resolves to the tab-separated `area_um  deform  emodulus`
text file `EModulusLut::loadFromFile` / `mib_processing.EModulusLut.load_from_file`
both consume directly.

### Processing core registry (schema v2)

The manifest a consumer resolves **first**: it names the pinned combination
of engine version and contract version, and cross-links the other two
manifests so one GET gives a consumer everything it needs to reproduce the
desktop app's results.

`latest.json` and `versions/<version>.json` have the same complete document.
This is deliberate: existing consumers can keep resolving `latest.json`, while
new consumers resolve a catalog entry's immutable `manifest_url`. Schema v2 is
additive to the original A4 shape: the existing wheel, contract, profile, and
LUT fields remain present.

```jsonc
{
  "processing_core_manifest_schema_version": 2,
  "channel": "stable",
  "version": "0.1.0", // canonical; must equal wheel.version and the release tag version
  "published_at": "2026-07-13T00:00:00Z",
  "contract_version": 1,
  "wheel": {
    "package": "mib-processing",
    "version": "0.1.0",
    "release_tag": "mib-processing-v0.1.0",
    "release_url": "https://github.com/KPT1020/mib-studio-qt/releases/tag/mib-processing-v0.1.0",
    "wheels": [
      {
        "filename": "mib_processing-0.1.0-cp311-cp311-manylinux_2_28_x86_64.whl",
        "platform_tag": "cp311-cp311-manylinux_2_28_x86_64",
        "url": "https://github.com/KPT1020/mib-studio-qt/releases/download/mib-processing-v0.1.0/mib_processing-0.1.0-cp311-cp311-manylinux_2_28_x86_64.whl",
        "sha256": "<64-hex>",
        "size_bytes": 12345678
      }
    ]
  },
  "native_plugins": [
    {
      "filename": "mib_processing_core-0.1.0-windows_x86_64.dll",
      "os": "windows",
      "arch": "x86_64",
      "artifact_kind": "shared_library",
      "version": "0.1.0",
      "contract_version": 1,
      "engine_abi_version": 1,
      "runtime_fingerprint": "windows-x86_64-msvc1942-md-cxx17",
      "app_min_version": "0.8.0",
      "app_max_version": null,
      "entrypoint": "mib_processing_get_api",
      "url": "https://github.com/KPT1020/mib-studio-qt/releases/download/mib-processing-v0.1.0/mib_processing_core-0.1.0-windows_x86_64.dll",
      "sha256": "<64-hex>",
      "size_bytes": 1234567,
      "descriptor_url": "https://github.com/KPT1020/mib-studio-qt/releases/download/mib-processing-v0.1.0/mib_processing_core-0.1.0-windows_x86_64.json",
      "signing": { "scheme": "authenticode", "required": true }
    }
  ],
  "profile_catalog_url": "https://updates.yofo.bio/profiles/stable/catalog.json",
  "emodulus_lut_manifest_url": "https://updates.yofo.bio/stable/emodulus-lut/latest.json"
}
```

`wheel.wheels[]` and `native_plugins[]` are derived from the bytes attached to
the GitHub Release. The publisher calculates URL, size, and SHA-256 itself;
native sidecars cannot override those fields. Native `signing` is descriptive,
not a trust root—the desktop applies the named platform trust adapter. Windows
uses Authenticode plus the approved signer public-key (DER SPKI) SHA-256
compiled into the application. Linux uses the `ed25519` scheme: the signing
block additionally carries `public_key_spki_base64` (44-byte DER SPKI),
`public_key_spki_sha256` (re-derived by the publisher from the key bytes),
and `signature_base64` (raw 64-byte detached signature over the artifact
bytes); the desktop verifies the signature and requires the key hash to
match its compiled pin (see
[processing-core-linux-signing](architecture/processing-core-linux-signing.md)).
macOS and unknown schemes are rejected until their production verifier is
implemented.

The registry contract is not DLL-specific. Native filenames must match their
declared OS: `.dll` for `windows`, `.so` for `linux`, and `.dylib` for `macos`.
Every native entry carries `signing.scheme` and `signing.required=true`; an
unknown, absent, or optional signature policy is never an unsigned fallback.
The current release workflow publishes Windows x86_64 only; CI now also
builds, audits, and signature-rehearses the Linux x86_64 `.so`, and the live
signed Linux publication plus distro validation remain tracked in A13/#245.

Python wheels are architecture-independent from that native-plugin rollout.
Each `mib-processing-v<version>` release requires the complete CPython
3.10–3.13 × `manylinux_2_28_{x86_64,aarch64}` wheel matrix (8 wheels).
Every wheel is installed and imported in its matching cibuildwheel container;
ARM64 uses QEMU on the x86_64 Actions runner. Release creation and immutable
release reuse both fail if any Python/architecture pair is absent or an
unexpected wheel is present.

A Python consumer selects the first wheel compatible with its ordered PEP 425
tags, downloads and SHA-256-verifies it, then fetches `profile_catalog_url` /
`emodulus_lut_manifest_url` for the config and LUT pinned to the same
`contract_version`.

### Desktop native selection

MIB Studio's **Settings → Processing Core…** selector reads `index.json`,
filters `native_plugins[]` for the running OS/architecture and declared app
range, then fetches the selected immutable version manifest. It refuses the
candidate unless the mutable and immutable metadata agree. The raw immutable
manifest SHA-256 is retained alongside the artifact SHA-256 in experiment
provenance.

Downloaded plugins live in a persistent content-addressed cache at
`<cache>/<version>/<sha256>/<filename>`. Preparation uses a directory lock,
same-directory staging, atomic rename, and a `.ready.json` marker; abandoned
locks older than ten minutes are recoverable. At the final load boundary the
desktop re-hashes the cached shared library, applies the artifact's required
platform trust policy, checks version/contract/ABI/runtime identity, and runs
the plugin self-test. Windows uses Authenticode, a compiled approved signer-
SPKI SHA-256, and restricted DLL search. Linux uses the detached Ed25519
verifier with its own compiled signer-SPKI pin and
`dlopen(..., RTLD_NOW | RTLD_LOCAL)`; a build without a compiled pin fails
closed, so production Linux activation stays unavailable until A13's live
signed release exists. Native modules remain resident for the rest of the
process.

Activation is allowed only between operations. The selected v1 core owns mask
generation and empty-frame classification across live, offline, playback,
recording, and buffer-save paths; host code still owns contour metrics,
tracking, target decisions, and orchestration. That remaining boundary is
tracked in A7/#242 and must not be described as a fully replaceable pipeline.

The catalog is short-cache mutable metadata. `latest.json` is the canonical
channel-active pointer because it is written last; `index.active_version` is a
selector hint that mirrors it after a complete publication. A client must
resolve `latest.json` before labeling an entry Active, so a failed publish
between the index and final pointer cannot activate a partial release:

```jsonc
{
  "processing_core_index_schema_version": 1,
  "channel": "stable",
  "active_version": "0.1.0",
  "updated_at": "2026-07-13T00:00:00Z",
  "versions": [
    {
      "version": "0.1.0",
      "contract_version": 1,
      "published_at": "2026-07-13T00:00:00Z",
      "release_tag": "mib-processing-v0.1.0",
      "release_url": "https://github.com/KPT1020/mib-studio-qt/releases/tag/mib-processing-v0.1.0",
      "manifest_url": "https://updates.yofo.bio/stable/processing-core/versions/0.1.0.json",
      "wheels": ["same wheel entries as the full manifest"],
      "native_plugins": ["same native entries as the full manifest"]
    }
  ]
}
```

Catalog order is newest-first, but `active_version` is independent of that
order. Promotion of an older immutable manifest therefore performs a rollback
without deleting newer history.

**Pin the whole set together.** `contract_version` only changes when the
metrics schema, `ProcessingConfig` schema, or LUT text format changes
incompatibly (see `docs/gold_standard_metrics.md`). A consumer should record
`contract_version` + `wheel.version` + the config/LUT `revision` values it
resolved on every processing run (e.g. as MLflow run params in Biowork's
`services/mib-processing`), so any result is traceable back to the exact
pinned combination that produced it -- not just "some build of the
pipeline."

## Publishing and promotion

The `mib-processing-v<version>` workflow runs wheel/native conformance, attaches
the assets to one GitHub Release, and invokes:

```bash
python publish-processing-core.py \
  --from-release "mib-processing-v0.1.0" \
  --channel stable \
  --upload-method s3
```

`--from-release` uses `gh release view/download`; manual `--wheel` inputs remain
available for local previews. A fixture-backed dry run exercises the exact
release classification without GitHub or R2:

```bash
python publish-processing-core.py \
  --from-release mib-processing-v0.1.0 \
  --release-assets-dir ./dist \
  --published-at 2026-07-13T00:00:00Z \
  --dry-run \
  --manifest-out ./out/latest.json \
  --version-manifest-out ./out/versions/0.1.0.json \
  --index-out ./out/index.json \
  --pep503-out ./out/simple/mib-processing/index.html
```

Publication reads the existing immutable manifest and catalog before writing.
It rejects unreadable state and conflicting bytes, treats an identical version
as idempotent, uploads the immutable document first, then the catalog/package
page, and writes `latest.json` last. Consequently a partial failure never
updates the legacy active pointer before the immutable/catalog/package writes
have succeeded. A failed retry is safe because immutable equality and catalog
deduplication are checked again. Versioned manifests use a
one-year immutable cache; `latest.json`, `index.json`, and the package page use
a one-minute revalidating cache.

Real publication requires the R2 S3 endpoint. Its authenticated preflight
reads are strongly consistent and bypass the public CDN; using a cached public
catalog as the merge base could silently drop a just-published history entry.
Wrangler/public reads remain suitable for non-mutating `--dry-run` previews.

Promote or roll back without rebuilding the old manifest:

```bash
python publish-processing-core.py \
  --promote-version 0.1.0 \
  --channel stable \
  --published-at 2026-07-13T01:02:03Z \
  --upload-method s3
```

This command reads and validates the existing version and catalog, copies the
immutable JSON bytes exactly, updates mutable catalog/package metadata, and
writes those original bytes to `latest.json` last. `--published-at` is also
required for non-release/manual real publication so a retry cannot invent
different immutable bytes. Real `--from-release` publication uses the GitHub
Release's stable `publishedAt` timestamp. Timestamps must include a timezone;
registry JSON is emitted as UTF-8 with LF newlines on every platform so the
same release serializes byte-for-byte identically on Windows and Linux.

Production operators normally run **Actions → Promote or roll back processing
core**, choose `stable` or `beta`, and enter an already-published version. The
Production-environment workflow serializes changes per channel, uses the R2 S3
API for consistent reads, writes `latest.json` last, and verifies the public
pointer plus all cross-links before succeeding.

Use `scripts/bump_mib_processing_version.py <version>` to update the
authoritative pyproject version and the import-time wrapper literal together.
Commit those files, then rerun with `--create-tag`; the command refuses to tag
an uncommitted bump so the tag cannot point at the old version.

## Verifying a channel is reachable (no Qt, no app)

```bash
python3 verify-emodulus-lut-manifest.py --manifest-url https://updates.yofo.bio/stable/emodulus-lut/latest.json
python3 verify-processing-core-manifest.py --manifest-url https://updates.yofo.bio/stable/processing-core/latest.json
```

Both use nothing but `urllib`/`json`/`hashlib` from the standard library --
demonstrating that a consumer needs no MIB Studio dependency, Qt or
otherwise, to resolve and validate these manifests.
