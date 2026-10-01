# 2026-09-15 — Rig PC: localized cl.exe `/showIncludes` prefix and the `cpuinfo` Conan conflict

> Moved from `build-and-run/Build.md` on 2026-09-21. The mechanical fixes
> (configure-time gate in `cmake/MIBCompilerSettings.cmake`, the
> `[replace_requires]` pin in `conan/profiles/windows-msvc194*`,
> `scripts/doctor.ps1`'s prefix check) are current truth in Build.md.

CMake's Ninja generator tracks MSVC header dependencies by parsing cl.exe's
`/showIncludes` lines and assumes the English prefix `Note: including file:`
unless it detects another at configure time. On a host whose Visual Studio
language is not English (the rig PC prints the Chinese prefix) no prefix was
detected and `ninja -t deps <obj>` showed `#deps 0`: editing a header did not
rebuild its includers, so a stale object could silently keep an old struct
layout. `VSLANG=1033` only helps when the English language pack is installed; the rig
PC's Build Tools carry only the system language, so cl.exe kept printing the
localized prefix. Fix: add the English pack (`vs_installer.exe modify
--installPath "<BuildTools>" --addProductLang en-US`) or pass the localized
prefix as `-DCMAKE_CL_SHOWINCLUDES_PREFIX=...` at configure. Since 2026-09-21
`cmake/MIBCompilerSettings.cmake` fails the Ninja configure when the prefix
is empty (override: `-DMIB_ALLOW_UNKNOWN_SHOWINCLUDES_PREFIX=ON`, then always
`--clean-first` after header edits), and `scripts/doctor.ps1` reports what
your cl.exe prints. sccache is not the cause (verified: the same prefix
appears with and without the launcher).

Also on the rig PC: ConanCenter now resolves `cpuinfo/[>=cci.20231129]` to
`cci.20251210` while `onnxruntime/1.18.1` pins `cci.20231129`; a cold Conan
cache fails with a version conflict. Since 2026-09-21 the repo profiles
`conan/profiles/windows-msvc194*` carry
`[replace_requires] cpuinfo/*: cpuinfo/cci.20231129`, and the Windows
workflows use those profiles instead of an inline `ci` profile, so a cold
cache resolves the same graph everywhere.

Related: [[../build-and-run/Build]], [[../task/2026-09-09-windows-ninja-fast-loop]].
