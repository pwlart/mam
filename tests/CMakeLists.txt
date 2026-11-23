add_executable(mam_tests
    test_main.cpp
    test_parameters.cpp
    test_commands.cpp
    test_dsp.cpp
    test_transport.cpp
    test_rational_frame_clock.cpp
    test_audio_file.cpp
    test_native_audio.cpp
    test_cli_options.cpp
    test_rack_timeline.cpp
    test_event_schedule.cpp
    test_session_feeding.cpp
    test_graph_playback.cpp
    test_bounded_callback_queue.cpp
    test_job_pool.cpp
    test_offline_worker_failures.cpp
    test_random.cpp
    test_random_rendering.cpp
    test_seed_config.cpp
)
target_include_directories(mam_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mam_tests PRIVATE doctest::doctest mam_render mam_app)
mam_configure_target(mam_tests)
include(${PROJECT_SOURCE_DIR}/vendor/doctest/cmake/doctest.cmake)
doctest_discover_tests(mam_tests TEST_PREFIX "unit." PROPERTIES TIMEOUT 30 LABELS unit)

# Keep these consumers independent of mam_render/mam_app: their link closure
# checks the owning library instead of accidentally borrowing aggregate edges.
add_executable(mam_node_factory_consumer node_factory_consumer.cpp)
target_include_directories(mam_node_factory_consumer PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mam_node_factory_consumer PRIVATE mam_nodes)
mam_configure_target(mam_node_factory_consumer)
add_test(NAME linkage.node_factory COMMAND mam_node_factory_consumer)
set_tests_properties(linkage.node_factory PROPERTIES TIMEOUT 10 LABELS integration)

add_executable(mam_session_runtime_consumer session_runtime_consumer.cpp)
target_include_directories(mam_session_runtime_consumer PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mam_session_runtime_consumer PRIVATE mam_session_runtime)
mam_configure_target(mam_session_runtime_consumer)
add_test(NAME linkage.session_runtime COMMAND mam_session_runtime_consumer)
set_tests_properties(linkage.session_runtime PROPERTIES TIMEOUT 10 LABELS integration)

# The C++ allocation probes override new/delete only in this dedicated harness.
# Device-free renderBlock is the same path used by the CoreAudio callbacks.
add_executable(mam_realtime_tests
    test_main.cpp
    AllocationWatch.cpp
    test_prepared_graph.cpp
    test_node_preparation.cpp
    test_graph_callback.cpp
    test_prepared_session.cpp
    test_realtime_allocations.cpp
    test_random_realtime.cpp
)
target_include_directories(mam_realtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mam_realtime_tests PRIVATE doctest::doctest mam_render mam_app)
mam_configure_target(mam_realtime_tests)
doctest_discover_tests(mam_realtime_tests TEST_PREFIX "realtime." PROPERTIES TIMEOUT 30 LABELS "unit;realtime")

find_program(MAM_BASH bash REQUIRED)
foreach(rate IN ITEMS 44100 48000)
    add_test(NAME render.parity.${rate}
        COMMAND ${MAM_BASH} ${PROJECT_SOURCE_DIR}/tools/topo_parity.sh
        $<TARGET_FILE:mam> ${PROJECT_SOURCE_DIR}/tests/fixtures/scheduler-parity.json
        2 ${rate} 42)
    set_tests_properties(render.parity.${rate} PROPERTIES TIMEOUT 60 LABELS integration)
endforeach()
foreach(case IN ITEMS missing malformed duplicate mismatch exit_failure missing_audio valid)
    add_test(NAME tooling.parity.${case} COMMAND ${CMAKE_COMMAND}
        -DCASE=${case} -DBASH=${MAM_BASH}
        -DPARITY_SCRIPT=${PROJECT_SOURCE_DIR}/tools/topo_parity.sh
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/parity-${case}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/parity_contract.cmake)
    set_tests_properties(tooling.parity.${case} PROPERTIES TIMEOUT 15 LABELS tooling)
endforeach()
add_test(NAME cli.strict_schema_unavailable COMMAND ${CMAKE_COMMAND}
    -DMAM=$<TARGET_FILE:mam>
    -DRACK=${PROJECT_SOURCE_DIR}/tests/fixtures/scheduler-parity.json
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/strict-schema
    -P ${CMAKE_CURRENT_SOURCE_DIR}/strict_schema.cmake)
set_tests_properties(cli.strict_schema_unavailable PROPERTIES TIMEOUT 15 LABELS integration)
add_test(NAME cli.random_seed_audio COMMAND ${CMAKE_COMMAND}
    -DMAM=$<TARGET_FILE:mam> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/random-seed-cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/random_seed_cli.cmake)
set_tests_properties(cli.random_seed_audio PROPERTIES TIMEOUT 60 LABELS integration)
add_test(NAME vendor.doctest_integrity COMMAND ${CMAKE_COMMAND}
    -DROOT=${PROJECT_SOURCE_DIR}/vendor/doctest
    -P ${CMAKE_CURRENT_SOURCE_DIR}/vendor_integrity.cmake)
set_tests_properties(vendor.doctest_integrity PROPERTIES TIMEOUT 10 LABELS tooling)

add_test(NAME cli.build_identity COMMAND ${CMAKE_COMMAND}
    -DMAM=$<TARGET_FILE:mam> -DVERSION=${PROJECT_VERSION}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/build_info_cli.cmake)
set_tests_properties(cli.build_identity PROPERTIES TIMEOUT 15 LABELS tooling)
add_test(NAME tooling.build_identity COMMAND ${CMAKE_COMMAND}
    -DGENERATOR_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/GenerateBuildIdentity.cmake
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/identity-fixture
    -P ${CMAKE_CURRENT_SOURCE_DIR}/build_identity.cmake)
set_tests_properties(tooling.build_identity PROPERTIES TIMEOUT 60 LABELS tooling)

if(MAM_ENABLE_PYTHON_TESTS)
    if(NOT MAM_PYTHON_EXECUTABLE)
        message(FATAL_ERROR "Set MAM_PYTHON_EXECUTABLE to the prepared Python environment")
    endif()
    execute_process(COMMAND ${MAM_PYTHON_EXECUTABLE} -I -c "import pytest, jsonschema, numpy, scipy, soundfile, pedalboard, librosa, soxr"
        RESULT_VARIABLE python_dependencies)
    if(NOT python_dependencies EQUAL 0)
        message(FATAL_ERROR "Python test dependencies missing. See python/README.md; no packages are installed automatically.")
    endif()
    add_test(NAME python.companion COMMAND ${CMAKE_COMMAND} -E env
        MAM_TEST_BINARY=$<TARGET_FILE:mam>
        MAM_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/python-cache
        ${MAM_PYTHON_EXECUTABLE} -I -B -m pytest -q
        -o cache_dir=${CMAKE_CURRENT_BINARY_DIR}/pytest-cache
        ${PROJECT_SOURCE_DIR}/python/tests)
    set_tests_properties(python.companion PROPERTIES TIMEOUT 240 LABELS "python;integration")
endif()

add_test(NAME vendor.nlohmann_integrity COMMAND ${CMAKE_COMMAND}
    -DROOT=${PROJECT_SOURCE_DIR}/vendor/nlohmann
    -P ${CMAKE_CURRENT_SOURCE_DIR}/vendor_integrity.cmake)
set_tests_properties(vendor.nlohmann_integrity PROPERTIES TIMEOUT 10 LABELS tooling)
