param([Parameter(Mandatory = $true)][string]$Resource, [string]$Out = "gate.txt",
      [string]$Python = "C:\Users\ERBG07\miniconda3\python.exe")
# Sets rf_generator.resource (enabled) in the config MIB Studio is watching,
# presses Start Experiment in the running app and captures the readiness
# dialog (if any) to $Out. No dialog = every gate passed and the file dialog
# opened: cancel it, or pick a destination to start the run.
# Use -Resource EMPTY for an empty address.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfg = (Get-ItemProperty "HKCU:\Software\MIB Studio\MIB Studio Qt\Config").ExternalAppConfigPath
if (-not $cfg) { throw "no external (profile) config active; edit <app>/include/config.json instead" }
$uia = Join-Path $here "uia.ps1"
& $Python (Join-Path $here "set_rf_generator.py") $cfg true $Resource
Start-Sleep -Seconds 7   # config reload + the service's 5 s reconnect back-off
Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$uia`" -Action click -Name `"Start Experiment`"" -WindowStyle Hidden
Start-Sleep -Seconds 6
$wins = powershell -NoProfile -ExecutionPolicy Bypass -File $uia -Action windows
$wins
if ($wins -match "Window \| Start Experiment") {
    powershell -NoProfile -ExecutionPolicy Bypass -File $uia -Action text -Window "Start Experiment" | Tee-Object (Join-Path $here $Out)
    powershell -NoProfile -ExecutionPolicy Bypass -File $uia -Action click -Window "Start Experiment" -Name "Close"
}
