cmake_minimum_required(VERSION 3.25)

project(miare VERSION 0.1.0 LANGUAGES CXX)

option(MIARE_BUILD_TESTS "Build the Miare test suite" ${PROJECT_IS_TOP_LEVEL})

add_library(miare INTERFACE)
add_library(miare::miare ALIAS miare)
target_compile_features(miare INTERFACE cxx_std_20)
target_include_directories(
    miare
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

find_path(MIARE_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(MIARE_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
find_path(MIARE_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
find_library(MIARE_ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)

add_library(miare::sodium UNKNOWN IMPORTED GLOBAL)
set_target_properties(
    miare::sodium
    PROPERTIES
        IMPORTED_LOCATION "${MIARE_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MIARE_SODIUM_INCLUDE_DIR}")
add_library(miare::zstd UNKNOWN IMPORTED GLOBAL)
set_target_properties(
    miare::zstd
    PROPERTIES
        IMPORTED_LOCATION "${MIARE_ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MIARE_ZSTD_INCLUDE_DIR}")
target_link_libraries(
    miare
    INTERFACE
        $<BUILD_INTERFACE:miare::sodium>
        $<BUILD_INTERFACE:miare::zstd>
        $<INSTALL_INTERFACE:miare::sodium>
        $<INSTALL_INTERFACE:miare::zstd>)

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
