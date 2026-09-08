cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(MIBVersion)

project(MIB_Studio_Qt VERSION ${PROJECT_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(MIBOptions)

# Qt AUTOGEN is only needed by the frontend — the backend is Qt-free (epic
# #246). Leaving these ON under MIB_BUILD_BACKEND_ONLY (no Qt SDK) makes CMake
# warn "No valid Qt version found" for every target, so gate them off.
if(NOT MIB_BUILD_BACKEND_ONLY)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
endif()

include(MIBCompilerSettings)
include(MIBDependencies)
include(CTest)

add_subdirectory(src/backend)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(MIB_BUILD_PYTHON_BINDINGS)
    add_subdirectory(bindings/python)
endif()

if(NOT MIB_BUILD_BACKEND_ONLY)
    add_subdirectory(src/frontend/qt)

    include(MIBWindowsPackaging)
    mib_add_windows_installer_targets(mib_studio_qt)
endif()
