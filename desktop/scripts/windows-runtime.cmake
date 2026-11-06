# Nonpublishing portable candidate closure. Run from a VS x64 developer shell.
cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/windows-system-dlls.cmake")
foreach(arg EXE MANIFEST DEST)
  if(NOT DEFINED ${arg})
    message(FATAL_ERROR "Missing -D${arg}")
  endif()
endforeach()
file(READ "${MANIFEST}" manifest_json)
string(JSON count LENGTH "${manifest_json}" runtime_dirs)
math(EXPR last "${count}-1")
set(search_dirs)
foreach(i RANGE ${last})
  string(JSON directory GET "${manifest_json}" runtime_dirs ${i})
  list(APPEND search_dirs "${directory}")
endforeach()
# App-local MSVC runtime, not an assumption that developer tools are installed.
file(GLOB crt_dirs "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT")
if(NOT crt_dirs)
  message(FATAL_ERROR "MSVC redistributable CRT directory unavailable; use a VS developer shell")
endif()
list(APPEND search_dirs ${crt_dirs})
# OpenCV may load these by name rather than a PE import table.
set(runtime_plugins)
foreach(directory IN LISTS search_dirs)
  file(GLOB plugins "${directory}/opencv_videoio*.dll")
  list(APPEND runtime_plugins ${plugins})
endforeach()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${EXE}"
  LIBRARIES ${runtime_plugins}
  DIRECTORIES ${search_dirs}
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  CONFLICTING_DEPENDENCIES_PREFIX conflicts
  # HvsiFileTrust.dll (Defender Application Guard) is an OS component that
  # System32 DLLs import; it is absent on some Windows images and never shipped.
  PRE_EXCLUDE_REGEXES "[Aa][Pp][Ii]-[Mm][Ss]-" "[Ee][Xx][Tt]-[Mm][Ss]-"
    "^[Hh][Vv][Ss][Ii][Ff][Ii][Ll][Ee][Tt][Rr][Uu][Ss][Tt]\\.[Dd][Ll][Ll]$"
  # Resolved paths look like "C:\WINDOWS\system32/kernel32.dll": any case, either separator
  # (#851). See windows-system-dlls.cmake.
  POST_EXCLUDE_REGEXES ${MIB_WINDOWS_SYSTEM_DLL_REGEXES})
# Second line of defence: whatever the regexes missed, drop anything under %SystemRoot% or
# with an OS DLL name, and say so in the log.
mib_filter_windows_system_dlls("${resolved}" "$ENV{SystemRoot}" resolved dropped_system_dlls)
if(dropped_system_dlls)
  list(JOIN dropped_system_dlls "\n  " dropped_text)
  message(STATUS "Not packaged (Windows OS DLLs):\n  ${dropped_text}")
endif()
if(unresolved OR conflicts_FILENAMES)
  message(FATAL_ERROR "Incomplete DLL closure: unresolved=${unresolved}; conflicts=${conflicts_FILENAMES}")
endif()
# ADR 0007: processing-core plugins reach rigs only as signed registry
# downloads, never inside the desktop package.
foreach(dependency IN LISTS resolved runtime_plugins)
  get_filename_component(dependency_name "${dependency}" NAME)
  if(dependency_name MATCHES "^mib_processing_core")
    message(FATAL_ERROR "Processing-core plugin must not be packaged: ${dependency}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${DEST}")
file(COPY "${EXE}" ${resolved} ${runtime_plugins} DESTINATION "${DEST}")
foreach(crt_dir IN LISTS crt_dirs)
  file(GLOB crt_dlls "${crt_dir}/*.dll")
  file(COPY ${crt_dlls} DESTINATION "${DEST}")
endforeach()
# Fails packaging if any Windows OS DLL was staged (#851).
mib_check_staged_runtime("${DEST}" "$ENV{SystemRoot}")
file(WRITE "${DEST}/native-runtime-files.txt" "${resolved}\n")
