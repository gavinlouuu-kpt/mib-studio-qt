<#
.SYNOPSIS
  Check or restore the bench camera's parked state on win11-agent (Coaxlink Quad
  CXP-12 + SVS-Vistek MC2066) after a test run of MIB Studio.

.DESCRIPTION
  Runs on win11-agent (copy it over, or: ssh win11-agent "powershell -NoProfile
  -ExecutionPolicy Bypass -File C:\MIBBuild\bin\win11_bench_camera.ps1 -Action Check").
  Uses the Euresys CLI that ships with eGrabber (bin\x86_64\gentl.exe genapi).

    Check     Read-only. Dumps the remote device, the Coaxlink device/interface
              modules and the data stream, and diffs them against the saved
              baseline, ignoring counters. Exit 0 when identical, 1 otherwise.
    Restore   AcquisitionStop, then writes back only the values a MIB Studio run
              changes (see docs/howto/win11-bench-build.md), then runs Check.
              Refuses to run while any mib_studio_qt.exe is running (another
              session's test would be overwritten and its stream is in use)
              unless -Force is given.

  The parked baseline is the state recorded on 2026-11-04 before any test:
  Width 1920, Height 1080, OffsetX 0, OffsetY 0, ExposureTime 20; device module
  CycleMinimumPeriod 40000, ExposureRecoveryTime 1000, StrobeDelay 0,
  StrobeDuration 1000, CycleTriggerSource DEL1_1 (the cycle trigger comes from a
  delay line that is not running, so the camera delivers no frames until it is set
  to Immediate); interface TTLIO11/TTLIO12 as inputs, source Low, TTLIO11 not
  inverted. The dumps are kept in C:\MIBBuild\camera-baseline (first use copies
  them from C:\MIBTest\baseline\base_*.txt, read-only).
#>
[CmdletBinding()]
param(
    [ValidateSet('Check', 'Restore')]
    [string]$Action = 'Check',
    [switch]$Force,
    [string]$Gentl = 'C:\Program Files\Euresys\eGrabber\bin\x86_64\gentl.exe',
    [string]$BaselineDir = 'C:\MIBBuild\camera-baseline',
    [string]$WorkDir = 'C:\MIBBuild\camera-check'
)
$ErrorActionPreference = 'Continue'
$modules = 'remote', 'dev', 'if', 'ds'
# Lines that change by themselves (counters, temperature) are not state.
$ignore = '^Reading|EventCount|ErrorCount|Statistics|LostTrigger|CycleLost|TriggerCount|InternalTime|Temperature'

if (-not (Test-Path -LiteralPath $Gentl)) { throw "gentl.exe not found at $Gentl (eGrabber SDK not installed?)" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

if (-not (Test-Path -LiteralPath $BaselineDir)) {
    New-Item -ItemType Directory -Force -Path $BaselineDir | Out-Null
    foreach ($m in $modules) {
        $src = "C:\MIBTest\baseline\base_$m.txt"
        if (-not (Test-Path -LiteralPath $src)) { throw "no saved baseline: $src is missing and $BaselineDir is empty" }
        Copy-Item -LiteralPath $src -Destination (Join-Path $BaselineDir "base_$m.txt")
    }
}

function Dump([string]$Prefix) {
    foreach ($m in $modules) {
        & $Gentl genapi --ro --module=$m --dump 2>&1 | Out-File -Encoding ascii (Join-Path $WorkDir "${Prefix}_$m.txt")
    }
}

# Prints the differences and sets $script:Identical. (Not a return value: a
# PowerShell function's output stream would swallow it.)
function Compare-ToBaseline([string]$Prefix) {
    $script:Identical = $true
    $script:Unreadable = @()
    foreach ($m in $modules) {
        $nowText = Get-Content (Join-Path $WorkDir "${Prefix}_$m.txt") -Raw
        if ($nowText -match 'GC_ERR_\w+') {
            # e.g. GC_ERR_CUSTOM_IOCTL_DS_OPEN_FAILED: another program holds the data stream.
            $script:Unreadable += $m
            Write-Output "SKIP $m : could not be read ($($Matches[0])); is a MIB Studio or eGrabber program using the camera?"
            continue
        }
        $a = Get-Content (Join-Path $BaselineDir "base_$m.txt") | Where-Object { $_ -notmatch $ignore }
        $b = Get-Content (Join-Path $WorkDir "${Prefix}_$m.txt") | Where-Object { $_ -notmatch $ignore }
        $d = Compare-Object $a $b
        if ($d) {
            $script:Identical = $false
            Write-Output "DIFF $m (<= baseline, => now):"
            $d | ForEach-Object { Write-Output ('  {0} {1}' -f $_.SideIndicator, $_.InputObject.Trim()) }
        } else {
            Write-Output "OK   $m identical to baseline"
        }
    }
}

function Set-Features([string]$Module, [string[]]$Commands) {
    $out = ($Commands -join "`n") + "`nexit`n" | & $Gentl genapi --module=$Module --cli 2>&1 | Out-String
    $bad = ($out -split "`n") | Where-Object { $_ -match 'Failed|error|Error|exception|Invalid|locked' }
    if ($bad) { Write-Output "WARN $Module : $($bad -join ' | ')" }
}

if ($Action -eq 'Restore') {
    $busy = Get-Process -Name 'mib_studio_qt' -ErrorAction SilentlyContinue
    if ($busy -and -not $Force) {
        Write-Output ("REFUSED: mib_studio_qt.exe is running (pid {0}, started {1}, {2}). Close it first; restoring now would overwrite a live test. -Force overrides." -f $busy[0].Id, $busy[0].StartTime, $busy[0].Path)
        exit 2
    }
    Write-Output '--- AcquisitionStop (a killed MIB Studio can leave the camera acquiring)'
    & $Gentl genapi --execute AcquisitionStop 2>&1 | Select-Object -Last 2
    Write-Output '--- remote device: window and exposure'
    Set-Features 'remote' @('set OffsetX 0', 'set OffsetY 0', 'set Width 1920', 'set Height 1080', 'set ExposureTime 20')
    Write-Output '--- device module: cycle timing, strobe, cycle trigger source'
    Set-Features 'dev' @('set CycleMinimumPeriod 40000', 'set ExposureRecoveryTime 1000', 'set StrobeDelay 0', 'set StrobeDuration 1000', 'set CycleTriggerSource DEL1_1')
    Write-Output '--- interface module: TTLIO lines'
    Set-Features 'if' @('set LineSelector TTLIO11', 'set LineSource Low', 'set LineInverter false', 'set LineMode Input',
        'set LineSelector TTLIO12', 'set LineSource Low', 'set LineMode Input', 'set LineSelector DIN11')
}

Dump 'now'
Write-Output '--- compare with the saved baseline'
Compare-ToBaseline 'now'
if (-not $script:Identical) { Write-Output 'RESULT=different'; exit 1 }
if ($script:Unreadable.Count -gt 0) { Write-Output ('RESULT=incomplete (unreadable: {0}); the rest is identical' -f ($script:Unreadable -join ', ')); exit 3 }
Write-Output 'RESULT=identical'
exit 0
