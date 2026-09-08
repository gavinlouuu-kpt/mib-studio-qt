set(MIB_ENABLE_HARDWARE_SDKS_DEFAULT OFF)
if(WIN32)
    set(MIB_ENABLE_HARDWARE_SDKS_DEFAULT ON)
endif()
option(MIB_ENABLE_HARDWARE_SDKS
    "Enable proprietary Windows hardware SDK integrations (EGrabber/Coremor)"
    ${MIB_ENABLE_HARDWARE_SDKS_DEFAULT})

set(MIB_ENABLE_WINDOWS_PACKAGING_DEFAULT OFF)
if(WIN32)
    set(MIB_ENABLE_WINDOWS_PACKAGING_DEFAULT ON)
endif()
option(MIB_ENABLE_WINDOWS_PACKAGING
    "Enable Windows runtime deployment and InnoSetup packaging targets"
    ${MIB_ENABLE_WINDOWS_PACKAGING_DEFAULT})

set(MIB_ENABLE_MINDVISION_DEFAULT ON)
option(MIB_ENABLE_MINDVISION
    "Enable MindVision camera SDK integration (requires the platform SDK)"
    ${MIB_ENABLE_MINDVISION_DEFAULT})

option(MIB_BUILD_BACKEND_ONLY
    "Build only backend targets (no frontend executables)"
    OFF)

option(MIB_BUILD_PYTHON_BINDINGS
    "Build the pybind11 Python bindings for mib_processing (bindings/python/)"
    OFF)

option(MIB_BUILD_PROCESSING_ONLY
    "Configure only the Qt-free mib_processing target and Python bindings"
    OFF)

set(MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256 "" CACHE STRING
    "Approved Authenticode signer SubjectPublicKeyInfo SHA-256 for native processing cores")
option(MIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI
    "Require a non-empty approved processing-core signer SPKI SHA-256"
    OFF)

set(MIB_PROCESSING_CORE_ED25519_SPKI_SHA256 "" CACHE STRING
    "Approved Ed25519 signer SubjectPublicKeyInfo SHA-256 for Linux native processing cores")
option(MIB_REQUIRE_PROCESSING_CORE_ED25519_SPKI
    "Require a non-empty approved Linux processing-core Ed25519 signer SPKI SHA-256"
    OFF)

set(MIB_PROCESSING_CORE_APP_MIN_VERSION "" CACHE STRING
    "Oldest desktop app version allowed to load a released native processing core")
set(MIB_PROCESSING_CORE_APP_MAX_VERSION "" CACHE STRING
    "Newest desktop app version allowed to load a released native processing core")
option(MIB_REQUIRE_PROCESSING_CORE_APP_COMPAT
    "Require explicit native processing-core desktop compatibility bounds"
    OFF)

string(STRIP "${MIB_PROCESSING_CORE_APP_MIN_VERSION}"
    _mib_processing_core_app_min_version)
string(STRIP "${MIB_PROCESSING_CORE_APP_MAX_VERSION}"
    _mib_processing_core_app_max_version)
if(MIB_REQUIRE_PROCESSING_CORE_APP_COMPAT AND
   (NOT _mib_processing_core_app_min_version OR
    NOT _mib_processing_core_app_max_version))
    message(FATAL_ERROR
        "A processing-core release requires explicit app compatibility bounds")
endif()
if(NOT _mib_processing_core_app_min_version)
    set(_mib_processing_core_app_min_version "${PROJECT_VERSION}")
endif()
if(NOT _mib_processing_core_app_max_version)
    set(_mib_processing_core_app_max_version "${PROJECT_VERSION}")
endif()
foreach(_mib_processing_core_app_version IN ITEMS
        "${_mib_processing_core_app_min_version}"
        "${_mib_processing_core_app_max_version}")
    if(NOT _mib_processing_core_app_version MATCHES
       "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "Processing-core app compatibility bounds must be numeric X.Y.Z versions")
    endif()
endforeach()
if(_mib_processing_core_app_min_version VERSION_GREATER
   _mib_processing_core_app_max_version)
    message(FATAL_ERROR
        "Processing-core app minimum version must not exceed its maximum version")
endif()
set(MIB_PROCESSING_CORE_APP_MIN_VERSION
    "${_mib_processing_core_app_min_version}" CACHE STRING
    "Oldest desktop app version allowed to load a released native processing core" FORCE)
set(MIB_PROCESSING_CORE_APP_MAX_VERSION
    "${_mib_processing_core_app_max_version}" CACHE STRING
    "Newest desktop app version allowed to load a released native processing core" FORCE)
unset(_mib_processing_core_app_min_version)
unset(_mib_processing_core_app_max_version)
unset(_mib_processing_core_app_version)

# Development and fork CI builds may intentionally leave the signer pin empty,
# but any supplied value must still be unambiguous. Official release entry
# points opt into the non-empty requirement before producing an installer.
string(STRIP "${MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256}"
    _mib_processing_core_signer_spki_sha256)
if(_mib_processing_core_signer_spki_sha256)
    string(LENGTH "${_mib_processing_core_signer_spki_sha256}"
        _mib_processing_core_signer_spki_sha256_length)
    if(NOT _mib_processing_core_signer_spki_sha256_length EQUAL 64 OR
       _mib_processing_core_signer_spki_sha256 MATCHES "[^0-9A-Fa-f]")
        message(FATAL_ERROR
            "MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256 must be exactly 64 hexadecimal characters")
    endif()
    string(TOLOWER "${_mib_processing_core_signer_spki_sha256}"
        _mib_processing_core_signer_spki_sha256)
elseif(MIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI)
    message(FATAL_ERROR
        "A production build requires MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256")
endif()
set(MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256
    "${_mib_processing_core_signer_spki_sha256}" CACHE STRING
    "Approved Authenticode signer SubjectPublicKeyInfo SHA-256 for native processing cores"
    FORCE)
unset(_mib_processing_core_signer_spki_sha256)
unset(_mib_processing_core_signer_spki_sha256_length)

# The Linux Ed25519 pin follows the same rules: optional for development and
# fork builds, unambiguous when supplied, and mandatory only for entry points
# that opt into producing a production Linux artifact.
string(STRIP "${MIB_PROCESSING_CORE_ED25519_SPKI_SHA256}"
    _mib_processing_core_ed25519_spki_sha256)
if(_mib_processing_core_ed25519_spki_sha256)
    string(LENGTH "${_mib_processing_core_ed25519_spki_sha256}"
        _mib_processing_core_ed25519_spki_sha256_length)
    if(NOT _mib_processing_core_ed25519_spki_sha256_length EQUAL 64 OR
       _mib_processing_core_ed25519_spki_sha256 MATCHES "[^0-9A-Fa-f]")
        message(FATAL_ERROR
            "MIB_PROCESSING_CORE_ED25519_SPKI_SHA256 must be exactly 64 hexadecimal characters")
    endif()
    string(TOLOWER "${_mib_processing_core_ed25519_spki_sha256}"
        _mib_processing_core_ed25519_spki_sha256)
elseif(MIB_REQUIRE_PROCESSING_CORE_ED25519_SPKI)
    message(FATAL_ERROR
        "A production Linux build requires MIB_PROCESSING_CORE_ED25519_SPKI_SHA256")
endif()
set(MIB_PROCESSING_CORE_ED25519_SPKI_SHA256
    "${_mib_processing_core_ed25519_spki_sha256}" CACHE STRING
    "Approved Ed25519 signer SubjectPublicKeyInfo SHA-256 for Linux native processing cores"
    FORCE)
unset(_mib_processing_core_ed25519_spki_sha256)
unset(_mib_processing_core_ed25519_spki_sha256_length)
