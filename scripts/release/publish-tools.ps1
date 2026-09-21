# Compatibility wrapper for the Python tools publishing command.
# Prefer: python scripts/release/publish-tools.py --zip tools\dist\MIB_Studio_Tools_v0.1.7_windows.zip

param(
    [Parameter(Mandatory=$false)]
    [string]$Version,

    [Parameter(Mandatory=$false)]
    [string]$Zip,

    [string]$Endpoint = $env:MIB_STUDIO_R2_ENDPOINT,
    [string]$Bucket = "mib-studio-qt-updates",
    [string]$PublicBaseUrl = "https://updates.yofo.bio",
    [string]$Channel = "stable",
    [string]$Profile = $env:MIB_STUDIO_R2_PROFILE,
    [string]$Acl = "",
    [string]$ManifestOut = "",
    [ValidateSet("auto", "s3", "wrangler")]
    [string]$UploadMethod = "auto",
    [string]$WranglerBin = $env:WRANGLER_BIN,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$script = Join-Path $PSScriptRoot "publish-tools.py"

$args = @($script, "--bucket", $Bucket, "--public-base-url", $PublicBaseUrl, "--channel", $Channel, "--upload-method", $UploadMethod)
if ($Version) { $args += @("--version", $Version) }
if ($Zip) { $args += @("--zip", $Zip) }
if ($Endpoint) { $args += @("--endpoint", $Endpoint) }
if ($Profile) { $args += @("--profile", $Profile) }
if ($Acl) { $args += @("--acl", $Acl) }
if ($ManifestOut) { $args += @("--manifest-out", $ManifestOut) }
if ($WranglerBin) { $args += @("--wrangler-bin", $WranglerBin) }
if ($DryRun) { $args += "--dry-run" }
if ($env:S3_UPLOAD_DEBUG) { $args += "--debug" }

& $python @args
exit $LASTEXITCODE
