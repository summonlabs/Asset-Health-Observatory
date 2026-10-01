cmake_minimum_required(VERSION 3.20)

project(AssetHealthObservatory
    VERSION 1.0.0
    DESCRIPTION "DCCP evidence-bound facility asset health observation runtime"
    HOMEPAGE_URL "https://github.com/summonlabs/Asset-Health-Observatory"
    LANGUAGES CXX)

# The standard is set once, globally, so every target in every subdirectory and
# every consumer of this build tree compiles at the same language level. Setting
# it per target is easy to forget for a new target, and forgetting it silently
# downgrades the language level instead of failing.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ASSET_HEALTH_IS_TOP_LEVEL ON)
else()
    set(ASSET_HEALTH_IS_TOP_LEVEL OFF)
endif()

option(ASSET_HEALTH_BUILD_SHARED "Build the observatory library as a shared library" ON)
option(ASSET_HEALTH_BUILD_TESTS "Build the observatory test suite" ${ASSET_HEALTH_IS_TOP_LEVEL})
option(ASSET_HEALTH_BUILD_CLI "Build the asset-health command line tool" ON)
option(ASSET_HEALTH_BUILD_EXAMPLES "Build the observatory examples" ${ASSET_HEALTH_IS_TOP_LEVEL})
option(ASSET_HEALTH_BUILD_BENCHMARKS "Build the observatory benchmarks" ${ASSET_HEALTH_IS_TOP_LEVEL})
option(ASSET_HEALTH_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
set(ASSET_HEALTH_SANITIZE "" CACHE STRING
    "Sanitizer instrumentation to apply to first-party targets (empty, 'address', or 'thread')")

if(NOT ASSET_HEALTH_BUILD_SHARED)
    set(ASSET_HEALTH_LIBRARY_TYPE STATIC)
else()
    set(ASSET_HEALTH_LIBRARY_TYPE SHARED)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Shared warning and sanitizer interfaces
# ---------------------------------------------------------------------------

add_library(asset_health_warnings INTERFACE)
add_library(AssetHealthObservatory::warnings ALIAS asset_health_warnings)

if(MSVC)
    target_compile_options(asset_health_warnings INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /EHsc)
    # C4251 warns that standard-library members of an exported class are not
    # themselves exported. The public surface here is deliberately made of value
    # types with standard-library members, so the warning fires on almost every
    # public type without describing a defect: a consumer is built with a
    # compatible toolchain and standard library, and no standard-library object is
    # constructed on one side of the boundary and destroyed on the other. The
    # suppression is stated once, here, rather than per class.
    target_compile_options(asset_health_warnings INTERFACE /wd4251)
    if(ASSET_HEALTH_WARNINGS_AS_ERRORS)
        target_compile_options(asset_health_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(asset_health_warnings INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(asset_health_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2)
    if(ASSET_HEALTH_WARNINGS_AS_ERRORS)
        target_compile_options(asset_health_warnings INTERFACE -Werror)
    endif()
endif()

set(ASSET_HEALTH_SANITIZER_FLAGS "")
if(ASSET_HEALTH_SANITIZE STREQUAL "address")
    if(MSVC)
        list(APPEND ASSET_HEALTH_SANITIZER_FLAGS /fsanitize=address)
        set(ASSET_HEALTH_MSVC_ASAN_RUNTIME ON)
    else()
        list(APPEND ASSET_HEALTH_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    endif()
elseif(ASSET_HEALTH_SANITIZE STREQUAL "thread")
    if(MSVC)
        message(FATAL_ERROR "ASSET_HEALTH_SANITIZE=thread is not supported by MSVC")
    endif()
    list(APPEND ASSET_HEALTH_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT ASSET_HEALTH_SANITIZE STREQUAL "")
    message(FATAL_ERROR "Unsupported ASSET_HEALTH_SANITIZE value: ${ASSET_HEALTH_SANITIZE}")
endif()

add_library(asset_health_sanitizer INTERFACE)
add_library(AssetHealthObservatory::sanitizer ALIAS asset_health_sanitizer)
if(ASSET_HEALTH_SANITIZER_FLAGS)
    target_compile_options(asset_health_sanitizer INTERFACE ${ASSET_HEALTH_SANITIZER_FLAGS})
    if(MSVC)
        # MSVC records the sanitizer runtime in the instrumented objects and
        # treats /fsanitize=address as a compile option only, so passing it to the
        # linker makes the linker warn about an option it does not recognise.
        # Incremental linking is incompatible with that metadata, so it is turned
        # off here rather than left for the linker to refuse with a warning.
        target_link_options(asset_health_sanitizer INTERFACE /INCREMENTAL:NO)
    else()
        target_link_options(asset_health_sanitizer INTERFACE ${ASSET_HEALTH_SANITIZER_FLAGS})
    endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(ASSET_HEALTH_PUBLIC_HEADERS
    include/asset_health/assessment.hpp
    include/asset_health/error.hpp
    include/asset_health/evidence.hpp
    include/asset_health/evaluation.hpp
    include/asset_health/export.hpp
    include/asset_health/freshness.hpp
    include/asset_health/history.hpp
    include/asset_health/identity.hpp
    include/asset_health/limits.hpp
    include/asset_health/observation.hpp
    include/asset_health/observatory.hpp
    include/asset_health/persistence.hpp
    include/asset_health/policy.hpp
    include/asset_health/provenance.hpp
    include/asset_health/strong_types.hpp
    include/asset_health/time.hpp)

set(ASSET_HEALTH_INTERNAL_HEADERS
    src/crc32.hpp
    src/fs_atomic.hpp
    src/store_format.hpp
    src/tlv.hpp)

set(ASSET_HEALTH_SOURCES
    src/assessment.cpp
    src/crc32.cpp
    src/error.cpp
    src/evidence.cpp
    src/evaluation.cpp
    src/export.cpp
    src/freshness.cpp
    src/fs_atomic.cpp
    src/history.cpp
    src/identity.cpp
    src/observation.cpp
    src/observatory.cpp
    src/persistence.cpp
    src/policy.cpp
    src/provenance.cpp
    src/store_format.cpp
    src/strong_types.cpp
    src/time.cpp
    src/tlv.cpp)

add_library(asset_health_objects OBJECT ${ASSET_HEALTH_SOURCES} ${ASSET_HEALTH_INTERNAL_HEADERS})
set_target_properties(asset_health_objects PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(asset_health_objects
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(asset_health_objects PRIVATE ASSET_HEALTH_BUILDING_LIBRARY)
if(ASSET_HEALTH_BUILD_SHARED)
    # Both macros are needed for the shared build: ASSET_HEALTH_SHARED selects the
    # __declspec form and ASSET_HEALTH_BUILDING_LIBRARY selects the export half of
    # it. Without the second, the library exports nothing and a consumer has no
    # import library to link against.
    target_compile_definitions(asset_health_objects PRIVATE ASSET_HEALTH_SHARED)
endif()
target_link_libraries(asset_health_objects PRIVATE AssetHealthObservatory::warnings
    AssetHealthObservatory::sanitizer)

add_library(AssetHealthObservatory ${ASSET_HEALTH_LIBRARY_TYPE} $<TARGET_OBJECTS:asset_health_objects>)
add_library(AssetHealthObservatory::AssetHealthObservatory ALIAS AssetHealthObservatory)
target_include_directories(AssetHealthObservatory
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(ASSET_HEALTH_BUILD_SHARED)
    target_compile_definitions(AssetHealthObservatory INTERFACE ASSET_HEALTH_SHARED)
endif()
if(ASSET_HEALTH_MSVC_ASAN_RUNTIME)
    target_compile_options(AssetHealthObservatory PRIVATE /fsanitize=address)
endif()

if(WIN32)
    # BCryptGenRandom supplies the process entropy used for evidence and store
    # identifiers. It is part of the operating system, not a third-party
    # dependency.
    target_link_libraries(AssetHealthObservatory PRIVATE bcrypt)
endif()

# Static build of the same objects, used by the test suite and the benchmarks so
# that they can reach internal accessors (the decoded durable state a recovery
# produced, the payload a generation holds) without exporting implementation
# detail from the shared library that consumers see. Never installed, never
# exported.
add_library(AssetHealthObservatoryStatic STATIC $<TARGET_OBJECTS:asset_health_objects>)
target_include_directories(AssetHealthObservatoryStatic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(AssetHealthObservatoryStatic PRIVATE bcrypt)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------

install(TARGETS AssetHealthObservatory
    EXPORT AssetHealthObservatoryTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/asset_health
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

set(ASSET_HEALTH_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/AssetHealthObservatory)

install(EXPORT AssetHealthObservatoryTargets
    FILE AssetHealthObservatoryTargets.cmake
    NAMESPACE AssetHealthObservatory::
    DESTINATION ${ASSET_HEALTH_CMAKE_DIR})

configure_package_config_file(
    cmake/AssetHealthObservatoryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AssetHealthObservatoryConfig.cmake
    INSTALL_DESTINATION ${ASSET_HEALTH_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AssetHealthObservatoryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AssetHealthObservatoryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AssetHealthObservatoryConfigVersion.cmake
    DESTINATION ${ASSET_HEALTH_CMAKE_DIR})

install(FILES README.md LICENSE NOTICE CONTRIBUTING.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# CLI, examples, tests, benchmarks
# ---------------------------------------------------------------------------

# A multi-configuration generator puts the shared library in a per-configuration
# directory, and that directory is not on the loader's search path for an
# executable sitting in its own directory. First-party executables that link the
# shared library therefore get a copy placed beside them, which is what keeps the
# CLI, the examples, the tests and the benchmarks runnable straight from the
# build tree.
function(asset_health_place_library target)
    if(ASSET_HEALTH_BUILD_SHARED)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:AssetHealthObservatory>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Placing the observatory library beside ${target}")
    endif()
endfunction()

# CTest has to be enabled before any directory that registers a test, and the
# examples register tests as well as the test suite does. A directory added
# before enable_testing() has its add_test() calls dropped silently, so the
# ordering here is what keeps the examples inside ctest.
if(ASSET_HEALTH_BUILD_TESTS OR ASSET_HEALTH_BUILD_EXAMPLES)
    enable_testing()
endif()

if(ASSET_HEALTH_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(ASSET_HEALTH_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ASSET_HEALTH_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ASSET_HEALTH_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
