# Windows OS DLLs must never be staged beside the Tauri exe (#851). Included by
# windows-runtime.cmake; tests/release/test_windows_runtime_filter.py runs these functions
# on Linux through windows-system-dlls-selftest.cmake.
#
# file(GET_RUNTIME_DEPENDENCIES) reports paths such as "C:\WINDOWS\system32/kernel32.dll":
# any case, and mixed separators. The old POST_EXCLUDE regex matched only "/System32/", so
# 31 OS DLLs were copied into the candidate. Shipping them can break the app on another
# Windows build.

# The core OS DLLs a desktop app imports. A backstop for the name check: the System32 lookup
# in mib_check_staged_runtime is the full test, but it needs a Windows host.
set(MIB_WINDOWS_OS_DLL_NAMES
  advapi32 bcrypt bcryptprimitives cfgmgr32 combase comctl32 comdlg32 crypt32 cryptbase
  d3d11 d3d12 dbghelp dnsapi dwmapi dxgi gdi32 gdi32full hid imm32 iphlpapi kernel.appcore
  kernel32 kernelbase msasn1 msctf msvcp_win msvcrt netapi32 normaliz nsi ntdll ole32
  oleaut32 opengl32 powrprof profapi propsys psapi rpcrt4 sechost secur32 setupapi shcore
  shell32 shlwapi sspicli ucrtbase user32 userenv uxtheme version win32u windows.storage
  winhttp wininet winmm wintrust wldap32 ws2_32)

# The MSVC runtime is app-local on purpose (copied from the VS redist directory), although
# the same names also exist in System32.
set(MIB_WINDOWS_APP_LOCAL_RUNTIME_REGEX
  "^(msvcp140.*|vcruntime140.*|concrt140|vccorlib140|vcamp140|vcomp140|mfc140.*)\\.dll$")

# Either separator, any case: the POST_EXCLUDE_REGEXES of file(GET_RUNTIME_DEPENDENCIES).
set(MIB_WINDOWS_SYSTEM_DLL_REGEXES
  "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]+([Ss][Yy][Ss][Tt][Ee][Mm]32|[Ss][Yy][Ss][Ww][Oo][Ww]64|[Ww][Ii][Nn][Ss][Xx][Ss])[/\\\\]"
  "(^|[/\\\\])[Aa][Pp][Ii]-[Mm][Ss]-[Ww][Ii][Nn]-[^/\\\\]*$"
  "(^|[/\\\\])[Ee][Xx][Tt]-[Mm][Ss]-[^/\\\\]*$")

# Lower case, forward slashes. Not file(TO_CMAKE_PATH): off Windows it splits "C:" at the colon.
function(mib_windows_normalise_path path out_var)
  string(REPLACE "\\" "/" path "${path}")
  string(REGEX REPLACE "/+" "/" path "${path}")
  string(TOLOWER "${path}" path)
  set(${out_var} "${path}" PARENT_SCOPE)
endfunction()

# Is this DLL name an OS component (api-ms-win-*, ext-ms-*, or a known core DLL)?
function(mib_is_windows_os_dll_name name out_var)
  string(TOLOWER "${name}" name)
  set(result FALSE)
  if(name MATCHES "^(api-ms-win-|ext-ms-)")
    set(result TRUE)
  else()
    string(REGEX REPLACE "\\.dll$" "" stem "${name}")
    if(stem IN_LIST MIB_WINDOWS_OS_DLL_NAMES)
      set(result TRUE)
    endif()
  endif()
  set(${out_var} ${result} PARENT_SCOPE)
endfunction()

# Is this resolved path an OS DLL: under <SystemRoot> (or any X:/windows/system32, syswow64,
# winsxs), or an OS name? system_root may be empty.
function(mib_is_windows_system_dll path system_root out_var)
  mib_windows_normalise_path("${path}" normal)
  get_filename_component(name "${normal}" NAME)
  mib_is_windows_os_dll_name("${name}" result)
  if(NOT result AND normal MATCHES "^[a-z]:/windows/(system32|syswow64|winsxs)/")
    set(result TRUE)
  endif()
  if(NOT result AND NOT "${system_root}" STREQUAL "")
    mib_windows_normalise_path("${system_root}" root)
    string(REGEX REPLACE "/$" "" root "${root}")
    string(LENGTH "${root}/" root_length)
    string(SUBSTRING "${normal}" 0 ${root_length} prefix)
    if(prefix STREQUAL "${root}/")
      set(result TRUE)
    endif()
  endif()
  set(${out_var} ${result} PARENT_SCOPE)
endfunction()

# Drop OS DLLs from a list of resolved paths. Sets <out_var> to the kept paths and
# <dropped_var> to the dropped ones.
function(mib_filter_windows_system_dlls paths system_root out_var dropped_var)
  set(kept)
  set(dropped)
  foreach(path IN LISTS paths)
    mib_is_windows_system_dll("${path}" "${system_root}" is_system)
    if(is_system)
      list(APPEND dropped "${path}")
    else()
      list(APPEND kept "${path}")
    endif()
  endforeach()
  set(${out_var} "${kept}" PARENT_SCOPE)
  set(${dropped_var} "${dropped}" PARENT_SCOPE)
endfunction()

# Fail if a staged DLL is an OS DLL: an OS name, or (with a system_root) a name that exists
# in <system_root>/System32 and is not the app-local MSVC runtime.
function(mib_check_staged_runtime dest system_root)
  string(REPLACE "\\" "/" system_root "${system_root}")
  file(GLOB staged LIST_DIRECTORIES false "${dest}/*")
  set(offending)
  foreach(file IN LISTS staged)
    get_filename_component(name "${file}" NAME)
    string(TOLOWER "${name}" lower)
    if(NOT lower MATCHES "\\.dll$")
      continue()
    endif()
    mib_is_windows_os_dll_name("${lower}" is_os)
    if(NOT is_os AND NOT "${system_root}" STREQUAL ""
       AND NOT lower MATCHES "${MIB_WINDOWS_APP_LOCAL_RUNTIME_REGEX}"
       AND EXISTS "${system_root}/System32/${name}")
      set(is_os TRUE)
    endif()
    if(is_os)
      list(APPEND offending "${name}")
    endif()
  endforeach()
  if(offending)
    list(JOIN offending ", " offending_text)
    message(FATAL_ERROR "Windows OS DLLs staged in ${dest} (#851): ${offending_text}")
  endif()
endfunction()
