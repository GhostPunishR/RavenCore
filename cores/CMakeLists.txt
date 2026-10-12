cmake_minimum_required(VERSION 3.22.1)
project(ravenemu_cores LANGUAGES CXX)

option(RAVENEMU_BUILD_TESTS "Construire les tests des cœurs" OFF)

# Les en-têtes publics exigent C++20 : seule cette exigence se propage aux
# projets qui consomment les cœurs.
add_library(ravenemu_options INTERFACE)
target_compile_features(ravenemu_options INTERFACE cxx_std_20)

# Avertissements stricts réservés aux cibles de ce dépôt. Portés par le
# répertoire et non par `ravenemu_options`, ils ne s'imposent plus au code
# d'un consommateur. La variable reste visible de `sdk/` et `libretro/`.
if(MSVC)
    set(RAVENEMU_WARNING_OPTIONS /W4 /permissive- CACHE INTERNAL "Avertissements des cibles RavenEmu")
else()
    set(RAVENEMU_WARNING_OPTIONS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        CACHE INTERNAL "Avertissements des cibles RavenEmu")
endif()
add_compile_options(${RAVENEMU_WARNING_OPTIONS})

if(RAVENEMU_BUILD_TESTS)
    enable_testing()
    # Les tests construisent des machines complètes sur la pile (une machine
    # GBA pèse ~640 Kio, un banc RTC ~470 Kio). Linux et macOS donnent 8 Mio
    # au fil principal, Windows seulement 1 Mio : plusieurs tests y
    # débordaient. Seuls les exécutables de ce répertoire, des tests, sont liés.
    if(WIN32)
        if(MSVC)
            add_link_options(/STACK:16777216)
        else()
            add_link_options(LINKER:--stack,16777216)
        endif()
    endif()
endif()

add_subdirectory(common)
add_subdirectory(gb)
add_subdirectory(gbc)
add_subdirectory(gba)

add_library(ravenemu_cores INTERFACE)
target_link_libraries(ravenemu_cores INTERFACE gb_raven_core gbc_raven_core gba_raven_core)

if(RAVENEMU_BUILD_TESTS)
    # Filet transversal : des états instantanés corrompus ne doivent planter
    # aucun des trois cœurs. La CI l'exécute aussi sous ASan/UBSan.
    add_executable(ravenemu_state_mutation_tests tests/state_mutation_test.cpp)
    target_include_directories(ravenemu_state_mutation_tests PRIVATE common/tests/support)
    target_link_libraries(ravenemu_state_mutation_tests PRIVATE ravenemu_cores)
    add_test(NAME ravenemu_state_mutation COMMAND ravenemu_state_mutation_tests)
endif()
