# Unsigned portable candidate, not an installer/update or hardware qualification.
param([string]$BuildDir = 'build', [string]$Destination = 'build/tauri-windows-candidate')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $repo
try {
    $exe = (Resolve-Path 'desktop/src-tauri/target/release/mib-studio-desktop.exe').Path
    $manifest = (Resolve-Path "$BuildDir/mib-bridge-link-manifest.json").Path
    if (Test-Path $Destination) { throw "Use an absent destination to avoid stale DLLs: $Destination" }
    cmake "-DEXE=$exe" "-DMANIFEST=$manifest" "-DDEST=$Destination" -P desktop/scripts/windows-runtime.cmake
    if ($LASTEXITCODE -ne 0) { throw 'Native dependency closure failed' }
    New-Item -ItemType Directory -Force "$Destination/resources" | Out-Null
    Copy-Item resources/defaults,resources/isoelastic_curve "$Destination/resources" -Recurse
    @'
UNSIGNED SDK-free Windows x64 Tauri candidate. Not a Qt replacement release.
Extract the entire directory. Requires Microsoft Edge WebView2 Evergreen Runtime.
Camera SDKs are disabled in this candidate; use mock acquisition for acceptance.
No Qt updater feed, release tag, installer registry state, signing or publication.
Run mib-studio-desktop.exe. Keep the runtime DLLs and resources beside it.
'@ | Set-Content "$Destination/README.txt"
    Get-ChildItem $Destination -Recurse -File | ForEach-Object {
        "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetRelativePath((Resolve-Path $Destination), $_.FullName))"
    } | Set-Content "$Destination/SHA256SUMS.txt"
    Compress-Archive -Path "$Destination/*" -DestinationPath "$Destination.zip"
} finally { Pop-Location }
