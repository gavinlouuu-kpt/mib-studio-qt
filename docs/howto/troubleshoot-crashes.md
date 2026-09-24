# Troubleshooting Application Crashes

This guide explains how to trace and diagnose crashes in the installed MIB Studio Qt application.

## Automatic crash capture (Sentry + local minidumps)

Starting with the crash-monitoring change (branch
`claude/crash-monitoring-logging-jUziR`), the app installs a process-level
crash handler at startup. Every unrecoverable failure — SEH access
violation, stack overflow, heap corruption, signal, uncaught C++
exception (`std::terminate`), Qt fatal — produces:

1. A Windows minidump (`.dmp`) capturing the faulting thread's stack and
   register state. For SEH faults with Sentry active, the dump is written
   and uploaded live by `crashpad_handler.exe`; for aborts and
   `std::terminate` (which Crashpad cannot see) the app writes the dump
   itself and submits it on the next launch.
2. A JSON sidecar (`.json`) with a snapshot of live service state
   (frame counts, queue depth, HDF5 open/path, autofocus port,
   recording mode, etc.).
3. For `std::terminate` reached via an unhandled exception, additionally a
   `.txt` file with the exception's `what()` message.

Artifacts land here:

```
%LOCALAPPDATA%\MIB_Studio_Qt\crashes\
    20260522T143015-pid12345-seh.dmp
    20260522T143015-pid12345-seh.json
```

When the installer was built with a Sentry DSN baked in (the system
env var `MIB_SENTRY_DSN` is set), live crashes are forwarded to Sentry by
the `crashpad_handler.exe` that lives next to the application. Pending
dumps from previous runs are submitted on the next launch via
`sentry_capture_minidump`, which attaches the actual `.dmp` binary to the
Sentry event for full stack-trace symbolication. Submitted dumps are
renamed from `.dmp` to `.dmp.queued`. The transport is only partially
durable: envelopes still waiting in the queue at shutdown are saved to
the Sentry database dir and re-sent on a later launch, but an envelope
whose send attempt fails outright (machine offline) is dropped. To cover
that window, a `.dmp.queued` file still present after 7 days is
re-submitted exactly once (tagged `crash_recovery: queued_retry`) and
renamed to `.dmp.queued2`, which is terminal. When Sentry is not active
(no DSN configured, or initialization failed), pending dumps are left in
place untouched and submitted on a later launch instead. Old
`.dmp.uploaded` files (from builds before this fix) are automatically
recovered to `.dmp` and re-submitted.

The crash directory is bounded: at most 50 (configurable) files are kept
per class — pending `.dmp`, queued `.dmp.queued`/`.queued2`, and orphan
`.json` sidecars — oldest deleted first, so a local-only install cannot
grow it without limit.

Quick checks:

```powershell
# Is the DSN configured for this install?
[System.Environment]::GetEnvironmentVariable('MIB_SENTRY_DSN','Machine')

# How many unsubmitted crashes are pending?
Get-ChildItem "$env:LOCALAPPDATA\MIB_Studio_Qt\crashes" -Filter *.dmp |
    Where-Object { $_.Name -notlike '*.queued' -and $_.Name -notlike '*.uploaded' } |
    Measure-Object

# How many have been queued for Sentry upload?
Get-ChildItem "$env:LOCALAPPDATA\MIB_Studio_Qt\crashes" -Filter *.dmp.queued |
    Measure-Object

# Inspect the JSON sidecar for the most recent crash
Get-ChildItem "$env:LOCALAPPDATA\MIB_Studio_Qt\crashes\*.json" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
    Get-Content | ConvertFrom-Json | Format-List
```

To configure / change the Sentry project, see
[sentry-setup.md](sentry-setup.md).

## Log File Location

The application writes logs to:
```
{InstallationDirectory}\data\logs\app.log
```

Default installation path:
```
C:\Program Files\MIB Studio Qt\data\logs\app.log
```

**Note:** The logger is configured to:
- Flush logs every 3 seconds automatically
- Use rotating file sink (max 10MB per file, 5 files)
- Flush on all log levels to ensure immediate writes

If logs don't appear, check:
1. File permissions on the `data\logs` directory
2. Antivirus software blocking file writes
3. Disk space availability

## Checking Log Files

1. **Navigate to the log directory:**
   ```powershell
   cd "C:\Program Files\MIB Studio Qt\data\logs"
   ```

2. **View the latest log entries:**
   ```powershell
   Get-Content app.log -Tail 100
   ```

3. **Search for errors:**
   ```powershell
   Select-String -Path app.log -Pattern "error|ERROR|exception|EXCEPTION|fatal|FATAL" -CaseSensitive:$false
   ```

## Windows Event Viewer

Windows Event Viewer can capture application crashes:

1. **Open Event Viewer:**
   - Press `Win + R`, type `eventvwr.msc`, press Enter
   - Or search "Event Viewer" in Start menu

2. **Navigate to Application Logs:**
   - Expand "Windows Logs"
   - Click "Application"

3. **Filter for MIB Studio Qt:**
   - In the right panel, click "Filter Current Log..."
   - In "Event sources", check "Application Error"
   - In "Includes/Excludes Event IDs", enter `1000` (Application Error)
   - Click OK

4. **Look for entries:**
   - Search for "mib_studio_qt.exe" or "MIB Studio Qt"
   - Check the timestamp around when the crash occurred
   - Note the "Fault Module" and "Exception Code"

## Windows Crash Dumps

### Enable Crash Dumps

1. **Enable Windows Error Reporting:**
   - Open Registry Editor (`regedit`)
   - Navigate to: `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps`
   - Create the key if it doesn't exist

2. **Configure dump settings:**
   - Create `DumpFolder` (REG_SZ): Set to `C:\CrashDumps` (or your preferred location)
   - Create `DumpType` (REG_DWORD): Set to `2` (Full dump) or `1` (Mini dump)
   - Create `DumpCount` (REG_DWORD): Set to `10` (number of dumps to keep)

3. **Alternative: Use WER (Windows Error Reporting) Settings:**
   - Control Panel → System and Security → Action Center
   - Click "Change Action Center settings"
   - Under "Problem reporting settings", ensure reporting is enabled

### Crash Dump Location

Crash dumps are typically stored in:
```
C:\Users\{Username}\AppData\Local\CrashDumps\
```

Or the custom location you configured in Registry.

## Application-Specific Debugging

### Enable Debug Logging

The application uses spdlog with INFO level by default. To enable DEBUG level logging:

1. **Modify the logger initialization** (requires rebuild):
   - Edit `src/backend/services/Logger.cpp`
   - Change `s_logger->set_level(spdlog::level::info);` to `s_logger->set_level(spdlog::level::debug);`

2. **Or set at runtime** (if code supports it):
   - Check if environment variable `MIB_LOG_LEVEL=debug` is supported

### Common Crash Scenarios

1. **Missing DLLs:**
   - Check if all Qt DLLs are in the installation directory
   - Verify `platforms\qwindows.dll` exists
   - Check for missing dependencies using Dependency Walker or `dumpbin /dependents`

2. **Camera/EGrabber Issues:**
   - Check if eGrabber SDK is installed
   - Verify camera hardware is connected
   - Check logs for eGrabber initialization errors

3. **Memory Issues:**
   - Check logs for memory-related errors
   - Look for "out of memory" or allocation failures
   - Review `mem_mb` and `peak_mb` values in logs

4. **File Access Issues:**
   - Verify write permissions to `data` directory
   - Check if antivirus is blocking file access
   - Ensure HDF5 files can be created/written

## Running from Command Line

To capture console output and errors:

1. **Open Command Prompt or PowerShell as Administrator**

2. **Navigate to installation directory:**
   ```powershell
   cd "C:\Program Files\MIB Studio Qt"
   ```

3. **Run the application:**
   ```powershell
   .\mib_studio_qt.exe
   ```

4. **Capture output to file:**
   ```powershell
   .\mib_studio_qt.exe 2>&1 | Tee-Object -FilePath crash_output.txt
   ```

## Using Process Monitor

Process Monitor (ProcMon) can trace file, registry, and network access:

1. **Download Process Monitor:**
   - https://learn.microsoft.com/en-us/sysinternals/downloads/procmon

2. **Configure filters:**
   - Process Name is `mib_studio_qt.exe`
   - Operation is `WriteFile` or `CreateFile` (for file access issues)

3. **Run the application and reproduce the crash**

4. **Review the trace** for:
   - Access denied errors
   - Missing files
   - Registry access issues

## Debugging with Visual Studio

If you have the source code and Visual Studio:

1. **Attach debugger to running process:**
   - Debug → Attach to Process
   - Select `mib_studio_qt.exe`
   - Set breakpoints in critical sections

2. **Load crash dump:**
   - File → Open → File
   - Select the `.dmp` file
   - Debug → Windows → Call Stack to see crash location

## Quick Diagnostic Checklist

- [ ] Check `data\logs\app.log` for error messages
- [ ] Check Windows Event Viewer for Application Errors
- [ ] Verify all DLLs are present in installation directory
- [ ] Check crash dump location for `.dmp` files
- [ ] Run from command line to capture console output
- [ ] Verify eGrabber SDK installation (if using hardware camera)
- [ ] Check file permissions on `data` directory
- [ ] Review system resources (memory, disk space)

## Reporting Crashes

When reporting a crash, include:

1. **Log file excerpt** (last 50-100 lines from `app.log`)
2. **Event Viewer details** (Application Error entry)
3. **Crash dump file** (if available)
4. **System information:**
   - Windows version
   - Installed eGrabber version
   - Camera hardware model
5. **Steps to reproduce** (if known)
6. **What you were doing** when the crash occurred

