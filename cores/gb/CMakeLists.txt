add_library(gb_raven_core STATIC src/core.cpp)
target_include_directories(gb_raven_core
    PUBLIC include
    PRIVATE src
)
# `gbc_hardware` est déclarée par `cores/gbc` et porte les composants CGB déjà
# extraits (KEY1, port infrarouge). Seuls les en-têtes privés de `src/` les
# incluent : l'API publique du cœur GB n'en expose aucun, d'où PRIVATE.
#
# Passer par la cible plutôt que par `../gbc/include` n'est pas cosmétique :
# un chemin d'inclusion brut ne transporte ni les objets ni les dépendances
# transitives, et l'extraction restait donc condamnée aux composants
# header-only.
target_link_libraries(gb_raven_core
    PUBLIC ravenemu_common
    PRIVATE gbc_hardware
)
set_target_properties(gb_raven_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(RAVENEMU_BUILD_TESTS)
    add_executable(gb_raven_core_tests tests/cgb_core_test.cpp)
    target_include_directories(gb_raven_core_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support)
    target_link_libraries(gb_raven_core_tests PRIVATE gb_raven_core)
    add_test(NAME gb_raven_core COMMAND gb_raven_core_tests)

    add_executable(gb_cpu_mcycle_tests tests/cpu_mcycle_test.cpp)
    target_include_directories(gb_cpu_mcycle_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_cpu_mcycle_tests PRIVATE gb_raven_core)
    add_test(NAME gb_cpu_mcycle COMMAND gb_cpu_mcycle_tests)

    add_executable(gb_timer_edge_tests tests/timer_edge_test.cpp)
    target_include_directories(gb_timer_edge_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_timer_edge_tests PRIVATE gb_raven_core)
    add_test(NAME gb_timer_edges COMMAND gb_timer_edge_tests)

    add_executable(gb_ppu_pipeline_tests tests/ppu_pipeline_test.cpp)
    target_include_directories(gb_ppu_pipeline_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_ppu_pipeline_tests PRIVATE gb_raven_core)
    add_test(NAME gb_ppu_pipeline COMMAND gb_ppu_pipeline_tests)

    add_executable(gb_cartridge_mbc_tests tests/cartridge_mbc_test.cpp)
    target_include_directories(gb_cartridge_mbc_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_cartridge_mbc_tests PRIVATE gb_raven_core)
    add_test(NAME gb_cartridge_mbc COMMAND gb_cartridge_mbc_tests)

    add_executable(gb_cartridge_mbc6_tests tests/cartridge_mbc6_test.cpp)
    target_include_directories(gb_cartridge_mbc6_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_cartridge_mbc6_tests PRIVATE gb_raven_core)
    add_test(NAME gb_cartridge_mbc6 COMMAND gb_cartridge_mbc6_tests)

    add_executable(gb_cartridge_mbc7_tests tests/cartridge_mbc7_test.cpp)
    target_include_directories(gb_cartridge_mbc7_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_cartridge_mbc7_tests PRIVATE gb_raven_core)
    add_test(NAME gb_cartridge_mbc7 COMMAND gb_cartridge_mbc7_tests)

    add_executable(gb_cartridge_huc1_tests tests/cartridge_huc1_test.cpp)
    target_include_directories(gb_cartridge_huc1_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_cartridge_huc1_tests PRIVATE gb_raven_core gbc_hardware)
    add_test(NAME gb_cartridge_huc1 COMMAND gb_cartridge_huc1_tests)

    add_executable(gb_cartridge_huc3_tests tests/cartridge_huc3_test.cpp)
    target_include_directories(gb_cartridge_huc3_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_cartridge_huc3_tests PRIVATE gb_raven_core gbc_hardware)
    add_test(NAME gb_cartridge_huc3 COMMAND gb_cartridge_huc3_tests)

    add_executable(gb_apu_hardware_tests tests/apu_hardware_test.cpp)
    target_include_directories(gb_apu_hardware_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gb_apu_hardware_tests PRIVATE gb_raven_core)
    add_test(NAME gb_apu_hardware COMMAND gb_apu_hardware_tests)

    # Runner générique pour les ROMs de conformité fournies séparément. CTest
    # n'enregistre que son auto-test synthétique ; aucune ROM externe n'entre
    # dans le dépôt ou dans la configuration par défaut.
    add_executable(gb_conformance_runner tests/conformance_runner.cpp)
    target_include_directories(gb_conformance_runner PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
    )
    target_link_libraries(gb_conformance_runner PRIVATE gb_raven_core gbc_hardware)
    add_test(NAME gb_conformance_harness COMMAND gb_conformance_runner --self-test)
    add_test(NAME gb_conformance_harness_memory COMMAND gb_conformance_runner --self-test
        --serial-pass __disabled__ --no-mooneye-signature
        --expect-memory 0xC000=0x42
        --memory-require-mismatch --memory-stability-samples 8)

    # L'orchestrateur de manifests reste un outil de développement : le cœur
    # et le runner natif ne dépendent pas de Python. Quand l'interpréteur est
    # disponible, CTest vérifie néanmoins tout le chemin manifeste -> SHA-256
    # -> exécution -> rapport avec une ROM synthétique créée à la volée.
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gb_conformance_manifest_harness
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/tests/conformance_manifest.py
                --self-test
                --runner $<TARGET_FILE:gb_conformance_runner>
        )
    endif()
endif()
