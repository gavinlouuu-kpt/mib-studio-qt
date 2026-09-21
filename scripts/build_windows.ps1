# Build script for Windows HDF5 Export GUI Application
# Creates a standalone .exe using PyInstaller

param(
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"

# Get script directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

Write-Host "Building HDF5 Export GUI Application for Windows..." -ForegroundColor Green

# Clean previous builds if requested
if ($Clean) {
    Write-Host "Cleaning previous builds..." -ForegroundColor Yellow
    if (Test-Path "build") {
        Remove-Item -Recurse -Force "build"
    }
    if (Test-Path "dist") {
        Remove-Item -Recurse -Force "dist"
    }
    if (Test-Path "__pycache__") {
        Remove-Item -Recurse -Force "__pycache__"
    }
    Get-ChildItem -Filter "*.spec" | ForEach-Object {
        if (Test-Path $_.BaseName) {
            Remove-Item -Recurse -Force $_.BaseName
        }
    }
}

# Check for Python
Write-Host "Checking Python installation..." -ForegroundColor Cyan
try {
    $pythonVersion = python --version 2>&1
    Write-Host "Found: $pythonVersion" -ForegroundColor Green
} catch {
    Write-Host "ERROR: Python not found. Please install Python 3.8 or later." -ForegroundColor Red
    exit 1
}

# Create virtual environment if it doesn't exist
$venvPath = ".venv"
if (-not (Test-Path $venvPath)) {
    Write-Host "Creating virtual environment..." -ForegroundColor Cyan
    python -m venv $venvPath
}

# Activate virtual environment
Write-Host "Activating virtual environment..." -ForegroundColor Cyan
& "$venvPath\Scripts\Activate.ps1"

# Upgrade pip
Write-Host "Upgrading pip..." -ForegroundColor Cyan
python -m pip install --upgrade pip

# Install dependencies
Write-Host "Installing dependencies..." -ForegroundColor Cyan
pip install -r ..\env\requirements-scripts.txt

# Verify we're in the right directory and files exist
Write-Host "Verifying files..." -ForegroundColor Cyan
if (-not (Test-Path "hdf5_export.spec")) {
    Write-Host "ERROR: hdf5_export.spec not found in current directory" -ForegroundColor Red
    Write-Host "Current directory: $(Get-Location)" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path "hdf5_export_app.py")) {
    Write-Host "ERROR: hdf5_export_app.py not found in current directory" -ForegroundColor Red
    Write-Host "Current directory: $(Get-Location)" -ForegroundColor Red
    exit 1
}

# Build with PyInstaller
Write-Host "Building executable with PyInstaller..." -ForegroundColor Cyan
Write-Host "Working directory: $(Get-Location)" -ForegroundColor Gray
Write-Host "Spec file: $(Resolve-Path hdf5_export.spec)" -ForegroundColor Gray
Write-Host "Script file: $(Resolve-Path hdf5_export_app.py)" -ForegroundColor Gray
pyinstaller hdf5_export.spec --clean --workpath build --distpath dist

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild successful!" -ForegroundColor Green
    Write-Host "Executable location: dist\hdf5_export_app.exe" -ForegroundColor Green
    
    # Check if executable exists
    $exePath = "dist\hdf5_export_app.exe"
    if (Test-Path $exePath) {
        $fileInfo = Get-Item $exePath
        Write-Host "File size: $([math]::Round($fileInfo.Length / 1MB, 2)) MB" -ForegroundColor Cyan
    }
} else {
    Write-Host "`nBuild failed!" -ForegroundColor Red
    exit 1
}

Write-Host "`nDone!" -ForegroundColor Green
