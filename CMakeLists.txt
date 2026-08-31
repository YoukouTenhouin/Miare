cmake_minimum_required(VERSION 3.25)

project(miare VERSION 0.1.0 LANGUAGES CXX)

option(MIARE_BUILD_TESTS "Build the Miare test suite" ${PROJECT_IS_TOP_LEVEL})
option(MIARE_BUILD_EXAMPLES "Build the Miare examples" ${PROJECT_IS_TOP_LEVEL})
option(MIARE_BUILD_DOCS "Build the Miare documentation" OFF)
option(
    MIARE_BUILD_QUALIFICATION_TESTS
    "Register long-running backend qualification tests"
    OFF)
option(MIARE_BUILD_FUZZERS "Build bounded libFuzzer targets" OFF)
option(MIARE_BUILD_BENCHMARKS "Build v1 qualification benchmarks" OFF)
option(MIARE_ENABLE_SODIUM "Enable the system libsodium provider" ON)
option(MIARE_ENABLE_ZSTD "Enable the system Zstandard provider" ON)

add_library(miare INTERFACE)
add_library(miare::miare ALIAS miare)
target_compile_features(miare INTERFACE cxx_std_20)
target_include_directories(
    miare
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

set(MIARE_HAS_SODIUM 0)
if(MIARE_ENABLE_SODIUM)
    find_path(MIARE_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
    find_library(MIARE_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
    add_library(miare::sodium UNKNOWN IMPORTED GLOBAL)
    set_target_properties(
        miare::sodium
        PROPERTIES
            IMPORTED_LOCATION "${MIARE_SODIUM_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MIARE_SODIUM_INCLUDE_DIR}")
    target_link_libraries(
        miare
        INTERFACE
            $<BUILD_INTERFACE:miare::sodium>
            $<INSTALL_INTERFACE:miare::sodium>)
    set(MIARE_HAS_SODIUM 1)
endif()

set(MIARE_HAS_ZSTD 0)
if(MIARE_ENABLE_ZSTD)
    find_path(MIARE_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
    find_library(MIARE_ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)
    add_library(miare::zstd UNKNOWN IMPORTED GLOBAL)
    set_target_properties(
        miare::zstd
        PROPERTIES
            IMPORTED_LOCATION "${MIARE_ZSTD_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MIARE_ZSTD_INCLUDE_DIR}")
    target_link_libraries(
        miare
        INTERFACE
            $<BUILD_INTERFACE:miare::zstd>
            $<INSTALL_INTERFACE:miare::zstd>)
    set(MIARE_HAS_ZSTD 1)
endif()

target_compile_definitions(
    miare
    INTERFACE
        MIARE_HAS_SODIUM=${MIARE_HAS_SODIUM}
        MIARE_HAS_ZSTD=${MIARE_HAS_ZSTD})

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS miare EXPORT miare-targets)
install(
    EXPORT miare-targets
    FILE miare-targets.cmake
    NAMESPACE miare::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/miare)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/miare-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/miare-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/miare)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/miare-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/miare-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/miare-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/miare)

if(MIARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MIARE_BUILD_EXAMPLES)
    if(MIARE_HAS_SODIUM AND MIARE_HAS_ZSTD)
        add_subdirectory(examples)
    else()
        message(STATUS "Miare examples require both system providers; skipping")
    endif()
endif()

if(MIARE_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    configure_file(
        docs/Doxyfile.in
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        @ONLY)
    add_custom_target(
        miare_docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating Miare API documentation"
        VERBATIM)
endif()

if(MIARE_BUILD_FUZZERS)
    if((NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
        NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang") OR
       NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        message(FATAL_ERROR "MIARE_BUILD_FUZZERS requires Clang libFuzzer")
    endif()
    add_subdirectory(fuzz)
endif()

if(MIARE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
