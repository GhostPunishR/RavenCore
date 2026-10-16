add_library(ravenemu_common STATIC
    src/core.cpp
    src/sha256.cpp
)
target_include_directories(ravenemu_common PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ravenemu_common PUBLIC ravenemu_options)
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
endif()
