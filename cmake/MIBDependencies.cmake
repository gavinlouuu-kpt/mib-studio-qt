# Optional sentry-native integration (CMake-managed clone).
include(Sentry)

set(MIB_QT_COMPONENTS Core Gui SerialPort Network)
if(NOT MIB_BUILD_BACKEND_ONLY)
    list(APPEND MIB_QT_COMPONENTS Widgets Charts Concurrent)
endif()
if(NOT MIB_BUILD_PROCESSING_ONLY)
    find_package(Qt6 COMPONENTS ${MIB_QT_COMPONENTS} REQUIRED)
endif()
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG QUIET)
find_package(OpenCV CONFIG REQUIRED)
if(NOT MIB_BUILD_PROCESSING_ONLY)
    find_package(onnxruntime CONFIG QUIET)
endif()

# Proprietary hardware SDKs are Windows-only, but their availability is kept
# independent so autofocus is not accidentally disabled with a camera SDK.
set(MIB_HAS_EGRABBER OFF)
if(WIN32 AND MIB_ENABLE_HARDWARE_SDKS)
    set(MIB_HAS_EGRABBER ON)
endif()
set(MIB_HAS_COREMOR OFF)
if(WIN32 AND MIB_ENABLE_HARDWARE_SDKS AND
   EXISTS "${PROJECT_SOURCE_DIR}/include/Coremor/XMT_DLL_SER.h" AND
   EXISTS "${PROJECT_SOURCE_DIR}/include/Coremor/XMT_DLL_SER.lib")
    set(MIB_HAS_COREMOR ON)
endif()
message(STATUS "EGrabber SDK integration: ${MIB_HAS_EGRABBER}")
message(STATUS "CoreMOR SDK integration: ${MIB_HAS_COREMOR}")

set(MIB_HAS_MINDVISION OFF)
if(WIN32 AND MIB_ENABLE_MINDVISION)
    set(MIB_HAS_MINDVISION ON)
endif()
message(STATUS "MindVision SDK integration: ${MIB_HAS_MINDVISION}")

set(MIB_HAS_ONNXRUNTIME OFF)
if(TARGET onnxruntime::onnxruntime)
    set(MIB_HAS_ONNXRUNTIME ON)
endif()

set(MIB_MINDVISION_SDK_ROOT "" CACHE PATH
    "Root directory of the MindVision SDK installation")
set(MIB_MINDVISION_RUNTIME_DIR "" CACHE PATH
    "Directory containing MVCAMSDK.dll / MVCAMSDK_X64.dll (defaults to SDK root)")

if(MIB_HAS_MINDVISION)
    set(_MINDVISION_SDK_PATHS)
    if(MIB_MINDVISION_SDK_ROOT)
        list(APPEND _MINDVISION_SDK_PATHS
            "${MIB_MINDVISION_SDK_ROOT}"
            "${MIB_MINDVISION_SDK_ROOT}/Include"
            "${MIB_MINDVISION_SDK_ROOT}/include"
        )
    endif()
    list(APPEND _MINDVISION_SDK_PATHS
        "$ENV{MIB_MINDVISION_SDK_ROOT}"
        "$ENV{MIB_MINDVISION_SDK_DIR}"
        "C:/Program Files/MindVision"
        "C:/Program Files (x86)/MindVision"
    )

    find_path(MIB_MINDVISION_INCLUDE_DIR
        NAMES MindVision/CameraApiLoad.h CameraApiLoad.h
        PATHS ${_MINDVISION_SDK_PATHS}
        NO_DEFAULT_PATH
    )
    if(NOT MIB_MINDVISION_INCLUDE_DIR)
        find_path(MIB_MINDVISION_INCLUDE_DIR
            NAMES MindVision/CameraApiLoad.h CameraApiLoad.h
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
        )
    endif()
    list(APPEND _MINDVISION_RUNTIME_PATHS
        "$ENV{MIB_MINDVISION_SDK_ROOT}"
        "$ENV{MIB_MINDVISION_SDK_DIR}"
    )

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

    if(NOT MIB_MINDVISION_INCLUDE_DIR OR NOT MIB_MINDVISION_RUNTIME_DLL)
        message(FATAL_ERROR
            "MIB_ENABLE_MINDVISION is ON, but the MindVision SDK could not be located. "
            "Set MIB_MINDVISION_SDK_ROOT (or environment variable MIB_MINDVISION_SDK_DIR) "
            "so CMake can find MindVision/CameraApiLoad.h and MVCAMSDK.dll/MVCAMSDK_X64.dll.")
    endif()

    message(STATUS "MindVision SDK include dir: ${MIB_MINDVISION_INCLUDE_DIR}")
    message(STATUS "MindVision SDK runtime DLL: ${MIB_MINDVISION_RUNTIME_DLL}")
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
