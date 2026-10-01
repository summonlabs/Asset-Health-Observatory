# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0

# The suite runs each executable plainly: no timeout, no watchdog, no
# process-kill-as-pass. A case that hangs is a defect to be diagnosed.

add_library(asset_health_test_support STATIC test_harness.cpp test_support.cpp)
target_include_directories(asset_health_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    # The durability and restart suites inspect the decoded durable state and
    # build generation files through the internal codec, which is the only way to
    # assert what recovery actually loaded without reimplementing the format.
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asset_health_test_support
    PUBLIC AssetHealthObservatoryStatic
    PRIVATE AssetHealthObservatory::warnings AssetHealthObservatory::sanitizer)

set(ASSET_HEALTH_TEST_TARGETS
    ah_test_values
    ah_test_evidence
    ah_test_freshness
    ah_test_evaluation
    ah_test_durability
    ah_test_property
    ah_test_adversarial
    ah_test_concurrency
    ah_test_restart
    ah_test_cli)

foreach(test_target IN LISTS ASSET_HEALTH_TEST_TARGETS)
    add_executable(${test_target} ${test_target}.cpp)
    target_link_libraries(${test_target} PRIVATE asset_health_test_support AssetHealthObservatory::warnings
        AssetHealthObservatory::sanitizer)
    target_include_directories(${test_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    asset_health_place_library(${test_target})
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
        LABELS "asset-health"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

# The command line suite drives the real executable, so it needs its path.
target_compile_definitions(ah_test_cli PRIVATE
    AH_CLI_PATH="$<TARGET_FILE:asset-health>")

# The process suites run the helper as a separate operating-system process. The
# restart suite is created in the loop above; the multiprocess suite is created
# below, so its definition is attached there.
target_compile_definitions(ah_test_restart PRIVATE
    AH_HELPER_PATH="$<TARGET_FILE:ah_process_helper>")

# The multiprocess suite spawns independent operating-system processes against
# one store, so it is its own binary: a crash there cannot be confused with a
# fault in the in-process suites.
add_executable(ah_test_multiprocess ah_test_multiprocess.cpp)
target_link_libraries(ah_test_multiprocess PRIVATE asset_health_test_support AssetHealthObservatory::warnings
    AssetHealthObservatory::sanitizer)
target_include_directories(ah_test_multiprocess PRIVATE ${CMAKE_SOURCE_DIR}/src)
asset_health_place_library(ah_test_multiprocess)
target_compile_definitions(ah_test_multiprocess PRIVATE
    AH_HELPER_PATH="$<TARGET_FILE:ah_process_helper>")
add_test(NAME ah_test_multiprocess COMMAND ah_test_multiprocess)
set_tests_properties(ah_test_multiprocess PROPERTIES
    LABELS "asset-health"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# The helper runs as a separate process under the multiprocess and restart
# suites. It is a real program, not a fixture, and it is copied next to the test
# binaries because a multi-configuration generator keeps them in the same
# directory while ctest runs from that directory.
add_executable(ah_process_helper helper/ah_process_helper.cpp)
target_link_libraries(ah_process_helper PRIVATE AssetHealthObservatoryStatic AssetHealthObservatory::warnings
    AssetHealthObservatory::sanitizer)
target_include_directories(ah_process_helper PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_custom_command(TARGET ah_test_multiprocess POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:ah_process_helper>
        $<TARGET_FILE_DIR:ah_test_multiprocess>
    COMMENT "Placing ah_process_helper next to the process suites")
add_custom_command(TARGET ah_test_restart POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:ah_process_helper>
        $<TARGET_FILE_DIR:ah_test_restart>
    COMMENT "Placing ah_process_helper next to the process suites")
