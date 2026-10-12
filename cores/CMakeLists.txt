cmake_minimum_required(VERSION 3.22.1)
project(ravenemu_cores LANGUAGES CXX)

option(RAVENEMU_BUILD_TESTS "Construire les tests des cœurs" OFF)

add_library(ravenemu_options INTERFACE)
target_compile_features(ravenemu_options INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(ravenemu_options INTERFACE /W4 /permissive-)
else()
    target_compile_options(ravenemu_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

if(RAVENEMU_BUILD_TESTS)
    enable_testing()
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
