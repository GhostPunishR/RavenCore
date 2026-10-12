cmake_minimum_required(VERSION 3.22.1)
project(RavenEmuCores VERSION 0.1.0 LANGUAGES CXX)

option(RAVENEMU_BUILD_LIBRETRO "Build the three Libretro frontend adapters" ON)
include(CTest)

# The imported cores independently use RAVENEMU_BUILD_TESTS.
set(RAVENEMU_BUILD_TESTS ${BUILD_TESTING} CACHE BOOL "Build imported core tests" FORCE)
add_subdirectory(cores)
add_subdirectory(sdk)

if(RAVENEMU_BUILD_LIBRETRO)
    add_subdirectory(libretro)
endif()
