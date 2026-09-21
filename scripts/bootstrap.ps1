<#
.SYNOPSIS
  bootstrap.ps1 — install what scripts/doctor.ps1 reports missing (Windows).

.DESCRIPTION
  Idempotent; safe to rerun. Uses winget for tools, pip for Conan, then
  provisions the MindVision SDK and the external assets from env/assets.json,
  and runs `conan install` with the repository profile. Reads the same files
  as the doctor. Visual Studio 2022 C++ build tools are installed via winget
  only with -InstallVisualStudio (large download).

  .\scripts\bootstrap.ps1                       # tools + SDK + required assets + conan install (VS generator)
  .\scripts\bootstrap.ps1 -Generator Ninja      # conan toolchain for the windows-ninja preset (build-ninja/)
  .\scripts\bootstrap.ps1 -PublicAssetsOnly     # never needs HF_TOKEN
  .\scripts\bootstrap.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [ValidateSet("VisualStudio", "Ninja")] [string]$Generator = "VisualStudio",
    [ValidateSet("Release", "Debug")] [string]$BuildType = "Release",
    [switch]$PublicAssetsOnly,
    [switch]$InstallVisualStudio,
    [switch]$SkipTools,
    [switch]$SkipSdk,
    [switch]$SkipAssets,
    [switch]$SkipConanInstall,
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $RepoRoot

function Step($t) { Write-Host ""; Write-Host "==> $t" -ForegroundColor White }
function Run { param([Parameter(ValueFromRemainingArguments)] $cmd)
    Write-Host "    $ $($cmd -join ' ')"
    if (-not $DryRun) { & $cmd[0] $cmd[1..($cmd.Count - 1)]; if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) { throw "command failed ($LASTEXITCODE): $($cmd -join ' ')" } }
}
function Has($name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }

Write-Host "MIB Studio Qt bootstrap — Windows, generator $Generator$(if ($DryRun) { ' (dry run)' })"

if (-not $SkipTools) {
    Step "Tools via winget (env/toolchain.toml)"
    if (-not (Has winget)) { throw "winget is required (App Installer from the Microsoft Store); or install git, cmake, ninja, python 3.12 manually and rerun with -SkipTools" }
    $wanted = @(
        @{ cmd = "git";    id = "Git.Git" },
        @{ cmd = "cmake";  id = "Kitware.CMake" },
        @{ cmd = "ninja";  id = "Ninja-build.Ninja" },
        @{ cmd = "python"; id = "Python.Python.3.12" }
    )
    foreach ($w in $wanted) {
        if (Has $w.cmd) { Write-Host "    $($w.cmd) present" } else { Run winget install --id $w.id -e --accept-source-agreements --accept-package-agreements }
    }
    if (-not (Has sccache)) { Write-Host "    (optional) sccache speeds Ninja rebuilds: winget install --id Mozilla.sccache -e" }
    if ($InstallVisualStudio) {
        Run winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --addProductLang en-US"
    }
    Step "Conan (env/requirements-build.txt)"
    if (Has conan) { Write-Host "    conan present: $(conan --version)" } else { Run python -m pip install -r (Join-Path $RepoRoot "env\requirements-build.txt") }
    $p = conan profile path default 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $p) { Run conan profile detect }
}

if (-not $SkipSdk) {
    Step "MindVision SDK (scripts/provision-mindvision-sdk.ps1)"
    $sdkRoot = if ($env:MIB_MINDVISION_SDK_ROOT) { $env:MIB_MINDVISION_SDK_ROOT } else { Join-Path $RepoRoot "build\vendor\mindvision-sdk\extracted" }
    if ((Test-Path (Join-Path $sdkRoot "include\CameraApiLoad.h")) -or (Test-Path (Join-Path $sdkRoot "include\CameraApi.h"))) {
        Write-Host "    already provisioned at $sdkRoot"
    } else {
        Run powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "scripts\provision-mindvision-sdk.ps1") -Destination (Join-Path $RepoRoot "build\vendor\mindvision-sdk")
    }
}

if (-not $SkipAssets) {
    $scope = if ($PublicAssetsOnly) { "--public-only" } else { "--required-only" }
    Step "External assets (env/assets.json) $scope"
    Run python (Join-Path $RepoRoot "scripts\provision-assets.py") $scope
}

if (-not $SkipConanInstall) {
    if ($Generator -eq "Ninja") {
        $profile = "conan/profiles/windows-msvc194-ninja"; $of = "build-ninja"; $preset = "windows-ninja"
        if (-not (Has cl.exe)) { Write-Host "    NOTE: run from a VS 2022 x64 developer shell (vcvars64.bat) before 'cmake --preset windows-ninja'" -ForegroundColor Yellow }
    } else {
        $profile = "conan/profiles/windows-msvc194"; $of = "build"; $preset = "windows-default"
    }
    Step "conan install ($profile -> $of/)"
    Run conan install . -of $of --build=missing -s build_type=$BuildType -pr $profile
    Step "Done"
    Write-Host "    cmake --preset $preset"
    Write-Host "    cmake --build --preset $preset-build$(if ($Generator -eq 'VisualStudio' -and $BuildType -eq 'Release') { '-release' })"
} else {
    Step "Done"
}
Write-Host "    verify any time with: .\scripts\doctor.ps1"
