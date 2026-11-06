# Tauri rig preamble (#869): can this PowerShell session drive the Tauri GUI? Takes at most about 2 minutes.
# (a) Chrome DevTools Protocol on WebView2's debug port: take only this app's page (origin tauri.localhost), wait for
#     the menu bar to render (up to 60 s after launch), read document.title, open and close the Help menu.
# (b) Otherwise UI Automation: look for a button named "Help" in the app window, bounded to 30 s. RESULT: UIA only proves
#     that UIA sees that button in the WebView2 tree; it does not prove that Invoke on it works.
# The app runs with the mock camera, so the probe never touches the camera. It always closes the app.
# The debug port listens on 127.0.0.1 only; the probe refuses to start if something already listens there.
# Last line: "RESULT: CDP", "RESULT: UIA" or "RESULT: no Tauri GUI control" (exit code 2).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File rig-tauri-gui-probe.ps1 -Exe <dir>\mib-studio-desktop.exe
param([Parameter(Mandatory = $true)][string]$Exe, [int]$Port = 9333)
$ErrorActionPreference = 'Stop'
if (Get-Process mib-studio-desktop -ErrorAction SilentlyContinue) { throw 'mib-studio-desktop is already running: close it first (WebView2 would ignore the debug port)' }
$tcp = New-Object Net.Sockets.TcpClient; $busy = $true
try { $tcp.Connect('127.0.0.1', $Port) } catch { $busy = $false } finally { $tcp.Close() }
if ($busy) { throw "something already listens on 127.0.0.1:${Port}: close it, or pass -Port <a free port>" }
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=$Port"
$env:MIB_CAMERA_MODE = 'mock'; $env:MIB_DISABLED_SERVICES = 'auto_update'
$app = Start-Process $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS, Env:MIB_CAMERA_MODE, Env:MIB_DISABLED_SERVICES
$result = 'no Tauri GUI control'; $ws = $null; $cdpDeadline = (Get-Date).AddSeconds(60)
$appOrigin = '^https?://tauri\.localhost(/|$)'
try {
    try {
        $page = $null; $others = @()
        while (-not $page -and (Get-Date) -lt $cdpDeadline -and -not $app.HasExited) {
            try {
                $pages = @(Invoke-RestMethod "http://127.0.0.1:$Port/json" -TimeoutSec 3 | ForEach-Object { $_ } | Where-Object { $_.type -eq 'page' })
                $page = $pages | Where-Object { $_.url -match $appOrigin } | Select-Object -First 1
                $others = @($pages | Where-Object { $_.url -notmatch $appOrigin } | ForEach-Object { $_.url })
            } catch { }
            if (-not $page) { Start-Sleep 2 }
        }
        if (-not $page) { throw "no tauri.localhost page on 127.0.0.1:$Port within 60 s of launch (other pages: $($others -join ', '))" }
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
        # The page target exists before React renders the menu bar: wait for the Help button first.
        while ((Eval $state) -eq 'no Help button' -and (Get-Date) -lt $cdpDeadline) { Start-Sleep -Milliseconds 500 }
        $title = Eval 'document.title'
        $null = Eval $toggle; Start-Sleep -Milliseconds 500; $open = Eval $state
        $null = Eval $toggle; Start-Sleep -Milliseconds 500; $closed = Eval $state
        Write-Output "cdp: document.title '$title'; Help menu after click 1: $open; after click 2: $closed (want true/true, false/false)"
        if ($title -and $open -eq 'true/true' -and $closed -eq 'false/false') { $result = 'CDP' }
    } catch { Write-Output "cdp: failed: $($_.Exception.Message)" }
    if ($result -ne 'CDP' -and -not $app.HasExited) {
        # FindFirst blocks while the app's UI thread hangs, so it runs in its own runspace and is waited for 30 s at most.
        $uia = [PowerShell]::Create().AddScript({
            param($procId)
            Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
            $AE = [Windows.Automation.AutomationElement]; $p = Get-Process -Id $procId; $deadline = (Get-Date).AddSeconds(28)
            $cond = New-Object Windows.Automation.AndCondition -ArgumentList (New-Object Windows.Automation.PropertyCondition -ArgumentList $AE::ControlTypeProperty, ([Windows.Automation.ControlType]::Button)), (New-Object Windows.Automation.PropertyCondition -ArgumentList $AE::NameProperty, 'Help')
            while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
                $p.Refresh()
                if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $b = $AE::FromHandle($p.MainWindowHandle).FindFirst([Windows.Automation.TreeScope]::Descendants, $cond); if ($b) { return $b.Current.Name } }
                Start-Sleep 2
            }
        }).AddArgument($app.Id)
        $async = $uia.BeginInvoke()
        if (-not $async.AsyncWaitHandle.WaitOne(30000)) { Write-Output 'uia: no answer within 30 s (FindFirst blocked; the UI thread may be hung)' }
        else {
            $name = @($uia.EndInvoke($async)) | Select-Object -First 1
            if ($uia.Streams.Error.Count) { Write-Output "uia: error: $($uia.Streams.Error[0])" }
            if ($name) { Write-Output "uia: found button '$name' in the WebView2 tree (Invoke not tried)"; $result = 'UIA' } else { Write-Output 'uia: no "Help" button in the window within 30 s' }
        }
    }
} finally {
    if ($ws) { $ws.Dispose() }
    if ($app.HasExited) { Write-Output "app: had exited by itself (exit code $($app.ExitCode))" }
    elseif ($app.CloseMainWindow() -and $app.WaitForExit(15000)) { Write-Output 'app: closed through its window' }
    else { Stop-Process -Id $app.Id -Force; Write-Output 'app: did not close within 15 s, killed' }
    Write-Output "RESULT: $result"
}
if ($result -eq 'no Tauri GUI control') { exit 2 }
