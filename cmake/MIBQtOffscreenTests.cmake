# Windows: let the headless Qt frontend tests find the "offscreen" platform
# plugin. The tests force QT_QPA_PLATFORM=offscreen so they run without a
# display on every host, but windeployqt only ships qwindows.dll next to the
# binaries, so without this every Qt-widget test fail-fast crashes
# (0xc0000409) on Windows and the crash dialog then holds the dead process
# until the CTest timeout. Point Qt at the Conan package's own plugin
# directory (per configuration) instead of copying qoffscreen.dll into the
# packaged app. The offscreen platform also has no font database on Windows
# (Qt ships none), which inflates every text metric and makes the layout
# budget assertions fail, so it gets the system font directory too. Call at
# the end of any CMakeLists.txt that registers frontend.* tests (the TESTS
# directory property is per directory).
function(mib_apply_qt_offscreen_plugin_path)
    if(NOT WIN32 OR NOT DEFINED qt_PACKAGE_FOLDER_RELEASE)
        return()
    endif()
    set(_release "${qt_PACKAGE_FOLDER_RELEASE}/plugins/platforms")
    if(DEFINED qt_PACKAGE_FOLDER_DEBUG)
        set(_debug "${qt_PACKAGE_FOLDER_DEBUG}/plugins/platforms")
    else()
        set(_debug "${_release}")
    endif()
    get_directory_property(_tests TESTS)
    foreach(_t IN LISTS _tests)
        if(_t MATCHES "^frontend\.")
            set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
                "QT_QPA_PLATFORM_PLUGIN_PATH=$<IF:$<CONFIG:Debug>,${_debug},${_release}>"
                "QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts")
        endif()
    endforeach()
endfunction()
