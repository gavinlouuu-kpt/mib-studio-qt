# Compatibility wrapper for the Python public update manifest verifier.
# Prefer: python scripts/release/verify-update-manifest.py --manifest-url https://updates.yofo.bio/stable/latest.json

param(
    [string]$ManifestUrl = "https://updates.yofo.bio/stable/latest.json",
    [int]$Timeout = 30
)

$ErrorActionPreference = "Stop"

$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$script = Join-Path $PSScriptRoot "verify-update-manifest.py"

& $python $script --manifest-url $ManifestUrl --timeout $Timeout
exit $LASTEXITCODE
