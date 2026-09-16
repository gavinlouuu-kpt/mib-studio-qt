option(MIB_BUILD_SYNC_TUNING "Build the MindVision illumination calibration sampler" ON)
if(MIB_BUILD_SYNC_TUNING)
    add_executable(mib_sync_capture capture.cpp)
    target_link_libraries(mib_sync_capture PRIVATE mib_backend)
    set_target_properties(mib_sync_capture PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
endif()
