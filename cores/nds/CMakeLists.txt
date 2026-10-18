add_library(nds_raven_core STATIC
    src/cartridge/cartridge.cpp
    src/memory/bus.cpp
    src/bios/boot_rom.cpp
    src/machine/machine.cpp
    src/cpu/cpu.cpp
    src/cpu/cpu_state.cpp
    src/cpu/arm.cpp
    src/cpu/thumb.cpp
    src/cpu/transfer.cpp
    src/cpu/psr.cpp
    src/cpu/exceptions.cpp
)
target_include_directories(nds_raven_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(nds_raven_core PUBLIC ravenemu_common)
set_target_properties(nds_raven_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(nds_cartridge_tests tests/cartridge_test.cpp)
    target_include_directories(nds_cartridge_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_cartridge_tests PRIVATE nds_raven_core)
    add_test(NAME nds_cartridge COMMAND nds_cartridge_tests)
    add_executable(nds_memory_tests tests/memory_test.cpp)
    target_include_directories(nds_memory_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_memory_tests PRIVATE nds_raven_core)
    add_test(NAME nds_memory COMMAND nds_memory_tests)
    add_executable(nds_boot_tests tests/boot_test.cpp)
    target_include_directories(nds_boot_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_boot_tests PRIVATE nds_raven_core)
    add_test(NAME nds_boot COMMAND nds_boot_tests)
    add_executable(nds_cpu_tests tests/cpu_test.cpp)
    target_include_directories(nds_cpu_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_cpu_tests PRIVATE nds_raven_core)
    add_test(NAME nds_cpu COMMAND nds_cpu_tests)
    add_executable(nds_calls_tests tests/calls_test.cpp)
    target_include_directories(nds_calls_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_calls_tests PRIVATE nds_raven_core)
    add_test(NAME nds_calls COMMAND nds_calls_tests)
    add_executable(nds_data_tests tests/data_test.cpp)
    target_include_directories(nds_data_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_data_tests PRIVATE nds_raven_core)
    add_test(NAME nds_data COMMAND nds_data_tests)
    add_executable(nds_exceptions_tests tests/exceptions_test.cpp)
    target_include_directories(nds_exceptions_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_exceptions_tests PRIVATE nds_raven_core)
    add_test(NAME nds_exceptions COMMAND nds_exceptions_tests)
endif()
