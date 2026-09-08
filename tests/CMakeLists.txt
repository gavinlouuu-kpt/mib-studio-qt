find_package(Python3 COMPONENTS Interpreter QUIET)

function(mib_add_backend_test_executable target source)
    add_executable(${target}
        ${PROJECT_SOURCE_DIR}/tests/${source}
    )
    target_link_libraries(${target} PRIVATE mib_backend)
    # Allow tests to include the shared support headers as "support/<x>.h".
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    )
endfunction()

mib_add_backend_test_executable(mib_backend_smoke_test backend/backend_smoke_test.cpp)
add_test(NAME backend.smoke COMMAND $<TARGET_FILE:mib_backend_smoke_test>)
set_tests_properties(backend.smoke PROPERTIES LABELS "backend")

mib_add_backend_test_executable(emodulus_lut_catalog_test backend/emodulus_lut_catalog_test.cpp)
add_test(NAME backend.emodulus_lut_catalog COMMAND $<TARGET_FILE:emodulus_lut_catalog_test>)
set_tests_properties(backend.emodulus_lut_catalog PROPERTIES LABELS "backend;processing")

mib_add_backend_test_executable(emodulus_lut_degenerate_test backend/emodulus_lut_degenerate_test.cpp)
add_test(NAME backend.emodulus_lut_degenerate COMMAND $<TARGET_FILE:emodulus_lut_degenerate_test>)
set_tests_properties(backend.emodulus_lut_degenerate PROPERTIES LABELS "backend;processing")

mib_add_backend_test_executable(backend_lifecycle_smoke_test backend/backend_lifecycle_smoke_test.cpp)
add_test(NAME backend.lifecycle_smoke COMMAND $<TARGET_FILE:backend_lifecycle_smoke_test>)
set_tests_properties(backend.lifecycle_smoke PROPERTIES LABELS "backend")

mib_add_backend_test_executable(frame_store_concurrency_test backend/frame_store_concurrency_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(frame_store_concurrency_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_concurrency COMMAND $<TARGET_FILE:frame_store_concurrency_test>)
set_tests_properties(backend.frame_store_concurrency PROPERTIES LABELS "backend")

mib_add_backend_test_executable(frame_store_bounds_test backend/frame_store_bounds_test.cpp)
add_test(NAME backend.frame_store_bounds COMMAND $<TARGET_FILE:frame_store_bounds_test>)
set_tests_properties(backend.frame_store_bounds PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(modbus_rtu_test backend/modbus_rtu_test.cpp)
add_test(NAME backend.modbus_rtu COMMAND $<TARGET_FILE:modbus_rtu_test>)
set_tests_properties(backend.modbus_rtu PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(syringe_pump_fake_serial_test backend/syringe_pump_fake_serial_test.cpp)
add_test(NAME backend.syringe_pump_fake_serial COMMAND $<TARGET_FILE:syringe_pump_fake_serial_test>)
set_tests_properties(backend.syringe_pump_fake_serial PROPERTIES LABELS "backend" TIMEOUT 30)

# BE-7 (#277): facade PumpCommand surface over the fake Modbus serial — both
# pump identities, structured errors, COM-port conflict rules, safe disconnect.
mib_add_backend_test_executable(pump_bridge_facade_test backend/pump_bridge_facade_test.cpp)
add_test(NAME backend.pump_bridge_facade COMMAND $<TARGET_FILE:pump_bridge_facade_test>)
set_tests_properties(backend.pump_bridge_facade PROPERTIES LABELS "backend;bridge" TIMEOUT 60)

mib_add_backend_test_executable(serial_port_posix_loopback_test backend/serial_port_posix_loopback_test.cpp)
add_test(NAME backend.serial_port_posix_loopback COMMAND $<TARGET_FILE:serial_port_posix_loopback_test>)
set_tests_properties(backend.serial_port_posix_loopback PROPERTIES LABELS "backend" TIMEOUT 30)

# Qt-free serial port enumeration (SetupAPI / sysfs) behind serialbus::availablePorts().
mib_add_backend_test_executable(serial_port_enumeration_test backend/serial_port_enumeration_test.cpp)
add_test(NAME backend.serial_port_enumeration COMMAND $<TARGET_FILE:serial_port_enumeration_test>)
set_tests_properties(backend.serial_port_enumeration PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(autofocus_math_test backend/autofocus_math_test.cpp)
add_test(NAME backend.autofocus_math COMMAND $<TARGET_FILE:autofocus_math_test>)
set_tests_properties(backend.autofocus_math PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(hdf_write_queue_test backend/hdf_write_queue_test.cpp)
target_link_libraries(hdf_write_queue_test PRIVATE Threads::Threads)
add_test(NAME backend.hdf_write_queue COMMAND $<TARGET_FILE:hdf_write_queue_test>)
set_tests_properties(backend.hdf_write_queue PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(frame_store_reserve_test backend/frame_store_reserve_test.cpp)
add_test(NAME backend.frame_store_reserve COMMAND $<TARGET_FILE:frame_store_reserve_test>)
set_tests_properties(backend.frame_store_reserve PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(recording_isframeempty_roi_test backend/recording_isframeempty_roi_test.cpp)
add_test(NAME backend.recording_isframeempty_roi COMMAND $<TARGET_FILE:recording_isframeempty_roi_test>)
set_tests_properties(backend.recording_isframeempty_roi PROPERTIES LABELS "backend;processing;recording" TIMEOUT 30)

mib_add_backend_test_executable(pr4_monitoring_snapshot_test backend/pr4_monitoring_snapshot_test.cpp)
add_test(NAME backend.pr4_monitoring_snapshot COMMAND $<TARGET_FILE:pr4_monitoring_snapshot_test>)
set_tests_properties(backend.pr4_monitoring_snapshot PROPERTIES LABELS "backend;processing;monitoring" TIMEOUT 30)

mib_add_backend_test_executable(roi_crop_test backend/roi_crop_test.cpp)
add_test(NAME backend.roi_crop COMMAND $<TARGET_FILE:roi_crop_test>)
set_tests_properties(backend.roi_crop PROPERTIES LABELS "backend" TIMEOUT 30)

mib_add_backend_test_executable(pulse_generator_frame_test backend/pulse_generator_frame_test.cpp)
add_test(NAME backend.pulse_generator_frame COMMAND $<TARGET_FILE:pulse_generator_frame_test>)
set_tests_properties(backend.pulse_generator_frame PROPERTIES LABELS "backend" TIMEOUT 30)

# Shared RS485 bus + pulse-generator discovery/control over a POSIX pty
# (compiles to a trivial pass on Windows).
mib_add_backend_test_executable(serial_bus_pty_test backend/serial_bus_pty_test.cpp)
target_link_libraries(serial_bus_pty_test PRIVATE Threads::Threads)
add_test(NAME backend.serial_bus_pty COMMAND $<TARGET_FILE:serial_bus_pty_test>)
set_tests_properties(backend.serial_bus_pty PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(mindvision_config_test backend/mindvision_config_test.cpp)
add_test(NAME backend.mindvision_config COMMAND $<TARGET_FILE:mindvision_config_test>)
set_tests_properties(backend.mindvision_config PROPERTIES LABELS "backend;camera" TIMEOUT 30)

# Issue #366: MindVision Mono8/geometry fail-closed conversion, driven through
# the injectable SDK seam (no hardware, no vendor headers).
mib_add_backend_test_executable(mindvision_conversion_fault_test backend/mindvision_conversion_fault_test.cpp)
target_link_libraries(mindvision_conversion_fault_test PRIVATE Threads::Threads)
add_test(NAME backend.mindvision_conversion_fault COMMAND $<TARGET_FILE:mindvision_conversion_fault_test>)
set_tests_properties(backend.mindvision_conversion_fault PROPERTIES LABELS "backend;camera;safety" TIMEOUT 60)

# Issue #365: single-owner capture lifecycle (start/stop/fault/restart,
# blocked grab, slow teardown, stress) and trigger session ownership.
mib_add_backend_test_executable(capture_lifecycle_test backend/capture_lifecycle_test.cpp)
target_link_libraries(capture_lifecycle_test PRIVATE Threads::Threads)
add_test(NAME backend.capture_lifecycle COMMAND $<TARGET_FILE:capture_lifecycle_test>)
set_tests_properties(backend.capture_lifecycle PROPERTIES LABELS "backend;camera;stress" TIMEOUT 120)

mib_add_backend_test_executable(trigger_session_test backend/trigger_session_test.cpp)
target_link_libraries(trigger_session_test PRIVATE Threads::Threads)
add_test(NAME backend.trigger_session COMMAND $<TARGET_FILE:trigger_session_test>)
set_tests_properties(backend.trigger_session PROPERTIES LABELS "backend;camera" TIMEOUT 60)

# Issue #368: timestamp semantics + per-metric telemetry validity/freshness.
mib_add_backend_test_executable(timestamp_telemetry_test backend/timestamp_telemetry_test.cpp)
target_link_libraries(timestamp_telemetry_test PRIVATE Threads::Threads)
add_test(NAME backend.timestamp_telemetry COMMAND $<TARGET_FILE:timestamp_telemetry_test>)
set_tests_properties(backend.timestamp_telemetry PROPERTIES LABELS "backend;camera;recording;hdf5" TIMEOUT 120)

mib_add_backend_test_executable(experiment_readiness_test backend/experiment_readiness_test.cpp)
target_link_libraries(experiment_readiness_test PRIVATE Threads::Threads)
add_test(NAME backend.experiment_readiness COMMAND $<TARGET_FILE:experiment_readiness_test>)
set_tests_properties(backend.experiment_readiness PROPERTIES LABELS "backend;processing;recording;hdf5;safety" TIMEOUT 180)

mib_add_backend_test_executable(camera_script_apply_test backend/camera_script_apply_test.cpp)
add_test(NAME backend.camera_script_apply COMMAND $<TARGET_FILE:camera_script_apply_test>)
set_tests_properties(backend.camera_script_apply PROPERTIES LABELS "backend;camera" TIMEOUT 30)

mib_add_backend_test_executable(frame_store_resize_under_load_test backend/frame_store_resize_under_load_test.cpp)
target_link_libraries(frame_store_resize_under_load_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_resize_under_load COMMAND $<TARGET_FILE:frame_store_resize_under_load_test>)
set_tests_properties(backend.frame_store_resize_under_load PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(pipeline_timing_benchmark performance/pipeline_timing_benchmark.cpp)
target_link_libraries(pipeline_timing_benchmark PRIVATE Threads::Threads)
add_test(NAME performance.pipeline_timing COMMAND $<TARGET_FILE:pipeline_timing_benchmark>)
set_tests_properties(performance.pipeline_timing PROPERTIES LABELS "performance" TIMEOUT 180)

# Issue #370: byte-budgeted ownership — deterministic policy test + benchmark.
mib_add_backend_test_executable(memory_budget_test processing/memory_budget_test.cpp)
target_link_libraries(memory_budget_test PRIVATE Threads::Threads)
add_test(NAME processing.memory_budget COMMAND $<TARGET_FILE:memory_budget_test>)
set_tests_properties(processing.memory_budget PROPERTIES LABELS "processing;backend;safety;memory" TIMEOUT 180)

mib_add_backend_test_executable(memory_budget_bench performance/memory_budget_bench.cpp)
target_link_libraries(memory_budget_bench PRIVATE Threads::Threads)
add_test(NAME performance.memory_budget COMMAND $<TARGET_FILE:memory_budget_bench>)
set_tests_properties(performance.memory_budget PROPERTIES LABELS "performance;memory" TIMEOUT 600)

mib_add_backend_test_executable(backend_facade_boundary_test backend/backend_facade_boundary_test.cpp)
add_test(NAME backend.facade_boundary COMMAND $<TARGET_FILE:backend_facade_boundary_test>)
set_tests_properties(backend.facade_boundary PROPERTIES LABELS "backend;bridge")

# BE-4 (#274): backend-owned experiment lifecycle state machine — pipeline e2e
# over the facade command surface plus fault-injection (fatal save error,
# double start/stop, shutdown during active experiment).
mib_add_backend_test_executable(experiment_coordinator_test backend/experiment_coordinator_test.cpp)
add_test(NAME e2e.experiment_coordinator COMMAND $<TARGET_FILE:experiment_coordinator_test>)
set_tests_properties(e2e.experiment_coordinator PROPERTIES LABELS "backend;bridge;e2e" TIMEOUT 180)

mib_add_backend_test_executable(mindvision_selection_state_test backend/mindvision_selection_state_test.cpp)
add_test(NAME backend.mindvision_selection_state COMMAND $<TARGET_FILE:mindvision_selection_state_test>)
set_tests_properties(backend.mindvision_selection_state PROPERTIES LABELS "backend;camera")

mib_add_backend_test_executable(mock_camera_smoke_test camera/mock_camera_smoke_test.cpp)
add_test(NAME camera.mock_smoke COMMAND $<TARGET_FILE:mock_camera_smoke_test>)
set_tests_properties(camera.mock_smoke PROPERTIES LABELS "camera;backend")

mib_add_backend_test_executable(delivery_mode_overload_test camera/delivery_mode_overload_test.cpp)
target_link_libraries(delivery_mode_overload_test PRIVATE Threads::Threads)
add_test(NAME camera.delivery_mode_overload COMMAND $<TARGET_FILE:delivery_mode_overload_test>)
set_tests_properties(camera.delivery_mode_overload PROPERTIES LABELS "camera;backend" TIMEOUT 60)

mib_add_backend_test_executable(delivery_mode_contract_test camera/delivery_mode_contract_test.cpp)
target_link_libraries(delivery_mode_contract_test PRIVATE Threads::Threads)
add_test(NAME camera.delivery_mode_contract COMMAND $<TARGET_FILE:delivery_mode_contract_test>)
set_tests_properties(camera.delivery_mode_contract PROPERTIES LABELS "camera;backend" TIMEOUT 60)

mib_add_backend_test_executable(processing_science_seam_test processing/processing_science_seam_test.cpp)
add_test(NAME processing.science_seam COMMAND $<TARGET_FILE:processing_science_seam_test>)
set_tests_properties(processing.science_seam PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_science_golden_test processing/processing_science_golden_test.cpp)
add_test(NAME processing.science_golden COMMAND $<TARGET_FILE:processing_science_golden_test>)
set_tests_properties(processing.science_golden PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_identification_metrics_test processing/processing_identification_metrics_test.cpp)
add_test(NAME processing.identification_metrics COMMAND $<TARGET_FILE:processing_identification_metrics_test>)
set_tests_properties(processing.identification_metrics PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_pipeline_smoke_test processing/processing_pipeline_smoke_test.cpp)
add_test(NAME processing.pipeline_smoke COMMAND $<TARGET_FILE:processing_pipeline_smoke_test>)
set_tests_properties(processing.pipeline_smoke PROPERTIES LABELS "processing;backend")

add_executable(processing_core_abi_c_test
    ${PROJECT_SOURCE_DIR}/tests/processing/processing_core_abi_c_test.c
)
target_include_directories(processing_core_abi_c_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(processing_core_abi_c_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
add_test(NAME processing.core_abi_c COMMAND $<TARGET_FILE:processing_core_abi_c_test>)
set_tests_properties(processing.core_abi_c PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_loader_test processing/processing_core_loader_test.cpp)
add_dependencies(processing_core_loader_test mib_processing_core)
add_test(NAME processing.core_loader
    COMMAND $<TARGET_FILE:processing_core_loader_test> $<TARGET_FILE:mib_processing_core>)
set_tests_properties(processing.core_loader PROPERTIES LABELS "processing;backend" TIMEOUT 30)

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
target_link_libraries(processing_core_activation_test PRIVATE Threads::Threads)
add_test(NAME processing.core_activation COMMAND $<TARGET_FILE:processing_core_activation_test>)
set_tests_properties(processing.core_activation PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(processing_core_activation_stress_test processing/processing_core_activation_stress_test.cpp)
target_link_libraries(processing_core_activation_stress_test PRIVATE Threads::Threads)
add_test(NAME processing.core_activation_stress COMMAND $<TARGET_FILE:processing_core_activation_stress_test>)
set_tests_properties(processing.core_activation_stress PROPERTIES LABELS "processing;backend;stress" TIMEOUT 60)

mib_add_backend_test_executable(processing_batch_pipeline_test processing/processing_batch_pipeline_test.cpp)
add_test(NAME backend.processing_batch_pipeline COMMAND $<TARGET_FILE:processing_batch_pipeline_test>)
set_tests_properties(backend.processing_batch_pipeline PROPERTIES LABELS "processing;backend")

# Issue #367: explicit frame accounting.
mib_add_backend_test_executable(frame_store_commit_test backend/frame_store_commit_test.cpp)
target_link_libraries(frame_store_commit_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_commit COMMAND $<TARGET_FILE:frame_store_commit_test>)
set_tests_properties(backend.frame_store_commit PROPERTIES LABELS "backend;safety" TIMEOUT 60)

mib_add_backend_test_executable(experiment_accounting_test processing/experiment_accounting_test.cpp)
target_link_libraries(experiment_accounting_test PRIVATE Threads::Threads)
add_test(NAME processing.experiment_accounting COMMAND $<TARGET_FILE:experiment_accounting_test>)
set_tests_properties(processing.experiment_accounting PROPERTIES LABELS "processing;backend;recording;safety" TIMEOUT 120)

mib_add_backend_test_executable(recording_accounting_test recording/recording_accounting_test.cpp)
target_link_libraries(recording_accounting_test PRIVATE Threads::Threads)
add_test(NAME recording.accounting COMMAND $<TARGET_FILE:recording_accounting_test>)
set_tests_properties(recording.accounting PROPERTIES LABELS "recording;backend;hdf5;safety" TIMEOUT 180)

mib_add_backend_test_executable(processing_fault_injection_test processing/processing_fault_injection_test.cpp)
add_test(NAME processing.fault_injection COMMAND $<TARGET_FILE:processing_fault_injection_test>)
set_tests_properties(processing.fault_injection PROPERTIES LABELS "processing;backend")

mib_add_backend_test_executable(recording_lifecycle_test recording/recording_lifecycle_test.cpp)
add_test(NAME recording.lifecycle COMMAND $<TARGET_FILE:recording_lifecycle_test>)
set_tests_properties(recording.lifecycle PROPERTIES LABELS "recording;backend")

mib_add_backend_test_executable(hdf5_resilience_test recording/hdf5_resilience_test.cpp)
add_test(NAME recording.hdf5_resilience COMMAND $<TARGET_FILE:hdf5_resilience_test>)
set_tests_properties(recording.hdf5_resilience PROPERTIES LABELS "recording;backend;hdf5")

mib_add_backend_test_executable(hdf5_save_performance_test recording/hdf5_save_performance_test.cpp)
add_test(NAME recording.hdf5_save_performance COMMAND $<TARGET_FILE:hdf5_save_performance_test>)
set_tests_properties(recording.hdf5_save_performance PROPERTIES LABELS "recording;backend;hdf5;performance")

mib_add_backend_test_executable(experiment_roundtrip_test recording/experiment_roundtrip_test.cpp)
add_test(NAME recording.experiment_roundtrip COMMAND $<TARGET_FILE:experiment_roundtrip_test>)
set_tests_properties(recording.experiment_roundtrip PROPERTIES LABELS "recording;backend" TIMEOUT 30)

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
    set_tests_properties(backend.kin10_hf_dataset_pipeline PROPERTIES
        LABELS "backend;integration"
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
    set_tests_properties(backend.kin10_hf_dataset_pipeline PROPERTIES
        LABELS "backend;integration"
        SKIP_RETURN_CODE 77
        TIMEOUT 180
    )
else()
    message(WARNING "Python3 interpreter not found; backend.kin10_hf_dataset_pipeline will not be registered")
endif()

mib_add_backend_test_executable(kin6_mib_app_capture_proof integration/kin6_mib_app_capture_proof.cpp)
add_test(NAME backend.kin6_mib_app_capture_proof COMMAND $<TARGET_FILE:kin6_mib_app_capture_proof>)
set_tests_properties(backend.kin6_mib_app_capture_proof PROPERTIES LABELS "backend;integration")

mib_add_backend_test_executable(e2e_storage_destinations_test integration/e2e_storage_destinations_test.cpp)
add_test(NAME integration.e2e_storage_destinations COMMAND $<TARGET_FILE:e2e_storage_destinations_test>)
set_tests_properties(integration.e2e_storage_destinations PROPERTIES LABELS "integration;e2e;recording" TIMEOUT 60)

mib_add_backend_test_executable(e2e_trigger_timing_test integration/e2e_trigger_timing_test.cpp)
target_link_libraries(e2e_trigger_timing_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_trigger_timing COMMAND $<TARGET_FILE:e2e_trigger_timing_test>)
set_tests_properties(integration.e2e_trigger_timing PROPERTIES LABELS "integration;e2e;trigger" TIMEOUT 120)

mib_add_backend_test_executable(frame_store_wait_test backend/frame_store_wait_test.cpp)
target_link_libraries(frame_store_wait_test PRIVATE Threads::Threads)
add_test(NAME backend.frame_store_wait COMMAND $<TARGET_FILE:frame_store_wait_test>)
set_tests_properties(backend.frame_store_wait PROPERTIES LABELS "backend" TIMEOUT 60)

mib_add_backend_test_executable(pipeline_timing_recorder_test backend/pipeline_timing_recorder_test.cpp)
target_link_libraries(pipeline_timing_recorder_test PRIVATE Threads::Threads)
add_test(NAME backend.pipeline_timing_recorder COMMAND $<TARGET_FILE:pipeline_timing_recorder_test>)
set_tests_properties(backend.pipeline_timing_recorder PROPERTIES LABELS "backend;diagnostics" TIMEOUT 30)

mib_add_backend_test_executable(crash_reporter_pending_upload_test backend/crash_reporter_pending_upload_test.cpp)
add_test(NAME backend.crash_reporter_pending_upload COMMAND $<TARGET_FILE:crash_reporter_pending_upload_test>)
set_tests_properties(backend.crash_reporter_pending_upload PROPERTIES LABELS "backend;observability" TIMEOUT 60)

mib_add_backend_test_executable(crash_reporter_terminate_test backend/crash_reporter_terminate_test.cpp)
target_link_libraries(crash_reporter_terminate_test PRIVATE Threads::Threads)
add_test(NAME backend.crash_reporter_terminate COMMAND $<TARGET_FILE:crash_reporter_terminate_test>)
set_tests_properties(backend.crash_reporter_terminate PROPERTIES LABELS "backend;observability" TIMEOUT 60)

mib_add_backend_test_executable(e2e_pipeline_timing_test integration/e2e_pipeline_timing_test.cpp)
target_link_libraries(e2e_pipeline_timing_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_pipeline_timing COMMAND $<TARGET_FILE:e2e_pipeline_timing_test>)
set_tests_properties(integration.e2e_pipeline_timing PROPERTIES LABELS "integration;e2e;trigger;diagnostics" TIMEOUT 120)

# Manual diagnosis harness (no add_test: needs a local frames folder).
# Full mock pipeline dry-run with latency instrumentation — see
# docs/howto/pipeline-latency-diagnosis.md.
mib_add_backend_test_executable(mock_pipeline_timing_run tools/mock_pipeline_timing_run.cpp)
target_link_libraries(mock_pipeline_timing_run PRIVATE Threads::Threads)

mib_add_backend_test_executable(e2e_pipeline_stress_test integration/e2e_pipeline_stress_test.cpp)
target_link_libraries(e2e_pipeline_stress_test PRIVATE Threads::Threads)
# Lifecycle start/stop race test. Has an in-process watchdog that _Exit(99)s
# with the stuck phase/cycle/step if a cycle deadlocks, so it fails fast instead
# of burning the full ctest timeout. Pass a larger [cycles] arg for heavy local
# stress runs. (The TriggerService lost-wakeup deadlock this used to expose is
# fixed; see backend.trigger_service_start_stop_race for the focused guard.)
add_test(NAME integration.e2e_pipeline_stress COMMAND $<TARGET_FILE:e2e_pipeline_stress_test>)
set_tests_properties(integration.e2e_pipeline_stress PROPERTIES LABELS "integration;e2e" TIMEOUT 120)

mib_add_backend_test_executable(e2e_live_view_latency_test integration/e2e_live_view_latency_test.cpp)
target_link_libraries(e2e_live_view_latency_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_live_view_latency COMMAND $<TARGET_FILE:e2e_live_view_latency_test>)
set_tests_properties(integration.e2e_live_view_latency PROPERTIES LABELS "integration;e2e;processing" TIMEOUT 120)

mib_add_backend_test_executable(e2e_realtime_throughput_test integration/e2e_realtime_throughput_test.cpp)
target_link_libraries(e2e_realtime_throughput_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_realtime_throughput COMMAND $<TARGET_FILE:e2e_realtime_throughput_test>)
set_tests_properties(integration.e2e_realtime_throughput PROPERTIES LABELS "integration;e2e;processing;performance" TIMEOUT 120)

mib_add_backend_test_executable(e2e_batch_backpressure_test integration/e2e_batch_backpressure_test.cpp)
target_link_libraries(e2e_batch_backpressure_test PRIVATE Threads::Threads)
add_test(NAME integration.e2e_batch_backpressure COMMAND $<TARGET_FILE:e2e_batch_backpressure_test>)
set_tests_properties(integration.e2e_batch_backpressure PROPERTIES LABELS "integration;e2e;processing;performance" TIMEOUT 120)

mib_add_backend_test_executable(realtime_drop_frames_default_test processing/realtime_drop_frames_default_test.cpp)
add_test(NAME processing.realtime_drop_frames_default COMMAND $<TARGET_FILE:realtime_drop_frames_default_test>)
set_tests_properties(processing.realtime_drop_frames_default PROPERTIES LABELS "processing;backend" TIMEOUT 30)

mib_add_backend_test_executable(mib_processing_multi_object_test processing/processing_multi_object_test.cpp)
add_test(NAME backend.processing_multi_object COMMAND $<TARGET_FILE:mib_processing_multi_object_test>)
set_tests_properties(backend.processing_multi_object PROPERTIES LABELS "processing;backend")

mib_add_backend_test_executable(mib_processing_object_tracking_test processing/processing_object_tracking_test.cpp)
add_test(NAME backend.processing_object_tracking COMMAND $<TARGET_FILE:mib_processing_object_tracking_test>)
set_tests_properties(backend.processing_object_tracking PROPERTIES LABELS "processing;backend")

# --- Hardware-present tests (LABEL "hardware") -----------------------------
# Excluded from the default presets; run via the hardware preset or
# `ctest -L hardware`. Each self-skips (exit 77 -> CTest "Skipped") when its
# device env var is absent, so a normal run on a dev box without hardware is
# clean.
mib_add_backend_test_executable(hw_syringe_pump_test hardware/hw_syringe_pump_test.cpp)
add_test(NAME hardware.syringe_pump COMMAND $<TARGET_FILE:hw_syringe_pump_test>)
set_tests_properties(hardware.syringe_pump PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)

mib_add_backend_test_executable(hw_nanopositioner_test hardware/hw_nanopositioner_test.cpp)
add_test(NAME hardware.nanopositioner COMMAND $<TARGET_FILE:hw_nanopositioner_test>)
set_tests_properties(hardware.nanopositioner PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)

mib_add_backend_test_executable(hw_camera_test hardware/hw_camera_test.cpp)
add_test(NAME hardware.camera COMMAND $<TARGET_FILE:hw_camera_test>)
set_tests_properties(hardware.camera PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)

mib_add_backend_test_executable(hw_egrabber_script_test hardware/hw_egrabber_script_test.cpp)
add_test(NAME hardware.egrabber_script COMMAND $<TARGET_FILE:hw_egrabber_script_test>)
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
add_test(NAME frontend.processing_core_catalog COMMAND $<TARGET_FILE:processing_core_catalog_test>)
set_tests_properties(frontend.processing_core_catalog PROPERTIES LABELS "frontend;utility" TIMEOUT 30)

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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
set_target_properties(config_document_state_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
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
set_target_properties(processing_config_draft_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
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
set_target_properties(run_status_model_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" AUTOMOC ON)
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
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
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
        NAME scripts.publish_processing_core
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_publish_processing_core.py
    )
    set_tests_properties(scripts.publish_processing_core PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.publish_profiles
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_publish_profiles.py
    )
    set_tests_properties(scripts.publish_profiles PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.contract_version_consistency
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_contract_version_consistency.py
    )
    set_tests_properties(scripts.contract_version_consistency PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.bump_mib_processing_version
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_bump_mib_processing_version.py
    )
    set_tests_properties(scripts.bump_mib_processing_version PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.verify_processing_core_manifest
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_verify_processing_core_manifest.py
    )
    set_tests_properties(scripts.verify_processing_core_manifest PROPERTIES
        LABELS "scripts;contract"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.processing_core_signer_release_gate
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_processing_core_signer_release_gate.py
    )
    set_tests_properties(scripts.processing_core_signer_release_gate PROPERTIES
        LABELS "scripts;contract;release"
        TIMEOUT 30
    )

    add_test(
        NAME scripts.desktop_release_version
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test_desktop_release_version.py
    )
    set_tests_properties(scripts.desktop_release_version PROPERTIES
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
        NAME scripts.compare_metrics
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_compare_metrics.py
    )
    set_tests_properties(scripts.compare_metrics PROPERTIES
        LABELS "scripts;contract;processing"
        TIMEOUT 30
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
target_link_libraries(hw_discovery_reentry_test PRIVATE Threads::Threads)
add_test(NAME hardware.discovery_reentry COMMAND $<TARGET_FILE:hw_discovery_reentry_test>)
set_tests_properties(hardware.discovery_reentry PROPERTIES LABELS "hardware;camera" SKIP_RETURN_CODE 77 TIMEOUT 120)
