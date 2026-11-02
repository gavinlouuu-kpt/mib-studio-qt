cmake_minimum_required(VERSION 3.22)
project(mib_analysis_module LANGUAGES CXX)
enable_testing()
get_filename_component(MIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(mib_analysis STATIC "${MIB_ROOT}/src/backend/analysis/Population.cpp" "${MIB_ROOT}/src/backend/analysis/PopulationGraph.cpp")
target_compile_features(mib_analysis PUBLIC cxx_std_17)
target_include_directories(mib_analysis PUBLIC "${MIB_ROOT}/include")
add_executable(analysis_population_test "${MIB_ROOT}/tests/analysis/population_test.cpp")
target_include_directories(analysis_population_test PRIVATE "${MIB_ROOT}/tests")
target_link_libraries(analysis_population_test PRIVATE mib_analysis)
add_test(NAME analysis.population COMMAND analysis_population_test)
set_tests_properties(analysis.population PROPERTIES TIMEOUT 20 LABELS "backend;analysis;invariant")

add_executable(analysis_graph_test "${MIB_ROOT}/tests/analysis/graph_test.cpp")
target_include_directories(analysis_graph_test PRIVATE "${MIB_ROOT}/tests")
target_link_libraries(analysis_graph_test PRIVATE mib_analysis)
add_test(NAME analysis.graph COMMAND analysis_graph_test)
set_tests_properties(analysis.graph PROPERTIES TIMEOUT 20 LABELS "backend;analysis;invariant")

target_sources(mib_analysis PRIVATE "${MIB_ROOT}/src/backend/analysis/PopulationStatistics.cpp"
                                    "${MIB_ROOT}/src/backend/analysis/AnalysisInference.cpp")
# F-4 (#808 F09, I10): native inference and 4PL fits vs scipy reference values.
add_executable(analysis_inference_test "${MIB_ROOT}/tests/analysis/inference_test.cpp")
target_include_directories(analysis_inference_test PRIVATE "${MIB_ROOT}/tests")
target_link_libraries(analysis_inference_test PRIVATE mib_analysis)
add_test(NAME analysis.inference COMMAND analysis_inference_test)
set_tests_properties(analysis.inference PROPERTIES TIMEOUT 20 LABELS "backend;analysis;invariant")

# F-1: the columnar full-recording evaluator and its gate compiler (JSON only
# for compiling graphs; no HDF5). Columnar == keyed parity at every node.
find_package(nlohmann_json CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
    target_sources(mib_analysis PRIVATE
        "${MIB_ROOT}/src/backend/analysis/PopulationGates.cpp"
        "${MIB_ROOT}/src/backend/analysis/ColumnarEvaluation.cpp")
    target_link_libraries(mib_analysis PUBLIC nlohmann_json::nlohmann_json)
    add_executable(analysis_columnar_test "${MIB_ROOT}/tests/analysis/columnar_test.cpp")
    target_include_directories(analysis_columnar_test PRIVATE "${MIB_ROOT}/tests")
    target_link_libraries(analysis_columnar_test PRIVATE mib_analysis)
    add_test(NAME analysis.columnar COMMAND analysis_columnar_test)
    set_tests_properties(analysis.columnar PROPERTIES TIMEOUT 60 LABELS "backend;analysis;invariant")
endif()
add_executable(analysis_statistics_test "${MIB_ROOT}/tests/analysis/statistics_test.cpp")
target_include_directories(analysis_statistics_test PRIVATE "${MIB_ROOT}/tests")
target_link_libraries(analysis_statistics_test PRIVATE mib_analysis)
add_test(NAME analysis.statistics COMMAND analysis_statistics_test)
set_tests_properties(analysis.statistics PROPERTIES TIMEOUT 20 LABELS "backend;analysis;invariant")

option(MIB_ANALYSIS_HOST_TESTS "Build native source/persistence/report qualification without Qt/HDF5" OFF)
if(MIB_ANALYSIS_HOST_TESTS)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    find_package(SQLite3 REQUIRED)
    find_package(Threads REQUIRED)
    add_library(mib_analysis_host STATIC
        "${MIB_ROOT}/src/backend/analysis/SourceAnalysis.cpp"
        "${MIB_ROOT}/src/backend/analysis/SourceCore.cpp"
        "${MIB_ROOT}/src/backend/analysis/FcsReader.cpp"
        "${MIB_ROOT}/src/backend/analysis/ParameterTransforms.cpp"
        "${MIB_ROOT}/src/backend/analysis/PopulationPlot.cpp"
        "${MIB_ROOT}/src/backend/analysis/PopulationScatter.cpp"
        "${MIB_ROOT}/src/backend/analysis/AnalysisHost.cpp"
        "${MIB_ROOT}/src/backend/analysis/AnalysisComparison.cpp"
        "${MIB_ROOT}/src/backend/analysis/AnalysisReports.cpp"
        "${MIB_ROOT}/src/backend/analysis/AnalysisOverlay.cpp"
        "${MIB_ROOT}/src/backend/analysis/AnalysisLibrary.cpp"
        "${MIB_ROOT}/src/backend/processing/ProcessingCoreSha256.cpp"
        "${MIB_ROOT}/src/backend/evidence/CommittedEventStore.cpp"
        "${MIB_ROOT}/src/backend/database/SqliteService.cpp"
        "${MIB_ROOT}/src/backend/workspace/WorkspaceStore.cpp")
    target_include_directories(mib_analysis_host PUBLIC "${MIB_ROOT}/include")
    target_link_libraries(mib_analysis_host PUBLIC mib_analysis nlohmann_json::nlohmann_json spdlog::spdlog SQLite::SQLite3 Threads::Threads)
    add_executable(analysis_host_test "${MIB_ROOT}/tests/analysis/analysis_host_test.cpp")
    target_include_directories(analysis_host_test PRIVATE "${MIB_ROOT}/tests")
    target_link_libraries(analysis_host_test PRIVATE mib_analysis_host)
    add_test(NAME analysis.host COMMAND analysis_host_test)
    set_tests_properties(analysis.host PROPERTIES TIMEOUT 40 LABELS "backend;analysis;persistence;stress")
    add_executable(analysis_comparison_spec_test "${MIB_ROOT}/tests/analysis/comparison_spec_test.cpp")
    target_include_directories(analysis_comparison_spec_test PRIVATE "${MIB_ROOT}/tests")
    target_link_libraries(analysis_comparison_spec_test PRIVATE mib_analysis_host)
    add_test(NAME analysis.comparison_spec COMMAND analysis_comparison_spec_test)
    set_tests_properties(analysis.comparison_spec PROPERTIES TIMEOUT 40 LABELS "backend;analysis")
endif()
