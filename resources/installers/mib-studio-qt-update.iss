; InnoSetup installer script for MIB Studio Qt Update Package
; This script packages only the application files (no eGrabber/VC++ redistributable)
; Used for auto-updates to minimize download size and installation time

#define AppName "MIB Studio Qt"
; AppVersion can be overridden from the command line:
;   ISCC.exe /DAppVersion=0.2.0 mib-studio-qt-update.iss
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
; SentryDSN is injected at build time by the release workflow.
; Updates re-write the env var so DSN/environment changes propagate.
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

[Setup]
; App identification (same AppId as full installer for upgrade compatibility)
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
OutputBaseFilename=MIB_Studio_Qt_Update_v{#AppVersion}
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

[Files]
Source: "{#SourceDir}docs\release-notes\*.md"; DestDir: "{app}\resources\release-notes"; Flags: ignoreversion
Source: "{#SourceDir}docs\manual\*"; DestDir: "{app}\resources\manual"; Flags: ignoreversion recursesubdirs createallsubdirs

; Main executables
Source: "{#SourceDir}{#BuildDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Crashpad handler for sentry-native (see mib-studio-qt.iss for notes).
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

[Dirs]
; Ensure data directory structure exists even if source data directory is empty
; This is critical for the application to write logs
Name: "{app}\data"; Flags: uninsneveruninstall
Name: "{app}\data\logs"; Flags: uninsneveruninstall

[Registry]
; Keep the system-wide Sentry env vars in sync with the latest release.
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
