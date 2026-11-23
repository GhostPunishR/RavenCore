# Cœur Nintendo DS, en construction : pas encore d'API publique, ni de cœur
# Libretro, ni d'installation. Ses deux processeurs viennent du CPU ARM commun.
add_library(nds_raven_core STATIC
    src/cartridge/header.cpp
    src/gpu/engine2d.cpp
    src/gpu/object_renderer.cpp
    src/machine/direct_boot.cpp
    src/machine/machine.cpp
    src/memory/io7.cpp
    src/memory/io9.cpp
)
target_include_directories(nds_raven_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(nds_raven_core PUBLIC ravenemu_arm)
set_target_properties(nds_raven_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    # Composants pris un à un : IPC, maths, timers, balayage, CP15, mémoire, en-tête.
    add_executable(nds_raven_hardware_tests tests/nds_hardware_test.cpp)
    target_include_directories(nds_raven_hardware_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support)
    target_link_libraries(nds_raven_hardware_tests PRIVATE nds_raven_core)
    add_test(NAME nds_raven_hardware COMMAND nds_raven_hardware_tests)

    # Machine complète : une ROM écrite pour le test, démarrée directement.
    add_executable(nds_raven_machine_tests tests/nds_machine_test.cpp)
    target_include_directories(nds_raven_machine_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support)
    target_link_libraries(nds_raven_machine_tests PRIVATE nds_raven_core)
    add_test(NAME nds_raven_machine COMMAND nds_raven_machine_tests)
endif()
