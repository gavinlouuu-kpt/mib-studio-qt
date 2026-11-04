<#
.SYNOPSIS
  Windows side of scripts/win11_bench_build.sh: build one branch of
  mib-studio-qt on the Windows bench PC (win11-agent) with the real eGrabber SDK.

.DESCRIPTION
  Not meant to be run by hand; scripts/win11_bench_build.sh copies it to
  C:\MIBBuild\bin and drives it over ssh. Everything lives under $Root
  (default C:\MIBBuild; override with the MIB_BENCH_ROOT environment variable):

    repo.git     bare repo that P5 pushes branches to (no credentials involved)
    src          checkout of the branch under test; build\ inside it is the
                 CMake/Ninja tree, build\Release\mib_studio_qt.exe the result
    conan        CONAN_HOME: the Conan cache, kept between builds so a rebuild
                 is incremental
    tools        user-scoped Python 3.12 with conan, ninja and numpy
    logs         <run id>.log and <run id>.state.json per build
    bin          this script

  Actions
    Setup    Idempotent. Checks Visual Studio 2022 and the eGrabber SDK, installs
             the user-scoped Python + conan + ninja when missing, creates the
             bare repo and the Conan profile/config. Changes nothing system-wide.
    Launch   Starts a detached 'Run' (it must outlive the ssh session) and
             prints RUN_ID=<id>. Prints BUSY=<id> and exits 3 when a build runs.
    Run      The build itself: checkout -> conan install -> cmake configure ->
             cmake build -> verify -> smoke start. Writes the log and state file.
    Status   Prints STATE= / STEP= / EXE= / LOG= ... lines and the log tail.
    Smoke    Starts the built exe for a few seconds, checks app.log, closes it.

  The CMake configure matches .github/workflows/build-windows.yml (preset
  windows-ninja-ci, Release, hardware SDKs on) except for the deviations listed
  in docs/howto/win11-bench-build.md: MindVision off, no Sentry DSN, no release
  version override, no installer packaging.
#>
[CmdletBinding()]
param(
    [ValidateSet('Setup', 'Launch', 'Run', 'Status', 'Smoke')]
    [string]$Action = 'Status',
    [string]$Branch = '',
    [string]$Sha = '',
    [string]$RunId = '',
    [string]$Target = 'mib_studio_qt',
    [int]$Jobs = 12,
    [int]$Tail = 0,
    [switch]$Clean,
    [switch]$SkipSmoke
)

$ErrorActionPreference = 'Stop'
$Root = if ($env:MIB_BENCH_ROOT) { $env:MIB_BENCH_ROOT } else { 'C:\MIBBuild' }
$Repo = Join-Path $Root 'repo.git'
$Src = Join-Path $Root 'src'
$LogDir = Join-Path $Root 'logs'
$ToolsDir = Join-Path $Root 'tools'
$PyDir = Join-Path $ToolsDir 'python312'
$ConanHome = Join-Path $Root 'conan'
$Script = $MyInvocation.MyCommand.Path
$ExeRel = 'build\Release\mib_studio_qt.exe'
$PythonUrl = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe'

$script:LogFile = $null
$script:StateFile = $null
$script:State = [ordered]@{}
$script:Steps = @()
$script:Echo = $false

function Log([string]$Message) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    if ($script:LogFile) { Add-Content -LiteralPath $script:LogFile -Value $line }
    if ($script:Echo) { Write-Host $line }
}

function Set-State([string]$Key, $Value) {
    $script:State[$Key] = $Value
    if ($script:StateFile) {
        ($script:State | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath $script:StateFile -Encoding ASCII
    }
}

function Read-State([string]$Id) {
    $file = Join-Path $LogDir "$Id.state.json"
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    return (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json)
}

function Get-LatestRunId {
    $latest = Join-Path $LogDir 'latest.txt'
    if (Test-Path -LiteralPath $latest) { return (Get-Content -LiteralPath $latest -Raw).Trim() }
    return ''
}

function Test-RunAlive($State) {
    if (-not $State -or -not $State.pid) { return $false }
    $p = Get-Process -Id ([int]$State.pid) -ErrorAction SilentlyContinue
    return [bool]($p -and $p.ProcessName -like 'powershell*')
}

# Runs a command line through cmd.exe so its stdout+stderr go straight into the
# run log (no PowerShell stderr-to-ErrorRecord conversion, no encoding games).
function Invoke-Logged([string]$Name, [string]$CmdLine, [string]$WorkDir = $Src) {
    Log "> $CmdLine"
    Push-Location -LiteralPath $WorkDir
    try {
        & cmd.exe /d /s /c ('"{0} >> "{1}" 2>&1"' -f $CmdLine, $script:LogFile)
        $rc = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($rc -ne 0) { throw "$Name failed with exit code $rc (see log)" }
}

function Invoke-Step([string]$Name, [scriptblock]$Body) {
    Set-State 'step' $Name
    Log "=== $Name ==="
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Body
    $sw.Stop()
    $script:Steps += @{ name = $Name; seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
    Set-State 'steps' $script:Steps
    Log ('--- {0} done in {1:N1}s' -f $Name, $sw.Elapsed.TotalSeconds)
}

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            $vc = Join-Path ($install | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $vc) { return $vc }
        }
    }
    $fallback = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $fallback) { return $fallback }
    throw 'Visual Studio 2022 with the C++ x64 toolset (vcvars64.bat) was not found'
}

# Same effect as ilammy/msvc-dev-cmd in CI: import the vcvars64 environment.
function Import-VsEnvironment {
    $vcvars = Find-VcVars
    $dump = & cmd.exe /d /c ('"{0}" >nul 2>&1 && set' -f $vcvars)
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

function Use-BenchEnvironment {
    Import-VsEnvironment
    $env:Path = "$PyDir;$PyDir\Scripts;$env:Path"
    $env:CONAN_HOME = $ConanHome
    $env:PYTHONUTF8 = '1'
    $env:PYTHONIOENCODING = 'utf-8'
    $env:CLICOLOR = '0'
    $env:NO_COLOR = '1'
}

function Invoke-Quiet([string]$Exe, [string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>&1
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    if ($rc -ne 0) { throw "$Exe $($Arguments -join ' ') failed ($rc): $($out | Out-String)" }
    return $out
}

# ---------------------------------------------------------------- Setup
function Invoke-Setup {
    $script:Echo = $true
    Write-Host "win11 bench build setup, root $Root"
    New-Item -ItemType Directory -Force -Path $Root, $LogDir, $ToolsDir, (Join-Path $Root 'bin'), (Join-Path $Root 'dl') | Out-Null

    $null = Find-VcVars
    Write-Host '  OK  Visual Studio 2022 C++ toolset'
    $egrabber = 'C:\Program Files\Euresys\eGrabber\include\EGrabber.h'
    if (-not (Test-Path -LiteralPath $egrabber)) {
        throw "Euresys eGrabber SDK headers not found ($egrabber). Install the SDK (build-windows.yml pins egrabber-win-x86_64-25.10.0.57) first."
    }
    Write-Host '  OK  Euresys eGrabber SDK headers'
    foreach ($tool in 'git', 'cmake') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not on PATH" }
    }
    $free = [math]::Round((Get-PSDrive -Name ($Root.Substring(0, 1))).Free / 1GB, 1)
    Write-Host "  OK  git, cmake; $free GB free on $($Root.Substring(0, 2)) (checkout, build tree and Conan cache take about 3 GB)"
    if ($free -lt 10) { throw "Only $free GB free; the checkout, build tree and Conan cache take about 3 GB" }

    $py = Join-Path $PyDir 'python.exe'
    if (-not (Test-Path -LiteralPath $py)) {
        $installer = Join-Path $Root 'dl\python-3.12.10-amd64.exe'
        Write-Host "  ... downloading $PythonUrl"
        Invoke-WebRequest -Uri $PythonUrl -OutFile $installer -UseBasicParsing
        $sig = Get-AuthenticodeSignature -LiteralPath $installer
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Python Software Foundation') {
            throw "Python installer signature check failed: $($sig.Status) $($sig.SignerCertificate.Subject)"
        }
        Write-Host '  ... installing Python 3.12.10 for this user (no PATH change, no launcher, no file associations)'
        $p = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
            '/quiet', 'InstallAllUsers=0', "TargetDir=$PyDir", 'PrependPath=0', 'Include_launcher=0',
            'InstallLauncherAllUsers=0', 'Include_test=0', 'Include_doc=0', 'Shortcuts=0', 'AssociateFiles=0')
        if ($p.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $py)) { throw "Python installer failed (exit $($p.ExitCode))" }
    }
    Write-Host ('  OK  ' + (Invoke-Quiet $py @('--version')))

    # Same Conan 2 / numpy requirement as env/requirements-build.txt, plus pip's ninja.
    $null = Invoke-Quiet $py @('-m', 'pip', 'install', '--disable-pip-version-check', '--quiet', '--no-warn-script-location', 'conan>=2.0,<3', 'numpy>=1.24', 'ninja')
    Write-Host ('  OK  ' + (Invoke-Quiet (Join-Path $PyDir 'Scripts\conan.exe') @('--version')))
    Write-Host ('  OK  ninja ' + (Invoke-Quiet (Join-Path $PyDir 'Scripts\ninja.exe') @('--version')))

    if (-not (Test-Path -LiteralPath $Repo)) {
        $null = Invoke-Quiet 'git' @('init', '--bare', '--quiet', $Repo)
        Write-Host "  OK  created bare repo $Repo"
    } else {
        Write-Host "  OK  bare repo $Repo"
    }

    $env:CONAN_HOME = $ConanHome
    $conan = Join-Path $PyDir 'Scripts\conan.exe'
    $defaultProfile = Join-Path $ConanHome 'profiles\default'
    if (-not (Test-Path -LiteralPath $defaultProfile)) {
        $null = Invoke-Quiet $conan @('profile', 'detect', '--force')
        Write-Host '  OK  conan default profile detected'
    }
    # The team Conan remote (https://conan.yofo.bio, scripts/setup-conan-remote.ps1) serves
    # the same prebuilt Qt/OpenCV/ffmpeg binaries CI uses and allows anonymous reads. No
    # login is ever configured here and nothing is uploaded from this machine. If the
    # remote ever stops answering anonymously, remove it (conan remote remove team-conan)
    # and the build falls back to compiling everything from conancenter.
    $remotes = & $conan remote list 2>&1 | Out-String
    if ($remotes -notmatch 'team-conan') {
        $null = Invoke-Quiet $conan @('remote', 'add', 'team-conan', 'https://conan.yofo.bio', '--index', '0')
        Write-Host '  OK  added read-only Conan remote team-conan (anonymous, no credentials)'
    } else {
        Write-Host '  OK  Conan remote team-conan'
    }
    # Conan writes a commented template on first use, so test for our lines, not the file.
    $globalConf = Join-Path $ConanHome 'global.conf'
    $confText = if (Test-Path -LiteralPath $globalConf) { Get-Content -LiteralPath $globalConf -Raw } else { '' }
    if ($confText -notmatch 'core\.download:retry') {
        Add-Content -LiteralPath $globalConf -Encoding ASCII -Value @(
            ''
            '# win11_bench_build.ps1: ride out flaky downloads of the many source archives'
            'core.download:retry=5'
            'core.download:retry_wait=10'
            'core.net.http:max_retries=5'
        )
        Write-Host "  OK  added download retry settings to $globalConf"
    }
    Write-Host 'setup complete'
}

# ---------------------------------------------------------------- Launch / Status
function Invoke-Launch {
    if (-not $Sha -or $Sha -notmatch '^[0-9a-f]{40}$') { throw 'Launch needs -Sha <40 hex>' }
    if ($Branch -notmatch '^[A-Za-z0-9._/+@-]+$') { throw "Unsafe branch name '$Branch'" }
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $prev = Get-LatestRunId
    if ($prev) {
        $st = Read-State $prev
        if ($st -and $st.state -eq 'running' -and (Test-RunAlive $st)) {
            Write-Output "BUSY=$prev"
            exit 3
        }
    }
    $id = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Sha.Substring(0, 7)
    Set-Content -LiteralPath (Join-Path $LogDir 'latest.txt') -Value $id -Encoding ASCII
    $cmdArgs = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Action Run -RunId {1} -Sha {2} -Branch "{3}" -Target {4} -Jobs {5}' -f $Script, $id, $Sha, $Branch, $Target, $Jobs
    if ($Clean) { $cmdArgs += ' -Clean' }
    if ($SkipSmoke) { $cmdArgs += ' -SkipSmoke' }
    # A process started with Start-Process dies with the ssh session; one created
    # through WMI belongs to no session job and survives it.
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine      = "powershell.exe $cmdArgs"
        CurrentDirectory = $Root
    }
    if ($r.ReturnValue -ne 0) { throw "Could not start the detached build (Win32_Process.Create returned $($r.ReturnValue))" }
    # The worker writes its own state; seed one so Status works before it starts.
    $seed = [ordered]@{ run = $id; state = 'running'; step = 'starting'; branch = $Branch; sha = $Sha; pid = [int]$r.ProcessId; started = (Get-Date).ToString('s') }
    ($seed | ConvertTo-Json) | Set-Content -LiteralPath (Join-Path $LogDir "$id.state.json") -Encoding ASCII
    Write-Output "RUN_ID=$id"
}

function Invoke-Status {
    if (-not $RunId) { $RunId = Get-LatestRunId }
    if (-not $RunId) { Write-Output 'STATE=none'; return }
    $st = Read-State $RunId
    if (-not $st) { Write-Output 'STATE=none'; return }
    $state = [string]$st.state
    $err = [string]$st.error
    if ($state -eq 'running' -and -not (Test-RunAlive $st)) {
        $state = 'failed'
        $err = 'the build process died without finishing (reboot, kill, or crash); see the log'
    }
    $log = Join-Path $LogDir "$RunId.log"
    Write-Output "RUN=$RunId"
    Write-Output "STATE=$state"
    Write-Output "STEP=$($st.step)"
    Write-Output "BRANCH=$($st.branch)"
    Write-Output "SHA=$($st.sha)"
    Write-Output "STARTED=$($st.started)"
    Write-Output "ELAPSED=$($st.elapsed_s)"
    Write-Output "EXE=$($st.exe)"
    Write-Output "SMOKE=$($st.smoke)"
    Write-Output "ERROR=$err"
    Write-Output "LOG=$log"
    if ($st.steps) {
        $summary = ($st.steps | ForEach-Object { '{0}={1}s' -f $_.name, $_.seconds }) -join ' '
        Write-Output "STEPS=$summary"
    }
    if ($Tail -gt 0 -and (Test-Path -LiteralPath $log)) {
        Write-Output '--- log tail ---'
        Get-Content -LiteralPath $log -Tail $Tail
    }
}

# ---------------------------------------------------------------- Smoke
# Starts the freshly built GUI for a few seconds and checks that it reaches the
# event loop and writes data\logs\app.log. It must never touch the camera:
# MIB Studio selects a lone Coaxlink camera by itself at start-up, writes the
# Overview script (overviewConfig.js: TTLIO line modes, strobe, cycle timing,
# window) to it and starts acquisition. The smoke start therefore points the
# eGrabber SDK at a GenTL producer for hardware this PC does not have
# (EURESYS_DEFAULT_GENTL_PRODUCER=grablink, documented in EGenTL.h; only this
# child process sees it) and kills the app at once if the log ever shows the
# camera being selected or configured. Updates are disabled too
# (MIB_DISABLED_SERVICES=auto_update) so the test build cannot fetch a release.
$script:CloseSource = @'
using System;
using System.Runtime.InteropServices;
public static class BenchWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static int PostClose(uint pid) {
        int n = 0;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) { PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); n++; } return true; }, IntPtr.Zero);
        return n;
    }
}
'@

function Invoke-Smoke([string]$ExePath, [int]$HoldSeconds = 15) {
    if (-not (Test-Path -LiteralPath $ExePath)) { return 'fail: exe missing' }
    $running = Get-Process -Name 'mib_studio_qt' -ErrorAction SilentlyContinue
    if ($running) {
        return 'skipped: another mib_studio_qt.exe is running (desktop.lock would block a second instance)'
    }
    if (-not ('BenchWin' -as [type])) { Add-Type -TypeDefinition $script:CloseSource }
    $appLog = Join-Path (Split-Path -Parent $ExePath) 'data\logs\app.log'
    # The logger rotates app.log at start-up; move the old one aside so the file read
    # below holds this start only.
    if (Test-Path -LiteralPath $appLog) { Move-Item -LiteralPath $appLog -Destination ($appLog + '.before-smoke') -Force }
    $tripwire = 'Hardware camera selected|Applying camera script|Applying script to camera|EGrabberCamera|Coaxlink|PC3603|CaptureService starting'

    $env:MIB_DISABLED_SERVICES = 'auto_update'
    $env:EURESYS_DEFAULT_GENTL_PRODUCER = 'grablink'
    $p = Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path -Parent $ExePath) -PassThru
    Remove-Item Env:\EURESYS_DEFAULT_GENTL_PRODUCER -ErrorAction SilentlyContinue
    Log "smoke: started pid $($p.Id) with the Coaxlink producer masked, holding $HoldSeconds s"

    function Read-NewLog {
        if (-not (Test-Path -LiteralPath $appLog)) { return @() }
        $fs = [IO.File]::Open($appLog, 'Open', 'Read', 'ReadWrite')
        try {
            $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
        } finally { $fs.Dispose() }
        return @($text -split "`r?`n" | Where-Object { $_ })
    }

    $deadline = (Get-Date).AddSeconds($HoldSeconds)
    $tripped = $null
    while ((Get-Date) -lt $deadline -and -not $p.HasExited -and -not $tripped) {
        Start-Sleep -Milliseconds 250
        $tripped = Read-NewLog | Where-Object { $_ -match $tripwire } | Select-Object -First 1
    }
    if ($tripped) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Log "smoke: TRIPWIRE, the app reached for the camera: $tripped"
        return 'fail: the smoke start touched the camera (killed at once); check the bench camera state with scripts/win11_bench_camera.ps1 -Action Check'
    }
    if ($p.HasExited) { return "fail: exited early with code $($p.ExitCode)" }

    $posted = [BenchWin]::PostClose([uint32]$p.Id)
    $closed = $p.WaitForExit(5000)
    Log "smoke: WM_CLOSE posted to $posted window(s); exited=$closed"
    if (-not $closed) { Log 'smoke: still running 5 s after WM_CLOSE (session 0 windows are not interactive), killing'; Stop-Process -Id $p.Id -Force }
    $lines = Read-NewLog
    foreach ($l in ($lines | Select-Object -First 14)) { Log "app.log: $l" }
    $late = $lines | Where-Object { $_ -match $tripwire } | Select-Object -First 1
    if ($late) { return "fail: camera activity in the log: $late" }
    if (-not ($lines | Where-Object { $_ -match 'Logger initialized' })) { return 'fail: no "Logger initialized" line in data\logs\app.log' }
    $egrabber = @($lines | Where-Object { $_ -match "provider 'egrabber'" }).Count
    return ('ok: ran {0} s with the Coaxlink producer masked, {1} app.log lines, egrabber discovery provider registered={2}, camera not touched; WM_CLOSE honoured={3} (killed otherwise)' -f $HoldSeconds, $lines.Count, ($egrabber -gt 0), $closed)
}

# ---------------------------------------------------------------- Run
function Invoke-Run {
    if (-not $RunId) { throw 'Run needs -RunId' }
    if ($Sha -notmatch '^[0-9a-f]{40}$') { throw 'Run needs -Sha <40 hex>' }
    if ($Branch -notmatch '^[A-Za-z0-9._/+@-]+$') { throw "Unsafe branch name '$Branch'" }
    if ($Target -notmatch '^[A-Za-z0-9_.+-]+$') { throw "Unsafe target name '$Target'" }
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $script:LogFile = Join-Path $LogDir "$RunId.log"
    $script:StateFile = Join-Path $LogDir "$RunId.state.json"
    $total = [Diagnostics.Stopwatch]::StartNew()

    # Keep the bench PC usable for the camera software while a build runs.
    try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch { }

    Set-State 'run' $RunId
    Set-State 'state' 'running'
    Set-State 'branch' $Branch
    Set-State 'sha' $Sha
    Set-State 'pid' $PID
    Set-State 'started' ((Get-Date).ToString('s'))
    Set-State 'step' 'starting'
    Log "win11 bench build: branch=$Branch sha=$Sha target=$Target clean=$($Clean.IsPresent) jobs=$Jobs"

    try {
        Invoke-Step 'environment' {
            Use-BenchEnvironment
            foreach ($t in 'cl', 'cmake', 'ninja', 'conan', 'git') {
                $cmd = Get-Command $t -ErrorAction SilentlyContinue
                if (-not $cmd) { throw "$t not found after setting up the build environment (run -Action Setup)" }
                Log ("{0} -> {1}" -f $t, $cmd.Source)
            }
            Invoke-Logged 'cmake --version' 'cmake --version' $Root
            Invoke-Logged 'ninja --version' 'ninja --version' $Root
            Invoke-Logged 'conan --version' 'conan --version' $Root
        }

        Invoke-Step 'checkout' {
            if (-not (Test-Path -LiteralPath (Join-Path $Src '.git'))) {
                Invoke-Logged 'git clone' ('git clone --quiet --no-checkout "{0}" "{1}"' -f $Repo, $Src) $Root
            }
            Invoke-Logged 'git fetch' 'git fetch --quiet --force --tags origin "+refs/heads/*:refs/remotes/origin/*"'
            Invoke-Logged 'git checkout' ('git checkout --quiet --force --detach {0}' -f $Sha)
            Invoke-Logged 'git clean' 'git clean -fdq'
            $head = (& git -C $Src rev-parse HEAD).Trim()
            if ($head -ne $Sha) { throw "checkout is at $head, expected $Sha" }
            Log "source at $head"
        }

        if ($Clean) {
            Invoke-Step 'clean' {
                $build = Join-Path $Src 'build'
                if (Test-Path -LiteralPath $build) {
                    # cmd rmdir copes with the long paths a Qt/CMake tree can have.
                    Invoke-Logged 'remove build dir' ('rmdir /s /q "{0}"' -f $build) $Root
                }
            }
        }

        Invoke-Step 'conan-install' {
            Invoke-Logged 'conan install' ('conan install . -of build --build=missing -s build_type=Release -pr conan/profiles/windows-msvc194-ninja -c tools.build:jobs={0}' -f $Jobs)
        }

        Invoke-Step 'cmake-configure' {
            Invoke-Logged 'cmake configure' 'cmake --preset windows-ninja-ci -DMIB_ENABLE_HARDWARE_SDKS=ON -DMIB_ENABLE_MINDVISION=OFF'
            $hit = Select-String -LiteralPath $script:LogFile -Pattern 'EGrabber SDK integration: ON' -Quiet
            if (-not $hit) { throw 'configure did not report "EGrabber SDK integration: ON"; this build would not contain the eGrabber code' }
            Log 'configure reports EGrabber SDK integration: ON'
        }

        Invoke-Step 'cmake-build' {
            Invoke-Logged 'cmake build' ('cmake --build build --config Release --target {0} -j {1}' -f $Target, $Jobs)
        }

        $exe = Join-Path $Src $ExeRel
        Invoke-Step 'verify' {
            if ($Target -eq 'mib_studio_qt' -or $Target -eq 'all') {
                foreach ($rel in 'build\Release\mib_studio_qt.exe', 'build\Release\platforms\qwindows.dll') {
                    if (-not (Test-Path -LiteralPath (Join-Path $Src $rel))) { throw "expected build output missing: $rel" }
                }
                $info = Get-Item -LiteralPath $exe
                Log ('exe {0} bytes, written {1}' -f $info.Length, $info.LastWriteTime)
                @(
                    "branch=$Branch"
                    "sha=$Sha"
                    "built=$((Get-Date).ToString('s'))"
                    "target=$Target"
                    'features=MIB_HAS_EGRABBER=1 MIB_HAS_MINDVISION=0'
                    "run=$RunId"
                ) | Set-Content -LiteralPath (Join-Path $Src 'build\Release\BENCH_BUILD.txt') -Encoding ASCII
                Set-State 'exe' $exe
            } else {
                Log "target '$Target' built; no GUI exe expected, skipping the exe checks"
            }
        }

        if ($Target -eq 'mib_studio_qt' -or $Target -eq 'all') {
            if ($SkipSmoke) {
                Set-State 'smoke' 'skipped by request'
            } else {
                Invoke-Step 'smoke' {
                    $result = Invoke-Smoke $exe
                    Log "smoke: $result"
                    Set-State 'smoke' $result
                    if ($result -like 'fail:*') { throw "smoke start failed: $result" }
                }
            }
        }

        $total.Stop()
        Set-State 'elapsed_s' ([math]::Round($total.Elapsed.TotalSeconds, 0))
        Set-State 'state' 'ok'
        Set-State 'step' 'done'
        Log ('BUILD OK in {0:N0}s' -f $total.Elapsed.TotalSeconds)
    } catch {
        $total.Stop()
        Log "FAILED at step '$($script:State['step'])': $($_.Exception.Message)"
        Set-State 'elapsed_s' ([math]::Round($total.Elapsed.TotalSeconds, 0))
        Set-State 'error' ("step '$($script:State['step'])': " + $_.Exception.Message)
        Set-State 'state' 'failed'
        exit 1
    }
}

switch ($Action) {
    'Setup'  { Invoke-Setup }
    'Launch' { Invoke-Launch }
    'Run'    { Invoke-Run }
    'Status' { Invoke-Status }
    'Smoke'  {
        $script:Echo = $true
        $script:LogFile = $null
        Write-Host (Invoke-Smoke (Join-Path $Src $ExeRel))
    }
}
