add_library(gbc_raven_core STATIC src/core.cpp)
target_include_directories(gbc_raven_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE src
)
# Le moteur Game Boy Color, qui émule aussi la Game Boy : le mode matériel
# (DMG, CGB, CGB en compatibilité, CGB d'une GBA) se choisit pour chaque jeu.
# Les composants propres à la CGB (KEY1 et double vitesse, port infrarouge)
# sont dans `src/speed` et `src/infrared`, avec le reste du matériel.
target_link_libraries(gbc_raven_core PUBLIC ravenemu_common)
set_target_properties(gbc_raven_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(gbc_raven_core_tests tests/cgb_core_test.cpp)
    target_include_directories(gbc_raven_core_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support)
    target_link_libraries(gbc_raven_core_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_raven_core COMMAND gbc_raven_core_tests)

    # Matériel propre à la Game Boy Color : KEY1, port infrarouge, VRAM et
    # WRAM en banques, palettes, HDMA, mode de compatibilité.
    add_executable(gbc_color_hardware_tests tests/cgb_hardware_test.cpp)
    target_include_directories(gbc_color_hardware_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_color_hardware_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_color_hardware COMMAND gbc_color_hardware_tests)

    add_executable(gbc_cpu_mcycle_tests tests/cpu_mcycle_test.cpp)
    target_include_directories(gbc_cpu_mcycle_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_cpu_mcycle_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_cpu_mcycle COMMAND gbc_cpu_mcycle_tests)

    add_executable(gbc_timer_edge_tests tests/timer_edge_test.cpp)
    target_include_directories(gbc_timer_edge_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_timer_edge_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_timer_edges COMMAND gbc_timer_edge_tests)

    # Tests du PPU, un fichier par composant (aides : tests/ppu_test_support.hpp).
    foreach(component ppu_fetcher ppu_object ppu_line_timing ppu_video_bus ppu_state post_boot)
        add_executable(gbc_${component}_tests tests/${component}_test.cpp)
        target_include_directories(gbc_${component}_tests PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/src
            ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
        )
        target_link_libraries(gbc_${component}_tests PRIVATE gbc_raven_core)
        add_test(NAME gbc_${component} COMMAND gbc_${component}_tests)
    endforeach()

    add_executable(gbc_cartridge_mbc_tests tests/cartridge_mbc_test.cpp)
    target_include_directories(gbc_cartridge_mbc_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_cartridge_mbc_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_cartridge_mbc COMMAND gbc_cartridge_mbc_tests)

    add_executable(gbc_cartridge_mbc6_tests tests/cartridge_mbc6_test.cpp)
    target_include_directories(gbc_cartridge_mbc6_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_cartridge_mbc6_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_cartridge_mbc6 COMMAND gbc_cartridge_mbc6_tests)

    add_executable(gbc_cartridge_mbc7_tests tests/cartridge_mbc7_test.cpp)
    target_include_directories(gbc_cartridge_mbc7_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_cartridge_mbc7_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_cartridge_mbc7 COMMAND gbc_cartridge_mbc7_tests)

    add_executable(gbc_cartridge_huc1_tests tests/cartridge_huc1_test.cpp)
    target_include_directories(gbc_cartridge_huc1_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_cartridge_huc1_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_cartridge_huc1 COMMAND gbc_cartridge_huc1_tests)

    add_executable(gbc_cartridge_huc3_tests tests/cartridge_huc3_test.cpp)
    target_include_directories(gbc_cartridge_huc3_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_cartridge_huc3_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_cartridge_huc3 COMMAND gbc_cartridge_huc3_tests)

    add_executable(gbc_apu_hardware_tests tests/apu_hardware_test.cpp)
    target_include_directories(gbc_apu_hardware_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gbc_apu_hardware_tests PRIVATE gbc_raven_core)
    add_test(NAME gbc_apu_hardware COMMAND gbc_apu_hardware_tests)

    # Runner générique pour les ROMs de conformité fournies séparément. CTest
    # n'enregistre que son auto-test synthétique ; aucune ROM externe n'entre
    # dans le dépôt ou dans la configuration par défaut.
    add_executable(gbc_conformance_runner tests/conformance_runner.cpp)
    target_include_directories(gbc_conformance_runner PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
    )
    target_link_libraries(gbc_conformance_runner PRIVATE gbc_raven_core)
    add_test(NAME gbc_conformance_harness COMMAND gbc_conformance_runner --self-test)
    add_test(NAME gbc_conformance_harness_memory COMMAND gbc_conformance_runner --self-test
        --serial-pass __disabled__ --no-mooneye-signature
        --expect-memory 0xC000=0x42
        --memory-require-mismatch --memory-stability-samples 8)

    # L'orchestrateur de manifests reste un outil de développement : le cœur
    # et le runner natif ne dépendent pas de Python. Quand l'interpréteur est
    # disponible, CTest vérifie néanmoins tout le chemin manifeste -> SHA-256
    # -> exécution -> rapport avec une ROM synthétique créée à la volée.
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gbc_conformance_manifest_harness
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/tests/conformance_manifest.py
                --self-test
                --runner $<TARGET_FILE:gbc_conformance_runner>
        )
    endif()

    # RAVENEMU_GB_TEST_ROMS désigne l'archive publique extraite par
    # tests/fetch_test_roms.sh : chaque test de tests/public-test-roms.json
    # devient un test CTest gbc_rom_<id>, dont l'orchestrateur vérifie la ROM
    # (et la capture attendue) par SHA-256 avant de lancer le runner.
    set(RAVENEMU_GB_TEST_ROMS "" CACHE PATH
        "Dossier des ROMs de test GB/GBC publiques (tests/fetch_test_roms.sh)")
    if(RAVENEMU_GB_TEST_ROMS)
        if(NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR "RAVENEMU_GB_TEST_ROMS exige Python 3.10 ou ultérieur")
        endif()
        set(gbc_rom_manifest ${CMAKE_CURRENT_LIST_DIR}/tests/public-test-roms.json)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${gbc_rom_manifest})
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tests/conformance_manifest.py
                ${gbc_rom_manifest} --list
            OUTPUT_VARIABLE gbc_rom_list
            ERROR_VARIABLE gbc_rom_list_error
            RESULT_VARIABLE gbc_rom_list_result)
        if(NOT gbc_rom_list_result EQUAL 0)
            message(FATAL_ERROR "Manifeste des ROMs de test GB invalide : ${gbc_rom_list_error}")
        endif()
        # --list écrit une ligne par test : identifiant, catégorie, matériel, ROM.
        string(REPLACE "\n" ";" gbc_rom_lines "${gbc_rom_list}")
        foreach(line IN LISTS gbc_rom_lines)
            if(line MATCHES "^([^\t]+)\t")
                set(id "${CMAKE_MATCH_1}")
                add_test(NAME gbc_rom_${id}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_LIST_DIR}/tests/conformance_manifest.py
                        ${gbc_rom_manifest} --id ${id}
                        --runner $<TARGET_FILE:gbc_conformance_runner>
                        --rom-root ${RAVENEMU_GB_TEST_ROMS}
                        --report ${CMAKE_CURRENT_BINARY_DIR}/gbc-rom-reports/${id}.json)
            endif()
        endforeach()
    endif()
endif()
