# Two independent shared Libretro adapters over the RavenCore engines: RavenGBC
# for the Game Boy Color and the Game Boy (one engine, the hardware chosen per
# game), RavenGBA for the Game Boy Advance.

# Only the Libretro interface is exported. The statically linked engines keep
# their C++ symbols internal, so they cannot clash with another core loaded
# by the same frontend. Windows already exports only `dllexport` symbols.
set(RAVENEMU_LIBRETRO_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/link.T")

foreach(system IN ITEMS gbc gba)
    set(target "ravenemu_${system}_libretro")
    add_library(${target} SHARED src/libretro.cpp)
    string(TOUPPER "${system}" system_upper)
    # The project version is the single source for every version string.
    target_compile_definitions(${target} PRIVATE
        RAVENEMU_LIBRETRO_${system_upper}=1 RAVENEMU_VERSION="${PROJECT_VERSION}")
    configure_file(info/${target}.info.in "${CMAKE_CURRENT_BINARY_DIR}/info/${target}.info" @ONLY)
    target_include_directories(${target} PRIVATE include)
    if(system STREQUAL "gbc")
        target_link_libraries(${target} PRIVATE gbc_raven_core)
    else()
        target_link_libraries(${target} PRIVATE gba_raven_core)
    endif()
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "ravenemu_${system}_libretro")
    if(UNIX AND NOT APPLE)
        target_link_options(${target} PRIVATE "LINKER:--version-script=${RAVENEMU_LIBRETRO_EXPORTS}")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${RAVENEMU_LIBRETRO_EXPORTS}")
    endif()
    target_compile_options(${target} PRIVATE ${RAVENEMU_WARNING_OPTIONS})
endforeach()

# The smoke test plays one scenario per console: a Game Boy game on RavenGBC as
# it comes, RavenGBC set to Game Boy Color hardware, a GBA game on RavenGBA.
if(BUILD_TESTING)
    foreach(scenario IN ITEMS gb gbc gba)
        if(scenario STREQUAL "gba")
            set(core ravenemu_gba_libretro)
        else()
            set(core ravenemu_gbc_libretro)
        endif()
        string(TOUPPER "${scenario}" scenario_upper)
        add_executable(libretro_smoke_${scenario} tests/smoke.cpp)
        target_include_directories(libretro_smoke_${scenario} PRIVATE
            include ${PROJECT_SOURCE_DIR}/cores/common/tests/support)
        target_compile_definitions(libretro_smoke_${scenario} PRIVATE
            SMOKE_${scenario_upper}=1 SMOKE_SCENARIO="${scenario}" RAVENEMU_VERSION="${PROJECT_VERSION}")
        # The smoke test only links the shared core, whose private C++20
        # requirement does not propagate; MSVC would otherwise build it as C++14.
        target_compile_features(libretro_smoke_${scenario} PRIVATE cxx_std_20)
        target_link_libraries(libretro_smoke_${scenario} PRIVATE ${core})
        add_test(NAME libretro_smoke_${scenario} COMMAND libretro_smoke_${scenario})
    endforeach()
endif()
