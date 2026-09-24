# Compatibility wrapper for the Python updater publishing command.
# Prefer: python scripts/release/publish-update.py --installer build\dist\MIB_Studio_Qt_Update_v0.2.0.exe

param(
    [Parameter(Mandatory=$false)]
    [string]$Version,

    [Parameter(Mandatory=$true)]
    [string]$Installer,

    [string]$Endpoint = $env:MIB_STUDIO_R2_ENDPOINT,
    [string]$Bucket = "mib-studio-qt-updates",
    [string]$PublicBaseUrl = "https://updates.yofo.bio",
    [string]$Channel = "stable",
    [string]$Profile = $env:MIB_STUDIO_R2_PROFILE,
    [string]$Acl = "",
    [string]$ReleaseNotesUrl = "",
    [string]$ManifestOut = "",
    [ValidateSet("auto", "s3", "wrangler")]
    [string]$UploadMethod = "auto",
    [string]$WranglerBin = $env:WRANGLER_BIN,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$script = Join-Path $PSScriptRoot "publish-update.py"

$args = @($script, "--installer", $Installer, "--bucket", $Bucket, "--public-base-url", $PublicBaseUrl, "--channel", $Channel, "--upload-method", $UploadMethod)
if ($Version) { $args += @("--version", $Version) }
if ($Endpoint) { $args += @("--endpoint", $Endpoint) }
if ($Profile) { $args += @("--profile", $Profile) }
if ($Acl) { $args += @("--acl", $Acl) }
if ($ReleaseNotesUrl) { $args += @("--release-notes-url", $ReleaseNotesUrl) }
if ($ManifestOut) { $args += @("--manifest-out", $ManifestOut) }
if ($WranglerBin) { $args += @("--wrangler-bin", $WranglerBin) }
if ($DryRun) { $args += "--dry-run" }
if ($env:S3_UPLOAD_DEBUG) { $args += "--debug" }

& $python @args
exit $LASTEXITCODE
