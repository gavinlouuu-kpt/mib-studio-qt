# Unified release script for MIB Studio Qt
# Builds locally, creates GitHub Release, and publishes to Cloudflare R2 — all from your machine.
#
# Usage:
#   .\release.ps1 --patch                    # Production: bump, build, tag (v0.2.2)
#   .\release.ps1 --patch --push             # Production: bump, build, tag, push, create GitHub Release, publish to stable
#   .\release.ps1 --patch --beta             # Beta: bump, build, tag as v0.2.2-beta.1
#   .\release.ps1 --patch --beta --push      # Beta: bump, build, tag, push, create GitHub pre-release, publish to beta
#   .\release.ps1 --patch --skip-build --push # Bump, tag, push (skip build + publish)
#   .\release.ps1 --patch --dry-run          # Preview what would happen

param(
    [switch]$Patch,
    [switch]$Minor,
    [switch]$Major,
    [switch]$Beta,
    [switch]$Push,
    [switch]$SkipBuild,
    [switch]$DryRun,
    [string]$Profile = $env:MIB_STUDIO_R2_PROFILE
)

$ErrorActionPreference = "Stop"

$resolvedScriptRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$resolvedWorkingDirectory = (Resolve-Path -LiteralPath (Get-Location).Path).Path
if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals(
        $resolvedScriptRoot.TrimEnd('\', '/'),
        $resolvedWorkingDirectory.TrimEnd('\', '/'))) {
    Write-Host "ERROR: Run release.ps1 from the repository root: $resolvedScriptRoot" -ForegroundColor Red
    exit 1
}

# --- Validate bump type ---
$bumpCount = 0
if ($Patch) { $bumpCount++ }
if ($Minor) { $bumpCount++ }
if ($Major) { $bumpCount++ }

if ($bumpCount -ne 1) {
    Write-Host "ERROR: Specify exactly one of --patch, --minor, or --major" -ForegroundColor Red
    Write-Host ""
    Write-Host "Usage: .\release.ps1 --patch|--minor|--major [--beta] [--push] [--skip-build] [--dry-run]" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Options:" -ForegroundColor Yellow
    Write-Host "  --patch       Bump patch version (bug fixes)"
    Write-Host "  --minor       Bump minor version (new features)"
    Write-Host "  --major       Bump major version (breaking changes)"
    Write-Host "  --beta        Create a beta/pre-release (publishes to beta channel)"
    Write-Host "  --push        Push tag, create GitHub Release, publish to Cloudflare R2"
    Write-Host "  --skip-build  Skip local build (tag and push only)"
    Write-Host "  --dry-run     Show what would happen without making changes"
    Write-Host "  --profile     AWS/R2 profile for publishing (default: MIB_STUDIO_R2_PROFILE env var)"
    exit 1
}

# --- Check prerequisites ---
Write-Host "=== MIB Studio Qt Release ===" -ForegroundColor Cyan

$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
try {
    & $python --version *> $null
    if ($LASTEXITCODE -ne 0) { throw "Python returned $LASTEXITCODE" }
} catch {
    Write-Host "ERROR: Python is required for release version resolution and publishing." -ForegroundColor Red
    exit 1
}

if (-not $SkipBuild) {
    # Check gh CLI is available
    try {
        $null = gh --version 2>&1
    } catch {
        Write-Host "ERROR: 'gh' CLI not found. Install from https://cli.github.com/" -ForegroundColor Red
        exit 1
    }
}

# Resolve the public trust pin from the same GitHub repository that will
# receive the release. Do this before bumping, committing, or tagging so a
# missing/malformed release configuration cannot leave version mutations
# behind. Even a no-push local run produces distributable installers, so every
# build is gated. --skip-build produces no binary; when it pushes a tag, the
# tag workflow owns the guarded build and performs this check itself.
$releaseRepository = $null
$processingCoreSignerSpki = $null
if (-not $SkipBuild) {
    $releaseRepository = (& gh repo view --json nameWithOwner --jq '.nameWithOwner' 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $releaseRepository) {
        Write-Host "ERROR: Could not resolve the destination GitHub repository." -ForegroundColor Red
        exit 1
    }
    $releaseRepository = $releaseRepository.Trim()

    $processingCoreSignerSpki = (& gh variable get MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256 --repo $releaseRepository 2>$null)
    if ($LASTEXITCODE -ne 0 -or
        $processingCoreSignerSpki -notmatch '^[0-9A-Fa-f]{64}$') {
        Write-Host "ERROR: Repository variable MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256 must be a non-empty 64-hex DER-SPKI SHA-256 in $releaseRepository." -ForegroundColor Red
        exit 1
    }
    $processingCoreSignerSpki = $processingCoreSignerSpki.Trim().ToLowerInvariant()
    Write-Host "Processing-core signer trust pin validated for $releaseRepository." -ForegroundColor Green
}

# --- Check git status ---
$currentBranch = (git rev-parse --abbrev-ref HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $currentBranch -or $currentBranch -eq 'HEAD') {
    Write-Host "ERROR: Release must run from a named branch" -ForegroundColor Red
    exit 1
}
if ($Push -and -not $Beta -and $currentBranch -ne 'main') {
    Write-Host "ERROR: A pushed stable release must run from main (current: $currentBranch)" -ForegroundColor Red
    exit 1
}

$gitStatus = git status --porcelain
if ($gitStatus) {
    Write-Host "ERROR: Release requires a clean working tree so built bytes match the tag:" -ForegroundColor Red
    Write-Host $gitStatus -ForegroundColor Gray
    exit 1
}

git fetch origin --tags
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Could not refresh release tags from origin" -ForegroundColor Red
    exit 1
}

# --- Step 1: Bump version ---
Write-Host "`n--- Step 1: Bump Version ---" -ForegroundColor Cyan

$bumpName = if ($Patch) { "patch" } elseif ($Minor) { "minor" } else { "major" }
$versionFile = "$PSScriptRoot\cmake\MIBVersion.cmake"
$cmakeContent = Get-Content $versionFile -Raw
$versionInfoJson = & $python "$PSScriptRoot\scripts\resolve_desktop_release_version.py" `
    --repo-root $PSScriptRoot --bump $bumpName
if ($LASTEXITCODE -ne 0 -or -not $versionInfoJson) {
    Write-Host "ERROR: Could not resolve the effective desktop release version" -ForegroundColor Red
    exit 1
}
$versionInfo = $versionInfoJson | ConvertFrom-Json
$fallbackVersion = [string]$versionInfo.default_version
$currentVersion = [string]$versionInfo.current_version
$newVersion = [string]$versionInfo.next_version

# Calculate the prospective version without mutating the tree so --dry-run
# exercises the same effective-version/tag decision as a real release.
# Determine the tag before writing so an existing immutable tag fails without
# leaving a version-file mutation behind.
if ($Beta) {
    # Find next beta number for this version
    $betaNum = 1
    $existingBetaTags = git tag -l "v$newVersion-beta.*" 2>$null
    if ($existingBetaTags) {
        $existingBetaTags -split "`n" | ForEach-Object {
            if ($_ -match "v$([regex]::Escape($newVersion))-beta\.(\d+)") {
                $num = [int]$matches[1]
                if ($num -ge $betaNum) { $betaNum = $num + 1 }
            }
        }
    }
    $tagName = "v$newVersion-beta.$betaNum"
    $channel = "beta"
} else {
    $tagName = "v$newVersion"
    $channel = "stable"
}

$existingTag = @(git tag -l $tagName)
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Could not inspect existing release tags" -ForegroundColor Red
    exit 1
}
if ($existingTag.Count -ne 0) {
    Write-Host "ERROR: Tag $tagName already exists; refusing to move an immutable release tag" -ForegroundColor Red
    exit 1
}

if ($DryRun) {
    Write-Host "[DRY RUN] Prospective version: $currentVersion -> $newVersion (fallback: $fallbackVersion)" -ForegroundColor Gray
    Write-Host "[DRY RUN] Would set DEFAULT_VERSION to $newVersion" -ForegroundColor Gray
} else {
    $updatedContent = $cmakeContent -replace `
        "set\(DEFAULT_VERSION\s+`"$([regex]::Escape($fallbackVersion))`"\)", `
        "set(DEFAULT_VERSION `"$newVersion`")"
    if ($updatedContent -eq $cmakeContent) {
        Write-Host "ERROR: Could not replace DEFAULT_VERSION $fallbackVersion with $newVersion" -ForegroundColor Red
        exit 1
    }
    Set-Content -Path $versionFile -Value $updatedContent -NoNewline
}

Write-Host "Version: $tagName (channel: $channel)" -ForegroundColor Green

# --- Step 2: Commit version bump ---
Write-Host "`n--- Step 2: Commit Version Bump ---" -ForegroundColor Cyan

if ($DryRun) {
    Write-Host "[DRY RUN] Would commit cmake\MIBVersion.cmake with version bump" -ForegroundColor Gray
} else {
    git add cmake/MIBVersion.cmake
    git commit -m "chore: bump version to $newVersion"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Could not commit version bump" -ForegroundColor Red
        exit 1
    }

    git rev-parse --verify --quiet "refs/tags/$tagName" *> $null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "ERROR: Tag $tagName already exists; refusing to move an immutable release tag" -ForegroundColor Red
        exit 1
    }
    git tag -a "$tagName" -m "Version $tagName"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Could not create tag $tagName" -ForegroundColor Red
        exit 1
    }
    Write-Host "Tag $tagName created on version bump commit" -ForegroundColor Green
}

$distDir = Join-Path $PSScriptRoot "build\dist"
$expectedSetupPath = Join-Path $distDir "MIB_Studio_Qt_Setup_v$newVersion.exe"
$expectedUpdatePath = Join-Path $distDir "MIB_Studio_Qt_Update_v$newVersion.exe"
$setupExe = $null
$updateExe = $null

# --- Step 3: Build (optional) ---
if (-not $SkipBuild) {
    Write-Host "`n--- Step 3: Build Release ---" -ForegroundColor Cyan

    if ($DryRun) {
        Write-Host "[DRY RUN] Would provision the pinned MindVision SDK and configure MIB_ENABLE_MINDVISION=ON" -ForegroundColor Gray
        Write-Host "[DRY RUN] Would reconfigure Release with the repository processing-core signer trust pin" -ForegroundColor Gray
        Write-Host "[DRY RUN] Would build the full Release target set and run CTest" -ForegroundColor Gray
    } else {
        Write-Host "Provisioning the pinned MindVision SDK..." -ForegroundColor Yellow
        $mindVisionSdk = & "$PSScriptRoot\scripts\provision-mindvision-sdk.ps1" `
            -Destination "$PSScriptRoot\build\vendor\mindvision-sdk" `
            -PassThru
        Write-Host "Configuring the repository processing-core signer trust pin..." -ForegroundColor Yellow
        cmake -S $PSScriptRoot -B "$PSScriptRoot\build" `
            -DMIB_ENABLE_MINDVISION=ON `
            "-DMIB_MINDVISION_SDK_ROOT=$($mindVisionSdk.SdkRoot)" `
            "-DMIB_MINDVISION_RUNTIME_DIR=$($mindVisionSdk.RuntimeDir)" `
            -DMIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI=ON `
            "-DMIB_PROCESSING_CORE_SIGNER_SPKI_SHA256=$processingCoreSignerSpki" `
            "-DMIB_RELEASE_VERSION_OVERRIDE=$newVersion" `
            "-DMIB_RELEASE_VERSION_FULL_OVERRIDE=$($tagName.Substring(1))"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: CMake rejected the production signer configuration" -ForegroundColor Red
            exit 1
        }
        $identity = @{}
        Get-Content -LiteralPath "$PSScriptRoot\build\mib-release-identity.txt" | ForEach-Object {
            $parts = $_ -split '=', 2
            if ($parts.Count -eq 2) { $identity[$parts[0]] = $parts[1] }
        }
        if ($identity.version -ne $newVersion -or
            $identity.full_version -ne $tagName.Substring(1)) {
            Write-Host "ERROR: Configured release identity does not match $tagName" -ForegroundColor Red
            exit 1
        }
        Write-Host "Building Release..." -ForegroundColor Yellow
        cmake --build "$PSScriptRoot\build" --config Release
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Build failed" -ForegroundColor Red
            exit 1
        }
        $mindVisionRuntime = "$PSScriptRoot\build\Release\MVCAMSDK_X64.dll"
        if (-not (Test-Path -LiteralPath $mindVisionRuntime -PathType Leaf)) {
            Write-Host "ERROR: MindVision runtime is missing from the release payload: $mindVisionRuntime" -ForegroundColor Red
            exit 1
        }
        Write-Host "Running Release tests..." -ForegroundColor Yellow
        ctest --test-dir "$PSScriptRoot\build" --build-config Release `
            --output-on-failure --timeout 30
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Release tests failed" -ForegroundColor Red
            exit 1
        }
        Write-Host "Build and tests succeeded" -ForegroundColor Green
    }

    # --- Step 4: Build installers ---
    Write-Host "`n--- Step 4: Build Installers ---" -ForegroundColor Cyan

    if ($DryRun) {
        Write-Host "[DRY RUN] Would remove prior MIB Studio installer outputs from $distDir" -ForegroundColor Gray
        Write-Host "[DRY RUN] Would build installers" -ForegroundColor Gray
        $setupExe = [System.IO.FileInfo]::new($expectedSetupPath)
        $updateExe = [System.IO.FileInfo]::new($expectedUpdatePath)
    } else {
        New-Item -ItemType Directory -Force $distDir | Out-Null
        Get-ChildItem -LiteralPath $distDir -Filter "MIB_Studio_Qt_*" -File `
            -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^MIB_Studio_Qt_(Setup|Update)_v.+\.exe$' } |
            Remove-Item -Force

        Write-Host "Building full installer..." -ForegroundColor Yellow
        cmake --build "$PSScriptRoot\build" --config Release --target package_installer
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Full installer build failed" -ForegroundColor Red
            exit 1
        }
        if (-not (Test-Path -LiteralPath $expectedSetupPath -PathType Leaf)) {
            Write-Host "ERROR: Full installer did not produce exact expected artifact $expectedSetupPath" -ForegroundColor Red
            exit 1
        }
        Write-Host "Full installer built" -ForegroundColor Green

        Write-Host "Building update package..." -ForegroundColor Yellow
        cmake --build "$PSScriptRoot\build" --config Release --target package_installer_update
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Update package build failed" -ForegroundColor Red
            exit 1
        }
        if (-not (Test-Path -LiteralPath $expectedUpdatePath -PathType Leaf)) {
            Write-Host "ERROR: Update package did not produce exact expected artifact $expectedUpdatePath" -ForegroundColor Red
            exit 1
        }
        Write-Host "Update package built" -ForegroundColor Green

        $setupExe = Get-Item -LiteralPath $expectedSetupPath
        $updateExe = Get-Item -LiteralPath $expectedUpdatePath
        Write-Host "`nInstaller output:" -ForegroundColor Cyan
        @($setupExe, $updateExe) | ForEach-Object {
            $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower().Substring(0, 16)
            Write-Host "  $($_.Name) ($([math]::Round($_.Length / 1MB, 1)) MB) sha256:$hash..." -ForegroundColor Green
        }
    }
} else {
    Write-Host "`n--- Step 3-4: Build (skipped) ---" -ForegroundColor Cyan
}

# --- Step 5: Push + Publish ---
if ($Push) {
    Write-Host "`n--- Step 5: Push to Remote ---" -ForegroundColor Cyan

    if ($DryRun) {
        Write-Host "[DRY RUN] Would atomically push branch and tag $tagName" -ForegroundColor Gray
    } else {
        Write-Host "Atomically pushing branch '$currentBranch' and tag $tagName..." -ForegroundColor Yellow
        git push --atomic origin "HEAD:refs/heads/$currentBranch" "refs/tags/$tagName"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Atomic branch/tag push failed; neither ref was published" -ForegroundColor Red
            exit 1
        }
        Write-Host "Branch and tag pushed atomically" -ForegroundColor Green
    }

    # --- Step 6: Create GitHub Release ---
    if (-not $SkipBuild) {
        Write-Host "`n--- Step 6: Create GitHub Release ---" -ForegroundColor Cyan

        if (-not $updateExe) {
            Write-Host "ERROR: Exact update installer was not prepared" -ForegroundColor Red
            exit 1
        }

        if ($DryRun) {
            Write-Host "[DRY RUN] Would create GitHub Release for $tagName with $($updateExe.Name)" -ForegroundColor Gray
        } else {
            $updateHash = (Get-FileHash -Algorithm SHA256 $updateExe.FullName).Hash.ToLower()
            $checksumLines = "$updateHash  $($updateExe.Name)`n"
            $releaseFiles = @($updateExe.FullName)

            $releaseBody = @"
## MIB Studio Qt $tagName

**Channel:** ``$channel``

### Downloads
- **Update Package** - For existing installations (app files only)

### Checksums (SHA-256)
``````
$checksumLines``````
"@

            $ghArgs = @("release", "create", "$tagName", "--title", "MIB Studio Qt $tagName", "--notes", $releaseBody)
            if ($Beta) {
                $ghArgs += "--prerelease"
            }
            $ghArgs += $releaseFiles

            Write-Host "Creating GitHub Release for $tagName..." -ForegroundColor Yellow
            & gh @ghArgs
            if ($LASTEXITCODE -ne 0) {
                Write-Host "ERROR: GitHub Release creation failed" -ForegroundColor Red
                exit 1
            }
            Write-Host "GitHub Release created" -ForegroundColor Green
        }

        # --- Step 7: Publish to Cloudflare R2 ---
        Write-Host "`n--- Step 7: Publish to Cloudflare R2 ($channel) ---" -ForegroundColor Cyan

        if ($updateExe) {
            if ($DryRun) {
                Write-Host "[DRY RUN] Would publish $($updateExe.Name) to $channel channel" -ForegroundColor Gray
            } else {
                Write-Host "Publishing to $channel channel..." -ForegroundColor Yellow
                $python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
                $publishArgs = @(
                    "$PSScriptRoot\publish-update.py",
                    "--installer", $updateExe.FullName,
                    "--version", $tagName.Substring(1),
                    "--channel", $channel,
                    "--release-notes-url", "https://github.com/$releaseRepository/releases/tag/$tagName"
                )
                if ($Profile) {
                    $publishArgs += @("--profile", $Profile)
                }
                & $python @publishArgs
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "ERROR: Cloudflare R2 publish failed" -ForegroundColor Red
                    exit 1
                }
                Write-Host "Published to Cloudflare R2 ($channel)" -ForegroundColor Green
            }
        } else {
            Write-Host "ERROR: Exact update package missing before Cloudflare R2 publish" -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host "`n--- Step 6-7: Publish (skipped - no build) ---" -ForegroundColor Cyan
    }
} else {
    Write-Host "`n--- Step 5: Push (manual) ---" -ForegroundColor Cyan
    Write-Host "To publish this release:" -ForegroundColor Yellow
    Write-Host "  .\release.ps1 --push   (or push manually below)" -ForegroundColor White
    Write-Host "  git push origin main" -ForegroundColor White
    Write-Host "  git push origin $tagName" -ForegroundColor White
}

# --- Done ---
Write-Host "`n=== Release Complete ===" -ForegroundColor Cyan
Write-Host "Version: $tagName (channel: $channel)" -ForegroundColor Green

if (-not $SkipBuild -and $setupExe -and $updateExe) {
    Write-Host "Installers: $($setupExe.Name), $($updateExe.Name)" -ForegroundColor Green
}

if ($Push -and -not $SkipBuild) {
    Write-Host "Channel: $channel" -ForegroundColor Green
    Write-Host "Status: Released (GitHub Release + Cloudflare R2)" -ForegroundColor Green
} elseif ($Push) {
    Write-Host "Status: Tag pushed (no installers published)" -ForegroundColor Yellow
} else {
    Write-Host "Status: Local only. Run with --push to publish." -ForegroundColor Yellow
}
