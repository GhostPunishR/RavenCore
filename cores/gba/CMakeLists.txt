add_library(gba_raven_core STATIC
    src/core.cpp
    src/cpu/cpu.cpp
    src/memory/bus.cpp
)
target_include_directories(gba_raven_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE src
)
target_link_libraries(gba_raven_core PUBLIC ravenemu_common)
set_target_properties(gba_raven_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(RAVENEMU_BUILD_TESTS)
    add_executable(gba_save_state_tests tests/gba_save_state_test.cpp)
    target_include_directories(gba_save_state_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_save_state_tests PRIVATE gba_raven_core)
    add_test(NAME gba_save_state COMMAND gba_save_state_tests)

    add_executable(gba_raven_core_tests tests/gba_core_test.cpp)
    target_include_directories(gba_raven_core_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support)
    target_link_libraries(gba_raven_core_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_core COMMAND gba_raven_core_tests)

    add_executable(gba_raven_timing_tests tests/gba_timing_test.cpp)
    target_include_directories(gba_raven_timing_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_raven_timing_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_timing COMMAND gba_raven_timing_tests)

    # Le RTC se pilote broche à broche derrière le bus : l'éprouver demande les
    # en-têtes internes du cœur, que `gba_raven_core` garde en PRIVATE.
    add_executable(gba_raven_rtc_tests tests/gba_rtc_test.cpp)
    target_include_directories(gba_raven_rtc_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_raven_rtc_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_rtc COMMAND gba_raven_rtc_tests)

    # L'affichage se pilote lui aussi par ses registres, dans les mêmes
    # en-têtes internes : même raison que pour le RTC.
    add_executable(gba_raven_ppu_tests tests/gba_ppu_test.cpp)
    target_include_directories(gba_raven_ppu_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_raven_ppu_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_ppu COMMAND gba_raven_ppu_tests)

    # Le mélangeur audio se pilote par ses registres, qui vivent dans les
    # en-têtes internes du cœur : même raison que pour le RTC.
    add_executable(gba_raven_apu_tests tests/gba_apu_test.cpp)
    target_include_directories(gba_raven_apu_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_raven_apu_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_apu COMMAND gba_raven_apu_tests)

    # Comportements de l'ARM7TDMI et du bus révélés par les suites publiques :
    # de petits programmes en IWRAM, exécutés instruction par instruction.
    add_executable(gba_raven_cpu_tests tests/gba_cpu_test.cpp)
    target_include_directories(gba_raven_cpu_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_raven_cpu_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_cpu COMMAND gba_raven_cpu_tests)

    # Fidélité matérielle d'après GBATEK : BIOS, HALTCNT, port série, capture
    # vidéo DMA3, temps de rendu des objets, bus ouvert, durées.
    add_executable(gba_raven_hardware_tests tests/gba_hardware_test.cpp)
    target_include_directories(gba_raven_hardware_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common/tests/support
    )
    target_link_libraries(gba_raven_hardware_tests PRIVATE gba_raven_core)
    add_test(NAME gba_raven_hardware COMMAND gba_raven_hardware_tests)

    # Runner des ROMs de test GBA fournies séparément (jamais dans le dépôt).
    # CTest n'enregistre que son auto-test synthétique, sauf si
    # RAVENEMU_GBA_TEST_ROMS désigne un dossier de ROMs : chaque entrée de
    # tests/external_test_roms.txt présente dans ce dossier devient un test.
    add_executable(gba_conformance_runner tests/conformance_runner.cpp)
    target_include_directories(gba_conformance_runner PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_link_libraries(gba_conformance_runner PRIVATE gba_raven_core)
    add_test(NAME gba_conformance_harness COMMAND gba_conformance_runner --self-test)

    set(RAVENEMU_GBA_TEST_ROMS "" CACHE PATH
        "Dossier des ROMs de test GBA publiques (tests/fetch_test_roms.sh)")
    if(RAVENEMU_GBA_TEST_ROMS)
        file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/tests/external_test_roms.txt manifest_lines ENCODING UTF-8)
        foreach(line IN LISTS manifest_lines)
            if(line MATCHES "^[ \t]*(#|$)")
                continue()
            endif()
            separate_arguments(fields UNIX_COMMAND "${line}")
            list(POP_FRONT fields name url sha256)
            set(rom "${RAVENEMU_GBA_TEST_ROMS}/${name}.gba")
            if(EXISTS "${rom}")
                add_test(NAME gba_rom_${name}
                    COMMAND gba_conformance_runner "${rom}" --rom-sha256 ${sha256} ${fields})
            else()
                message(WARNING "ROM de test GBA absente : ${rom}")
            endif()
        endforeach()
    endif()
endif()
