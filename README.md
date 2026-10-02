# mib-studio-qt

## Documentation
See `docs/README.md` for structure and links. Active tasks live in `knowledge_map/task/`.
Agents start at [AGENTS.md](AGENTS.md), then the knowledge vault (`knowledge_map/`).

**Operators:** the user guide is published at
<https://kpt1020.github.io/mib-studio-qt/> — an intro tour plus workflow
guides (connect, acquire & record, review, troubleshooting) with
screenshots regenerated from the app itself on every release. The same
pages live in-repo at [docs/manual/README.md](docs/manual/README.md); the
site is built from them by
[.github/workflows/docs-site.yml](.github/workflows/docs-site.yml).


## Algorithm Experiments & MLflow

When running algorithm experiments (reanalysis, parameter sweeps, pipeline comparisons), all intermediate images and results must be uploaded to the MLflow tracking server at `mlflow.yofo.bio`. This includes intermediate pipeline images (original, blurred, diff, thresh, mask, overlay), metrics CSV, processing parameters, and summary metrics. Credentials come from `MLFLOW_TRACKING_USERNAME` / `MLFLOW_TRACKING_PASSWORD` environment variables — never hardcode them.

## Post-processing tools

Standalone tools for working with HDF5 files after recording (export, reanalyse) are built and distributed separately. See [docs/howto/tools.md](docs/howto/tools.md) for download location, quickstart, and compatibility.
## Building

```bash
scripts/doctor.sh        # Linux/macOS: what is missing, with fix commands   (Windows: .\scripts\doctor.ps1)
scripts/bootstrap.sh     # install it                                        (Windows: .\scripts\bootstrap.ps1)
cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build
```

Windows builds use Conan + VS 2022: `.\scripts\bootstrap.ps1` runs
`conan install` with `conan/profiles/windows-msvc194`, then
`cmake --preset windows-default` and `cmake --build build --config Release`
(fast loop: `-Generator Ninja` + `windows-ninja`). Presets, targets, and the
one-home-per-list layout under `env/` are documented in
[knowledge_map/build-and-run/Build.md](knowledge_map/build-and-run/Build.md).

### Building Windows Installer

To create a Windows installer for distribution, see [docs/howto/build-installer.md](docs/howto/build-installer.md).

Quick start:
1. Build Release configuration: `cmake --build build --config Release`
2. Build installer: `cmake --build build --target package_installer`
3. Build update package: `cmake --build build --target package_installer_update`
4. Find installer at: `resources/build/dist/MIB_Studio_Qt_Setup_v0.1.0.exe`

After building installers, see [Publishing Updates](#publishing-updates) below to publish them for distribution.

## Publishing Updates

After building installers, use `scripts/release/publish-update.py` to upload them to the dedicated Cloudflare R2 update bucket for distribution through `https://updates.yofo.bio`.

**Prerequisites:**
- Python
- Either Wrangler logged in with R2 access, or `boto3` plus S3-compatible R2 credentials
- For S3 uploads: configure `MIB_STUDIO_R2_ENDPOINT` and credentials via `MIB_STUDIO_R2_PROFILE`, AWS environment variables, or CI secrets

**Publish update package (for auto-updates):**
```bash
# Uses Wrangler automatically when MIB_STUDIO_R2_ENDPOINT is not set.
python scripts/release/publish-update.py --installer "resources/build/dist/MIB_Studio_Qt_Update_v0.2.0.exe"
```

**Publish full installer (optional, for manual downloads):**
```bash
python scripts/release/publish-update.py --installer "resources/build/dist/MIB_Studio_Qt_Setup_v0.2.0.exe"
```

The script auto-detects version from filename, computes SHA-256, generates a manifest, uploads files to R2, and prints the final public URLs. Windows PowerShell wrappers remain available as `scripts/release/publish-update.ps1`, `scripts/release/publish-tools.ps1`, and `scripts/release/verify-update-manifest.ps1`.

For complete release workflow, see [docs/howto/release-workflow.md](docs/howto/release-workflow.md).  
For detailed publishing information, see [docs/howto/auto-update-r2.md](docs/howto/auto-update-r2.md).

## Publishing Profile Catalogs

Profile catalogs for manual profile updates are hosted in the same public
Cloudflare R2 bucket under `profiles/<channel>/`.

```bash
# Dry-run first; uses Wrangler unless MIB_STUDIO_R2_ENDPOINT is set.
python scripts/release/publish-profiles.py --profiles-root "./profile-catalog/stable" --dry-run
python scripts/release/publish-profiles.py --profiles-root "./profile-catalog/stable"
```

The production catalog URL is
`https://updates.yofo.bio/profiles/stable/catalog.json`. See
[docs/howto/auto-update-r2.md](docs/howto/auto-update-r2.md#profile-catalog-hosting)
for Cloudflare setup, cache policy, schema, and credential requirements.

## Version Management

The project uses semantic versioning (X.Y.Z). Version can be managed in two ways:

> **For complete release workflow** (version bump → build → publish), see [docs/howto/release-workflow.md](docs/howto/release-workflow.md).

### Manual Version Bumping

Use the `scripts/release/bump-version.ps1` script to increment the version:

```powershell
# Bump patch version (0.1.0 → 0.1.1)
.\scripts\release\bump-version.ps1 --patch

# Bump minor version (0.1.0 → 0.2.0)
.\scripts\release\bump-version.ps1 --minor

# Bump major version (0.1.0 → 1.0.0)
.\scripts\release\bump-version.ps1 --major

# Bump and create git tag automatically
.\scripts\release\bump-version.ps1 --patch --tag
```

The script updates `cmake/MIBVersion.cmake` and optionally creates a git tag (e.g., `v0.1.1`). If you create a tag, remember to push it:
```bash
git push origin v0.1.1
```

### Automatic Version from Git Tags

CMake automatically detects version from git tags during configuration. If a git tag matching the pattern `vX.Y.Z` or `X.Y.Z` exists and is newer than the hardcoded version in `cmake/MIBVersion.cmake`, it will be used. This allows versioning based on git tags while maintaining a fallback default version.

The version is used throughout the build:
- Application version (shown in About dialog and used by auto-updater)
- Installer filename and metadata
- All build artifacts
