# Three independent shared Libretro adapters over the original RavenEmu engines.

# Only the Libretro interface is exported. The statically linked engines keep
# their C++ symbols internal, so they cannot clash with another core loaded
# by the same frontend. Windows already exports only `dllexport` symbols.
set(RAVENEMU_LIBRETRO_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/link.T")

foreach(system IN ITEMS gb gbc gba)
    set(target "ravenemu_${system}_libretro")
    add_library(${target} SHARED src/libretro.cpp)
    string(TOUPPER "${system}" system_upper)
    target_compile_definitions(${target} PRIVATE RAVENEMU_LIBRETRO_${system_upper}=1)
    target_include_directories(${target} PRIVATE include)
    if(system STREQUAL "gb")
        target_link_libraries(${target} PRIVATE gb_raven_core)
    elseif(system STREQUAL "gbc")
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
    if(RAVENEMU_BUILD_TESTS)
        add_executable(libretro_smoke_${system} tests/smoke.cpp)
        target_include_directories(libretro_smoke_${system} PRIVATE
            include ${PROJECT_SOURCE_DIR}/cores/common/tests/support)
        target_compile_definitions(libretro_smoke_${system} PRIVATE RAVENEMU_LIBRETRO_${system_upper}=1)
        target_link_libraries(libretro_smoke_${system} PRIVATE ${target})
        add_test(NAME libretro_smoke_${system} COMMAND libretro_smoke_${system})
    endif()
endforeach()
