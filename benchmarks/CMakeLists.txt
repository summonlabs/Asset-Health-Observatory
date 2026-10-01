# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0
#
# Benchmarks measure completed work only: each one states how many operations
# finished and how long they took. They are not ctest cases, because a benchmark
# result on a loaded machine is not a pass or a fail.

add_executable(ah_benchmarks benchmarks.cpp)
target_link_libraries(ah_benchmarks PRIVATE AssetHealthObservatoryStatic AssetHealthObservatory::warnings
    AssetHealthObservatory::sanitizer)
target_include_directories(ah_benchmarks PRIVATE ${CMAKE_SOURCE_DIR}/src)
asset_health_place_library(ah_benchmarks)

add_custom_target(run_benchmarks_small
    COMMAND ah_benchmarks --small
    DEPENDS ah_benchmarks
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running the small benchmark set")

add_custom_target(run_benchmarks_full
    COMMAND ah_benchmarks --full
    DEPENDS ah_benchmarks
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running the full benchmark set")
