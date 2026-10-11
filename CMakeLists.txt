cmake_minimum_required(VERSION 3.22.1)
project(RavenEmuCores VERSION 0.1.0 LANGUAGES CXX)

option(RAVENEMU_BUILD_LIBRETRO "Build the three Libretro frontend adapters" ON)
option(RAVENEMU_BUILD_EXAMPLES "Build standalone C++ API usage example" ON)
include(CTest)

# The imported cores independently use RAVENEMU_BUILD_TESTS.
set(RAVENEMU_BUILD_TESTS ${BUILD_TESTING} CACHE BOOL "Build imported core tests" FORCE)
add_subdirectory(cores)

if(RAVENEMU_BUILD_EXAMPLES)
    add_executable(ravenemu_minimal_player examples/minimal_player.cpp)
    target_include_directories(ravenemu_minimal_player PRIVATE sdk/include)
    target_link_libraries(ravenemu_minimal_player PRIVATE ravenemu_cores)
endif()

if(RAVENEMU_BUILD_LIBRETRO)
    add_subdirectory(libretro)
endif()
