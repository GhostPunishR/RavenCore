cmake_minimum_required(VERSION 3.22.1)
project(RavenCore VERSION 0.5.2 LANGUAGES CXX)

# An unoptimized emulator is unusable: default to Release when this repository
# is the main project and no build type was requested.
get_property(RAVENEMU_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND NOT RAVENEMU_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RAVENEMU_BUILD_LIBRETRO "Build the three Libretro frontend adapters" ON)
option(RAVENEMU_INSTALL "Install the engines, the C API and the CMake package" ${PROJECT_IS_TOP_LEVEL})
include(CTest)

add_subdirectory(cores)
add_subdirectory(sdk)

if(RAVENEMU_BUILD_LIBRETRO)
    add_subdirectory(libretro)
endif()

if(RAVENEMU_INSTALL)
    # find_package(RavenCore) then link RavenCore::ravencore (C++)
    # or RavenCore::ravenemu_c_api (C ABI). Libretro cores are not installed:
    # they belong in the frontend's own directories.
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(RAVENEMU_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/RavenCore")

    install(TARGETS
            ravenemu_options ravenemu_common gbc_hardware gb_raven_core gbc_raven_core
            gba_raven_core ravencore ravenemu_c_api
        EXPORT RavenCoreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(DIRECTORY
            cores/common/include/ cores/gb/include/ cores/gbc/include/ cores/gba/include/
            sdk/include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT RavenCoreTargets
        NAMESPACE RavenCore::
        DESTINATION "${RAVENEMU_PACKAGE_DIR}")

    configure_package_config_file(cmake/RavenCoreConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/RavenCoreConfig.cmake"
        INSTALL_DESTINATION "${RAVENEMU_PACKAGE_DIR}")
    # Pre-1.0: the C ABI and save states may change between minor versions.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/RavenCoreConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/RavenCoreConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/RavenCoreConfigVersion.cmake"
        DESTINATION "${RAVENEMU_PACKAGE_DIR}")
endif()
