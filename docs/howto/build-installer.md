# Building Windows Installer

This guide explains how to build a Windows installer for MIB Studio Qt using InnoSetup.

## Prerequisites

1. **InnoSetup 6** - Download and install from [https://jrsoftware.org/isdl.php](https://jrsoftware.org/isdl.php)
   - The installer will automatically detect InnoSetup if installed to the default location
   - Alternatively, you can specify the path via environment variable or CMake cache

2. **Release Build** - Ensure you have a complete Release build with all dependencies deployed
   - The installer packages files from `build/Release/` directory
   - windeployqt must have run successfully to deploy Qt runtime and plugins

## Build Steps

### 1. Build Release Configuration

First, build the application in Release mode:

```powershell
cmake --build build --config Release --target mib_studio_qt
```

This will:
- Compile the application
- Run windeployqt to deploy Qt runtime and plugins
- Copy all required DLLs and dependencies to `build/Release/`

### 2. Verify Release Build

Before building the installer, verify that the Release build is complete:

```powershell
# Check that executable exists
Test-Path build\Release\mib_studio_qt.exe

# Check that Qt plugins are deployed
Test-Path build\Release\platforms\qwindows.dll
Test-Path build\Release\imageformats\qjpeg.dll
```

### 3. Build Installer

Build the installer using CMake:

```powershell
cmake --build build --target package_installer
```

Or if using Visual Studio:

```powershell
cmake --build build --config Release --target package_installer
```

The installer will be created at:
```
build\dist\MIB_Studio_Qt_Setup_v0.1.0.exe
```

### 4. Alternative: Manual InnoSetup Build

If you prefer to build the installer manually using InnoSetup IDE:

1. Open `resources/installers/mib-studio-qt.iss` in InnoSetup Compiler
2. Ensure the source paths in the script point to your build directory
3. Build → Compile (F9)
4. The installer will be created in `build/dist/`

## Installer Features

The installer includes:

- **Application Files**: `mib_studio_qt.exe`
- **Qt Runtime**: All Qt DLLs, plugins, and dependencies deployed by windeployqt
- **Third-party Libraries**: OpenCV, HDF5, spdlog, and other dependencies
- **eGrabber Installer**: Bundled eGrabber SDK installer (optional installation)
- **MindVision Runtime**: `MVCAMSDK_X64.dll` is copied next to the application
  by default on Windows; the camera device driver is still installed
  separately on target systems
- **Start Menu Shortcuts**: Shortcut for the application
- **Desktop Shortcut**: Optional desktop icon
- **Uninstaller**: Complete uninstallation support

### Visual C++ Redistributable Installation

During installation, users can choose to install the bundled Visual C++ Redistributable:

- The installer checks if Visual C++ 2015-2022 runtime is already installed via registry keys
- If already installed, skips the installation step
- If selected and runtime not found, runs the VC++ redistributable installer silently
- VC++ redistributable is required for the application to run (fixes error 0xc0000142)
- VC++ installer is located at: `resources/installers/vc_redist.x64.exe`

### eGrabber Installation

During installation, users can choose to install the bundled eGrabber SDK:

- The installer checks if eGrabber is already installed
- If found, prompts user to skip installation
- If selected, runs the eGrabber installer silently
- eGrabber installer is located at: `resources/installers/egrabber-win-x86_64-25.10.0.57.exe`

### MindVision Installation

- Windows builds enable MindVision by default. Official release entry points
  use `scripts/provision-mindvision-sdk.ps1` to download, SHA-256 verify, and
  extract the pinned SDK from team R2; local builds may use the same script or
  an installed SDK.
- MIB Studio installers include `MVCAMSDK_X64.dll`, but not the full 181 MiB
  vendor installer. Operators must install the MindVision device driver on the
  target machine before launching the app.

## Testing the Installer

1. **Test Installation**:
   - Run the installer on a clean system (or VM)
   - Verify all files are installed correctly
   - Test the application executable
   - Verify Start Menu shortcut works

2. **Test Uninstallation**:
   - Uninstall via Control Panel or Start Menu
   - Verify all files are removed
   - Check that eGrabber (if installed separately) is not removed

3. **Test eGrabber Integration**:
   - Install on a system without eGrabber
   - Select the eGrabber installation option
   - Verify eGrabber installs correctly
   - Test camera functionality

## Distribution Notes

- **File Size**: The installer includes all Qt runtime files, so it will be large (typically 100-200 MB)
- **Version Number**: Update the version in `cmake/MIBVersion.cmake`. The CMake `package_installer` target passes `/DAppVersion=<PROJECT_VERSION>` to Inno Setup so the output filename matches the app version.
- **Digital Signing**: Consider code signing the installer for distribution (requires a code signing certificate)
- **Antivirus**: Some antivirus software may flag installers - test with common AV products
- **Windows Version**: The installer requires Windows 7 or later (64-bit)

## Troubleshooting

### InnoSetup Not Found

If CMake cannot find InnoSetup:

```
InnoSetup not found - installer target will not be available
```

Solutions:
1. Install InnoSetup to the default location: `C:\Program Files (x86)\Inno Setup 6\`
2. Add InnoSetup to your PATH environment variable
3. Manually specify the path in CMake cache: `-DISCC_EXE="C:/Path/To/ISCC.exe"`

### Missing Files in Installer

If the installer is missing files:

1. Verify the Release build is complete
2. Check that windeployqt ran successfully
3. Verify all DLLs are in `build/Release/`
4. Check the InnoSetup script paths match your build directory structure

### eGrabber Installer Not Found

If the eGrabber installer is missing:

1. Verify `resources/installers/egrabber-win-x86_64-25.10.0.57.exe` exists
2. Check the file name matches the `#define EgrabberInstaller` in the InnoSetup script
3. Update the script if the eGrabber installer version has changed

### MindVision SDK Not Found

If CMake fails with a MindVision SDK error:

1. Run `scripts/provision-mindvision-sdk.ps1 -PassThru`, or verify the SDK is
   installed on the machine building the installer
2. Set `MIB_MINDVISION_SDK_ROOT` or `MIB_MINDVISION_SDK_DIR` to the SDK root
3. Confirm the build can find `MindVision/CameraApiLoad.h` and
   `MVCAMSDK.dll` / `MVCAMSDK_X64.dll`
4. Reconfigure (MindVision defaults on for Windows); use
   `-DMIB_ENABLE_MINDVISION=ON` explicitly for release builds

## Updating the Installer Script

The installer script is located at `resources/installers/mib-studio-qt.iss`. Key sections:

- `[Setup]`: Installer metadata and configuration
- `[Files]`: Files to include in the installer
- `[Icons]`: Start Menu and desktop shortcuts
- `[Run]`: Post-installation tasks (e.g., eGrabber installer)
- `[Code]`: Custom Pascal script for installation logic

To update the version number, modify the `AppVersion` define at the top of the script, or update `CMakeLists.txt` and regenerate the script if using CMake variables.

If you compile via CMake (`package_installer` target), the version is injected automatically via:

```
ISCC.exe /DAppVersion=<PROJECT_VERSION> mib-studio-qt.iss
```
