## 2026-11-04 — Tauri Windows packaging no longer copies OS DLLs (#851)

On win11-agent the candidate packaging staged 31 OS DLLs (kernel32, ntdll, user32…) beside the
exe: `file(GET_RUNTIME_DEPENDENCIES)` reported `C:\WINDOWS\system32/…`, and the
`POST_EXCLUDE` regex matched only `/System32/`.

`desktop/scripts/windows-system-dlls.cmake` (included by `windows-runtime.cmake`): regexes for
any case and either separator (System32, SysWOW64, WinSxS, `api-ms-win-*`, `ext-ms-*`); a second
filter on the resolved list (under `%SystemRoot%`, or a core OS DLL name); and a check that fails
packaging when a staged DLL is an OS DLL. CTest `scripts.windows_runtime_excludes_os_dlls`.
Verified on Linux with `cmake -P`; the Windows packaging run is still to do (candidate workflow
or win11-agent). See [[Desktop-Shell]].
