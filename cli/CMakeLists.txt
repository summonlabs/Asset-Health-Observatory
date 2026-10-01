# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0

add_executable(asset-health main.cpp)
target_link_libraries(asset-health PRIVATE AssetHealthObservatory AssetHealthObservatory::warnings
    AssetHealthObservatory::sanitizer)
asset_health_place_library(asset-health)

# The command line tool is part of the package: an operator who installs this
# project gets the library and the tool that uses it.
install(TARGETS asset-health RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ASSET_HEALTH_BUILD_TESTS)
    add_test(NAME ah_cli_help COMMAND asset-health help)
    set_tests_properties(ah_cli_help PROPERTIES LABELS "asset-health")
endif()
