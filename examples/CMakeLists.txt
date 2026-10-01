# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0
#
# Each example is a real program that is also a ctest case: it builds a store in
# a temporary directory, drives the public API, and asserts what it observed. The
# examples exist so that a reader has a working program to start from, and so
# that the documented API is exercised from outside the library.

set(ASSET_HEALTH_EXAMPLES
    example_assess
    example_offline)

foreach(example IN LISTS ASSET_HEALTH_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE AssetHealthObservatory AssetHealthObservatory::warnings
        AssetHealthObservatory::sanitizer)
    asset_health_place_library(${example})
    add_test(NAME ${example} COMMAND ${example})
    set_tests_properties(${example} PROPERTIES
        LABELS "asset-health;example"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
