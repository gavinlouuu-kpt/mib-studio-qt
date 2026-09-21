## Auto-update via Cloudflare R2

MIB Studio checks a public update manifest, downloads the latest Windows update package, verifies SHA-256, and launches the installer elevated. Production update artifacts are hosted in a dedicated Cloudflare R2 bucket and exposed through the public custom hostname `https://updates.yofo.bio`.

### Overview

- **Public update hostname**: `https://updates.yofo.bio`
- **R2 bucket**: `mib-studio-qt-updates`
- **R2 S3 API endpoint**: set locally with `MIB_STUDIO_R2_ENDPOINT` (`https://<account-id>.r2.cloudflarestorage.com`)
- **Channel prefixes**: `stable/` for production, `beta/` for beta/pre-release builds
- **Production manifest**: `https://updates.yofo.bio/stable/latest.json`
- **Override for testing**: set `MIB_STUDIO_UPDATE_MANIFEST_URL` to any HTTPS URL returning a compatible manifest JSON

Do not commit R2 credentials or account IDs that are intended to remain private. Use a logged-in Wrangler session, local environment variables, an AWS CLI profile, CI secrets, or Cloudflare-managed credentials.

### Object Layout

Keep objects at the root of the public custom domain:

- `stable/latest.json`
- `stable/index.json` — full version history for the channel (see below)
- `stable/MIB_Studio_Qt_Update_v<full-version>.exe`
- `stable/MIB_Studio_Qt_Setup_v<full-version>.exe` (optional, full installer for manual downloads)
- `stable/tools/tools-latest.json`
- `stable/tools/MIB_Studio_Tools_v<version>_windows.zip`
- `profiles/stable/catalog.json`
- `profiles/stable/<profile-id>/profile.meta.json`
- `profiles/stable/<profile-id>/config.json`
- `profiles/stable/<profile-id>/egrabberConfig.js` (optional)
- `profiles/stable/<profile-id>/CHANGELOG.md` (optional)
- `beta/...` for beta/pre-release equivalents

#### `index.json` (version catalog)

`{channel}/index.json` lists every published version so the in-app **Help ▸
Software Updates…** dialog can offer channel + specific-version selection
(including rollback). `latest.json` is unchanged and still drives the silent
auto-check; `index.json` is additive.

```json
{
  "schema_version": 1,
  "channel": "beta",
  "versions": [
    {
      "version": "1.0.4-beta.1",
      "installer_url": "https://updates.yofo.bio/beta/MIB_Studio_Qt_Update_v1.0.4-beta.1.exe",
      "installer_sha256": "…",
      "installer_size_bytes": 12345678,
      "release_notes_url": "https://github.com/KPT1020/mib-studio-qt/releases/tag/v1.0.4-beta.1",
      "published_utc": "2026-06-24T04:54:58Z"
    }
  ]
}
```

Newest-first; a release sorts above its own betas. Equal numeric SHA betas use
`published_utc` newest-first, then their version string as a deterministic
tie-break. `scripts/release/publish-update.py` maintains it automatically on every publish (see
*Publishing a New App Version*). The app
parses it with `UpdateCatalog`; entries missing `version`/`installer_url`/
`installer_sha256` are skipped, and a missing/invalid `index.json` degrades to
"use Check for Latest" without affecting the auto-check.

### Profile Catalogs

Public profile catalogs live under `profiles/<channel>/` and are read
without authentication:

- `profiles/stable/catalog.json`
- `profiles/stable/<profile-id>/profile.meta.json`
- `profiles/stable/<profile-id>/config.json`
- `profiles/stable/<profile-id>/egrabberConfig.js`

The app checks these only when the user requests it from `ConfigTabs`.
Catalogs and mutable per-profile metadata should use short cache lifetimes;
versioned immutable objects can be cached longer if they are added later.

### Young's Modulus LUT

The Young's modulus LUT is managed separately from app installers and follows
the same operational pattern: publish a public manifest, download/update into
a user-writable cache, verify SHA-256, and fall back to the bundled copy when
offline or incompatible.

Public object layout:

- `stable/emodulus-lut/latest.json`
- `stable/emodulus-lut/scaled_isoelastic_data_LUT_6.16-4.24.txt`
- `beta/emodulus-lut/...` for beta/testing LUTs if you need a separate track

Manifest format:

- `manifest_schema_version` (number): currently `1`
- `lut_id` (string): stable LUT identifier
- `display_name` (string): human-friendly label
- `revision` (string): published LUT revision
- `download_url` (string): HTTPS or `file://` URL to the LUT payload
- `sha256` (string): lowercase or uppercase hex SHA-256 of the LUT file
- `size_bytes` (number): file size in bytes
- `published_at` (optional string): ISO8601 timestamp
- `app_min_version` / `app_max_version` (optional strings): compatibility bounds

Example:

```json
{
  "manifest_schema_version": 1,
  "lut_id": "scaled_isoelastic_data_LUT_6.16-4.24",
  "display_name": "Scaled Isoelastic LUT",
  "revision": "2026.06.11-1",
  "download_url": "https://updates.yofo.bio/stable/emodulus-lut/scaled_isoelastic_data_LUT_6.16-4.24.txt",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size_bytes": 1234567,
  "published_at": "2026-06-11T12:34:56Z"
}
```

Publishing:

```bash
python scripts/release/publish-emodulus-lut.py \
  --lut "resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt" \
  --revision "2026.06.11-1"
```

Verification:

```bash
python scripts/release/verify-emodulus-lut-manifest.py
```

Runtime behavior:

- New builds check `MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL` first; otherwise they
  use `https://updates.yofo.bio/stable/emodulus-lut/latest.json`.
- Downloaded LUTs are cached under the user-local app data tree, in
  `isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt` by default.
- `MIB_STUDIO_EMODULUS_LUT_CACHE_DIR` can redirect the cache for testing.
- If the manifest fetch fails, the app keeps the last known-good local copy or
  falls back to the bundled LUT on first run/offline launches.

Rollback:

1. Publish a corrected `stable/emodulus-lut/latest.json` that points to the
   last known-good LUT.
2. Verify with `python scripts/release/verify-emodulus-lut-manifest.py`.
3. If the cache is corrupted locally, delete the LUT cache directory to force a
   fresh seed from the bundled copy or the next successful remote manifest.

Example public URLs:

```text
https://updates.yofo.bio/profiles/stable/catalog.json
https://updates.yofo.bio/profiles/stable/lab-default/profile.meta.json
https://updates.yofo.bio/profiles/stable/lab-default/config.json
https://updates.yofo.bio/profiles/stable/lab-default/egrabberConfig.js
```

The public URL is `https://updates.yofo.bio/<object-key>`. Do not include the bucket name in public manifest URLs when using the R2 custom domain.

### Processing Core Manifest

Pins the current `mib-processing` Python wheel version (`bindings/python/`,
issue #223) and `contract_version` together, and cross-links the profile
catalog and Young's-modulus LUT manifest above — the single manifest a
non-Qt consumer (e.g. Biowork's `services/mib-processing`) resolves first.
Full schema, field-by-field: [`docs/portable-processing-sync.md`](../portable-processing-sync.md).

Public object layout:

- `stable/processing-core/latest.json`
- `stable/processing-core/versions/<version>.json`
- `stable/processing-core/index.json`
- `stable/processing-core/simple/mib-processing/index.html`
- `stable/processing-core/simple/mib-processing/` (the exact pip request route)
- `beta/processing-core/...` for a separate track

Configure pip/uv with the directory
`https://updates.yofo.bio/<channel>/processing-core/simple/` as the index base;
clients append the normalized `mib-processing/` package path themselves. The
publisher stores the package HTML at both `index.html` and the trailing-slash
route because an R2 custom domain does not generate directory indexes.

Normal publication is automatic. Pushing `mib-processing-v<version>` runs
wheel/native conformance, creates the GitHub Release, then derives and hashes
the release assets before updating R2:

```bash
python scripts/release/publish-processing-core.py \
  --from-release mib-processing-v0.1.0 \
  --channel stable \
  --upload-method s3
```

The release job requires `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, and
`MIB_STUDIO_R2_ENDPOINT`. A release tag also requires
`WINDOWS_SIGNING_CERTIFICATE_BASE64` and
`WINDOWS_SIGNING_CERTIFICATE_PASSWORD`, plus the public repository variable
`MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256`. After signing, CI hashes the actual
signer certificate's DER `SubjectPublicKeyInfo` and requires it to equal that
64-hex desktop trust pin before uploading the DLL. Missing or mismatched
production signing, trust identity, or R2 credentials fail the release job
instead of silently publishing an unusable registry.

To preview already-downloaded release assets without GitHub or R2:

```bash
python scripts/release/publish-processing-core.py \
  --from-release mib-processing-v0.1.0 \
  --release-assets-dir ./dist \
  --published-at 2026-07-13T00:00:00Z \
  --dry-run \
  --manifest-out ./tmp/processing-core/latest.json \
  --version-manifest-out ./tmp/processing-core/versions/0.1.0.json \
  --index-out ./tmp/processing-core/index.json \
  --pep503-out ./tmp/processing-core/simple/mib-processing/index.html
```

The publisher enforces this order: read/check immutable history and the
catalog; upload a new immutable manifest (or accept identical content);
conditionally merge `index.json`; regenerate the PEP 503 page; upload
`latest.json` last. It refuses to overwrite a version with different content
or to replace a catalog it could not read. Mutable documents use a short cache;
version manifests use `max-age=31536000, immutable`.

Mutating processing-core publication requires `--upload-method s3` plus
`MIB_STUDIO_R2_ENDPOINT`. The publisher must read immutable/catalog state
directly from R2 before merging; a public-CDN or Wrangler fallback can be
stale and is therefore limited to non-mutating dry runs.

`latest.json` is the canonical channel-active pointer and is written last.
`index.active_version` mirrors it after a complete publication, but selectors
must consult `latest.json` before labeling a catalog entry Active.

Promote or roll back by copying an existing immutable manifest byte-for-byte:

```bash
python scripts/release/publish-processing-core.py \
  --promote-version 0.1.0 \
  --channel stable \
  --published-at 2026-07-13T01:02:03Z \
  --upload-method s3
```

History remains newest-first while the mutable pointer moves to the chosen
version. Never edit an object under `versions/` in place or reconstruct an old
manifest with a newer publisher. Manual real publication and promotion require
`--published-at`; release-driven publication derives the stable timestamp from
GitHub.

The supported production path is the manual
`.github/workflows/processing-core-promote.yml` action. Its `Production`
environment and per-channel concurrency prevent a release publish and rollback
from racing; the job verifies `latest.json`, wheels/native assets, the profile
catalog, and the LUT through the public hostname after writing.

Version bump/tag sequence:

```bash
python scripts/bump_mib_processing_version.py 0.2.0
git add bindings/python/pyproject.toml bindings/python/python/mib_processing/__init__.py
git commit -m "chore(processing): bump mib-processing to 0.2.0"
python scripts/bump_mib_processing_version.py 0.2.0 --create-tag
git push origin HEAD --follow-tags
```

The tag step verifies both version files are clean and already present at
`HEAD`; it cannot tag the pre-bump commit accidentally.

Verification:

```bash
python scripts/release/verify-processing-core-manifest.py
```

### Manifest Format

`stable/latest.json` must be JSON with these fields:

- `version` (string): semantic version like `"0.2.0"`
- `installer_url` (string): HTTPS URL to the update package
- `installer_sha256` (string): lowercase or uppercase hex SHA-256 of the installer exe
- `installer_size_bytes` (number): file size in bytes
- `release_notes_url` (optional string): HTTPS URL, usually the GitHub release
- `published_at` (optional string): ISO8601 timestamp

Example:

```json
{
  "version": "0.2.0",
  "installer_url": "https://updates.yofo.bio/stable/MIB_Studio_Qt_Update_v0.2.0.exe",
  "installer_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "installer_size_bytes": 123456789,
  "release_notes_url": "https://github.com/gavinlouuu-kpt/mib-studio-qt/releases/tag/v0.2.0",
  "published_at": "2026-01-21T12:34:56Z"
}
```

The `installer_url` should point to
`MIB_Studio_Qt_Update_v<full-version>.exe` for auto-updates. Inno Setup emits a
numeric local/GitHub filename, but `publish-update.py --version
X.Y.Z-beta.<identifier>` stores beta bytes under the full-version R2 key so a
later beta never overwrites a one-year-cached object. Publish the full setup
installer separately only when manual first-time downloads need it.

### Cloudflare Configuration

Configure these outside the repo:

1. Create a dedicated R2 bucket named `mib-studio-qt-updates`.
2. Attach the public custom domain `updates.yofo.bio` to that bucket.
3. Configure public read access through Cloudflare for update artifacts.
4. Configure cache behavior:
   - `stable/latest.json`, `beta/latest.json`, and `*/tools/tools-latest.json`: short TTL or bypass cache because manifests are mutable.
   - `profiles/*/catalog.json`: short TTL or bypass cache because profile catalogs are mutable.
   - `profiles/*/<profile-id>/{profile.meta.json,config.json,egrabberConfig.js,CHANGELOG.md}`: moderate TTL because current profile revisions are mutable.
   - `*/processing-core/{latest.json,index.json,simple/**}`: short TTL because active selection and package history are mutable.
   - `*/processing-core/versions/*.json`: one-year immutable caching; these keys are never overwritten with different content.
   - Versioned `.exe` and `.zip` artifacts: long TTL because filenames are immutable.
5. Create least-privilege write credentials for release publishing. Credentials need object write access to the updater bucket only.
6. Store credentials in a local AWS profile such as `mib-studio-r2`, environment variables, or CI secrets.

Recommended S3-compatible local environment:

```bash
export MIB_STUDIO_R2_ENDPOINT="https://<account-id>.r2.cloudflarestorage.com"
export MIB_STUDIO_R2_PROFILE="mib-studio-r2"
```

If `MIB_STUDIO_R2_ENDPOINT` is not set, the Python publish scripts use `wrangler r2 object put --remote` with the currently authenticated Wrangler session.

### Migration From RustFS

Preserve the existing channel layout when copying objects from the old RustFS bucket:

```bash
export MIB_STUDIO_R2_ENDPOINT="https://<account-id>.r2.cloudflarestorage.com"

aws --endpoint-url https://s3.yofo.bio --profile rustfs s3 sync s3://mib-studio-qt-updates/stable ./tmp-updates/stable
aws --endpoint-url "$MIB_STUDIO_R2_ENDPOINT" --profile mib-studio-r2 s3 sync ./tmp-updates/stable s3://mib-studio-qt-updates/stable

aws --endpoint-url https://s3.yofo.bio --profile rustfs s3 sync s3://mib-studio-qt-updates/beta ./tmp-updates/beta
aws --endpoint-url "$MIB_STUDIO_R2_ENDPOINT" --profile mib-studio-r2 s3 sync ./tmp-updates/beta s3://mib-studio-qt-updates/beta
```

After copying, update migrated manifests if they still point at `https://s3.yofo.bio/mib-studio-qt-updates/...`. The app expects the manifest's `installer_url` to be publicly downloadable without credentials.

### Publishing a New App Version

Build both installers:

```bash
cmake --build build --target package_installer --config Release
cmake --build build --target package_installer_update --config Release
```

Publish the update package:

```bash
python scripts/release/publish-update.py \
  --installer "build/dist/MIB_Studio_Qt_Update_v0.2.0.exe" \
  --version "0.2.0" \
  --release-notes-url "https://github.com/gavinlouuu-kpt/mib-studio-qt/releases/tag/v0.2.0"
```

Publish the optional full installer:

```bash
python scripts/release/publish-update.py --installer "build/dist/MIB_Studio_Qt_Setup_v0.2.0.exe"
```

`scripts/release/publish-update.py` uploads to `s3://mib-studio-qt-updates/<channel>/...`, generates `<channel>/latest.json`, **updates `<channel>/index.json`** (reads the current index, inserts the new version via `merge_index` — dedupe by version, newest-first — and re-uploads), and prints final public URLs under `https://updates.yofo.bio`. An explicit beta `--version` must share the numeric version in the local installer filename and becomes the immutable object filename. It uses S3/boto3 when `MIB_STUDIO_R2_ENDPOINT` is set, otherwise Wrangler. `scripts/release/publish-update.ps1` is a Windows compatibility wrapper around the Python command.

**Reading the existing index** uses the **S3 API** (same endpoint/credentials as the upload), not the public `updates.yofo.bio` URL — the public CDN can block/cache reads from CI runners (Cloudflare challenges the default `Python-urllib` user agent), which previously caused every release to *replace* the index with a single entry instead of growing it. If the existing index cannot be read (a genuine read error, distinct from "no index yet"), the publish **skips** the index update rather than clobber a good catalog. The `index.json` accumulates from each publish onward; older releases predating index maintenance need a one-time backfill (re-publish their update packages — `merge_index` dedupes — or build the index and upload it via the S3 API).

For legacy S3-compatible targets that require object ACLs, pass `--acl public-read`. R2 public access is configured at the bucket/custom-domain layer, so ACLs are not sent by default.

### Publishing Tools

```bash
python scripts/release/publish-tools.py --zip "tools/dist/MIB_Studio_Tools_v0.1.7_windows.zip"
```

The tools manifest is published to `https://updates.yofo.bio/stable/tools/tools-latest.json`.

### Profile Catalog Hosting

Profile catalogs use the same public R2 bucket and custom domain as app
updates, but live under a separate `profiles/<channel>/` namespace:

```text
profiles/
  stable/
    catalog.json
    lab-default/
      profile.meta.json
      config.json
      egrabberConfig.js
      CHANGELOG.md
  beta/
    catalog.json
```

The production catalog URL is:

```text
https://updates.yofo.bio/profiles/stable/catalog.json
```

Profile downloads are public HTTPS reads. Do not add authentication to the app
download path. Publishing credentials stay out-of-band through Wrangler,
`MIB_STUDIO_R2_ENDPOINT` plus AWS-compatible credentials, or CI secrets.

Each profile directory prepared for publishing must contain:

- `config.json` with top-level `config_schema_version: 1`
- optional `profile.meta.json` with display metadata
- optional `egrabberConfig.js`
- optional `CHANGELOG.md`

The publisher derives checksums, generates `profile.meta.json` for upload, and
writes `profiles/<channel>/catalog.json`:

```bash
python scripts/release/publish-profiles.py --profiles-root "./profile-catalog/stable"
```

Use dry-run first when preparing a new catalog:

```bash
python scripts/release/publish-profiles.py \
  --profiles-root "./profile-catalog/stable" \
  --catalog-out "build/profile-catalog/catalog.json" \
  --dry-run
```

If importing legacy config files during catalog setup, this option generates an
upload copy with `config_schema_version: 1` before checksums are calculated:

```bash
python scripts/release/publish-profiles.py \
  --profiles-root "./profile-catalog/stable" \
  --add-missing-config-schema \
  --dry-run
```

For a real publish, the script uses Wrangler automatically when
`MIB_STUDIO_R2_ENDPOINT` is not set. With S3-compatible R2 credentials, export
the same variables used by app update publishing:

```bash
export MIB_STUDIO_R2_ENDPOINT="https://<account-id>.r2.cloudflarestorage.com"
export MIB_STUDIO_R2_PROFILE="mib-studio-r2"
python scripts/release/publish-profiles.py --profiles-root "./profile-catalog/stable"
```

After publishing, verify public reads:

```bash
python -m json.tool <(curl -fsSL "https://updates.yofo.bio/profiles/stable/catalog.json")
curl -fsI "https://updates.yofo.bio/profiles/stable/lab-default/config.json"
```

Cloudflare setup required for KIN-47:

1. Keep `updates.yofo.bio` attached to the `mib-studio-qt-updates` R2 bucket.
2. Ensure public read access covers `profiles/*` object keys.
3. Add or confirm cache rules for mutable profile catalog/config paths.
4. Grant write credentials only to operators or CI jobs that publish profiles.
5. Do not expose account IDs, access keys, or secret keys in catalog JSON,
   profile metadata, app config, docs examples, or committed scripts.

### Verification

Run the public manifest verifier after publishing:

```bash
python scripts/release/verify-update-manifest.py
```

For beta channel smoke tests:

```bash
python scripts/release/verify-update-manifest.py --manifest-url "https://updates.yofo.bio/beta/latest.json"
```

Manual checks:

```powershell
Invoke-WebRequest -Uri "https://updates.yofo.bio/stable/latest.json" -Method Head
$manifest = Invoke-WebRequest -Uri "https://updates.yofo.bio/stable/latest.json" | ConvertFrom-Json
Invoke-WebRequest -Uri $manifest.installer_url -Method Head
```

Local app smoke test:

```powershell
$env:MIB_STUDIO_UPDATE_MANIFEST_URL = "https://updates.yofo.bio/stable/latest.json"
```

Then launch the app and use Help -> Check for Updates.

### Legacy Client Compatibility

`s3.yofo.bio` is **retired** — releases publish only to `https://updates.yofo.bio`
(the compiled default since PR #169). Do not reintroduce an `s3.yofo.bio`
manifest or redirect.

Clients built before PR #169 still request
`https://s3.yofo.bio/mib-studio-qt-updates/stable/latest.json` and will **not**
auto-update. Upgrade them manually by running the current full installer once;
afterwards they track `updates.yofo.bio` like every other client.

### Rollback

If a bad R2 release is published:

1. Generate and publish a corrected `stable/latest.json` that points to the last known-good update package.
2. Verify with `python scripts/release/verify-update-manifest.py`.
3. If R2 public access is unhealthy, set `MIB_STUDIO_UPDATE_MANIFEST_URL` for smoke tests or publish a temporary manifest on a known-good HTTPS endpoint.
4. Confirm clients pick up the corrected `updates.yofo.bio` manifest (no legacy `s3.yofo.bio` endpoint is involved).

### Troubleshooting

**Manifest returns 403 or 404**

- Confirm `updates.yofo.bio` is attached to the correct R2 bucket.
- Confirm public read access is enabled for the bucket/custom domain.
- Confirm the object key is `<channel>/latest.json` and does not include the bucket name.

**Manifest is stale after publishing**

- Check Cloudflare cache rules for mutable manifest paths.
- Purge cache for `https://updates.yofo.bio/<channel>/latest.json` if needed.

**Upload fails with credentials or endpoint errors**

- Confirm `MIB_STUDIO_R2_ENDPOINT` is set to the account-specific R2 S3 API endpoint.
- Confirm `MIB_STUDIO_R2_PROFILE` points to a profile with write access to `mib-studio-qt-updates`.
- Avoid committing access keys or endpoint-specific secrets to the repo.
