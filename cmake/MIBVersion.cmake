# Default version (fallback if git is not available or no tags exist)
set(DEFAULT_VERSION "1.0.7")

# Release publishers pass both values explicitly so the binary identity being
# tested is the same identity later used for the tag, installer, and manifest.
# Capture and remove the command-line cache entries immediately: these are
# deliberately one-configure overrides and must not leak into a later local
# development configure that reuses the build directory.
if(DEFINED MIB_RELEASE_VERSION_OVERRIDE OR
   DEFINED MIB_RELEASE_VERSION_FULL_OVERRIDE)
    if(NOT DEFINED MIB_RELEASE_VERSION_OVERRIDE OR
       NOT DEFINED MIB_RELEASE_VERSION_FULL_OVERRIDE)
        message(FATAL_ERROR
            "MIB_RELEASE_VERSION_OVERRIDE and "
            "MIB_RELEASE_VERSION_FULL_OVERRIDE must be provided together")
    endif()

    set(_MIB_RELEASE_VERSION "${MIB_RELEASE_VERSION_OVERRIDE}")
    set(_MIB_RELEASE_VERSION_FULL "${MIB_RELEASE_VERSION_FULL_OVERRIDE}")
    unset(MIB_RELEASE_VERSION_OVERRIDE CACHE)
    unset(MIB_RELEASE_VERSION_FULL_OVERRIDE CACHE)

    if(NOT _MIB_RELEASE_VERSION MATCHES
       "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "MIB_RELEASE_VERSION_OVERRIDE must be numeric X.Y.Z")
    endif()
    if(NOT _MIB_RELEASE_VERSION_FULL MATCHES
       "^[0-9]+\\.[0-9]+\\.[0-9]+(-beta\\.[0-9A-Za-z][0-9A-Za-z.-]*)?$")
        message(FATAL_ERROR
            "MIB_RELEASE_VERSION_FULL_OVERRIDE must be X.Y.Z or "
            "X.Y.Z-beta.<identifier>")
    endif()
    string(REGEX REPLACE
        "^([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1"
        _MIB_RELEASE_FULL_NUMERIC "${_MIB_RELEASE_VERSION_FULL}")
    if(NOT _MIB_RELEASE_FULL_NUMERIC STREQUAL _MIB_RELEASE_VERSION)
        message(FATAL_ERROR
            "Release numeric and full overrides must name the same numeric version")
    endif()

    set(PROJECT_VERSION "${_MIB_RELEASE_VERSION}")
    set(PROJECT_VERSION_FULL "${_MIB_RELEASE_VERSION_FULL}")
    file(WRITE "${CMAKE_BINARY_DIR}/mib-release-identity.txt"
        "version=${PROJECT_VERSION}\n"
        "full_version=${PROJECT_VERSION_FULL}\n")
    message(STATUS
        "Using explicit release version: ${PROJECT_VERSION} "
        "(full: ${PROJECT_VERSION_FULL})")
    return()
endif()

# Try to get version from git tags
set(GIT_VERSION "")
# Full version retains any pre-release suffix (e.g. 1.0.4-beta.1) so the running
# app knows its channel; PROJECT_VERSION stays numeric for project()/installers.
set(GIT_VERSION_FULL "")
if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
        # Get the latest tag matching version pattern (vX.Y.Z or X.Y.Z)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "v[0-9]*" --match "[0-9]*"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE GIT_TAG_RESULT
        )

        if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG)
            # Extract version from tag (strip optional 'v' prefix)
            string(REGEX REPLACE "^v?([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1" GIT_VERSION "${GIT_TAG}")
            # Full version keeps the suffix (strip only the optional 'v').
            string(REGEX REPLACE "^v" "" GIT_VERSION_FULL "${GIT_TAG}")
            # Verify it's a valid semantic version
            if(GIT_VERSION AND GIT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
                message(STATUS "Found git tag version: ${GIT_VERSION} (full: ${GIT_VERSION_FULL}, from tag: ${GIT_TAG})")
            else()
                set(GIT_VERSION "")
                set(GIT_VERSION_FULL "")
            endif()
        endif()
    endif()
endif()

# Use git version if available and newer than default, otherwise use default
if(GIT_VERSION)
    # Compare versions: if git version is newer, use it; otherwise use default
    if(GIT_VERSION VERSION_GREATER DEFAULT_VERSION OR GIT_VERSION VERSION_EQUAL DEFAULT_VERSION)
        set(PROJECT_VERSION "${GIT_VERSION}")
        set(PROJECT_VERSION_FULL "${GIT_VERSION_FULL}")
        message(STATUS "Using version from git tag: ${PROJECT_VERSION} (full: ${PROJECT_VERSION_FULL})")
    else()
        set(PROJECT_VERSION "${DEFAULT_VERSION}")
        set(PROJECT_VERSION_FULL "${DEFAULT_VERSION}")
        message(STATUS "Using default version (git tag ${GIT_VERSION} is older): ${PROJECT_VERSION}")
    endif()
else()
    set(PROJECT_VERSION "${DEFAULT_VERSION}")
    set(PROJECT_VERSION_FULL "${DEFAULT_VERSION}")
    message(STATUS "Using default version (git tag not available): ${PROJECT_VERSION}")
endif()
