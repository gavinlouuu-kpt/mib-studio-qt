# Build script for MIB Studio Tools (Windows)
# Builds hdf5_export_app.exe and mib_reanalyse_hdf5.exe into tools/dist/

param(
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"

$ToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ToolsDir

Write-Host "Building MIB Studio Tools for Windows..." -ForegroundColor Green

if ($Clean) {
    Write-Host "Cleaning previous builds..." -ForegroundColor Yellow
    if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
    if (Test-Path "dist") { Remove-Item -Recurse -Force "dist" }
}

Write-Host "Checking Python installation..." -ForegroundColor Cyan
try {
    $pythonVersion = python --version 2>&1
    Write-Host "Found: $pythonVersion" -ForegroundColor Green
} catch {
    Write-Host "ERROR: Python not found. Please install Python 3.8 or later." -ForegroundColor Red
    exit 1
}

$venvPath = ".venv"
if (-not (Test-Path $venvPath)) {
    Write-Host "Creating virtual environment..." -ForegroundColor Cyan
    python -m venv $venvPath
}

Write-Host "Activating virtual environment..." -ForegroundColor Cyan
& "$venvPath\Scripts\Activate.ps1"

Write-Host "Upgrading pip..." -ForegroundColor Cyan
python -m pip install --upgrade pip

Write-Host "Installing dependencies..." -ForegroundColor Cyan
pip install -r ..\env\requirements-tools-runtime.txt
pip install -r ..\env\requirements-tools-build.txt

$distPath = "dist"
$workPath = "build"
New-Item -ItemType Directory -Force -Path $distPath | Out-Null
New-Item -ItemType Directory -Force -Path $workPath | Out-Null

# Build HDF5 Export GUI
Write-Host "Building hdf5_export_app..." -ForegroundColor Cyan
pyinstaller hdf5_export_app/hdf5_export.spec --clean --workpath $workPath --distpath $distPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed: hdf5_export_app" -ForegroundColor Red
    exit 1
}

# Build Reanalyse HDF5 CLI
Write-Host "Building mib_reanalyse_hdf5..." -ForegroundColor Cyan
pyinstaller reanalyse_hdf5/reanalyse_hdf5.spec --clean --workpath $workPath --distpath $distPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed: mib_reanalyse_hdf5" -ForegroundColor Red
    exit 1
}

Write-Host "`nBuild successful!" -ForegroundColor Green
Write-Host "Output: $ToolsDir\$distPath\" -ForegroundColor Green
foreach ($name in @("hdf5_export_app.exe", "mib_reanalyse_hdf5.exe")) {
    $p = Join-Path $distPath $name
    if (Test-Path $p) {
        $size = [math]::Round((Get-Item $p).Length / 1MB, 2)
        Write-Host "  $name ($size MB)" -ForegroundColor Cyan
    }
}
Write-Host "`nDone!" -ForegroundColor Green
