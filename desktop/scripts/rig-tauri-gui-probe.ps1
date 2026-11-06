# Tauri rig preamble (#869): can this PowerShell session drive the Tauri GUI? Takes at most about 2 minutes.
# (a) Chrome DevTools Protocol on WebView2's debug port: read document.title, open and close the Help menu.
# (b) Otherwise UI Automation: find the "Help" button in the WebView2 accessibility tree.
# The app runs with the mock camera, so the probe never touches the camera. It always closes the app.
# The debug port listens on 127.0.0.1 only. Last line: "RESULT: CDP", "RESULT: UIA" or "RESULT: no Tauri GUI control".
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File rig-tauri-gui-probe.ps1 -Exe <dir>\mib-studio-desktop.exe
param([Parameter(Mandatory = $true)][string]$Exe, [int]$Port = 9333)
$ErrorActionPreference = 'Stop'
if (Get-Process mib-studio-desktop -ErrorAction SilentlyContinue) { throw 'mib-studio-desktop is already running: close it first (WebView2 would ignore the debug port)' }
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=$Port"
$env:MIB_CAMERA_MODE = 'mock'; $env:MIB_DISABLED_SERVICES = 'auto_update'
$app = Start-Process $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS, Env:MIB_CAMERA_MODE, Env:MIB_DISABLED_SERVICES
$result = 'no Tauri GUI control'; $ws = $null
try {
    try {
        $page = $null; $deadline = (Get-Date).AddSeconds(45)
        while (-not $page -and (Get-Date) -lt $deadline -and -not $app.HasExited) {
            try { $targets = Invoke-RestMethod "http://127.0.0.1:$Port/json" -TimeoutSec 3; $page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1 } catch { Start-Sleep 2 }
        }
        if (-not $page) { throw "nothing on http://127.0.0.1:$Port/json after 45 s" }
        Write-Output "cdp: page '$($page.title)' at $($page.url)"
        $ws = New-Object Net.WebSockets.ClientWebSocket; $ct = [Threading.CancellationToken]::None
        if (-not $ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, $ct).Wait(10000)) { throw 'websocket connect timed out' }
        $script:id = 0
        function Eval([string]$js) {
            $script:id++
            $msg = [Text.Encoding]::UTF8.GetBytes((@{ id = $script:id; method = 'Runtime.evaluate'; params = @{ expression = $js; returnByValue = $true } } | ConvertTo-Json -Compress))
            if (-not $ws.SendAsync((New-Object ArraySegment[byte] -ArgumentList (, $msg)), 'Text', $true, $ct).Wait(5000)) { throw 'send timed out' }
            $buf = New-Object byte[] 1048576
            while ($true) {
                $n = 0
                do {
                    $r = $ws.ReceiveAsync((New-Object ArraySegment[byte] -ArgumentList $buf, $n, ($buf.Length - $n)), $ct)
                    if (-not $r.Wait(10000)) { throw 'no reply' }
                    $n += $r.Result.Count
                } until ($r.Result.EndOfMessage)
                $reply = [Text.Encoding]::UTF8.GetString($buf, 0, $n) | ConvertFrom-Json
                if ($reply.id -eq $script:id) { return $reply.result.result.value }
            }
        }
        $help = '[...document.querySelectorAll("nav[aria-label=\"Main menu\"] button")].find(b => b.textContent.trim() === "Help")'
        $toggle = "(() => { const b = $help; if (!b) return 'no Help button'; b.click(); return 'clicked'; })()"
        $state = "(() => { const b = $help; return b ? b.getAttribute('aria-expanded') + '/' + !!document.querySelector('[role=menu]') : 'no Help button'; })()"
        $title = Eval 'document.title'
        $null = Eval $toggle; Start-Sleep -Milliseconds 500; $open = Eval $state
        $null = Eval $toggle; Start-Sleep -Milliseconds 500; $closed = Eval $state
        Write-Output "cdp: document.title '$title'; Help menu after click 1: $open; after click 2: $closed (want true/true, false/false)"
        if ($title -and $open -eq 'true/true' -and $closed -eq 'false/false') { $result = 'CDP' }
    } catch { Write-Output "cdp: failed: $($_.Exception.Message)" }
    if ($result -ne 'CDP' -and -not $app.HasExited) {
        try {
            Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
            $AE = [Windows.Automation.AutomationElement]; $btn = $null; $deadline = (Get-Date).AddSeconds(30)
            $cond = New-Object Windows.Automation.AndCondition -ArgumentList (New-Object Windows.Automation.PropertyCondition -ArgumentList $AE::ControlTypeProperty, ([Windows.Automation.ControlType]::Button)), (New-Object Windows.Automation.PropertyCondition -ArgumentList $AE::NameProperty, 'Help')
            while (-not $btn -and (Get-Date) -lt $deadline -and -not $app.HasExited) {
                $app.Refresh()
                if ($app.MainWindowHandle -ne [IntPtr]::Zero) { $btn = $AE::FromHandle($app.MainWindowHandle).FindFirst([Windows.Automation.TreeScope]::Descendants, $cond) }
                if (-not $btn) { Start-Sleep 2 }
            }
            if ($btn) { Write-Output "uia: found button '$($btn.Current.Name)' in the WebView2 tree"; $result = 'UIA' } else { Write-Output 'uia: no "Help" button in the window after 30 s' }
        } catch { Write-Output "uia: failed: $($_.Exception.Message)" }
    }
} finally {
    if ($ws) { $ws.Dispose() }
    if ($app.HasExited) { Write-Output "app: had exited by itself (exit code $($app.ExitCode))" }
    elseif ($app.CloseMainWindow() -and $app.WaitForExit(15000)) { Write-Output 'app: closed through its window' }
    else { Stop-Process -Id $app.Id -Force; Write-Output 'app: did not close within 15 s, killed' }
    Write-Output "RESULT: $result"
}
if ($result -eq 'no Tauri GUI control') { exit 2 }
