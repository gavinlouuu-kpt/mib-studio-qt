<#
.SYNOPSIS
  doctor.ps1 — report what this Windows host is missing to build MIB Studio Qt.

.DESCRIPTION
  Checks only; never installs. Every MISSING line carries the command that
  fixes it and the summary repeats them. Exit 0 when complete, 1 otherwise.
  Reads env/toolchain.toml, conan/profiles/, env/assets.json (through
  scripts/provision-assets.py).

  .\scripts\doctor.ps1                 # VS 2022 + Conan + Ninja fast loop
  .\scripts\doctor.ps1 -WithPrivate    # also require HF_TOKEN for private assets
#>
[CmdletBinding()]
param(
    [switch]$WithPrivate,
    [switch]$Quiet
)
$ErrorActionPreference = "Continue"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$script:Missing = 0
$script:Fixes = @()

function Write-Ok($m)   { if (-not $Quiet) { Write-Host "  OK       $m" -ForegroundColor Green } }
function Write-Warn($m) { Write-Host "  WARN     $m" -ForegroundColor Yellow }
function Write-Miss($m, $fix) { Write-Host "  MISSING  $m" -ForegroundColor Red; $script:Missing++; if ($fix) { $script:Fixes += $fix } }
function Write-Old($m, $fix)  { Write-Host "  OLD      $m" -ForegroundColor Red; $script:Missing++; if ($fix) { $script:Fixes += $fix } }
function Section($t) { Write-Host ""; Write-Host $t -ForegroundColor White }

function Get-ToolMin($name) {
    $line = Get-Content (Join-Path $RepoRoot "env\toolchain.toml") | Where-Object { $_ -match "^$name\s*=\s*`">=([0-9.]+)" } | Select-Object -First 1
    if ($line -match '">=([0-9.]+)') { return $Matches[1] }
    return $null
}
function Get-ToolVersion($name) {
    $exe = switch ($name) { "python" { "python" } "rust" { "cargo" } default { $name } }
    # An absent executable is a MISSING line, not a CommandNotFound error.
    if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { return $null }
    $out = switch ($name) {
        "git"    { git --version 2>$null }
        "cmake"  { (cmake --version 2>$null) | Select-Object -First 1 }
        "ninja"  { ninja --version 2>$null }
        "conan"  { conan --version 2>$null }
        "python" { python --version 2>$null }
        "node"   { node --version 2>$null }
        "rust"   { cargo --version 2>$null }
    }
    if ($out -and ("$out" -match '([0-9]+(\.[0-9]+)+)')) { return $Matches[1] }
    return $null
}
function Test-VersionGe($have, $min) { try { return ([version]$have -ge [version]$min) } catch { return $true } }

Write-Host "MIB Studio Qt doctor — Windows"

Section "Toolchain (env/toolchain.toml)"
$fixFor = @{
    git    = "winget install --id Git.Git -e"
    cmake  = "winget install --id Kitware.CMake -e"
    ninja  = "winget install --id Ninja-build.Ninja -e"
    python = "winget install --id Python.Python.3.12 -e"
    conan  = "python -m pip install 'conan>=2,<3'"
    node   = "winget install --id OpenJS.NodeJS.LTS -e   (pin: .nvmrc)"
    rust   = "winget install --id Rustlang.Rustup -e     (pin: rust-toolchain.toml)"
}
foreach ($tool in @("git", "cmake", "ninja", "python", "conan")) {
    $min = Get-ToolMin $tool; $have = Get-ToolVersion $tool
    if (-not $have) { Write-Miss "$tool (>= $min)" $fixFor[$tool] }
    elseif ($min -and -not (Test-VersionGe $have $min)) { Write-Old "$tool $have (need >= $min)" $fixFor[$tool] }
    else { Write-Ok "$tool $have" }
}
foreach ($tool in @("node", "rust")) {
    $have = Get-ToolVersion $tool
    if ($have) { Write-Ok "$tool $have (optional: desktop shell / Rust bridge)" } else { Write-Warn "$tool not installed (only for desktop/ and crates/mib-bridge)" }
}

Section "Visual Studio 2022 (MSVC 19.4x, conan/profiles/windows-msvc194*)"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vs) { Write-Ok "VC++ tools at $vs" } else { Write-Miss "VS 2022 C++ build tools" "winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" }
} else {
    Write-Miss "Visual Studio installer (vswhere.exe)" "winget install --id Microsoft.VisualStudio.2022.BuildTools -e"
}
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    Write-Ok "cl.exe on PATH (developer shell active) — required for the windows-ninja preset"
    # The Ninja generator only tracks header dependencies with the English
    # /showIncludes prefix (see knowledge_map/build-and-run/Build.md).
    $tmp = Join-Path $env:TEMP "mib-doctor-showincludes"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Set-Content -Path (Join-Path $tmp "h.h") -Value "#pragma once"
    Set-Content -Path (Join-Path $tmp "t.cpp") -Value "#include `"h.h`"`nint main(){return 0;}"
    Push-Location $tmp
    $inc = (& cl.exe /nologo /showIncludes /c t.cpp 2>&1) -join "`n"
    Pop-Location
    if ($inc -match "Note: including file:") { Write-Ok "cl.exe prints the English /showIncludes prefix" }
    elseif ($inc -match "including file") { Write-Miss "cl.exe prints a localized /showIncludes prefix; Ninja will not rebuild on header edits" "Install the en-US language pack (vs_installer.exe modify --addProductLang en-US) or pass -DCMAKE_CL_SHOWINCLUDES_PREFIX=<prefix>" }
} else {
    Write-Warn "cl.exe not on PATH: open a 'x64 Native Tools Command Prompt' (vcvars64.bat) for the windows-ninja preset; windows-default (VS generator) works without it"
}

Section "Conan"
if (Get-Command conan -ErrorAction SilentlyContinue) {
    $p = & conan profile path default 2>$null
    if ($LASTEXITCODE -eq 0 -and $p) { Write-Ok "default profile: $p" } else { Write-Miss "Conan default profile" "conan profile detect" }
    Write-Ok "repo profiles: $((Get-ChildItem (Join-Path $RepoRoot 'conan\profiles') | ForEach-Object Name) -join ' ')"
}

Section "MindVision SDK (scripts/provision-mindvision-sdk.ps1)"
$sdkRoot = if ($env:MIB_MINDVISION_SDK_ROOT) { $env:MIB_MINDVISION_SDK_ROOT } else { Join-Path $RepoRoot "build\vendor\mindvision-sdk\extracted" }
if (Test-Path (Join-Path $sdkRoot "include\CameraApiLoad.h")) { Write-Ok "headers at $sdkRoot" }
elseif (Test-Path (Join-Path $sdkRoot "include\CameraApi.h")) { Write-Ok "headers at $sdkRoot" }
else { Write-Miss "MindVision SDK not provisioned ($sdkRoot)" ".\scripts\provision-mindvision-sdk.ps1   # or configure with -DMIB_ENABLE_MINDVISION=OFF" }

Section "External assets (env/assets.json)"
if (Get-Command python -ErrorAction SilentlyContinue) {
    $out = python (Join-Path $RepoRoot "scripts\provision-assets.py") --check --required-only 2>&1
    if ($LASTEXITCODE -eq 0) { Write-Ok "required assets present" }
    else { Write-Miss "required assets incomplete" "python scripts\provision-assets.py --required-only"; if (-not $Quiet) { $out | ForEach-Object { "           $_" } } }
    if ($WithPrivate) {
        $tokenFile = Join-Path $env:USERPROFILE ".cache\huggingface\token"
        if ($env:HF_TOKEN -or (Test-Path $tokenFile)) { Write-Ok "Hugging Face token available" } else { Write-Miss "HF_TOKEN (private assets requested)" "`$env:HF_TOKEN = '<token>'   # or: hf auth login" }
    }
} else { Write-Warn "python missing; cannot check assets" }

Write-Host ""
if ($script:Missing -eq 0) {
    Write-Host "doctor: everything present."
    Write-Host "next (VS generator): conan install . -of build --build=missing -s build_type=Release -pr conan/profiles/windows-msvc194 ; cmake --preset windows-default"
    Write-Host "next (fast Ninja loop, from a VS x64 dev shell): conan install . -of build-ninja --build=missing -s build_type=Release -pr conan/profiles/windows-msvc194-ninja ; cmake --preset windows-ninja"
    exit 0
}
Write-Host "doctor: $($script:Missing) item(s) missing. Run these:"
$script:Fixes | ForEach-Object { Write-Host "  $_" }
Write-Host "or let .\scripts\bootstrap.ps1 do it."
exit 1
