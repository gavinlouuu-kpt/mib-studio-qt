function(mib_configure_windows_deployment app_target)
    if(NOT WIN32 OR NOT MIB_ENABLE_WINDOWS_PACKAGING)
        return()
    endif()

    # Find Qt6 installation path from Conan. Qt6_DIR is set by Conan's
    # CMakeDeps generator.
    if(DEFINED Qt6_DIR)
        get_filename_component(QT6_ROOT "${Qt6_DIR}/../.." ABSOLUTE)
    elseif(TARGET Qt6::qmake)
        get_target_property(QT6_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
        if(QT6_QMAKE_EXECUTABLE)
            get_filename_component(QT6_ROOT "${QT6_QMAKE_EXECUTABLE}/../.." ABSOLUTE)
        endif()
    endif()

    # Fallback to CMAKE_PREFIX_PATH if Qt6_DIR not found.
    if(NOT DEFINED QT6_ROOT)
        set(QT6_ROOT "${CMAKE_PREFIX_PATH}")
    endif()

    # Set Qt bin directories (Conan typically installs in CMAKE_PREFIX_PATH).
    # For multi-config builds, use imported targets first (they handle Debug/Release automatically).
    set(CONAN_QT_BIN_DIR "")

    # Try to get path from Qt6::Core imported target (handles multi-config properly).
    if(TARGET Qt6::Core)
        # Try Release location first (more common for deployment).
        get_target_property(_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION_RELEASE)
        if(_QT_CORE_LOCATION)
            get_filename_component(CONAN_QT_BIN_DIR "${_QT_CORE_LOCATION}" DIRECTORY)
            get_filename_component(QT6_ROOT "${CONAN_QT_BIN_DIR}" DIRECTORY)
        else()
            # Try Debug location as fallback.
            get_target_property(_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION_DEBUG)
            if(_QT_CORE_LOCATION)
                get_filename_component(CONAN_QT_BIN_DIR "${_QT_CORE_LOCATION}" DIRECTORY)
                get_filename_component(QT6_ROOT "${CONAN_QT_BIN_DIR}" DIRECTORY)
            endif()
        endif()
    endif()

    # Fallback: search CMAKE_PREFIX_PATH (may only have paths from last conan install).
    if(NOT CONAN_QT_BIN_DIR)
        foreach(_path IN LISTS CMAKE_PREFIX_PATH)
            if(EXISTS "${_path}/Qt6Core.dll" OR EXISTS "${_path}/Qt6Cored.dll")
                set(CONAN_QT_BIN_DIR "${_path}")
                get_filename_component(QT6_ROOT "${_path}" DIRECTORY)
                break()
            endif()
        endforeach()
    endif()

    # Final fallback: use computed path if not found.
    if(NOT CONAN_QT_BIN_DIR)
        if(DEFINED QT6_ROOT)
            set(CONAN_QT_BIN_DIR "${QT6_ROOT}/bin")
        else()
            message(WARNING "Could not determine Qt bin directory - Qt deployment may fail")
        endif()
    endif()

    set(CONAN_QT_DEBUG_BIN_DIR "${CONAN_QT_BIN_DIR}")  # Debug and Release DLLs in same directory.
    set(CONAN_QT_TOOLS_DIR "${QT6_ROOT}/tools/qt6/bin")

    # Find windeployqt: prefer the exact Conan package trees (CMakeDeps sets qt_PACKAGE_FOLDER_*).
    # A cached find_program result can keep pointing at an older qt/*/p folder after `conan install`,
    # which then mismatches the Qt DLLs we link (different package IDs).
    unset(WINDEPLOYQT_RELEASE CACHE)
    unset(WINDEPLOYQT_DEBUG CACHE)
    set(_WINDEPLOYQT_RELEASE_CAND "")
    set(_WINDEPLOYQT_DEBUG_CAND "")
    if(DEFINED qt_PACKAGE_FOLDER_RELEASE AND EXISTS "${qt_PACKAGE_FOLDER_RELEASE}/bin/windeployqt.exe")
        set(_WINDEPLOYQT_RELEASE_CAND "${qt_PACKAGE_FOLDER_RELEASE}/bin/windeployqt.exe")
    endif()
    if(DEFINED qt_PACKAGE_FOLDER_DEBUG AND EXISTS "${qt_PACKAGE_FOLDER_DEBUG}/bin/windeployqt.exe")
        set(_WINDEPLOYQT_DEBUG_CAND "${qt_PACKAGE_FOLDER_DEBUG}/bin/windeployqt.exe")
    endif()
    if(_WINDEPLOYQT_RELEASE_CAND)
        set(WINDEPLOYQT_RELEASE "${_WINDEPLOYQT_RELEASE_CAND}" CACHE FILEPATH "windeployqt for Release deployment" FORCE)
    else()
        find_program(WINDEPLOYQT_RELEASE windeployqt.exe
            PATHS "${CONAN_QT_TOOLS_DIR}" "${CONAN_QT_BIN_DIR}"
            NO_DEFAULT_PATH
        )
        if(NOT WINDEPLOYQT_RELEASE)
            find_program(WINDEPLOYQT_RELEASE windeployqt.exe
                PATHS "${CONAN_QT_BIN_DIR}" "${CMAKE_PREFIX_PATH}/bin"
            )
        endif()
    endif()
    if(_WINDEPLOYQT_DEBUG_CAND)
        set(WINDEPLOYQT_DEBUG "${_WINDEPLOYQT_DEBUG_CAND}" CACHE FILEPATH "windeployqt for Debug deployment" FORCE)
    else()
        find_program(WINDEPLOYQT_DEBUG windeployqt.debug.bat
            PATHS "${CONAN_QT_TOOLS_DIR}" "${CONAN_QT_BIN_DIR}"
            NO_DEFAULT_PATH
        )
    endif()

    # Discover extra third-party codec runtimes used by Qt imageformats plugins (copy after windeployqt).
    # This helps ensure qjpeg/qtiff/qwebp/qjp2 load on machines without Conan packages in PATH.
    set(CONAN_SEARCH_PATHS
        "${CONAN_QT_BIN_DIR}"
        "${CMAKE_PREFIX_PATH}/bin"
        "${CMAKE_BINARY_DIR}/conan/bin"
    )
    set(_EXTRA_CODEC_PATTERNS)
    foreach(_search_path IN LISTS CONAN_SEARCH_PATHS)
        list(APPEND _EXTRA_CODEC_PATTERNS
            "${_search_path}/jpeg*.dll"
            "${_search_path}/libjpeg*.dll"
            "${_search_path}/tiff*.dll"
            "${_search_path}/libtiff*.dll"
            "${_search_path}/liblzma*.dll"
            "${_search_path}/libwebp*.dll"
            "${_search_path}/libwebpdemux*.dll"
            "${_search_path}/libwebpmux*.dll"
            "${_search_path}/libsharpyuv*.dll"
            "${_search_path}/openjp*.dll"
            "${_search_path}/zstd*.dll"
            "${_search_path}/libdeflate*.dll"
        )
    endforeach()
    set(_EXTRA_CODEC_DLLS "")
    foreach(_pat IN LISTS _EXTRA_CODEC_PATTERNS)
        file(GLOB _found "${_pat}")
        list(APPEND _EXTRA_CODEC_DLLS ${_found})
    endforeach()
    list(REMOVE_DUPLICATES _EXTRA_CODEC_DLLS)

    # Use different windeployqt for Debug vs Release.
    if(WINDEPLOYQT_RELEASE)
        # Check if Debug and Release Qt DLLs exist.
        # For multi-config, Debug and Release packages may be in different Conan directories.
        set(_QT_DEBUG_DLL_FOUND FALSE)
        set(_QT_RELEASE_DLL_FOUND FALSE)
        set(_QT_DEBUG_BIN_DIR "")
        set(_QT_RELEASE_BIN_DIR "")

        # Prefer CMakeDeps package roots (unique per build type). Avoids cache-wide globs picking a
        # random Qt6Core.dll when several Conan package folders exist.
        if(DEFINED qt_PACKAGE_FOLDER_DEBUG AND EXISTS "${qt_PACKAGE_FOLDER_DEBUG}/bin/Qt6Cored.dll")
            set(_QT_DEBUG_BIN_DIR "${qt_PACKAGE_FOLDER_DEBUG}/bin")
            set(_QT_DEBUG_DLL_FOUND TRUE)
        endif()
        if(DEFINED qt_PACKAGE_FOLDER_RELEASE AND EXISTS "${qt_PACKAGE_FOLDER_RELEASE}/bin/Qt6Core.dll")
            set(_QT_RELEASE_BIN_DIR "${qt_PACKAGE_FOLDER_RELEASE}/bin")
            set(_QT_RELEASE_DLL_FOUND TRUE)
        endif()

        # Check Debug / Release locations using imported target (non-Conan or unusual layouts).
        if(TARGET Qt6::Core)
            if(NOT _QT_DEBUG_DLL_FOUND)
                get_target_property(_QT_CORE_LOCATION_DEBUG Qt6::Core IMPORTED_LOCATION_DEBUG)
                if(_QT_CORE_LOCATION_DEBUG)
                    get_filename_component(_QT_DEBUG_BIN_DIR "${_QT_CORE_LOCATION_DEBUG}" DIRECTORY)
                    if(EXISTS "${_QT_DEBUG_BIN_DIR}/Qt6Cored.dll")
                        set(_QT_DEBUG_DLL_FOUND TRUE)
                    endif()
                else()
                    get_target_property(_QT_CORE_IMPLIB_DEBUG Qt6::Core IMPORTED_IMPLIB_DEBUG)
                    if(_QT_CORE_IMPLIB_DEBUG)
                        get_filename_component(_QT_DEBUG_LIB_DIR "${_QT_CORE_IMPLIB_DEBUG}" DIRECTORY)
                        get_filename_component(_QT_DEBUG_PREFIX "${_QT_DEBUG_LIB_DIR}" DIRECTORY)
                        set(_QT_DEBUG_BIN_DIR "${_QT_DEBUG_PREFIX}/bin")
                        if(EXISTS "${_QT_DEBUG_BIN_DIR}/Qt6Cored.dll")
                            set(_QT_DEBUG_DLL_FOUND TRUE)
                        endif()
                    endif()
                endif()
            endif()
            if(NOT _QT_RELEASE_DLL_FOUND)
                get_target_property(_QT_CORE_LOCATION_RELEASE Qt6::Core IMPORTED_LOCATION_RELEASE)
                if(_QT_CORE_LOCATION_RELEASE)
                    get_filename_component(_QT_RELEASE_BIN_DIR "${_QT_CORE_LOCATION_RELEASE}" DIRECTORY)
                    if(EXISTS "${_QT_RELEASE_BIN_DIR}/Qt6Core.dll")
                        set(_QT_RELEASE_DLL_FOUND TRUE)
                    endif()
                else()
                    get_target_property(_QT_CORE_IMPLIB_RELEASE Qt6::Core IMPORTED_IMPLIB_RELEASE)
                    if(_QT_CORE_IMPLIB_RELEASE)
                        get_filename_component(_QT_RELEASE_LIB_DIR "${_QT_CORE_IMPLIB_RELEASE}" DIRECTORY)
                        get_filename_component(_QT_RELEASE_PREFIX "${_QT_RELEASE_LIB_DIR}" DIRECTORY)
                        set(_QT_RELEASE_BIN_DIR "${_QT_RELEASE_PREFIX}/bin")
                        if(EXISTS "${_QT_RELEASE_BIN_DIR}/Qt6Core.dll")
                            set(_QT_RELEASE_DLL_FOUND TRUE)
                        endif()
                    endif()
                endif()
            endif()
        endif()

        # Fallback: check CONAN_QT_BIN_DIR (may only have one build_type).
        if(CONAN_QT_BIN_DIR)
            if(NOT _QT_DEBUG_DLL_FOUND AND EXISTS "${CONAN_QT_BIN_DIR}/Qt6Cored.dll")
                set(_QT_DEBUG_DLL_FOUND TRUE)
                set(_QT_DEBUG_BIN_DIR "${CONAN_QT_BIN_DIR}")
            endif()
            if(NOT _QT_RELEASE_DLL_FOUND AND EXISTS "${CONAN_QT_BIN_DIR}/Qt6Core.dll")
                set(_QT_RELEASE_DLL_FOUND TRUE)
                set(_QT_RELEASE_BIN_DIR "${CONAN_QT_BIN_DIR}")
            endif()
        endif()

        # Last resort: search Conan cache (non-deterministic; only when still unknown).
        if(NOT _QT_DEBUG_DLL_FOUND OR NOT _QT_RELEASE_DLL_FOUND)
            set(_CONAN_CACHE_ROOT "$ENV{USERPROFILE}/.conan2")
            if(EXISTS "${_CONAN_CACHE_ROOT}")
                if(NOT _QT_DEBUG_DLL_FOUND)
                    file(GLOB _qt_debug_dll_candidates "${_CONAN_CACHE_ROOT}/p/b/*/p/bin/Qt6Cored.dll")
                    list(LENGTH _qt_debug_dll_candidates _qt_debug_count)
                    if(_qt_debug_count GREATER 0)
                        list(SORT _qt_debug_dll_candidates)
                        list(GET _qt_debug_dll_candidates 0 _qt_debug_dll)
                        get_filename_component(_QT_DEBUG_BIN_DIR "${_qt_debug_dll}" DIRECTORY)
                        set(_QT_DEBUG_DLL_FOUND TRUE)
                    endif()
                endif()
                if(NOT _QT_RELEASE_DLL_FOUND)
                    file(GLOB _qt_release_dll_candidates "${_CONAN_CACHE_ROOT}/p/b/*/p/bin/Qt6Core.dll")
                    list(LENGTH _qt_release_dll_candidates _qt_release_count)
                    if(_qt_release_count GREATER 0)
                        list(SORT _qt_release_dll_candidates)
                        list(GET _qt_release_dll_candidates 0 _qt_release_dll)
                        get_filename_component(_QT_RELEASE_BIN_DIR "${_qt_release_dll}" DIRECTORY)
                        set(_QT_RELEASE_DLL_FOUND TRUE)
                    endif()
                endif()
            endif()
        endif()

        # Warn if required DLLs are missing (for multi-config, both should be available).
        if(NOT _QT_DEBUG_DLL_FOUND)
            if(_QT_DEBUG_BIN_DIR)
                message(WARNING "Qt Debug DLLs not found in ${_QT_DEBUG_BIN_DIR}")
            else()
                message(WARNING "Qt Debug DLLs not found (Debug package location not determined)")
            endif()
            message(WARNING "  Debug builds may fail. Install Debug packages:")
            message(WARNING "  conan install . --output-folder=build --build=missing -s build_type=Debug")
        endif()
        if(NOT _QT_RELEASE_DLL_FOUND)
            if(_QT_RELEASE_BIN_DIR)
                message(WARNING "Qt Release DLLs not found in ${_QT_RELEASE_BIN_DIR}")
            else()
                message(WARNING "Qt Release DLLs not found (Release package location not determined)")
            endif()
            message(WARNING "  Release builds may fail. Install Release packages:")
            message(WARNING "  conan install . --output-folder=build --build=missing -s build_type=Release")
        endif()

        # Use config-specific deployment flags and paths.
        set(_WINDEPLOYQT_DEBUG_EXE "")
        if(_QT_DEBUG_BIN_DIR)
            if(EXISTS "${_QT_DEBUG_BIN_DIR}/windeployqt.debug.bat")
                set(_WINDEPLOYQT_DEBUG_EXE "${_QT_DEBUG_BIN_DIR}/windeployqt.debug.bat")
            elseif(EXISTS "${_QT_DEBUG_BIN_DIR}/windeployqt.exe")
                set(_WINDEPLOYQT_DEBUG_EXE "${_QT_DEBUG_BIN_DIR}/windeployqt.exe")
            endif()
        endif()

        if(_WINDEPLOYQT_DEBUG_EXE)
            set(_WINDEPLOYQT_EXE "$<IF:$<STREQUAL:$<CONFIG>,Debug>,${_WINDEPLOYQT_DEBUG_EXE},${WINDEPLOYQT_RELEASE}>")
        elseif(WINDEPLOYQT_DEBUG)
            set(_WINDEPLOYQT_EXE "$<IF:$<STREQUAL:$<CONFIG>,Debug>,${WINDEPLOYQT_DEBUG},${WINDEPLOYQT_RELEASE}>")
        else()
            # Use windeployqt.exe with --debug/--release flag if windeployqt.debug.bat doesn't exist.
            set(_WINDEPLOYQT_EXE "${WINDEPLOYQT_RELEASE}")
        endif()
        set(_WINDEPLOYQT_FLAG "$<IF:$<STREQUAL:$<CONFIG>,Debug>,--debug,--release>")

        # Use appropriate bin directory for PATH based on configuration.
        # For Debug builds, prefer Debug bin dir; for Release, prefer Release bin dir.
        set(_WINDEPLOYQT_PATH "")
        if(_QT_DEBUG_BIN_DIR AND _QT_RELEASE_BIN_DIR)
            # Both found - use generator expression to select based on config.
            set(_WINDEPLOYQT_PATH "$<IF:$<STREQUAL:$<CONFIG>,Debug>,${_QT_DEBUG_BIN_DIR},${_QT_RELEASE_BIN_DIR}>")
        elseif(_QT_RELEASE_BIN_DIR)
            set(_WINDEPLOYQT_PATH "${_QT_RELEASE_BIN_DIR}")
        elseif(_QT_DEBUG_BIN_DIR)
            set(_WINDEPLOYQT_PATH "${_QT_DEBUG_BIN_DIR}")
        elseif(CONAN_QT_BIN_DIR)
            set(_WINDEPLOYQT_PATH "${CONAN_QT_BIN_DIR}")
        endif()

        add_custom_command(TARGET ${app_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env "PATH=${_WINDEPLOYQT_PATH};$ENV{PATH}"
                    "${_WINDEPLOYQT_EXE}"
                    "${_WINDEPLOYQT_FLAG}"
                    --compiler-runtime
                    "$<TARGET_FILE:${app_target}>"
            COMMENT "windeployqt ($<CONFIG>): ${app_target}"
        )
    else()
        message(WARNING "windeployqt not found - Qt deployment will be skipped")
    endif()

    # Copy extra codec/runtime DLLs if present.
    foreach(_dll IN LISTS _EXTRA_CODEC_DLLS)
        add_custom_command(TARGET ${app_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${app_target}>/"
            VERBATIM
        )
    endforeach()

    # Copy Coremor DLL.
    add_custom_command(TARGET ${app_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/include/Coremor/XMT_DLL_SER.dll"
                "$<TARGET_FILE_DIR:${app_target}>/"
        COMMENT "Copy Coremor DLL: ${app_target}"
    )

    # Copy MindVision SDK runtime DLL when the integration is enabled.
    if(MIB_HAS_MINDVISION)
        add_custom_command(TARGET ${app_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MIB_MINDVISION_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:${app_target}>/"
            COMMENT "Copy MindVision DLL: ${app_target}"
        )
    endif()

    # Copy Conan package DLLs (OpenCV, HDF5, SQLite3, etc.).
    add_custom_command(TARGET ${app_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${app_target}>
                $<TARGET_FILE_DIR:${app_target}>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying Conan package DLLs for ${app_target}"
    )

    # Copy the provisioned YOLO model (env/assets.json -> MIB_YOLO_MODEL_PATH,
    # resolved in MIBDependencies.cmake) to the runtime path AppBackend expects:
    # <exe>/resources/models/yolo11n-seg.onnx.
    if(MIB_HAS_ONNXRUNTIME)
        add_custom_command(TARGET ${app_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${app_target}>/resources/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MIB_YOLO_MODEL_PATH}"
                    "$<TARGET_FILE_DIR:${app_target}>/resources/models/"
            COMMENT "Copying YOLO model asset for ${app_target}"
        )
    endif()

    # crashpad_handler.exe is required next to the application when
    # sentry-native is built with the Crashpad backend (the default on
    # Windows). Without it Sentry falls back to in-process capture and
    # cannot collect dumps for non-recoverable failures.
    if(MIB_SENTRY_AVAILABLE AND TARGET crashpad_handler)
        add_custom_command(TARGET ${app_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:crashpad_handler>"
                    "$<TARGET_FILE_DIR:${app_target}>/"
            COMMENT "Copying crashpad_handler.exe for ${app_target}"
        )
    endif()
endfunction()
