; InnoSetup installer script for MIB Studio Qt
; This script packages the application with all dependencies and optionally
; installs eGrabber SDK, MindVision Camera SDK, and VC++ Redistributable

#define AppName "MIB Studio Qt"
; AppVersion can be overridden from the command line:
;   ISCC.exe /DAppVersion=0.2.0 mib-studio-qt.iss
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
; SentryDSN is injected at build time by the release workflow:
;   ISCC.exe /DSentryDSN=https://<key>@sentry.yofo.bio/<project>
; Leave empty for builds that should run in local-only crash mode.
#ifndef SentryDSN
  #define SentryDSN ""
#endif
#ifndef SentryEnvironment
  #define SentryEnvironment "production"
#endif
#define AppPublisher "MIB Studio"
#define AppURL "https://github.com/your-org/mib-studio-qt"
#define AppExeName "mib_studio_qt.exe"
#define BuildDir "build\Release"
#define SourceDir "..\..\"
#define EgrabberInstaller "egrabber-win-x86_64-25.10.0.57.exe"
#define VCRedistInstaller "vc_redist.x64.exe"
#define MindVisionInstaller "MindVision-Camera-Platform-Setup2.1.10.195_202604021438.exe"
#define EgrabberPath AddBackslash(SourceDir) + "resources\\installers\\" + EgrabberInstaller
#define VCRedistPath AddBackslash(SourceDir) + "resources\\installers\\" + VCRedistInstaller
#define MindVisionPath AddBackslash(SourceDir) + "resources\\installers\\" + MindVisionInstaller

[Setup]
; App identification
AppId={{A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
LicenseFile=
OutputDir=build\dist
OutputBaseFilename=MIB_Studio_Qt_Setup_v{#AppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
; Help upgrades when the app is running
CloseApplications=yes
CloseApplicationsFilter={#AppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
#if FileExists(VCRedistPath)
Name: "installvcredist"; Description: "Install Visual C++ Redistributable (required for application to run)"; GroupDescription: "Additional components"
#endif
#if FileExists(EgrabberPath)
Name: "installegrabber"; Description: "Install eGrabber SDK (required for camera functionality)"; GroupDescription: "Additional components"
#endif
#if FileExists(MindVisionPath)
Name: "installmindvision"; Description: "Install MindVision Camera SDK (required for MindVision cameras)"; GroupDescription: "Additional components"
#endif

[Files]
; Main executables
Source: "{#SourceDir}{#BuildDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Crashpad handler (required for sentry-native dump collection on Windows).
; "skipifsourcedoesntexist" lets local Debug builds (without crash handler)
; still package cleanly; release builds always produce it.
Source: "{#SourceDir}{#BuildDir}\crashpad_handler.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Application icon (used for shortcuts)
Source: "{#SourceDir}resources\favicon\favicon.ico"; DestDir: "{app}"; Flags: ignoreversion

; Qt DLLs (deployed by windeployqt)
; Expected: Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, Qt6Charts.dll, Qt6OpenGL.dll, Qt6OpenGLWidgets.dll
Source: "{#SourceDir}{#BuildDir}\Qt6*.dll"; DestDir: "{app}"; Flags: ignoreversion

; Third-party DLLs (deployed by windeployqt and CMake post-build)
; Expected: spdlog.dll, OpenCV DLLs, HDF5 DLLs, SQLite DLLs, codec DLLs (jpeg, tiff, webp, etc.)
; Also includes: XMT_DLL_SER.dll (Coremor), and other dependencies
Source: "{#SourceDir}{#BuildDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; Qt plugins (critical: platforms/qwindows.dll must be present)
Source: "{#SourceDir}{#BuildDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}{#BuildDir}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}{#BuildDir}\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}{#BuildDir}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}{#BuildDir}\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}{#BuildDir}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs createallsubdirs

; Isoelastic curve data files
Source: "{#SourceDir}resources\isoelastic_curve\*"; DestDir: "{app}\resources\isoelastic_curve"; Flags: ignoreversion recursesubdirs createallsubdirs

; Visual C++ Redistributable installer (bundled but only run if user selects the task and runtime is not already installed)
#if FileExists(VCRedistPath)
Source: "{#VCRedistPath}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: installvcredist
#endif

; eGrabber installer (bundled but only run if user selects the task)
#if FileExists(EgrabberPath)
Source: "{#EgrabberPath}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: installegrabber
#endif

; MindVision Camera SDK installer (bundled but only run if user selects the task)
#if FileExists(MindVisionPath)
Source: "{#MindVisionPath}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: installmindvision
#endif

[Dirs]
; Ensure data directory structure exists even if source data directory is empty
; This is critical for the application to write logs
Name: "{app}\data"; Flags: uninsneveruninstall
Name: "{app}\data\logs"; Flags: uninsneveruninstall

[Registry]
; Persist the Sentry DSN as a SYSTEM environment variable so every process
; spawned after install (including the application's auto-launched shortcut)
; picks it up. The entries are only emitted when SentryDSN is non-empty —
; offline / air-gapped builds can ship without a DSN and the app falls back
; to local-only crash dumps.
#if SentryDSN != ""
Root: HKLM; Subkey: "System\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: string; ValueName: "MIB_SENTRY_DSN"; ValueData: "{#SentryDSN}"; \
    Flags: uninsdeletevalue
Root: HKLM; Subkey: "System\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: string; ValueName: "MIB_CRASH_ENV"; ValueData: "{#SentryEnvironment}"; \
    Flags: uninsdeletevalue
#endif

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\favicon.ico"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\favicon.ico"; Tasks: desktopicon

[Run]
; Run VC++ Redistributable installer if selected and runtime not already installed
#if FileExists(VCRedistPath)
Filename: "{tmp}\{#VCRedistInstaller}"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ Redistributable..."; Tasks: installvcredist; Flags: runhidden waituntilterminated; Check: VCRuntimeNotInstalled
#endif

; Run eGrabber installer if selected (silent mode)
#if FileExists(EgrabberPath)
Filename: "{tmp}\{#EgrabberInstaller}"; Parameters: "/S"; StatusMsg: "Installing eGrabber SDK..."; Tasks: installegrabber; Flags: runhidden waituntilterminated
#endif

; Run MindVision Camera SDK installer if selected (silent mode)
#if FileExists(MindVisionPath)
Filename: "{tmp}\{#MindVisionInstaller}"; Parameters: "/S"; StatusMsg: "Installing MindVision Camera SDK..."; Tasks: installmindvision; Flags: runhidden waituntilterminated
#endif

[Code]

// Function to check if Visual C++ 2015-2022 runtime is already installed
function VCRuntimeNotInstalled: Boolean;
var
  Version: Cardinal;
begin
  // Check for VC++ 2015-2022 runtime (14.0.x.x) - registry method
  if RegQueryDWordValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Version) then
  begin
    if Version = 1 then
    begin
      Result := False; // Already installed
      Exit;
    end;
  end;

  // Fallback: Check for VC++ 2015-2019 runtime (14.0.x.x) - older key
  if RegQueryDWordValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Version) then
  begin
    if Version = 1 then
    begin
      Result := False; // Already installed
      Exit;
    end;
  end;

  // Additional fallback: Check for presence of key runtime DLLs in System32
  if FileExists(ExpandConstant('{syswow64}\msvcp140.dll')) and FileExists(ExpandConstant('{syswow64}\vcruntime140.dll')) then
  begin
    Result := False; // Runtime DLLs found
    Exit;
  end;

  // If none of the checks pass, assume runtime is not installed
  Result := True;
end;

// Function to check if eGrabber is already installed (similar to original logic)
function EGrabberNotInstalled: Boolean;
var
  Version: String;
begin
  // Check for eGrabber installation
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Euresys\eGrabber', 'Version', Version) then
  begin
    Result := False; // Already installed
    Exit;
  end;

  // Additional check for eGrabber in different registry locations
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Euresys', 'eGrabber', Version) then
  begin
    Result := False; // Already installed
    Exit;
  end;

  // If not found, assume not installed
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // Additional post-install checks can be added here if needed
  end;
end;
