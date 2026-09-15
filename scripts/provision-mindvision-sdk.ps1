# Downloads and extracts the pinned MindVision SDK used by Windows builds.
# The full vendor installer is not added to the MIB Studio installer; only the
# runtime DLL copied by CMake is shipped with the application.

param(
    [string]$InstallerUrl = "https://updates.yofo.bio/mindvision-sdk/MindVision-Camera-Platform-Setup2.1.10.195_202604021438.exe",
    [string]$InstallerSha256 = "a62f58a8aef103d0061dc2c12b709d0655136cc488840e22361dff08adc4d4f4",
    [string]$Destination = (Join-Path $PSScriptRoot "..\build\vendor\mindvision-sdk"),
    [switch]$PassThru
)

$ErrorActionPreference = "Stop"

$destinationPath = [System.IO.Path]::GetFullPath($Destination)
$installerPath = Join-Path $destinationPath "mindvision-sdk-installer.exe"
$extractRoot = Join-Path $destinationPath "extracted"
$sdkRoot = Join-Path $extractRoot "Demo\VC++"
$includeDir = Join-Path $sdkRoot "Include"
$runtimeDir = Join-Path $extractRoot "SDK\X64"
$runtimeDll = Join-Path $runtimeDir "MVCAMSDK_X64.dll"
$loaderHeader = Join-Path $includeDir "CameraApiLoad.h"

$requiredFiles = @(
    $loaderHeader,
    (Join-Path $includeDir "CameraApi.h"),
    (Join-Path $includeDir "CameraDefine.H"),
    (Join-Path $includeDir "CameraStatus.h"),
    $runtimeDll
)

function Test-ExtractedSdk {
    foreach ($path in $requiredFiles) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            return $false
        }
    }
    $loader = Get-Content -LiteralPath $loaderHeader -Raw
    $requiredApis = @(
        "CameraGetImageBufferPriority",
        "CameraSetExtTrigSignalType",
        "CameraSetExtTrigJitterTime",
        "CameraSetTriggerDelayTime",
        "CameraSetTriggerCount",
        "CameraSetStrobeMode",
        "CameraSetStrobePulseWidth",
        "CameraSetStrobeDelayTime",
        "CameraSetStrobePolarity",
        "CameraSetOutPutIOMode",
        "CameraSetIOStateEx",
        "CameraSoftTrigger"
    )
    foreach ($api in $requiredApis) {
        if (-not $loader.Contains($api)) {
            return $false
        }
    }
    return $true
}

New-Item -ItemType Directory -Force -Path $destinationPath | Out-Null

$needsDownload = $true
if (Test-Path -LiteralPath $installerPath -PathType Leaf) {
    $existingHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $needsDownload = $existingHash -ne $InstallerSha256.ToLowerInvariant()
}

if ($needsDownload) {
    Write-Host "Downloading pinned MindVision SDK from $InstallerUrl"
    & curl.exe --fail --location --retry 3 --output $installerPath $InstallerUrl
    if ($LASTEXITCODE -ne 0) {
        throw "MindVision SDK download failed with exit code $LASTEXITCODE"
    }
}

$actualHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $InstallerSha256.ToLowerInvariant()) {
    throw "MindVision SDK SHA-256 mismatch: expected $InstallerSha256, got $actualHash"
}

if ($needsDownload -or -not (Test-ExtractedSdk)) {
    $sevenZipCommand = Get-Command 7z.exe -ErrorAction SilentlyContinue
    $sevenZipPath = if ($sevenZipCommand) { $sevenZipCommand.Source } else { $null }
    if (-not $sevenZipPath) {
        $programFilesSevenZip = Join-Path $env:ProgramFiles "7-Zip\7z.exe"
        if (Test-Path -LiteralPath $programFilesSevenZip -PathType Leaf) {
            $sevenZipPath = $programFilesSevenZip
        }
    }
    if (-not $sevenZipPath) {
        throw "7-Zip is required to extract the MindVision SDK installer"
    }

    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    & $sevenZipPath x -y "-o$extractRoot" $installerPath `
        "Demo/VC++/Include/*" "SDK/X64/MVCAMSDK_X64.dll" | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "MindVision SDK extraction failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-ExtractedSdk)) {
    throw "Extracted MindVision SDK is missing required headers, MVCAMSDK_X64.dll, or camera APIs"
}

$result = [PSCustomObject]@{
    SdkRoot = $sdkRoot
    RuntimeDir = $runtimeDir
    RuntimeDll = $runtimeDll
    InstallerSha256 = $actualHash
}

Write-Host "MindVision SDK ready: $sdkRoot"
Write-Host "MindVision runtime ready: $runtimeDll"
if ($PassThru) {
    $result
}
