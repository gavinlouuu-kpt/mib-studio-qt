# Nonpublishing portable candidate closure. Run from a VS x64 developer shell.
cmake_minimum_required(VERSION 3.21)
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
# OpenCV/ONNX may load these by name rather than a PE import table.
set(runtime_plugins)
foreach(directory IN LISTS search_dirs)
  file(GLOB plugins "${directory}/opencv_videoio*.dll" "${directory}/onnxruntime_providers_shared.dll")
  list(APPEND runtime_plugins ${plugins})
endforeach()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${EXE}"
  LIBRARIES ${runtime_plugins}
  DIRECTORIES ${search_dirs}
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  CONFLICTING_DEPENDENCIES_PREFIX conflicts
  PRE_EXCLUDE_REGEXES "[Aa][Pp][Ii]-[Mm][Ss]-" "[Ee][Xx][Tt]-[Mm][Ss]-"
  POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Tt][Ee][Mm]32/.*")
if(unresolved OR conflicts_FILENAMES)
  message(FATAL_ERROR "Incomplete DLL closure: unresolved=${unresolved}; conflicts=${conflicts_FILENAMES}")
endif()
file(MAKE_DIRECTORY "${DEST}")
file(COPY "${EXE}" ${resolved} ${runtime_plugins} DESTINATION "${DEST}")
foreach(crt_dir IN LISTS crt_dirs)
  file(GLOB crt_dlls "${crt_dir}/*.dll")
  file(COPY ${crt_dlls} DESTINATION "${DEST}")
endforeach()
file(WRITE "${DEST}/native-runtime-files.txt" "${resolved}\n")
