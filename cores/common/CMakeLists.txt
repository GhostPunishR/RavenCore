add_library(ravenemu_common STATIC
    src/cheats.cpp
    src/core.cpp
    src/remote_link.cpp
    src/sha256.cpp
)
target_include_directories(ravenemu_common PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Le câble distant attend son partenaire sur un autre fil.
find_package(Threads REQUIRED)
target_link_libraries(ravenemu_common PUBLIC ravenemu_options Threads::Threads)
set_target_properties(ravenemu_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(ravenemu_common_tests tests/sha256_test.cpp)
    target_include_directories(ravenemu_common_tests PRIVATE tests/support)
    target_link_libraries(ravenemu_common_tests PRIVATE ravenemu_common)
    add_test(NAME ravenemu_common COMMAND ravenemu_common_tests)

    add_executable(ravenemu_binary_io_tests tests/binary_io_test.cpp)
    target_include_directories(ravenemu_binary_io_tests PRIVATE tests/support)
    target_link_libraries(ravenemu_binary_io_tests PRIVATE ravenemu_common)
    add_test(NAME ravenemu_binary_io COMMAND ravenemu_binary_io_tests)

    add_executable(ravenemu_cheat_tests tests/cheats_test.cpp)
    target_include_directories(ravenemu_cheat_tests PRIVATE tests/support)
    target_link_libraries(ravenemu_cheat_tests PRIVATE ravenemu_common)
    add_test(NAME ravenemu_cheats COMMAND ravenemu_cheat_tests)
endif()
