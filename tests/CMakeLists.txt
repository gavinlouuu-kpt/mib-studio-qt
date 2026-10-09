find_package(Python3 COMPONENTS Interpreter QUIET)

if(WIN32)
    add_executable(serial_port_win32_timeout_test backend/serial_port_win32_timeout_test.cpp)
    target_include_directories(serial_port_win32_timeout_test PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(serial_port_win32_timeout_test PRIVATE setupapi advapi32)
    add_test(NAME backend.serial_port_win32_timeout COMMAND serial_port_win32_timeout_test)
    set_tests_properties(backend.serial_port_win32_timeout PROPERTIES LABELS "backend;serial;fault-injection" TIMEOUT 15)
endif()

# Catch a MindVision-only Windows build silently substituting the nanopositioner
# stub merely because the EGrabber SDK is disabled.
if(WIN32 AND MIB_ENABLE_COREMOR)
    get_target_property(_backend_sources mib_backend SOURCES)
    if(NOT "${PROJECT_SOURCE_DIR}/src/backend/services/AutofocusService.cpp" IN_LIST _backend_sources)
        message(FATAL_ERROR "Coremor enabled, but the real nanopositioner service is missing")
    endif()
    get_target_property(_backend_links mib_backend LINK_LIBRARIES)
    if(NOT "${PROJECT_SOURCE_DIR}/include/Coremor/XMT_DLL_SER.lib" IN_LIST _backend_links)
        message(FATAL_ERROR "Coremor enabled, but its SDK is not linked")
    endif()
endif()

# Backend tests are compiled into one runner, mib_backend_tests (see
# cmake/MIBTestRunner.cmake for the how and why). CTest entries run
# `mib_backend_tests <target> [args]`, one process per test. Tests that must
# stay their own executable are listed here with the reason.
include(MIBTestRunner)
set(MIB_STANDALONE_BACKEND_TESTS
    processing_core_authenticode_test   # Windows signing workflow invokes this executable directly
    mib_backend_smoke_test              # tools/gen_bridge_link_manifest.py uses its .vcxproj as reference project
    hdf_export_service_test             # exporter-soak.yml builds and runs it directly
    fcs_writer_test                     # standalone FCS binary/layout checks
    crash_reporter_segv_test            # re-executes argv[0] --crash-child
    crash_reporter_terminate_test       # re-executes argv[0]
    hdf5_exit_teardown_test             # std::exit with a file open IS the test
    kin6_batch_pipeline_evidence        # uses argv[0]
    kin10_hf_dataset_pipeline_test      # uses argv[0]; the harness passes the binary path
    processing_core_loader_test         # add_dependencies on the plugin fixtures
    processing_core_v2_plugin_test      # add_dependencies on the plugin (Contract v2 dlopen test)
    processing_core_contract2_equivalence_test # add_dependencies on the Contract-2 plugin
    processing_core_fixture_matrix_test # add_dependencies on the plugin fixtures
    processing_core_cache_test          # add_dependencies on the plugin fixtures
    processing_core_ed25519_test        # Linux-only; add_dependencies on the plugin + OpenSSL link
)

function(mib_add_backend_test_executable target source)
    if(target IN_LIST MIB_STANDALONE_BACKEND_TESTS)
        add_executable(${target}
            ${PROJECT_SOURCE_DIR}/tests/${source}
        )
        target_link_libraries(${target} PRIVATE mib_backend)
        # Allow tests to include the shared support headers as "support/<x>.h".
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
        )
        return()
    endif()
    mib_test_runner_add(mib_backend_tests ${target} ${PROJECT_SOURCE_DIR}/tests/${source})
endfunction()

# target_link_libraries() for a backend test: standalone targets link
# directly, consolidated ones put the libraries on the runner.
function(mib_backend_test_link target)
    if(target IN_LIST MIB_STANDALONE_BACKEND_TESTS)
        target_link_libraries(${target} ${ARGN})
    else()
        mib_test_runner_link(mib_backend_tests ${ARGN})
    endif()
endfunction()

if(MIB_HAS_ARAVIS)
    find_package(Threads REQUIRED)
    mib_add_backend_test_executable(aravis_camera_test camera/aravis_camera_test.cpp)
    add_test(NAME camera.aravis_fake COMMAND $<TARGET_FILE:mib_backend_tests> aravis_camera_test)
    set_tests_properties(camera.aravis_fake PROPERTIES LABELS "camera;aravis;concurrency" TIMEOUT 30)
    mib_add_backend_test_executable(aravis_capture_lifecycle_test backend/aravis_capture_lifecycle_test.cpp)
    mib_backend_test_link(aravis_capture_lifecycle_test PRIVATE Threads::Threads)
    add_test(NAME backend.aravis_capture_lifecycle
             COMMAND $<TARGET_FILE:mib_backend_tests> aravis_capture_lifecycle_test)
    set_tests_properties(backend.aravis_capture_lifecycle PROPERTIES
                         LABELS "backend;camera;aravis;concurrency;stress" TIMEOUT 30)
    mib_add_backend_test_executable(aravis_camera_stress_test camera/aravis_camera_stress_test.cpp)
    mib_backend_test_link(aravis_camera_stress_test PRIVATE Threads::Threads)
    add_test(NAME camera.aravis_stress COMMAND $<TARGET_FILE:mib_backend_tests> aravis_camera_stress_test)
    set_tests_properties(camera.aravis_stress PROPERTIES LABELS "camera;aravis;concurrency;stress" TIMEOUT 90)
    # The PZ7035 GenTL producer's pattern device (pz7035-imx426 gentl/); opt-in
    # because the producer lives in another repository.
    set(MIB_PZ7035_GENTL_CTI "" CACHE FILEPATH "PZ7035 GenTL producer (.cti) for camera.aravis_pz7035_pattern")
    mib_add_backend_test_executable(aravis_pz7035_pattern_test camera/aravis_pz7035_pattern_test.cpp)
    if(MIB_PZ7035_GENTL_CTI)
        get_filename_component(_mib_pz7035_cti_dir "${MIB_PZ7035_GENTL_CTI}" DIRECTORY)
        add_test(NAME camera.aravis_pz7035_pattern
                 COMMAND $<TARGET_FILE:mib_backend_tests> aravis_pz7035_pattern_test)
        set_tests_properties(camera.aravis_pz7035_pattern PROPERTIES
                             LABELS "camera;aravis;pz7035" TIMEOUT 90
                             ENVIRONMENT "GENICAM_GENTL64_PATH=${_mib_pz7035_cti_dir};PZ_GENTL_PATTERN=1")
    endif()
    mib_add_backend_test_executable(aravis_camera_overview_test backend/aravis_camera_overview_test.cpp)
    add_test(NAME backend.aravis_camera_overview
             COMMAND $<TARGET_FILE:mib_backend_tests> aravis_camera_overview_test)
    set_tests_properties(backend.aravis_camera_overview PROPERTIES LABELS "backend;camera;aravis" TIMEOUT 60)
    mib_add_backend_test_executable(aravis_pipeline_e2e integration/aravis_pipeline_e2e_test.cpp)
    mib_backend_test_link(aravis_pipeline_e2e PRIVATE Threads::Threads)
    add_test(NAME integration.aravis_pipeline_e2e
             COMMAND $<TARGET_FILE:mib_backend_tests> aravis_pipeline_e2e)
    set_tests_properties(integration.aravis_pipeline_e2e PROPERTIES
                         LABELS "backend;camera;aravis;integration;e2e;concurrency"
                         TIMEOUT 90)
endif()

# Explicit Aravis source selection is validated in both ON and OFF builds:
# OFF must remain visibly unavailable instead of silently falling back.
mib_add_backend_test_executable(aravis_appbackend_source_test backend/aravis_appbackend_source_test.cpp)
add_test(NAME backend.aravis_appbackend_source COMMAND $<TARGET_FILE:mib_backend_tests> aravis_appbackend_source_test)
set_tests_properties(backend.aravis_appbackend_source PROPERTIES LABELS "backend;aravis" TIMEOUT 60)

mib_add_backend_test_executable(mib_backend_smoke_test backend/backend_smoke_test.cpp)
add_test(NAME backend.smoke COMMAND $<TARGET_FILE:mib_backend_smoke_test>)
set_tests_properties(backend.smoke PROPERTIES LABELS "backend")

mib_add_backend_test_executable(emodulus_lut_catalog_test backend/emodulus_lut_catalog_test.cpp)
add_test(NAME backend.emodulus_lut_catalog COMMAND $<TARGET_FILE:mib_backend_tests> emodulus_lut_catalog_test)
set_tests_properties(backend.emodulus_lut_catalog PROPERTIES LABELS "backend;processing")

mib_add_backend_test_executable(emodulus_lut_degenerate_test backend/emodulus_lut_degenerate_test.cpp)
add_test(NAME backend.emodulus_lut_degenerate COMMAND $<TARGET_FILE:mib_backend_tests> emodulus_lut_degenerate_test)
set_tests_properties(backend.emodulus_lut_degenerate PROPERTIES LABELS "backend;processing")

mib_add_backend_test_executable(pl_science_test backend/pl_science_test.cpp)
add_test(NAME backend.pl_science COMMAND $<TARGET_FILE:mib_backend_tests> pl_science_test)
set_tests_properties(backend.pl_science PROPERTIES LABELS "backend;pl" TIMEOUT 60)
mib_add_backend_test_executable(instrument_modes_test backend/instrument_modes_test.cpp)
mib_backend_test_link(instrument_modes_test PRIVATE Threads::Threads)
add_test(NAME backend.instrument_modes COMMAND $<TARGET_FILE:mib_backend_tests> instrument_modes_test)
set_tests_properties(backend.instrument_modes PROPERTIES LABELS "backend;concurrency;pl" TIMEOUT 90)
mib_add_backend_test_executable(pl_science_provider_test backend/pl_science_provider_test.cpp)
mib_backend_test_link(pl_science_provider_test PRIVATE Threads::Threads)
add_test(NAME backend.pl_science_provider COMMAND $<TARGET_FILE:mib_backend_tests> pl_science_provider_test
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(backend.pl_science_provider PROPERTIES LABELS "backend;recording;concurrency;pl" TIMEOUT 120)
mib_add_backend_test_executable(backend_lifecycle_smoke_test backend/backend_lifecycle_smoke_test.cpp)
mib_add_backend_test_executable(hardware_shutdown_test backend/hardware_shutdown_test.cpp)
add_test(NAME backend.hardware_shutdown COMMAND $<TARGET_FILE:mib_backend_tests> hardware_shutdown_test)
set_tests_properties(backend.hardware_shutdown PROPERTIES LABELS "backend;concurrency;stress" TIMEOUT 60)
add_test(NAME backend.lifecycle_smoke COMMAND $<TARGET_FILE:mib_backend_tests> backend_lifecycle_smoke_test)
set_tests_properties(backend.lifecycle_smoke PROPERTIES LABELS "backend")

mib_add_backend_test_executable(opencv_threads_test backend/opencv_threads_test.cpp)
add_test(NAME backend.opencv_threads COMMAND $<TARGET_FILE:mib_backend_tests> opencv_threads_test)
set_tests_properties(backend.opencv_threads PROPERTIES LABELS "backend;processing" TIMEOUT 120)

mib_add_backend_test_executable(frame_store_concurrency_test backend/frame_store_concurrency_test.cpp)
find_package(Threads REQUIRED)
mib_backend_test_link(frame_store_concurrency_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_concurrency COMMAND $<TARGET_FILE:mib_backend_tests> frame_store_concurrency_test)
set_tests_properties(backend.frame_store_concurrency PROPERTIES LABELS "backend")

mib_add_backend_test_executable(frame_store_bounds_test backend/frame_store_bounds_test.cpp)
add_test(NAME backend.frame_store_bounds COMMAND $<TARGET_FILE:mib_backend_tests> frame_store_bounds_test)
set_tests_properties(backend.frame_store_bounds PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(modbus_rtu_test backend/modbus_rtu_test.cpp)
add_test(NAME backend.modbus_rtu COMMAND $<TARGET_FILE:mib_backend_tests> modbus_rtu_test)
set_tests_properties(backend.modbus_rtu PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(syringe_pump_fake_serial_test backend/syringe_pump_fake_serial_test.cpp)
add_test(NAME backend.syringe_pump_fake_serial COMMAND $<TARGET_FILE:mib_backend_tests> syringe_pump_fake_serial_test)
set_tests_properties(backend.syringe_pump_fake_serial PROPERTIES LABELS "backend" TIMEOUT 30)

# Tushui peristaltic pump in a pump slot: read-only connect, flow -> rpm,
# continuous run, purge restore, volume integration (TushuiPumpProtocol.h).
mib_add_backend_test_executable(peristaltic_pump_fake_serial_test backend/peristaltic_pump_fake_serial_test.cpp)
add_test(NAME backend.peristaltic_pump_fake_serial COMMAND $<TARGET_FILE:mib_backend_tests> peristaltic_pump_fake_serial_test)
set_tests_properties(backend.peristaltic_pump_fake_serial PROPERTIES LABELS "backend" TIMEOUT 30)

# BE-7 (#277): facade PumpCommand surface over the fake Modbus serial — both
# pump identities, structured errors, COM-port conflict rules, safe disconnect.
mib_add_backend_test_executable(pump_bridge_facade_test backend/pump_bridge_facade_test.cpp)
add_test(NAME backend.pump_bridge_facade COMMAND $<TARGET_FILE:mib_backend_tests> pump_bridge_facade_test)
set_tests_properties(backend.pump_bridge_facade PROPERTIES LABELS "backend;bridge" TIMEOUT 60)

mib_add_backend_test_executable(serial_port_posix_loopback_test backend/serial_port_posix_loopback_test.cpp)
add_test(NAME backend.serial_port_posix_loopback COMMAND $<TARGET_FILE:mib_backend_tests> serial_port_posix_loopback_test)
set_tests_properties(backend.serial_port_posix_loopback PROPERTIES LABELS "backend" TIMEOUT 30)

# Qt-free serial port enumeration (SetupAPI / sysfs) behind serialbus::availablePorts().
mib_add_backend_test_executable(serial_port_enumeration_test backend/serial_port_enumeration_test.cpp)
add_test(NAME backend.serial_port_enumeration COMMAND $<TARGET_FILE:mib_backend_tests> serial_port_enumeration_test)
set_tests_properties(backend.serial_port_enumeration PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(autofocus_math_test backend/autofocus_math_test.cpp)
mib_add_backend_test_executable(nanopositioner_discovery_test backend/nanopositioner_discovery_test.cpp)
add_test(NAME backend.nanopositioner_discovery COMMAND $<TARGET_FILE:mib_backend_tests> nanopositioner_discovery_test)
set_tests_properties(backend.nanopositioner_discovery PROPERTIES LABELS "backend;concurrency;stress" TIMEOUT 30)
add_test(NAME backend.autofocus_math COMMAND $<TARGET_FILE:mib_backend_tests> autofocus_math_test)
set_tests_properties(backend.autofocus_math PROPERTIES LABELS "backend" TIMEOUT 30)

# Issue #419: backend device-discovery job service over fake providers.
mib_add_backend_test_executable(device_discovery_service_test backend/device_discovery_service_test.cpp)
mib_backend_test_link(device_discovery_service_test PRIVATE Threads::Threads)
add_test(NAME backend.device_discovery_service COMMAND $<TARGET_FILE:mib_backend_tests> device_discovery_service_test)
set_tests_properties(backend.device_discovery_service PROPERTIES LABELS "backend;discovery;concurrency;stress" TIMEOUT 60)
mib_add_backend_test_executable(device_discovery_fault_test backend/device_discovery_fault_test.cpp)
mib_backend_test_link(device_discovery_fault_test PRIVATE Threads::Threads)
add_test(NAME backend.device_discovery_fault COMMAND $<TARGET_FILE:mib_backend_tests> device_discovery_fault_test)
set_tests_properties(backend.device_discovery_fault PROPERTIES LABELS "backend;discovery;fault-injection" TIMEOUT 60)
mib_add_backend_test_executable(startup_discovery_policy_test backend/startup_discovery_policy_test.cpp)
mib_backend_test_link(startup_discovery_policy_test PRIVATE Threads::Threads)
add_test(NAME backend.startup_discovery_policy COMMAND $<TARGET_FILE:mib_backend_tests> startup_discovery_policy_test)
set_tests_properties(backend.startup_discovery_policy PROPERTIES LABELS "backend;discovery;concurrency" TIMEOUT 60)
mib_add_backend_test_executable(device_discovery_providers_test backend/device_discovery_providers_test.cpp)
mib_backend_test_link(device_discovery_providers_test PRIVATE Threads::Threads)
add_test(NAME backend.device_discovery_providers COMMAND $<TARGET_FILE:mib_backend_tests> device_discovery_providers_test)
set_tests_properties(backend.device_discovery_providers PROPERTIES LABELS "backend;discovery;nanopositioner;serial" TIMEOUT 60)
mib_add_backend_test_executable(device_discovery_stress_test backend/device_discovery_stress_test.cpp)
mib_backend_test_link(device_discovery_stress_test PRIVATE Threads::Threads)
add_test(NAME backend.device_discovery_stress COMMAND $<TARGET_FILE:mib_backend_tests> device_discovery_stress_test)
set_tests_properties(backend.device_discovery_stress PROPERTIES LABELS "backend;discovery;concurrency;stress" TIMEOUT 120)
mib_add_backend_test_executable(discovery_facade_test backend/discovery_facade_test.cpp)
mib_backend_test_link(discovery_facade_test PRIVATE Threads::Threads)
add_test(NAME backend.discovery_facade COMMAND $<TARGET_FILE:mib_backend_tests> discovery_facade_test)
set_tests_properties(backend.discovery_facade PROPERTIES LABELS "backend;discovery;bridge" TIMEOUT 90)

mib_add_backend_test_executable(zc300_protocol_test backend/zc300_protocol_test.cpp)
mib_backend_test_link(zc300_protocol_test PRIVATE stage_zc300_protocol)
add_test(NAME backend.zc300_protocol COMMAND $<TARGET_FILE:mib_backend_tests> zc300_protocol_test)
set_tests_properties(backend.zc300_protocol PROPERTIES LABELS "backend;stage" TIMEOUT 30)

mib_add_backend_test_executable(zc300_stage_test backend/zc300_stage_test.cpp)
mib_backend_test_link(zc300_stage_test PRIVATE stage_zc300)
add_test(NAME backend.zc300_stage COMMAND $<TARGET_FILE:mib_backend_tests> zc300_stage_test)
set_tests_properties(backend.zc300_stage PROPERTIES LABELS "backend;stage;safety" TIMEOUT 90)
# The same test pinned to two cores: the scheduling under which a tight
# status poller starved a move for 60 s on PR #511's TSan runner. Keeps the
# priority-access fix (Zc300Stage::Access) under that pressure on every run.
find_program(MIB_TASKSET_EXECUTABLE taskset)
if(UNIX AND NOT APPLE AND MIB_TASKSET_EXECUTABLE)
    add_test(NAME backend.zc300_stage_two_cores
             COMMAND ${MIB_TASKSET_EXECUTABLE} -c 0,1 $<TARGET_FILE:mib_backend_tests> zc300_stage_test)
    set_tests_properties(backend.zc300_stage_two_cores PROPERTIES LABELS "backend;stage;safety;concurrency" TIMEOUT 120)
endif()

mib_add_backend_test_executable(stage_bridge_facade_test backend/stage_bridge_facade_test.cpp)
mib_backend_test_link(stage_bridge_facade_test PRIVATE Threads::Threads)
add_test(NAME backend.stage_bridge_facade COMMAND $<TARGET_FILE:mib_backend_tests> stage_bridge_facade_test)
set_tests_properties(backend.stage_bridge_facade PROPERTIES LABELS "backend;stage;bridge;safety" TIMEOUT 180)

mib_add_backend_test_executable(stage_limit_verification_test backend/stage_limit_verification_test.cpp)
mib_backend_test_link(stage_limit_verification_test PRIVATE Threads::Threads)
add_test(NAME backend.stage_limit_verification COMMAND $<TARGET_FILE:mib_backend_tests> stage_limit_verification_test)
set_tests_properties(backend.stage_limit_verification PROPERTIES LABELS "backend;stage;safety" TIMEOUT 180)

mib_add_backend_test_executable(stage_config_test backend/stage_config_test.cpp)
add_test(NAME backend.stage_config COMMAND $<TARGET_FILE:mib_backend_tests> stage_config_test)
set_tests_properties(backend.stage_config PROPERTIES LABELS "backend;stage" TIMEOUT 30)

mib_add_backend_test_executable(stage_service_test backend/stage_service_test.cpp)
mib_backend_test_link(stage_service_test PRIVATE Threads::Threads)
add_test(NAME backend.stage_service COMMAND $<TARGET_FILE:mib_backend_tests> stage_service_test)
set_tests_properties(backend.stage_service PROPERTIES LABELS "backend;stage;safety" TIMEOUT 120)

mib_add_backend_test_executable(stage_service_stress_test backend/stage_service_stress_test.cpp)
mib_backend_test_link(stage_service_stress_test PRIVATE Threads::Threads)
add_test(NAME backend.stage_service_stress COMMAND $<TARGET_FILE:mib_backend_tests> stage_service_stress_test)
set_tests_properties(backend.stage_service_stress PROPERTIES LABELS "backend;stage;concurrency;stress" TIMEOUT 180)

mib_add_backend_test_executable(oeabt_protocol_test backend/oeabt_protocol_test.cpp)
add_test(NAME backend.oeabt_protocol COMMAND $<TARGET_FILE:mib_backend_tests> oeabt_protocol_test)
set_tests_properties(backend.oeabt_protocol PROPERTIES LABELS "backend;nanopositioner" TIMEOUT 30)

mib_add_backend_test_executable(autofocus_backend_safety_test backend/autofocus_backend_safety_test.cpp)
mib_backend_test_link(autofocus_backend_safety_test PRIVATE Threads::Threads)
add_test(NAME backend.autofocus_backend_safety COMMAND $<TARGET_FILE:mib_backend_tests> autofocus_backend_safety_test)
set_tests_properties(backend.autofocus_backend_safety PROPERTIES
    LABELS "backend;nanopositioner;concurrency"
    TIMEOUT 30)

if(UNIX AND NOT APPLE)
    mib_add_backend_test_executable(oeabt_serial_pty_test backend/oeabt_serial_pty_test.cpp)
    mib_backend_test_link(oeabt_serial_pty_test PRIVATE Threads::Threads)
    add_test(NAME backend.oeabt_serial_pty COMMAND $<TARGET_FILE:mib_backend_tests> oeabt_serial_pty_test)
    set_tests_properties(backend.oeabt_serial_pty PROPERTIES
        LABELS "backend;nanopositioner;concurrency"
        TIMEOUT 30)
endif()
mib_add_backend_test_executable(autofocus_focus_score_test backend/autofocus_focus_score_test.cpp)
add_test(NAME backend.autofocus_focus_score COMMAND $<TARGET_FILE:mib_backend_tests> autofocus_focus_score_test)
set_tests_properties(backend.autofocus_focus_score PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(autofocus_focus_feed_test backend/autofocus_focus_feed_test.cpp)
mib_backend_test_link(autofocus_focus_feed_test PRIVATE Threads::Threads)
add_test(NAME backend.autofocus_focus_feed COMMAND $<TARGET_FILE:mib_backend_tests> autofocus_focus_feed_test)
set_tests_properties(backend.autofocus_focus_feed PROPERTIES LABELS "backend;stress" TIMEOUT 60)

mib_add_backend_test_executable(hdf_write_queue_test backend/hdf_write_queue_test.cpp)
mib_backend_test_link(hdf_write_queue_test PRIVATE Threads::Threads)
add_test(NAME backend.hdf_write_queue COMMAND $<TARGET_FILE:mib_backend_tests> hdf_write_queue_test)
set_tests_properties(backend.hdf_write_queue PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(frame_store_reserve_test backend/frame_store_reserve_test.cpp)
add_test(NAME backend.frame_store_reserve COMMAND $<TARGET_FILE:mib_backend_tests> frame_store_reserve_test)
set_tests_properties(backend.frame_store_reserve PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(recording_isframeempty_roi_test backend/recording_isframeempty_roi_test.cpp)
add_test(NAME backend.recording_isframeempty_roi COMMAND $<TARGET_FILE:mib_backend_tests> recording_isframeempty_roi_test)
set_tests_properties(backend.recording_isframeempty_roi PROPERTIES LABELS "backend;processing;recording" TIMEOUT 30)

# Monitoring tab ROI crop is clamped (cv::Mat ROI assertion aborts on 1.0.7).
mib_add_backend_test_executable(monitoring_roi_crop_test frontend/monitoring_roi_crop_test.cpp)
add_test(NAME frontend.monitoring_roi_crop COMMAND $<TARGET_FILE:mib_backend_tests> monitoring_roi_crop_test)
set_tests_properties(frontend.monitoring_roi_crop PROPERTIES LABELS "frontend;safety" TIMEOUT 30)

# Monitoring scatter density (KDE): invariants, per-axis bandwidth, degenerate
# input, colour ramp and a ratio-gated cost budget (no Qt).
mib_add_backend_test_executable(monitoring_density_test processing/monitoring_density_test.cpp)
add_test(NAME processing.monitoring_density COMMAND $<TARGET_FILE:mib_backend_tests> monitoring_density_test)
set_tests_properties(processing.monitoring_density PROPERTIES LABELS "processing;monitoring;performance" TIMEOUT 60)

# MonitoringDensityService (backend-owned live KDE): settings, lowest-priority
# worker, fingerprint skip, load back-off, compute budget, record sink, stop,
# and concurrent callers (TSan lane).
mib_add_backend_test_executable(monitoring_density_service_test backend/monitoring_density_service_test.cpp)
add_test(NAME backend.monitoring_density_service COMMAND $<TARGET_FILE:mib_backend_tests> monitoring_density_service_test)
set_tests_properties(backend.monitoring_density_service PROPERTIES LABELS "backend;monitoring;concurrency;safety" TIMEOUT 120)

# The density worker never costs the processing pipeline throughput: real
# computeProcessedFrame on every core, density service off vs on (ratio gate),
# duty cycle within the compute budget when uncontended.
mib_add_backend_test_executable(monitoring_density_contention_test performance/monitoring_density_contention_test.cpp)
add_test(NAME performance.monitoring_density_contention COMMAND $<TARGET_FILE:mib_backend_tests> monitoring_density_contention_test)
set_tests_properties(performance.monitoring_density_contention PROPERTIES LABELS "backend;monitoring;performance" TIMEOUT 120)

mib_add_backend_test_executable(pr4_monitoring_snapshot_test backend/pr4_monitoring_snapshot_test.cpp)
add_test(NAME backend.pr4_monitoring_snapshot COMMAND $<TARGET_FILE:mib_backend_tests> pr4_monitoring_snapshot_test)
set_tests_properties(backend.pr4_monitoring_snapshot PROPERTIES LABELS "backend;processing;monitoring" TIMEOUT 30)

mib_add_backend_test_executable(roi_crop_test backend/roi_crop_test.cpp)
add_test(NAME backend.roi_crop COMMAND $<TARGET_FILE:mib_backend_tests> roi_crop_test)
set_tests_properties(backend.roi_crop PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(pulse_generator_frame_test backend/pulse_generator_frame_test.cpp)
add_test(NAME backend.pulse_generator_frame COMMAND $<TARGET_FILE:mib_backend_tests> pulse_generator_frame_test)
set_tests_properties(backend.pulse_generator_frame PROPERTIES LABELS "backend" TIMEOUT 30)

# Shared RS485 bus + pulse-generator discovery/control over a POSIX pty
# (compiles to a trivial pass on Windows).
mib_add_backend_test_executable(serial_bus_pty_test backend/serial_bus_pty_test.cpp)
mib_backend_test_link(serial_bus_pty_test PRIVATE Threads::Threads)
add_test(NAME backend.serial_bus_pty COMMAND $<TARGET_FILE:mib_backend_tests> serial_bus_pty_test)
set_tests_properties(backend.serial_bus_pty PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(mindvision_overview_mode_test backend/mindvision_overview_mode_test.cpp)
mib_backend_test_link(mindvision_overview_mode_test PRIVATE Threads::Threads)
add_test(NAME backend.mindvision_overview_mode COMMAND $<TARGET_FILE:mib_backend_tests> mindvision_overview_mode_test)
set_tests_properties(backend.mindvision_overview_mode PROPERTIES LABELS "backend;camera;safety;stress;invariant" TIMEOUT 100)

mib_add_backend_test_executable(mindvision_config_test backend/mindvision_config_test.cpp)
add_test(NAME backend.mindvision_config COMMAND $<TARGET_FILE:mib_backend_tests> mindvision_config_test)
set_tests_properties(backend.mindvision_config PROPERTIES LABELS "backend;camera" TIMEOUT 30)

# Issue #366: MindVision Mono8/geometry fail-closed conversion, driven through
# the injectable SDK seam (no hardware, no vendor headers).
mib_add_backend_test_executable(illuminated_live_test backend/illuminated_live_test.cpp)
add_test(NAME backend.illuminated_live COMMAND $<TARGET_FILE:mib_backend_tests> illuminated_live_test)
set_tests_properties(backend.illuminated_live PROPERTIES LABELS "backend;camera;concurrency;safety" TIMEOUT 70)

mib_add_backend_test_executable(mindvision_conversion_fault_test backend/mindvision_conversion_fault_test.cpp)
mib_backend_test_link(mindvision_conversion_fault_test PRIVATE Threads::Threads)
add_test(NAME backend.mindvision_conversion_fault COMMAND $<TARGET_FILE:mib_backend_tests> mindvision_conversion_fault_test)
set_tests_properties(backend.mindvision_conversion_fault PROPERTIES LABELS "backend;camera;safety" TIMEOUT 60)

# Issue #365: single-owner capture lifecycle (start/stop/fault/restart,
# blocked grab, slow teardown, stress) and trigger session ownership.
mib_add_backend_test_executable(capture_lifecycle_test backend/capture_lifecycle_test.cpp)
mib_backend_test_link(capture_lifecycle_test PRIVATE Threads::Threads)
add_test(NAME backend.capture_lifecycle COMMAND $<TARGET_FILE:mib_backend_tests> capture_lifecycle_test)
set_tests_properties(backend.capture_lifecycle PROPERTIES LABELS "backend;camera;stress" TIMEOUT 120)

mib_add_backend_test_executable(trigger_session_test backend/trigger_session_test.cpp)
mib_backend_test_link(trigger_session_test PRIVATE Threads::Threads)
add_test(NAME backend.trigger_session COMMAND $<TARGET_FILE:mib_backend_tests> trigger_session_test)
set_tests_properties(backend.trigger_session PROPERTIES LABELS "backend;camera" TIMEOUT 60)

# TriggerService canonical pulse records (/trigger_events source): one record
# per request with its real outcome, bounded buffer, loopback edge pairing.
mib_add_backend_test_executable(trigger_event_log_test backend/trigger_event_log_test.cpp)
mib_backend_test_link(trigger_event_log_test PRIVATE Threads::Threads)
add_test(NAME backend.trigger_event_log COMMAND $<TARGET_FILE:mib_backend_tests> trigger_event_log_test)
set_tests_properties(backend.trigger_event_log PROPERTIES LABELS "backend;camera;trigger" TIMEOUT 60)

# RF sort generator (SSG3021X) SCPI service: identify-first, readback,
# preflight, verified writes, link faults, real LAN transport over loopback.
mib_add_backend_test_executable(rf_generator_service_test backend/rf_generator_service_test.cpp)
mib_backend_test_link(rf_generator_service_test PRIVATE Threads::Threads)
add_test(NAME backend.rf_generator_service COMMAND $<TARGET_FILE:mib_backend_tests> rf_generator_service_test)
set_tests_properties(backend.rf_generator_service PROPERTIES LABELS "backend;trigger;safety" TIMEOUT 60)

# Issue #368: timestamp semantics + per-metric telemetry validity/freshness.
mib_add_backend_test_executable(timestamp_telemetry_test backend/timestamp_telemetry_test.cpp)
mib_backend_test_link(timestamp_telemetry_test PRIVATE Threads::Threads)
add_test(NAME backend.timestamp_telemetry COMMAND $<TARGET_FILE:mib_backend_tests> timestamp_telemetry_test)
set_tests_properties(backend.timestamp_telemetry PROPERTIES LABELS "backend;camera;recording;hdf5" TIMEOUT 120)

mib_add_backend_test_executable(experiment_readiness_test backend/experiment_readiness_test.cpp)
mib_backend_test_link(experiment_readiness_test PRIVATE Threads::Threads)
add_test(NAME backend.experiment_readiness COMMAND $<TARGET_FILE:mib_backend_tests> experiment_readiness_test)
set_tests_properties(backend.experiment_readiness PROPERTIES LABELS "backend;processing;recording;hdf5;safety" TIMEOUT 180)

mib_add_backend_test_executable(camera_script_apply_test backend/camera_script_apply_test.cpp)
add_test(NAME backend.camera_script_apply COMMAND $<TARGET_FILE:mib_backend_tests> camera_script_apply_test)
set_tests_properties(backend.camera_script_apply PROPERTIES LABELS "backend;camera" TIMEOUT 30)

mib_add_backend_test_executable(frame_store_resize_under_load_test backend/frame_store_resize_under_load_test.cpp)
mib_backend_test_link(frame_store_resize_under_load_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_resize_under_load COMMAND $<TARGET_FILE:mib_backend_tests> frame_store_resize_under_load_test)
set_tests_properties(backend.frame_store_resize_under_load PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(pipeline_timing_benchmark performance/pipeline_timing_benchmark.cpp)
mib_backend_test_link(pipeline_timing_benchmark PRIVATE Threads::Threads)
add_test(NAME performance.pipeline_timing COMMAND $<TARGET_FILE:mib_backend_tests> pipeline_timing_benchmark)
set_tests_properties(performance.pipeline_timing PROPERTIES LABELS "performance" TIMEOUT 180)

# Issue #370: byte-budgeted ownership — deterministic policy test + benchmark.
mib_add_backend_test_executable(memory_budget_test processing/memory_budget_test.cpp)
mib_backend_test_link(memory_budget_test PRIVATE Threads::Threads)
add_test(NAME processing.memory_budget COMMAND $<TARGET_FILE:mib_backend_tests> memory_budget_test)
set_tests_properties(processing.memory_budget PROPERTIES LABELS "processing;backend;safety;memory" TIMEOUT 180)

mib_add_backend_test_executable(memory_budget_bench performance/memory_budget_bench.cpp)
mib_backend_test_link(memory_budget_bench PRIVATE Threads::Threads)
add_test(NAME performance.memory_budget COMMAND $<TARGET_FILE:mib_backend_tests> memory_budget_bench)
set_tests_properties(performance.memory_budget PROPERTIES LABELS "performance;memory" TIMEOUT 600)

mib_add_backend_test_executable(core_management_test backend/core_management_test.cpp)
add_test(NAME backend.core_management COMMAND $<TARGET_FILE:mib_backend_tests> core_management_test)
set_tests_properties(backend.core_management PROPERTIES LABELS "backend;roundtrip;fault-injection;stress" TIMEOUT 60)

mib_add_backend_test_executable(profile_catalog_test backend/profile_catalog_test.cpp)
add_test(NAME backend.profile_catalog COMMAND $<TARGET_FILE:mib_backend_tests> profile_catalog_test)
set_tests_properties(backend.profile_catalog PROPERTIES LABELS "backend;fault-injection;stress" TIMEOUT 60)

mib_add_backend_test_executable(profile_store_test backend/profile_store_test.cpp)
add_test(NAME backend.profile_store COMMAND $<TARGET_FILE:mib_backend_tests> profile_store_test)
set_tests_properties(backend.profile_store PROPERTIES LABELS "backend;roundtrip;fault-injection;stress" TIMEOUT 60)

mib_add_backend_test_executable(processing_config_transaction_test backend/processing_config_transaction_test.cpp)
add_test(NAME backend.processing_config_transaction COMMAND $<TARGET_FILE:mib_backend_tests> processing_config_transaction_test)
set_tests_properties(backend.processing_config_transaction PROPERTIES LABELS "backend;roundtrip;fault-injection;stress" TIMEOUT 60)

mib_add_backend_test_executable(backend_facade_boundary_test backend/backend_facade_boundary_test.cpp)
add_test(NAME backend.facade_boundary COMMAND $<TARGET_FILE:mib_backend_tests> backend_facade_boundary_test)
set_tests_properties(backend.facade_boundary PROPERTIES LABELS "backend;bridge")

# BE-4 (#274): backend-owned experiment lifecycle state machine — pipeline e2e
# over the facade command surface plus fault-injection (fatal save error,
# double start/stop, shutdown during active experiment).
mib_add_backend_test_executable(experiment_coordinator_test backend/experiment_coordinator_test.cpp)
add_test(NAME e2e.experiment_coordinator COMMAND $<TARGET_FILE:mib_backend_tests> experiment_coordinator_test)
set_tests_properties(e2e.experiment_coordinator PROPERTIES LABELS "backend;bridge;e2e" TIMEOUT 180)

mib_add_backend_test_executable(mindvision_selection_state_test backend/mindvision_selection_state_test.cpp)
add_test(NAME backend.mindvision_selection_state COMMAND $<TARGET_FILE:mib_backend_tests> mindvision_selection_state_test)
set_tests_properties(backend.mindvision_selection_state PROPERTIES LABELS "backend;camera")

mib_add_backend_test_executable(mock_camera_smoke_test camera/mock_camera_smoke_test.cpp)
add_test(NAME camera.mock_smoke COMMAND $<TARGET_FILE:mib_backend_tests> mock_camera_smoke_test)
set_tests_properties(camera.mock_smoke PROPERTIES LABELS "camera;backend")

mib_add_backend_test_executable(delivery_mode_overload_test camera/delivery_mode_overload_test.cpp)
mib_backend_test_link(delivery_mode_overload_test PRIVATE Threads::Threads)
add_test(NAME camera.delivery_mode_overload COMMAND $<TARGET_FILE:mib_backend_tests> delivery_mode_overload_test)
set_tests_properties(camera.delivery_mode_overload PROPERTIES LABELS "camera;backend" TIMEOUT 60)

mib_add_backend_test_executable(delivery_mode_contract_test camera/delivery_mode_contract_test.cpp)
mib_backend_test_link(delivery_mode_contract_test PRIVATE Threads::Threads)
add_test(NAME camera.delivery_mode_contract COMMAND $<TARGET_FILE:mib_backend_tests> delivery_mode_contract_test)
set_tests_properties(camera.delivery_mode_contract PROPERTIES LABELS "camera;backend" TIMEOUT 60)

mib_add_backend_test_executable(processing_science_seam_test processing/processing_science_seam_test.cpp)
add_test(NAME processing.science_seam COMMAND $<TARGET_FILE:mib_backend_tests> processing_science_seam_test)
set_tests_properties(processing.science_seam PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_contract_v2_migration_test processing/processing_contract_v2_migration_test.cpp)
add_test(NAME processing.contract_v2_migration COMMAND $<TARGET_FILE:mib_backend_tests> processing_contract_v2_migration_test)
set_tests_properties(processing.contract_v2_migration PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_contract2_conformance_test processing/processing_contract2_conformance_test.cpp)
add_test(NAME processing.contract2_conformance COMMAND $<TARGET_FILE:mib_backend_tests> processing_contract2_conformance_test)
set_tests_properties(processing.contract2_conformance PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_contract3_cells_conformance_test processing/processing_contract3_cells_conformance_test.cpp)
add_test(NAME processing.contract3_cells_conformance COMMAND $<TARGET_FILE:mib_backend_tests> processing_contract3_cells_conformance_test
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(processing.contract3_cells_conformance PROPERTIES LABELS "processing;backend;gold;pl" TIMEOUT 60)

mib_add_backend_test_executable(pz_platform_monitor_test processing/pz_platform_monitor_test.cpp)
add_test(NAME processing.pz_platform_monitor COMMAND $<TARGET_FILE:mib_backend_tests> pz_platform_monitor_test)
set_tests_properties(processing.pz_platform_monitor PROPERTIES LABELS "processing;backend;pl" TIMEOUT 30)
mib_add_backend_test_executable(pz_bridge_preview_test processing/pz_bridge_preview_test.cpp)
add_test(NAME processing.pz_bridge_preview COMMAND $<TARGET_FILE:mib_backend_tests> pz_bridge_preview_test)
set_tests_properties(processing.pz_bridge_preview PROPERTIES LABELS "processing;backend;pl" TIMEOUT 30)

mib_add_backend_test_executable(pz_records_test processing/pz_records_test.cpp)
add_test(NAME processing.pz_records COMMAND $<TARGET_FILE:mib_backend_tests> pz_records_test
         ${PROJECT_SOURCE_DIR}/third_party/pz7035-abi
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(processing.pz_records PROPERTIES LABELS "processing;backend;pl" TIMEOUT 60)

# #649 v1: the results13 frame ring reader (placement, freeze, the reader rule, the browser packet).
mib_add_backend_test_executable(pz_frame_ring_test processing/pz_frame_ring_test.cpp)
add_test(NAME processing.pz_frame_ring COMMAND $<TARGET_FILE:mib_backend_tests> pz_frame_ring_test)
set_tests_properties(processing.pz_frame_ring PROPERTIES LABELS "processing;backend;pl" TIMEOUT 60)

mib_add_backend_test_executable(channel_roi_detect_test processing/channel_roi_detect_test.cpp)
add_test(NAME processing.channel_roi_detect COMMAND $<TARGET_FILE:mib_backend_tests> channel_roi_detect_test)
set_tests_properties(processing.channel_roi_detect PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(pl_preview_background_test processing/pl_preview_background_test.cpp)
mib_backend_test_link(pl_preview_background_test PRIVATE Threads::Threads)
add_test(NAME processing.pl_preview_background COMMAND $<TARGET_FILE:mib_backend_tests> pl_preview_background_test)
set_tests_properties(processing.pl_preview_background PROPERTIES LABELS "processing;backend;concurrency;pl" TIMEOUT 60)

mib_add_backend_test_executable(pz_profile_compiler_test processing/pz_profile_compiler_test.cpp)
add_test(NAME processing.pz_profile_compiler COMMAND $<TARGET_FILE:mib_backend_tests> pz_profile_compiler_test
         ${PROJECT_SOURCE_DIR}/third_party/pz7035-abi)
set_tests_properties(processing.pz_profile_compiler PROPERTIES LABELS "processing;backend;pl" TIMEOUT 30)

mib_add_backend_test_executable(pz_execution_provider_test processing/pz_execution_provider_test.cpp)
mib_backend_test_link(pz_execution_provider_test PRIVATE Threads::Threads)
add_test(NAME processing.pz_execution_provider COMMAND $<TARGET_FILE:mib_backend_tests> pz_execution_provider_test
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(processing.pz_execution_provider PROPERTIES LABELS "processing;backend;concurrency;pl" TIMEOUT 90)

mib_add_backend_test_executable(pz_provider_ingest_test processing/pz_provider_ingest_test.cpp)
mib_backend_test_link(pz_provider_ingest_test PRIVATE Threads::Threads)
add_test(NAME processing.pz_provider_ingest COMMAND $<TARGET_FILE:mib_backend_tests> pz_provider_ingest_test
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(processing.pz_provider_ingest PROPERTIES LABELS "processing;backend;recording;concurrency;pl" TIMEOUT 90)
mib_add_backend_test_executable(pz_unet_cells_host_test processing/pz_unet_cells_host_test.cpp)
add_test(NAME processing.pz_unet_cells_host COMMAND $<TARGET_FILE:mib_backend_tests> pz_unet_cells_host_test
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(processing.pz_unet_cells_host PROPERTIES LABELS "processing;backend;gold;pl" TIMEOUT 60)

mib_add_backend_test_executable(unet_c4_test processing/unet_c4_test.cpp)
add_test(NAME processing.unet_c4 COMMAND $<TARGET_FILE:mib_backend_tests> unet_c4_test)
set_tests_properties(processing.unet_c4 PROPERTIES LABELS "processing;backend;gold;pl" TIMEOUT 300 SKIP_RETURN_CODE 77
    ENVIRONMENT "MIB_UNET_C4_PARAMS=${MIB_ASSETS_DIR}/models/unet-c4-multiline-v1/c4_multiline_params.npz;MIB_UNET_C4_FIXTURES=${MIB_ASSETS_DIR}/models/unet-c4-multiline-v1/fixtures")

mib_add_backend_test_executable(pz_board_run_host_test processing/pz_board_run_host_test.cpp)
add_test(NAME processing.pz_board_run_host COMMAND $<TARGET_FILE:mib_backend_tests> pz_board_run_host_test
         ${PROJECT_SOURCE_DIR}/scripts/conformance/unet-cells-v2-pl-vectors.json)
set_tests_properties(processing.pz_board_run_host PROPERTIES LABELS "processing;backend;gold;pl" TIMEOUT 300 SKIP_RETURN_CODE 77)

mib_add_backend_test_executable(image_filter_pipeline_test processing/image_filter_pipeline_test.cpp)
add_test(NAME processing.image_filter_pipeline COMMAND $<TARGET_FILE:mib_backend_tests> image_filter_pipeline_test)
set_tests_properties(processing.image_filter_pipeline PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_laplacian_variance_test processing/processing_laplacian_variance_test.cpp)
add_test(NAME processing.laplacian_variance COMMAND $<TARGET_FILE:mib_backend_tests> processing_laplacian_variance_test)
set_tests_properties(processing.laplacian_variance PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_science_golden_test processing/processing_science_golden_test.cpp)
add_test(NAME processing.science_golden COMMAND $<TARGET_FILE:mib_backend_tests> processing_science_golden_test)
set_tests_properties(processing.science_golden PROPERTIES LABELS "processing;backend" TIMEOUT 30)

# Dot-grid wafer localization (docs/architecture/dot-grid-localization.md).
mib_add_backend_test_executable(dot_grid_codebook_test processing/dot_grid_codebook_test.cpp)
add_test(NAME processing.dot_grid_codebook COMMAND $<TARGET_FILE:mib_backend_tests> dot_grid_codebook_test)
set_tests_properties(processing.dot_grid_codebook PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(dot_grid_decoder_test processing/dot_grid_decoder_test.cpp)
add_test(NAME processing.dot_grid_decoder COMMAND $<TARGET_FILE:mib_backend_tests> dot_grid_decoder_test)
set_tests_properties(processing.dot_grid_decoder PROPERTIES LABELS "processing;backend" TIMEOUT 120)

mib_add_backend_test_executable(dot_grid_registry_test processing/dot_grid_registry_test.cpp)
add_test(NAME processing.dot_grid_registry COMMAND $<TARGET_FILE:mib_backend_tests> dot_grid_registry_test
         ${PROJECT_SOURCE_DIR}/resources/defaults/dot_grid/registry.json)
set_tests_properties(processing.dot_grid_registry PROPERTIES LABELS "processing;backend" TIMEOUT 120)

mib_add_backend_test_executable(dot_grid_codec_gold_test processing/dot_grid_codec_gold_test.cpp)
add_test(NAME processing.dot_grid_codec_gold COMMAND $<TARGET_FILE:mib_backend_tests> dot_grid_codec_gold_test
         ${PROJECT_SOURCE_DIR}/scripts/dot_grid/gold/codec-contract1.json)
set_tests_properties(processing.dot_grid_codec_gold PROPERTIES LABELS "processing;backend;gold" TIMEOUT 120)

mib_add_backend_test_executable(dot_grid_service_test backend/dot_grid_service_test.cpp)
mib_backend_test_link(dot_grid_service_test PRIVATE Threads::Threads)
add_test(NAME backend.dot_grid_service COMMAND $<TARGET_FILE:mib_backend_tests> dot_grid_service_test)
set_tests_properties(backend.dot_grid_service PROPERTIES LABELS "backend;processing;stress" TIMEOUT 120)

mib_add_backend_test_executable(processing_identification_metrics_test processing/processing_identification_metrics_test.cpp)
add_test(NAME processing.identification_metrics COMMAND $<TARGET_FILE:mib_backend_tests> processing_identification_metrics_test)
set_tests_properties(processing.identification_metrics PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_pipeline_smoke_test processing/processing_pipeline_smoke_test.cpp)
add_test(NAME processing.pipeline_smoke COMMAND $<TARGET_FILE:mib_backend_tests> processing_pipeline_smoke_test)
set_tests_properties(processing.pipeline_smoke PROPERTIES LABELS "processing;backend")

add_executable(processing_core_abi_c_test
    ${PROJECT_SOURCE_DIR}/tests/processing/processing_core_abi_c_test.c
)
target_include_directories(processing_core_abi_c_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(processing_core_abi_c_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME processing.core_abi_c COMMAND $<TARGET_FILE:processing_core_abi_c_test>)
set_tests_properties(processing.core_abi_c PROPERTIES LABELS "processing;backend" TIMEOUT 30)

add_executable(processing_core_abi_v2_c_test
    ${PROJECT_SOURCE_DIR}/tests/processing/processing_core_abi_v2_c_test.c
)
target_include_directories(processing_core_abi_v2_c_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(processing_core_abi_v2_c_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
add_test(NAME processing.core_abi_v2_c COMMAND $<TARGET_FILE:processing_core_abi_v2_c_test>)
set_tests_properties(processing.core_abi_v2_c PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_capabilities_test processing/processing_core_capabilities_test.cpp)
add_test(NAME processing.core_capabilities COMMAND $<TARGET_FILE:mib_backend_tests> processing_core_capabilities_test)
set_tests_properties(processing.core_capabilities PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_loader_test processing/processing_core_loader_test.cpp)
add_dependencies(processing_core_loader_test mib_processing_core)
add_test(NAME processing.core_loader
    COMMAND $<TARGET_FILE:processing_core_loader_test> $<TARGET_FILE:mib_processing_core>)
set_tests_properties(processing.core_loader PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_v2_plugin_test processing/processing_core_v2_plugin_test.cpp)
add_dependencies(processing_core_v2_plugin_test mib_processing_core_absdiff_laplacian)
add_test(NAME processing.core_v2_plugin
    COMMAND $<TARGET_FILE:processing_core_v2_plugin_test> $<TARGET_FILE:mib_processing_core_absdiff_laplacian>)
set_tests_properties(processing.core_v2_plugin PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_contract2_equivalence_test processing/processing_core_contract2_equivalence_test.cpp)
add_dependencies(processing_core_contract2_equivalence_test mib_processing_core_absdiff_laplacian)
add_test(NAME processing.core_contract2_equivalence
    COMMAND $<TARGET_FILE:processing_core_contract2_equivalence_test> $<TARGET_FILE:mib_processing_core_absdiff_laplacian>)
set_tests_properties(processing.core_contract2_equivalence PROPERTIES LABELS "processing;backend" TIMEOUT 30)

add_subdirectory(processing/fixtures)
mib_add_backend_test_executable(processing_core_fixture_matrix_test processing/processing_core_fixture_matrix_test.cpp)
add_dependencies(processing_core_fixture_matrix_test
    mib_processing_fixture_good
    mib_processing_fixture_truncated
    mib_processing_fixture_incompatible
    mib_processing_fixture_malformed
    mib_processing_fixture_throwing
)
add_test(NAME processing.core_fixture_matrix
    COMMAND $<TARGET_FILE:processing_core_fixture_matrix_test>
        $<TARGET_FILE:mib_processing_fixture_good>
        $<TARGET_FILE:mib_processing_fixture_truncated>
        $<TARGET_FILE:mib_processing_fixture_incompatible>
        $<TARGET_FILE:mib_processing_fixture_malformed>
        $<TARGET_FILE:mib_processing_fixture_throwing>)
set_tests_properties(processing.core_fixture_matrix PROPERTIES LABELS "processing;backend" TIMEOUT 30)

if(WIN32)
    mib_add_backend_test_executable(processing_core_authenticode_test processing/processing_core_authenticode_test.cpp)
endif()

if(UNIX AND NOT APPLE)
    # Imported find_package targets are directory-scoped, so resolve OpenSSL
    # here as well; the desktop Linux build already requires it.
    find_package(OpenSSL REQUIRED)
    mib_add_backend_test_executable(processing_core_ed25519_test processing/processing_core_ed25519_test.cpp)
    add_dependencies(processing_core_ed25519_test mib_processing_core)
    target_link_libraries(processing_core_ed25519_test PRIVATE OpenSSL::Crypto)
    add_test(NAME processing.core_ed25519
        COMMAND $<TARGET_FILE:processing_core_ed25519_test> $<TARGET_FILE:mib_processing_core>)
    set_tests_properties(processing.core_ed25519 PROPERTIES LABELS "processing;backend" TIMEOUT 30)
endif()

mib_add_backend_test_executable(processing_core_cache_test processing/processing_core_cache_test.cpp)
add_dependencies(processing_core_cache_test mib_processing_core)
target_link_libraries(processing_core_cache_test PRIVATE Threads::Threads)
add_test(NAME processing.core_cache
    COMMAND $<TARGET_FILE:processing_core_cache_test> $<TARGET_FILE:mib_processing_core>)
set_tests_properties(processing.core_cache PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_activation_test processing/processing_core_activation_test.cpp)
mib_backend_test_link(processing_core_activation_test PRIVATE Threads::Threads)
add_test(NAME processing.core_activation COMMAND $<TARGET_FILE:mib_backend_tests> processing_core_activation_test)
set_tests_properties(processing.core_activation PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_activation_stress_test processing/processing_core_activation_stress_test.cpp)
mib_backend_test_link(processing_core_activation_stress_test PRIVATE Threads::Threads)
add_test(NAME processing.core_activation_stress COMMAND $<TARGET_FILE:mib_backend_tests> processing_core_activation_stress_test)
set_tests_properties(processing.core_activation_stress PROPERTIES LABELS "processing;backend;stress" TIMEOUT 60)

mib_add_backend_test_executable(processing_batch_pipeline_test processing/processing_batch_pipeline_test.cpp)
add_test(NAME backend.processing_batch_pipeline COMMAND $<TARGET_FILE:mib_backend_tests> processing_batch_pipeline_test)
set_tests_properties(backend.processing_batch_pipeline PROPERTIES LABELS "processing;backend")

# Issue #367: explicit frame accounting.
mib_add_backend_test_executable(frame_store_commit_test backend/frame_store_commit_test.cpp)
mib_backend_test_link(frame_store_commit_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_commit COMMAND $<TARGET_FILE:mib_backend_tests> frame_store_commit_test)
set_tests_properties(backend.frame_store_commit PROPERTIES LABELS "backend;safety" TIMEOUT 60)

mib_add_backend_test_executable(experiment_accounting_test processing/experiment_accounting_test.cpp)
mib_backend_test_link(experiment_accounting_test PRIVATE Threads::Threads)
add_test(NAME processing.experiment_accounting COMMAND $<TARGET_FILE:mib_backend_tests> experiment_accounting_test)
set_tests_properties(processing.experiment_accounting PROPERTIES LABELS "processing;backend;recording;safety" TIMEOUT 240)

mib_add_backend_test_executable(experiment_flush_backpressure_test processing/experiment_flush_backpressure_test.cpp)
mib_backend_test_link(experiment_flush_backpressure_test PRIVATE Threads::Threads)
add_test(NAME processing.experiment_flush_backpressure COMMAND $<TARGET_FILE:mib_backend_tests> experiment_flush_backpressure_test)
set_tests_properties(processing.experiment_flush_backpressure PROPERTIES LABELS "processing;backend;recording;safety" TIMEOUT 120)

mib_add_backend_test_executable(flush_byte_watermark_test processing/flush_byte_watermark_test.cpp)
mib_backend_test_link(flush_byte_watermark_test PRIVATE Threads::Threads)
add_test(NAME processing.flush_byte_watermark COMMAND $<TARGET_FILE:mib_backend_tests> flush_byte_watermark_test)
set_tests_properties(processing.flush_byte_watermark PROPERTIES LABELS "processing;backend;recording;safety" TIMEOUT 60)

mib_add_backend_test_executable(recording_accounting_test recording/recording_accounting_test.cpp)
mib_backend_test_link(recording_accounting_test PRIVATE Threads::Threads)
add_test(NAME recording.accounting COMMAND $<TARGET_FILE:mib_backend_tests> recording_accounting_test)
set_tests_properties(recording.accounting PROPERTIES LABELS "recording;backend;hdf5;safety" TIMEOUT 180)

mib_add_backend_test_executable(processing_fault_injection_test processing/processing_fault_injection_test.cpp)
add_test(NAME processing.fault_injection COMMAND $<TARGET_FILE:mib_backend_tests> processing_fault_injection_test)
set_tests_properties(processing.fault_injection PROPERTIES LABELS "processing;backend")

mib_add_backend_test_executable(recording_lifecycle_test recording/recording_lifecycle_test.cpp)
add_test(NAME recording.lifecycle COMMAND $<TARGET_FILE:mib_backend_tests> recording_lifecycle_test)
set_tests_properties(recording.lifecycle PROPERTIES LABELS "recording;backend")

mib_add_backend_test_executable(hdf5_resilience_test recording/hdf5_resilience_test.cpp)
add_test(NAME recording.hdf5_resilience COMMAND $<TARGET_FILE:mib_backend_tests> hdf5_resilience_test)
set_tests_properties(recording.hdf5_resilience PROPERTIES LABELS "recording;backend;hdf5")

# Process exit with an HDF5 file left open under a live writer must not crash
# in the library's atexit teardown (installed-app WER crash, 2026-09-07).
mib_add_backend_test_executable(hdf5_exit_teardown_test recording/hdf5_exit_teardown_test.cpp)
target_link_libraries(hdf5_exit_teardown_test PRIVATE Threads::Threads)
add_test(NAME recording.hdf5_exit_teardown COMMAND $<TARGET_FILE:hdf5_exit_teardown_test>)
set_tests_properties(recording.hdf5_exit_teardown PROPERTIES LABELS "recording;backend;hdf5;safety" TIMEOUT 60)

mib_add_backend_test_executable(hdf5_save_performance_test recording/hdf5_save_performance_test.cpp)
add_test(NAME recording.hdf5_save_performance COMMAND $<TARGET_FILE:mib_backend_tests> hdf5_save_performance_test)
set_tests_properties(recording.hdf5_save_performance PROPERTIES LABELS "recording;backend;hdf5;performance")

mib_add_backend_test_executable(experiment_roundtrip_test recording/experiment_roundtrip_test.cpp)
add_test(NAME recording.experiment_roundtrip COMMAND $<TARGET_FILE:mib_backend_tests> experiment_roundtrip_test)
set_tests_properties(recording.experiment_roundtrip PROPERTIES LABELS "recording;backend" TIMEOUT 30)

# Series identity (/valid_frames/series_meta, series_contiguous) and sort
# pulse records (/trigger_events): round-trip + fault injection.
mib_add_backend_test_executable(trigger_alignment_roundtrip_test recording/trigger_alignment_roundtrip_test.cpp)
add_test(NAME recording.trigger_alignment_roundtrip COMMAND $<TARGET_FILE:mib_backend_tests> trigger_alignment_roundtrip_test)
set_tests_properties(recording.trigger_alignment_roundtrip PROPERTIES LABELS "recording;backend;hdf5;trigger" TIMEOUT 30)

# KDE core contour records (/monitoring @kde_live_json, /analysis @kde_core_json):
# round-trip + fault injection for the save path and the frontend codec.
# ReviewSession (plan 2026-10-01-standalone-review-app): the one review
# implementation behind the Qt tab, the Tauri shell and YOFO Review.
# Round-trip on experiment/legacy/recording fixtures + fault injection.
mib_add_backend_test_executable(review_session_test review/review_session_test.cpp)
add_test(NAME review.session COMMAND $<TARGET_FILE:mib_backend_tests> review_session_test)
set_tests_properties(review.session PROPERTIES LABELS "review;recording;backend;hdf5;safety" TIMEOUT 120)
# ReviewJobs: exports (= direct HdfExportService output), cancel, batch,
# regenerate masks, core contour, density (grid ≈ direct), single flight.
mib_add_backend_test_executable(review_jobs_test review/review_jobs_test.cpp)
mib_backend_test_link(review_jobs_test PRIVATE Threads::Threads)
add_test(NAME review.jobs COMMAND $<TARGET_FILE:mib_backend_tests> review_jobs_test)
set_tests_properties(review.jobs PROPERTIES LABELS "review;recording;backend;hdf5;concurrency;safety" TIMEOUT 300)

mib_add_backend_test_executable(kde_core_roundtrip_test recording/kde_core_roundtrip_test.cpp)
add_test(NAME recording.kde_core_roundtrip COMMAND $<TARGET_FILE:mib_backend_tests> kde_core_roundtrip_test)
set_tests_properties(recording.kde_core_roundtrip PROPERTIES LABELS "recording;backend;monitoring" TIMEOUT 30)
mib_add_backend_test_executable(kde_core_fault_test recording/kde_core_fault_test.cpp)
add_test(NAME recording.kde_core_fault COMMAND $<TARGET_FILE:mib_backend_tests> kde_core_fault_test)
set_tests_properties(recording.kde_core_fault PROPERTIES LABELS "recording;backend;monitoring;safety" TIMEOUT 30)
mib_add_backend_test_executable(kde_full_run_core_test recording/kde_full_run_core_test.cpp)
add_test(NAME recording.kde_full_run_core COMMAND $<TARGET_FILE:mib_backend_tests> kde_full_run_core_test)
set_tests_properties(recording.kde_full_run_core PROPERTIES LABELS "recording;backend;monitoring" TIMEOUT 60)

# Issue #344: native Qt-free export service (round-trip, cancellation, faults,
# bounded names, repeated runs with HDF5 open-object accounting). The
# `performance`-labelled variant is the 50-round soak release gate.
mib_add_backend_test_executable(hdf_export_service_test recording/hdf_export_service_test.cpp)
target_link_libraries(hdf_export_service_test PRIVATE Threads::Threads)
add_test(NAME recording.hdf_export_service COMMAND $<TARGET_FILE:hdf_export_service_test>)
set_tests_properties(recording.hdf_export_service PROPERTIES LABELS "recording;backend;hdf5;safety" TIMEOUT 300)
add_test(NAME recording.hdf_export_soak COMMAND $<TARGET_FILE:hdf_export_service_test>)
set_tests_properties(recording.hdf_export_soak PROPERTIES
    LABELS "recording;performance;soak"
    ENVIRONMENT "MIB_EXPORT_SOAK_CYCLES=50"
    TIMEOUT 900)

mib_add_backend_test_executable(fcs_writer_test recording/fcs_writer_test.cpp)
add_test(NAME recording.fcs_writer COMMAND $<TARGET_FILE:fcs_writer_test>)
set_tests_properties(recording.fcs_writer PROPERTIES LABELS "recording;backend;fcs;safety" TIMEOUT 30)

if(Python3_Interpreter_FOUND)
    add_test(NAME recording.fcs_flowio
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_fcs_flowio.py
            --fixture-generator $<TARGET_FILE:fcs_writer_test>
            --native-cli $<TARGET_FILE:hdf_export_cli>)
    set_tests_properties(recording.fcs_flowio PROPERTIES
        LABELS "recording;backend;fcs;round-trip" SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()

mib_add_backend_test_executable(kin6_batch_pipeline_evidence integration/kin6_batch_pipeline_evidence.cpp)

mib_add_backend_test_executable(kin10_hf_dataset_pipeline_test integration/kin10_hf_dataset_pipeline_test.cpp)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME backend.kin10_hf_dataset_pipeline
        COMMAND
            ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tools/kin10_run_hf_dataset_test.py
            --out-dir ${PROJECT_BINARY_DIR}/test-output/kin10_hf_dataset_pipeline
            --binary $<TARGET_FILE:kin10_hf_dataset_pipeline_test>
    )
    # The harness downloads dataset rows from Hugging Face on every run:
    # 2 s or 46 s depending on the datasets-server that day. `network` keeps
    # it out of the release gate (build-windows.yml runs ctest -LE network).
    set_tests_properties(backend.kin10_hf_dataset_pipeline PROPERTIES
        LABELS "backend;integration;network"
        SKIP_RETURN_CODE 77
        TIMEOUT 180
    )
elseif(UNIX)
    add_test(
        NAME backend.kin10_hf_dataset_pipeline
        COMMAND
            ${PROJECT_SOURCE_DIR}/tools/kin10_run_hf_dataset_test.sh
            ${PROJECT_BINARY_DIR}/kin10_hf_dataset_pipeline
            $<TARGET_FILE:kin10_hf_dataset_pipeline_test>
    )
    # The harness downloads dataset rows from Hugging Face on every run:
    # 2 s or 46 s depending on the datasets-server that day. `network` keeps
    # it out of the release gate (build-windows.yml runs ctest -LE network).
    set_tests_properties(backend.kin10_hf_dataset_pipeline PROPERTIES
        LABELS "backend;integration;network"
        SKIP_RETURN_CODE 77
        TIMEOUT 180
    )
else()
    message(WARNING "Python3 interpreter not found; backend.kin10_hf_dataset_pipeline will not be registered")
endif()

mib_add_backend_test_executable(kin6_mib_app_capture_proof integration/kin6_mib_app_capture_proof.cpp)
add_test(NAME backend.kin6_mib_app_capture_proof COMMAND $<TARGET_FILE:mib_backend_tests> kin6_mib_app_capture_proof)
set_tests_properties(backend.kin6_mib_app_capture_proof PROPERTIES LABELS "backend;integration")

mib_add_backend_test_executable(e2e_storage_destinations_test integration/e2e_storage_destinations_test.cpp)
add_test(NAME integration.e2e_storage_destinations COMMAND $<TARGET_FILE:mib_backend_tests> e2e_storage_destinations_test)
set_tests_properties(integration.e2e_storage_destinations PROPERTIES LABELS "integration;e2e;recording" TIMEOUT 60)

mib_add_backend_test_executable(e2e_trigger_timing_test integration/e2e_trigger_timing_test.cpp)
mib_backend_test_link(e2e_trigger_timing_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_trigger_timing COMMAND $<TARGET_FILE:mib_backend_tests> e2e_trigger_timing_test)
set_tests_properties(integration.e2e_trigger_timing PROPERTIES LABELS "integration;e2e;trigger" TIMEOUT 120)

mib_add_backend_test_executable(frame_store_wait_test backend/frame_store_wait_test.cpp)
mib_backend_test_link(frame_store_wait_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_wait COMMAND $<TARGET_FILE:mib_backend_tests> frame_store_wait_test)
set_tests_properties(backend.frame_store_wait PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(pipeline_timing_recorder_test backend/pipeline_timing_recorder_test.cpp)
mib_backend_test_link(pipeline_timing_recorder_test PRIVATE Threads::Threads)
add_test(NAME backend.pipeline_timing_recorder COMMAND $<TARGET_FILE:mib_backend_tests> pipeline_timing_recorder_test)
set_tests_properties(backend.pipeline_timing_recorder PROPERTIES LABELS "backend;diagnostics" TIMEOUT 30)

mib_add_backend_test_executable(crash_reporter_pending_upload_test backend/crash_reporter_pending_upload_test.cpp)
add_test(NAME backend.crash_reporter_pending_upload COMMAND $<TARGET_FILE:mib_backend_tests> crash_reporter_pending_upload_test)
set_tests_properties(backend.crash_reporter_pending_upload PROPERTIES LABELS "backend;observability" TIMEOUT 60)

mib_add_backend_test_executable(crash_reporter_terminate_test backend/crash_reporter_terminate_test.cpp)
target_link_libraries(crash_reporter_terminate_test PRIVATE Threads::Threads)
add_test(NAME backend.crash_reporter_terminate COMMAND $<TARGET_FILE:crash_reporter_terminate_test>)
set_tests_properties(backend.crash_reporter_terminate PROPERTIES LABELS "backend;observability" TIMEOUT 60)

# A hardware fault leaves a dump with the faulting context and a sidecar
# naming the fault (crash review 2026-09-08).
mib_add_backend_test_executable(crash_reporter_segv_test backend/crash_reporter_segv_test.cpp)
target_link_libraries(crash_reporter_segv_test PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(crash_reporter_segv_test PRIVATE dbghelp)
endif()
add_test(NAME backend.crash_reporter_segv COMMAND $<TARGET_FILE:crash_reporter_segv_test>)
set_tests_properties(backend.crash_reporter_segv PROPERTIES LABELS "backend;observability;safety" TIMEOUT 60 SKIP_RETURN_CODE 77)

mib_add_backend_test_executable(e2e_pipeline_timing_test integration/e2e_pipeline_timing_test.cpp)
mib_backend_test_link(e2e_pipeline_timing_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_pipeline_timing COMMAND $<TARGET_FILE:mib_backend_tests> e2e_pipeline_timing_test)
set_tests_properties(integration.e2e_pipeline_timing PROPERTIES LABELS "integration;e2e;trigger;diagnostics" TIMEOUT 120)

# Invariant: multi-image series members carry their frame identity and every
# fired sort pulse names a saved series member (pipeline -> HDF5 -> reload).
mib_add_backend_test_executable(e2e_series_alignment_test integration/e2e_series_alignment_test.cpp)
mib_backend_test_link(e2e_series_alignment_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_series_alignment COMMAND $<TARGET_FILE:mib_backend_tests> e2e_series_alignment_test)
set_tests_properties(integration.e2e_series_alignment PROPERTIES LABELS "integration;e2e;trigger;recording" TIMEOUT 120)

# Manual diagnosis harness (no add_test: needs a local frames folder).
# Full mock pipeline dry-run with latency instrumentation — see
# docs/howto/pipeline-latency-diagnosis.md.
mib_add_backend_test_executable(mock_pipeline_timing_run tools/mock_pipeline_timing_run.cpp)
mib_backend_test_link(mock_pipeline_timing_run PRIVATE Threads::Threads)

mib_add_backend_test_executable(e2e_pipeline_stress_test integration/e2e_pipeline_stress_test.cpp)
mib_backend_test_link(e2e_pipeline_stress_test PRIVATE Threads::Threads)
# Lifecycle start/stop race test. Has an in-process watchdog that _Exit(99)s
# with the stuck phase/cycle/step if a cycle deadlocks, so it fails fast instead
# of burning the full ctest timeout. Pass a larger [cycles] arg for heavy local
# stress runs. (The TriggerService lost-wakeup deadlock this used to expose is
# fixed; see backend.trigger_service_start_stop_race for the focused guard.)
add_test(NAME integration.e2e_pipeline_stress COMMAND $<TARGET_FILE:mib_backend_tests> e2e_pipeline_stress_test)
set_tests_properties(integration.e2e_pipeline_stress PROPERTIES LABELS "integration;e2e" TIMEOUT 120)

# Issue #419: end-to-end device-discovery lifecycle over a real AppBackend
# (startup policy, explicit pulse-generator scan on the fake Modbus bench,
# cancel, shutdown ordering versus hardware release).
mib_add_backend_test_executable(e2e_stage_startup_test integration/e2e_stage_startup_test.cpp)
mib_backend_test_link(e2e_stage_startup_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_stage_startup COMMAND $<TARGET_FILE:mib_backend_tests> e2e_stage_startup_test)
set_tests_properties(integration.e2e_stage_startup PROPERTIES LABELS "integration;e2e;stage" TIMEOUT 120)

mib_add_backend_test_executable(e2e_device_discovery_lifecycle_test integration/e2e_device_discovery_lifecycle_test.cpp)
mib_backend_test_link(e2e_device_discovery_lifecycle_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_device_discovery_lifecycle COMMAND $<TARGET_FILE:mib_backend_tests> e2e_device_discovery_lifecycle_test)
set_tests_properties(integration.e2e_device_discovery_lifecycle PROPERTIES LABELS "integration;e2e;discovery" TIMEOUT 120)

mib_add_backend_test_executable(e2e_live_view_latency_test integration/e2e_live_view_latency_test.cpp)
mib_backend_test_link(e2e_live_view_latency_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_live_view_latency COMMAND $<TARGET_FILE:mib_backend_tests> e2e_live_view_latency_test)
set_tests_properties(integration.e2e_live_view_latency PROPERTIES LABELS "integration;e2e;processing" TIMEOUT 120)

mib_add_backend_test_executable(e2e_realtime_throughput_test integration/e2e_realtime_throughput_test.cpp)
mib_backend_test_link(e2e_realtime_throughput_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_realtime_throughput COMMAND $<TARGET_FILE:mib_backend_tests> e2e_realtime_throughput_test)
set_tests_properties(integration.e2e_realtime_throughput PROPERTIES LABELS "integration;e2e;processing;performance" TIMEOUT 120)

mib_add_backend_test_executable(e2e_batch_backpressure_test integration/e2e_batch_backpressure_test.cpp)
mib_backend_test_link(e2e_batch_backpressure_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_batch_backpressure COMMAND $<TARGET_FILE:mib_backend_tests> e2e_batch_backpressure_test)
set_tests_properties(integration.e2e_batch_backpressure PROPERTIES LABELS "integration;e2e;processing;performance" TIMEOUT 120)

mib_add_backend_test_executable(realtime_drop_frames_default_test processing/realtime_drop_frames_default_test.cpp)
add_test(NAME processing.realtime_drop_frames_default COMMAND $<TARGET_FILE:mib_backend_tests> realtime_drop_frames_default_test)
set_tests_properties(processing.realtime_drop_frames_default PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(mib_processing_multi_object_test processing/processing_multi_object_test.cpp)
add_test(NAME backend.processing_multi_object COMMAND $<TARGET_FILE:mib_backend_tests> mib_processing_multi_object_test)
set_tests_properties(backend.processing_multi_object PROPERTIES LABELS "processing;backend")

mib_add_backend_test_executable(mib_processing_object_tracking_test processing/processing_object_tracking_test.cpp)
add_test(NAME backend.processing_object_tracking COMMAND $<TARGET_FILE:mib_backend_tests> mib_processing_object_tracking_test)
set_tests_properties(backend.processing_object_tracking PROPERTIES LABELS "processing;backend")

# --- Hardware-present tests (LABEL "hardware") -----------------------------
# Excluded from the default presets; run via the hardware preset or
# `ctest -L hardware`. Each self-skips (exit 77 -> CTest "Skipped") when its
# device env var is absent, so a normal run on a dev box without hardware is
# clean.
mib_add_backend_test_executable(hw_syringe_pump_test hardware/hw_syringe_pump_test.cpp)
add_test(NAME hardware.syringe_pump COMMAND $<TARGET_FILE:mib_backend_tests> hw_syringe_pump_test)
set_tests_properties(hardware.syringe_pump PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)

mib_add_backend_test_executable(hw_peristaltic_pump_test hardware/hw_peristaltic_pump_test.cpp)
add_test(NAME hardware.peristaltic_pump COMMAND $<TARGET_FILE:mib_backend_tests> hw_peristaltic_pump_test)
set_tests_properties(hardware.peristaltic_pump PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 120)

mib_add_backend_test_executable(hw_nanopositioner_test hardware/hw_nanopositioner_test.cpp)
add_test(NAME hardware.nanopositioner COMMAND $<TARGET_FILE:mib_backend_tests> hw_nanopositioner_test)
set_tests_properties(hardware.nanopositioner PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)

mib_add_backend_test_executable(hw_camera_test hardware/hw_camera_test.cpp)
add_test(NAME hardware.camera COMMAND $<TARGET_FILE:mib_backend_tests> hw_camera_test)
set_tests_properties(hardware.camera PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)

# Issue #413: the complete one-click illuminated Live View cycle against the
# real XGC/R5D rig (discovery, coordinated start, frames, coordinated stop,
# generator-OFF readback, restart). Gated by MIB_TEST_ILLUMINATED_LIVE.
mib_add_backend_test_executable(hw_illuminated_live_test hardware/hw_illuminated_live_test.cpp)
add_test(NAME hardware.illuminated_live COMMAND $<TARGET_FILE:mib_backend_tests> hw_illuminated_live_test)
set_tests_properties(hardware.illuminated_live PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 300)

mib_add_backend_test_executable(hw_egrabber_script_test hardware/hw_egrabber_script_test.cpp)
add_test(NAME hardware.egrabber_script COMMAND $<TARGET_FILE:mib_backend_tests> hw_egrabber_script_test)
set_tests_properties(hardware.egrabber_script PROPERTIES LABELS "hardware;camera" SKIP_RETURN_CODE 77 TIMEOUT 60)

# Frontend utility tests exercise src/frontend/utils code that legitimately uses
# Qt (Qt-JSON, QSettings, QCryptographicHash, QDir, ...) and is only built in the
# full (frontend) build. They link Qt6::Core directly, so gate them out of
# MIB_BUILD_BACKEND_ONLY — that lets the backend-only build require no Qt SDK
# (epic #246 Phase 1 exit gate). They still build/run in the full build
# (Windows CI), the only place the frontend is compiled.
if(NOT MIB_BUILD_BACKEND_ONLY)

add_executable(json_flatten_roundtrip_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/json_flatten_roundtrip_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/JsonFlatten.cpp
)
target_include_directories(json_flatten_roundtrip_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_definitions(json_flatten_roundtrip_test PRIVATE
    MIB_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(json_flatten_roundtrip_test PRIVATE Qt6::Core)
set_target_properties(json_flatten_roundtrip_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.json_flatten_roundtrip COMMAND $<TARGET_FILE:json_flatten_roundtrip_test>)
set_tests_properties(frontend.json_flatten_roundtrip PROPERTIES LABELS "frontend;utility")

add_executable(json_config_merge_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/json_config_merge_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/JsonConfigMerge.cpp
)
target_include_directories(json_config_merge_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(json_config_merge_test PRIVATE Qt6::Core)
set_target_properties(json_config_merge_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.json_config_merge COMMAND $<TARGET_FILE:json_config_merge_test>)
set_tests_properties(frontend.json_config_merge PROPERTIES LABELS "frontend;utility" TIMEOUT 30)

add_executable(processing_core_catalog_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/processing_core_catalog_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/ProcessingCoreCatalog.cpp
)
target_include_directories(processing_core_catalog_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(processing_core_catalog_test PRIVATE Qt6::Core)
set_target_properties(processing_core_catalog_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.processing_core_catalog COMMAND $<TARGET_FILE:processing_core_catalog_test>)
set_tests_properties(frontend.processing_core_catalog PROPERTIES LABELS "frontend;utility" TIMEOUT 30)

add_executable(registry_http_transport_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/registry_http_transport_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/system/RegistryHttpTransport.cpp
)
target_include_directories(registry_http_transport_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(registry_http_transport_test PRIVATE Qt6::Core Qt6::Network Threads::Threads)
set_target_properties(registry_http_transport_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.registry_http_transport COMMAND $<TARGET_FILE:registry_http_transport_test>)
set_tests_properties(frontend.registry_http_transport PROPERTIES LABELS "frontend;profiles" TIMEOUT 60)

add_executable(application_settings_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/application_settings_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/ApplicationSettings.cpp
)
target_include_directories(application_settings_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(application_settings_test PRIVATE Qt6::Core)
set_target_properties(application_settings_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.application_settings COMMAND $<TARGET_FILE:application_settings_test>)
set_tests_properties(frontend.application_settings PROPERTIES LABELS "frontend;utility" TIMEOUT 30)

add_executable(processing_core_settings_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/processing_core_settings_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/ProcessingCoreSettings.cpp
)
target_include_directories(processing_core_settings_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(processing_core_settings_test PRIVATE Qt6::Core)
set_target_properties(processing_core_settings_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.processing_core_settings COMMAND $<TARGET_FILE:processing_core_settings_test>)
set_tests_properties(frontend.processing_core_settings PROPERTIES LABELS "frontend;utility" TIMEOUT 30)


add_executable(hdf_review_export_paths_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/hdf_review_export_paths_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/HdfReviewExportPaths.cpp
)
target_include_directories(hdf_review_export_paths_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(hdf_review_export_paths_test PRIVATE Qt6::Core)
set_target_properties(hdf_review_export_paths_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.hdf_review_export_paths COMMAND $<TARGET_FILE:hdf_review_export_paths_test>)
set_tests_properties(frontend.hdf_review_export_paths PROPERTIES LABELS "frontend;utility" TIMEOUT 30)

# Issues #358/#359: pure window/sidebar geometry decisions (no QApplication).
add_executable(window_geometry_policy_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/window_geometry_policy_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/WindowGeometryPolicy.cpp
)
target_include_directories(window_geometry_policy_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(window_geometry_policy_test PRIVATE Qt6::Core)
set_target_properties(window_geometry_policy_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.window_geometry_policy COMMAND $<TARGET_FILE:window_geometry_policy_test>)
set_tests_properties(frontend.window_geometry_policy PROPERTIES LABELS "frontend;utility;layout" TIMEOUT 30)

# Issue #361: pure editor-document state + checked QSaveFile document store.
add_executable(config_document_state_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/config_document_state_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/system/ConfigDocumentStore.cpp
)
target_include_directories(config_document_state_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(config_document_state_test PRIVATE Qt6::Core)
set_target_properties(config_document_state_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
add_test(NAME frontend.config_document_state COMMAND $<TARGET_FILE:config_document_state_test>)
set_tests_properties(frontend.config_document_state PROPERTIES LABELS "frontend;utility;safety" TIMEOUT 30)

# Issue #364: pure processing tune-panel draft model (field mapping, patch
# precision, dirty/conflict/apply lifecycle). QtCore + the Qt-free backend
# types only.
add_executable(processing_config_draft_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/processing_config_draft_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/models/ProcessingConfigDraft.cpp
)
target_include_directories(processing_config_draft_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(processing_config_draft_test PRIVATE Qt6::Core mib_processing)
set_target_properties(processing_config_draft_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>")
add_test(NAME frontend.config_draft COMMAND $<TARGET_FILE:processing_config_draft_test>)
set_tests_properties(frontend.config_draft PROPERTIES LABELS "frontend;utility;safety" TIMEOUT 30)

# Issue #363: pure run-state / alert model semantics (QtCore only; automoc for the QObject header).
add_executable(run_status_model_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/run_status_model_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/models/RunStatusModel.cpp
    ${PROJECT_SOURCE_DIR}/include/frontend/models/RunStatusModel.h
)
target_include_directories(run_status_model_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(run_status_model_test PRIVATE Qt6::Core)
set_target_properties(run_status_model_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>" AUTOMOC ON)
add_test(NAME frontend.run_status_model COMMAND $<TARGET_FILE:run_status_model_test>)
set_tests_properties(frontend.run_status_model PROPERTIES LABELS "frontend;utility;safety" TIMEOUT 30)

add_executable(update_catalog_test
    ${PROJECT_SOURCE_DIR}/tests/frontend/update_catalog_test.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/utils/UpdateCatalog.cpp
)
target_include_directories(update_catalog_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)
target_compile_definitions(update_catalog_test PRIVATE
    MIB_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(update_catalog_test PRIVATE Qt6::Core)
set_target_properties(update_catalog_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
)
add_test(NAME frontend.update_catalog COMMAND $<TARGET_FILE:update_catalog_test>)
set_tests_properties(frontend.update_catalog PROPERTIES LABELS "frontend;utility" TIMEOUT 30)

endif() # NOT MIB_BUILD_BACKEND_ONLY (frontend;utility tests)

if(Python3_Interpreter_FOUND)
    add_test(
        NAME scripts.mindvision_release_gate
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_mindvision_release_gate.py
    )
    set_tests_properties(scripts.mindvision_release_gate PROPERTIES
        LABELS "scripts;camera;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.yofo_standing_package
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_yofo_standing_package.py
    )
    set_tests_properties(scripts.yofo_standing_package PROPERTIES
        LABELS "scripts;pl"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.export_hdf5_paths
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_export_hdf5_paths.py
    )
    set_tests_properties(scripts.export_hdf5_paths PROPERTIES
        LABELS "scripts;hdf5"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.export_hdf5_streaming
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_export_hdf5_streaming.py
    )
    set_tests_properties(scripts.export_hdf5_streaming PROPERTIES
        LABELS "scripts;hdf5;safety"
        TIMEOUT 120
    )

    add_test(
        NAME scripts.hdf5_export_app_lifecycle
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_hdf5_export_app_lifecycle.py
    )
    set_tests_properties(scripts.hdf5_export_app_lifecycle PROPERTIES
        LABELS "scripts;hdf5;safety"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 300
    )

    # 50-round PySide soak release gate (scheduled/manual; excluded from fast lanes).
    add_test(
        NAME scripts.exporter_soak
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/exporter_soak.py --cycles 50 --mode gui
    )
    set_tests_properties(scripts.exporter_soak PROPERTIES
        LABELS "scripts;performance;soak"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 1800
        SKIP_RETURN_CODE 77
    )

    add_test(
        NAME scripts.convert_legacy_csv_to_json
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_convert_legacy_csv_to_json.py
    )
    set_tests_properties(scripts.convert_legacy_csv_to_json PROPERTIES
        LABELS "scripts;hdf5"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.gold_standard_schema_contract
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_gold_standard_schema_contract.py
    )
    set_tests_properties(scripts.gold_standard_schema_contract PROPERTIES
        LABELS "scripts;hdf5"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.pz7035_abi_vendor
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/vendor_pz7035_abi.py --check
    )
    set_tests_properties(scripts.pz7035_abi_vendor PROPERTIES
        LABELS "scripts;pl"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.contract3_export_review
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_contract3_export_review.py
    )
    set_tests_properties(scripts.contract3_export_review PROPERTIES
        LABELS "scripts;hdf5"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.contract2_export_review
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_contract2_export_review.py
    )
    set_tests_properties(scripts.contract2_export_review PROPERTIES
        LABELS "scripts;hdf5"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.publish_processing_core
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_publish_processing_core.py
    )
    set_tests_properties(scripts.publish_processing_core PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.publish_profiles
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_publish_profiles.py
    )
    set_tests_properties(scripts.publish_profiles PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.installer_excludes_cores
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_installer_excludes_cores.py
    )
    set_tests_properties(scripts.installer_excludes_cores PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    # ADR 0007 / rollout T1.2: the built absdiff-laplacian core reproduces the
    # Contract-2 real-frame gold through engine ABI v2 (ctypes, no host code).
    # Not in sanitizer builds: an uninstrumented python cannot dlopen a
    # sanitizer-instrumented core (TSan: "cannot allocate memory in static TLS
    # block"). The native-core CI jobs run it on the unsanitized artifacts.
    if(TARGET mib_processing_core_absdiff_laplacian AND NOT MIB_SANITIZER)
        add_test(
            NAME processing.native_core_contract2_gold
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run_native_core_conformance.py
                --core $<TARGET_FILE:mib_processing_core_absdiff_laplacian>
                --frames-npz ${PROJECT_SOURCE_DIR}/scripts/conformance/focus-50v-real.npz
                --reference ${PROJECT_SOURCE_DIR}/scripts/conformance/focus-50v-real-contract2.json
                --skip-if-missing-numpy
        )
        set_tests_properties(processing.native_core_contract2_gold PROPERTIES
            LABELS "processing;contract"
            TIMEOUT 120
            SKIP_RETURN_CODE 77
        )
    endif()

    add_test(
        NAME scripts.contract_version_consistency
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_contract_version_consistency.py
    )
    set_tests_properties(scripts.contract_version_consistency PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.bump_mib_processing_version
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_bump_mib_processing_version.py
    )
    set_tests_properties(scripts.bump_mib_processing_version PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.verify_processing_core_manifest
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_verify_processing_core_manifest.py
    )
    set_tests_properties(scripts.verify_processing_core_manifest PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.processing_core_signer_release_gate
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_processing_core_signer_release_gate.py
    )
    set_tests_properties(scripts.processing_core_signer_release_gate PROPERTIES
        LABELS "scripts;contract;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.desktop_release_version
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_desktop_release_version.py
    )
    set_tests_properties(scripts.desktop_release_version PROPERTIES
        LABELS "scripts;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.release_notes
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/test_release_notes.py
    )
    set_tests_properties(scripts.release_notes PROPERTIES
        LABELS "scripts;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.publish_update_index
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/test_publish_index_merge.py
    )
    set_tests_properties(scripts.publish_update_index PROPERTIES
        LABELS "scripts;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.publish_review_update
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/test_publish_review_update.py
    )
    set_tests_properties(scripts.publish_review_update PROPERTIES
        LABELS "scripts;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.compare_metrics
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_compare_metrics.py
    )
    set_tests_properties(scripts.compare_metrics PROPERTIES
        LABELS "scripts;contract;processing"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.dot_grid_reference
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/dot_grid/test_dotgrid.py
    )
    set_tests_properties(scripts.dot_grid_reference PROPERTIES
        LABELS "scripts;processing"
        TIMEOUT 120
        SKIP_RETURN_CODE 77
    )

    add_test(
        NAME scripts.run_processing_conformance_input
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_run_processing_conformance.py
    )
    set_tests_properties(scripts.run_processing_conformance_input PROPERTIES
        LABELS "scripts;contract;processing;hdf5"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.kin10_hf_dataset_runner
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_kin10_run_hf_dataset_test.py
    )
    set_tests_properties(scripts.kin10_hf_dataset_runner PROPERTIES
        LABELS "scripts;integration"
        TIMEOUT 30
    )
endif()

# Windows: headless Qt frontend tests need the offscreen platform plugin (see cmake/MIBQtOffscreenTests.cmake).
include(MIBQtOffscreenTests)
mib_apply_qt_offscreen_plugin_path()
mib_add_backend_test_executable(hw_discovery_reentry_test hardware/hw_discovery_reentry_test.cpp)
mib_backend_test_link(hw_discovery_reentry_test PRIVATE Threads::Threads)
add_test(NAME hardware.discovery_reentry COMMAND $<TARGET_FILE:mib_backend_tests> hw_discovery_reentry_test)
set_tests_properties(hardware.discovery_reentry PROPERTIES LABELS "hardware;camera" SKIP_RETURN_CODE 77 TIMEOUT 120)

mib_add_backend_test_executable(export_bridge_facade_test backend/export_bridge_facade_test.cpp)
add_test(NAME backend.export_bridge_facade COMMAND $<TARGET_FILE:mib_backend_tests> export_bridge_facade_test)
set_tests_properties(backend.export_bridge_facade PROPERTIES LABELS "backend;bridge;recording;stress" TIMEOUT 45)

mib_add_backend_test_executable(frame_identity_test backend/frame_identity_test.cpp)
add_test(NAME backend.frame_identity COMMAND $<TARGET_FILE:mib_backend_tests> frame_identity_test)
set_tests_properties(backend.frame_identity PROPERTIES LABELS "backend;bridge;stress;invariant" TIMEOUT 45)
mib_add_backend_test_executable(processed_preview_packet_test backend/processed_preview_packet_test.cpp)
add_test(NAME backend.processed_preview_packet COMMAND $<TARGET_FILE:mib_backend_tests> processed_preview_packet_test)
set_tests_properties(backend.processed_preview_packet PROPERTIES LABELS "backend;bridge;processing;concurrency" TIMEOUT 45)

mib_add_backend_test_executable(profile_registry_test profiles/profile_registry_test.cpp)
add_test(NAME profiles.registry COMMAND $<TARGET_FILE:mib_backend_tests> profile_registry_test)
set_tests_properties(profiles.registry PROPERTIES LABELS "backend;profiles;roundtrip;fault-injection" TIMEOUT 30)

mib_add_backend_test_executable(profile_registry_worker_test profiles/profile_registry_worker_test.cpp)
mib_backend_test_link(profile_registry_worker_test PRIVATE Threads::Threads)
add_test(NAME profiles.registry_worker COMMAND $<TARGET_FILE:mib_backend_tests> profile_registry_worker_test)
set_tests_properties(profiles.registry_worker PROPERTIES LABELS "backend;profiles;concurrency;fault-injection" TIMEOUT 60)

mib_add_backend_test_executable(profile_registry_backend_test profiles/profile_registry_backend_test.cpp)
mib_backend_test_link(profile_registry_backend_test PRIVATE Threads::Threads)
add_test(NAME profiles.registry_backend COMMAND $<TARGET_FILE:mib_backend_tests> profile_registry_backend_test)
set_tests_properties(profiles.registry_backend PROPERTIES LABELS "backend;profiles;concurrency" TIMEOUT 60)

mib_add_backend_test_executable(registry_facade_test profiles/registry_facade_test.cpp)
mib_backend_test_link(registry_facade_test PRIVATE Threads::Threads)
add_test(NAME profiles.registry_facade COMMAND $<TARGET_FILE:mib_backend_tests> registry_facade_test)
set_tests_properties(profiles.registry_facade PROPERTIES LABELS "backend;profiles;contract" TIMEOUT 60)

mib_add_backend_test_executable(instrument_identity_test profiles/instrument_identity_test.cpp)
add_test(NAME profiles.instrument_identity COMMAND $<TARGET_FILE:mib_backend_tests> instrument_identity_test)
set_tests_properties(profiles.instrument_identity PROPERTIES LABELS "backend;profiles;fault-injection" TIMEOUT 30)

mib_add_backend_test_executable(registry_method_test profiles/registry_method_test.cpp)
mib_backend_test_link(registry_method_test PRIVATE Threads::Threads)
add_test(NAME profiles.registry_method COMMAND $<TARGET_FILE:mib_backend_tests> registry_method_test)
set_tests_properties(profiles.registry_method PROPERTIES LABELS "backend;profiles;roundtrip;fault-injection" TIMEOUT 60)

mib_add_backend_test_executable(method_provenance_test backend/method_provenance_test.cpp)
add_test(NAME backend.method_provenance COMMAND $<TARGET_FILE:mib_backend_tests> method_provenance_test)
set_tests_properties(backend.method_provenance PROPERTIES LABELS "backend;profiles;contract" TIMEOUT 30)

mib_add_backend_test_executable(method_gate_e2e_test profiles/method_gate_e2e_test.cpp)
mib_backend_test_link(method_gate_e2e_test PRIVATE Threads::Threads)
add_test(NAME e2e.method_gate COMMAND $<TARGET_FILE:mib_backend_tests> method_gate_e2e_test)
set_tests_properties(e2e.method_gate PROPERTIES LABELS "e2e;profiles;hdf5;roundtrip" TIMEOUT 120)

mib_add_backend_test_executable(startup_configuration_test profiles/startup_configuration_test.cpp)
mib_backend_test_link(startup_configuration_test PRIVATE Threads::Threads)
add_test(NAME profiles.startup_configuration COMMAND $<TARGET_FILE:mib_backend_tests> startup_configuration_test)
set_tests_properties(profiles.startup_configuration PROPERTIES LABELS "backend;profiles;e2e;roundtrip" TIMEOUT 90)

mib_add_backend_test_executable(registry_authoring_test profiles/registry_authoring_test.cpp)
mib_backend_test_link(registry_authoring_test PRIVATE Threads::Threads)
add_test(NAME profiles.registry_authoring COMMAND $<TARGET_FILE:mib_backend_tests> registry_authoring_test)
set_tests_properties(profiles.registry_authoring PROPERTIES LABELS "backend;profiles;e2e;fault-injection" TIMEOUT 90)

mib_add_backend_test_executable(config_document_apply_test backend/config_document_apply_test.cpp)
add_test(NAME backend.config_document_apply COMMAND $<TARGET_FILE:mib_backend_tests> config_document_apply_test)
set_tests_properties(backend.config_document_apply PROPERTIES LABELS "backend;profiles;fault-injection;roundtrip" TIMEOUT 60)

mib_add_backend_test_executable(multi_image_series_roundtrip_test recording/multi_image_series_roundtrip_test.cpp)
add_test(NAME recording.multi_image_series_roundtrip COMMAND $<TARGET_FILE:mib_backend_tests> multi_image_series_roundtrip_test)
set_tests_properties(recording.multi_image_series_roundtrip PROPERTIES LABELS "recording;hdf5;roundtrip;invariant" TIMEOUT 30)
mib_add_backend_test_executable(hdf_write_queue_fault_test backend/hdf_write_queue_fault_test.cpp)
mib_backend_test_link(hdf_write_queue_fault_test PRIVATE Threads::Threads)
add_test(NAME backend.hdf_write_queue_fault COMMAND $<TARGET_FILE:mib_backend_tests> hdf_write_queue_fault_test)
set_tests_properties(backend.hdf_write_queue_fault PROPERTIES LABELS "backend;recording;stress;fault-injection" TIMEOUT 30)

mib_test_runner_finalize(mib_backend_tests)
target_link_libraries(mib_backend_tests PRIVATE mib_backend)
# The vendored PZ7035 ABI header, for tests that fake bridge registers
# (processing.pz_platform_monitor).
target_include_directories(mib_backend_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/pz7035-abi)

if(NOT MIB_ENABLE_HARDWARE_SDKS)
    add_executable(autofocus_callback_test backend/autofocus_callback_test.cpp)
    target_include_directories(autofocus_callback_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(autofocus_callback_test PRIVATE mib_backend Threads::Threads)
    add_test(NAME backend.autofocus_callback COMMAND autofocus_callback_test)
    set_tests_properties(backend.autofocus_callback PROPERTIES LABELS "backend;concurrency;safety" TIMEOUT 30)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME installers.vc_runtime_check COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/installers/vc_runtime_check_test.py)
    set_tests_properties(installers.vc_runtime_check PROPERTIES LABELS "installer;regression" TIMEOUT 10)
endif()
# Issue 403: persistent regressions backed by the mock-camera lifecycle harness.
foreach(scenario IN ITEMS byte_pressure partial_series partial_series_no_frames partial_series_restart)
    add_test(NAME e2e.recording_403_${scenario}
        COMMAND $<TARGET_FILE:mib_backend_tests> experiment_coordinator_test)
    set_tests_properties(e2e.recording_403_${scenario} PROPERTIES
        LABELS "backend;e2e;recording;regression" TIMEOUT 40
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MIB_REPRO_ASSERT_SAFE=1;MIB_REPRO_INTERVAL_MS=100")
endforeach()
set_property(TEST e2e.recording_403_byte_pressure APPEND PROPERTY ENVIRONMENT
    "MIB_REPRO_BUFFER_CAP=393216;MIB_REPRO_FLUSH=100")
foreach(scenario IN ITEMS partial_series partial_series_no_frames partial_series_restart)
    set_property(TEST e2e.recording_403_${scenario} APPEND PROPERTY ENVIRONMENT
        "MIB_REPRO_BUFFER_CAP=10485760;MIB_REPRO_SERIES=100")
endforeach()
set_property(TEST e2e.recording_403_partial_series_no_frames APPEND PROPERTY ENVIRONMENT
    "MIB_REPRO_NO_MORE_FRAMES=1")

set_property(TEST e2e.recording_403_partial_series_restart APPEND PROPERTY ENVIRONMENT
    "MIB_REPRO_RESTART=1")
