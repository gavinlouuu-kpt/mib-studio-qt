# Optional sentry-native integration (CMake-managed clone).
include(Sentry)

# Qt is a frontend-only dependency now (epic #246): the backend is fully Qt-free
# — Gui went with the OpenCV mock-camera decode, SerialPort with ISerialPort,
# Network with the injected LUT HTTP seam (ADR 0002), and Core with the crash
# reporter's Qt log handler moving to the frontend. So MIB_BUILD_BACKEND_ONLY
# (⊇ MIB_BUILD_PROCESSING_ONLY) needs no Qt SDK at all — the Phase 1 exit gate.
if(NOT MIB_BUILD_BACKEND_ONLY)
    find_package(Qt6 COMPONENTS Core Gui Network Widgets Charts Concurrent REQUIRED)
endif()
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG QUIET)
find_package(OpenCV CONFIG REQUIRED)
if(NOT MIB_BUILD_PROCESSING_ONLY)
    find_package(onnxruntime CONFIG QUIET)
endif()

# EGrabber/Coremor remain Windows-only. MindVision publishes separate Windows,
# Linux, and macOS SDKs, so that integration is enabled on every desktop OS.
set(MIB_HAS_EGRABBER OFF)
if(WIN32 AND MIB_ENABLE_HARDWARE_SDKS)
    set(MIB_HAS_EGRABBER ON)
endif()
message(STATUS "Hardware SDK integrations: ${MIB_HAS_EGRABBER}")

set(MIB_HAS_MINDVISION OFF)
if(MIB_ENABLE_MINDVISION AND NOT MIB_BUILD_PROCESSING_ONLY)
    set(MIB_HAS_MINDVISION ON)
endif()
message(STATUS "MindVision SDK integration: ${MIB_HAS_MINDVISION}")

set(MIB_HAS_ONNXRUNTIME OFF)
if(TARGET onnxruntime::onnxruntime)
    set(MIB_HAS_ONNXRUNTIME ON)
endif()

set(MIB_MINDVISION_SDK_ROOT "$ENV{MIB_MINDVISION_SDK_ROOT}" CACHE PATH
    "Root directory of the MindVision SDK installation")
set(MIB_MINDVISION_RUNTIME_DIR "$ENV{MIB_MINDVISION_RUNTIME_DIR}" CACHE PATH
    "Directory containing the MindVision shared library (defaults to SDK root)")

if(MIB_HAS_MINDVISION)
    set(_MINDVISION_PROVISIONED_ROOT
        "${PROJECT_SOURCE_DIR}/build/vendor/mindvision-sdk/extracted")
    set(_MINDVISION_SDK_PATHS)
    if(MIB_MINDVISION_SDK_ROOT)
        list(APPEND _MINDVISION_SDK_PATHS
            "${MIB_MINDVISION_SDK_ROOT}"
            "${MIB_MINDVISION_SDK_ROOT}/Include"
            "${MIB_MINDVISION_SDK_ROOT}/include"
            "${MIB_MINDVISION_SDK_ROOT}/Demo/VC++/Include"
        )
    endif()
    list(APPEND _MINDVISION_SDK_PATHS
        "${_MINDVISION_PROVISIONED_ROOT}"
        "${_MINDVISION_PROVISIONED_ROOT}/include"
        "${_MINDVISION_PROVISIONED_ROOT}/Demo/VC++/Include"
        "$ENV{MIB_MINDVISION_SDK_ROOT}"
        "$ENV{MIB_MINDVISION_SDK_DIR}"
        "C:/Program Files/MindVision"
        "C:/Program Files (x86)/MindVision"
    )

    if(WIN32)
        set(_MINDVISION_HEADER_NAMES MindVision/CameraApiLoad.h CameraApiLoad.h)
    else()
        set(_MINDVISION_HEADER_NAMES MindVision/CameraApi.h CameraApi.h)
    endif()

    find_path(MIB_MINDVISION_INCLUDE_DIR
        NAMES ${_MINDVISION_HEADER_NAMES}
        PATHS ${_MINDVISION_SDK_PATHS}
        NO_DEFAULT_PATH
    )
    if(NOT MIB_MINDVISION_INCLUDE_DIR)
        find_path(MIB_MINDVISION_INCLUDE_DIR
            NAMES ${_MINDVISION_HEADER_NAMES}
        )
    endif()

    set(_MINDVISION_RUNTIME_PATHS)
    if(MIB_MINDVISION_RUNTIME_DIR)
        list(APPEND _MINDVISION_RUNTIME_PATHS
            "${MIB_MINDVISION_RUNTIME_DIR}"
        )
    endif()
    if(MIB_MINDVISION_SDK_ROOT)
        list(APPEND _MINDVISION_RUNTIME_PATHS
            "${MIB_MINDVISION_SDK_ROOT}"
            "${MIB_MINDVISION_SDK_ROOT}/bin"
            "${MIB_MINDVISION_SDK_ROOT}/Bin"
            "${MIB_MINDVISION_SDK_ROOT}/SDK/X64"
            "${MIB_MINDVISION_SDK_ROOT}/lib"
        )
    endif()
    list(APPEND _MINDVISION_RUNTIME_PATHS
        "${_MINDVISION_PROVISIONED_ROOT}/lib"
        "${_MINDVISION_PROVISIONED_ROOT}/SDK/X64"
        "$ENV{MIB_MINDVISION_SDK_ROOT}"
        "$ENV{MIB_MINDVISION_SDK_DIR}"
    )

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(_MINDVISION_ARCH_DIR x64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_MINDVISION_ARCH_DIR arm64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
        set(_MINDVISION_ARCH_DIR x86)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)$")
        set(_MINDVISION_ARCH_DIR arm)
    endif()
    if(_MINDVISION_ARCH_DIR AND MIB_MINDVISION_SDK_ROOT)
        list(APPEND _MINDVISION_RUNTIME_PATHS
            "${MIB_MINDVISION_SDK_ROOT}/lib/${_MINDVISION_ARCH_DIR}")
    endif()

    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_MINDVISION_RUNTIME_NAMES MVCAMSDK_X64.dll MVCAMSDK.dll)
        else()
            set(_MINDVISION_RUNTIME_NAMES MVCAMSDK.dll MVCAMSDK_X64.dll)
        endif()

        find_file(MIB_MINDVISION_RUNTIME_DLL
            NAMES ${_MINDVISION_RUNTIME_NAMES}
            PATHS ${_MINDVISION_RUNTIME_PATHS}
            NO_DEFAULT_PATH
        )
        if(NOT MIB_MINDVISION_RUNTIME_DLL)
            find_file(MIB_MINDVISION_RUNTIME_DLL
                NAMES ${_MINDVISION_RUNTIME_NAMES}
            )
        endif()
        set(MIB_MINDVISION_RUNTIME "${MIB_MINDVISION_RUNTIME_DLL}")
    elseif(APPLE)
        # The pinned archive calls this libmvsdk.dylib.
        set(_MINDVISION_RUNTIME_NAMES mvsdk libmvsdk.dylib)
    else()
        # The pinned archive calls this libMVSDK.so.
        set(_MINDVISION_RUNTIME_NAMES MVSDK libMVSDK.so)
    endif()
    if(NOT WIN32)
        find_library(MIB_MINDVISION_LIBRARY
            NAMES ${_MINDVISION_RUNTIME_NAMES}
            PATHS ${_MINDVISION_RUNTIME_PATHS}
            NO_DEFAULT_PATH
        )
        if(NOT MIB_MINDVISION_LIBRARY)
            find_library(MIB_MINDVISION_LIBRARY
                NAMES ${_MINDVISION_RUNTIME_NAMES}
            )
        endif()
        set(MIB_MINDVISION_RUNTIME "${MIB_MINDVISION_LIBRARY}")
    endif()

    if(NOT MIB_MINDVISION_INCLUDE_DIR OR NOT MIB_MINDVISION_RUNTIME)
        if(WIN32)
            set(_MINDVISION_PROVISION_COMMAND "scripts/provision-mindvision-sdk.ps1")
            set(_MINDVISION_REQUIRED_FILES
                "CameraApiLoad.h and MVCAMSDK.dll/MVCAMSDK_X64.dll")
        else()
            set(_MINDVISION_PROVISION_COMMAND "scripts/provision-mindvision-sdk.sh")
            set(_MINDVISION_REQUIRED_FILES
                "CameraApi.h and the platform MindVision shared library")
        endif()
        message(FATAL_ERROR
            "MIB_ENABLE_MINDVISION is ON, but the MindVision SDK could not be located. "
            "Run ${_MINDVISION_PROVISION_COMMAND} or set MIB_MINDVISION_SDK_ROOT "
            "(or environment variable MIB_MINDVISION_SDK_DIR) so CMake can find "
            "${_MINDVISION_REQUIRED_FILES}. Use -DMIB_ENABLE_MINDVISION=OFF only "
            "for an intentional SDK-free build.")
    endif()

    message(STATUS "MindVision SDK include dir: ${MIB_MINDVISION_INCLUDE_DIR}")
    message(STATUS "MindVision SDK runtime: ${MIB_MINDVISION_RUNTIME}")
endif()

# Prefer official SQLite3 package; fallback to unofficial config if needed.
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    find_package(unofficial-sqlite3 CONFIG QUIET)
endif()

# HDF5 - try both CONFIG and MODULE mode.
find_package(HDF5 CONFIG QUIET)
if(NOT HDF5_FOUND)
    find_package(HDF5 MODULE QUIET)
endif()
