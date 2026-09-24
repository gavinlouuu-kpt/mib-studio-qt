# Windows: Deploy Qt runtime for running from PowerShell/File Explorer

Use windeployqt to copy required Qt DLLs and plugins next to the executable.

## Release build

First, find windeployqt in your Conan installation. It's typically located in:
- `CMAKE_PREFIX_PATH/tools/qt6/bin/windeployqt.exe`, or
- `CMAKE_PREFIX_PATH/bin/windeployqt.exe`

Run:
```
powershell -NoProfile -Command "& '<path-to-windeployqt>\windeployqt.exe' --release 'D:\mib-studio-qt\build\Release\mib_studio_qt.exe'"
```

Or if using CMake presets, the build process will automatically run windeployqt post-build.

Verify that `D:\mib-studio-qt\build\Release\platforms\qwindows.dll` exists, then launch:
```
D:\mib-studio-qt\build\Release\mib_studio_qt.exe
```

## Debug build
If your Conan Qt package does not include debug Qt DLLs, windeploy for Debug may fail (e.g., missing `Qt6Widgetsd.dll`). Either:
- Ensure Conan Qt package includes debug binaries, or
- Run the Release build for testing.

## Notes
- No env vars needed when plugins are deployed next to the exe.
- If you must run from a different folder, set `QT_QPA_PLATFORM_PLUGIN_PATH` to the `platforms` directory.
- Windows defaults `MIB_ENABLE_MINDVISION=ON`, and the post-build deployment
  step copies `MVCAMSDK_X64.dll` next to the exe. Official release paths
  provision the pinned SDK from R2; local builds may run
  `scripts/provision-mindvision-sdk.ps1` or use an installed SDK. The target
  machine still needs the MindVision device driver.
