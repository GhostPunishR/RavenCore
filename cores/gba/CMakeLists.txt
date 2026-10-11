add_library(gba_raven_core STATIC
    src/core.cpp
    src/cpu/cpu.cpp
    src/memory/bus.cpp
)
target_include_directories(gba_raven_core
    PUBLIC include
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
endif()
