add_library(nds_raven_core STATIC
    src/cartridge/cartridge.cpp
    src/memory/bus.cpp
    src/memory/io.cpp
    src/memory/input.cpp
    src/input/input.cpp
    src/spi/spi.cpp
    src/spi/touchscreen.cpp
    src/memory/clock.cpp
    src/video/lcd_timing.cpp
    src/video/vram.cpp
    src/video/frame.cpp
    src/video/capture.cpp
    src/video/fifo.cpp
    src/video/renderer.cpp
    src/video/objects.cpp
    src/video/mosaic.cpp
    src/video/palettes.cpp
    src/video/compositor.cpp
    src/interrupts/interrupt_controller.cpp
    src/ipc/ipc.cpp
    src/dma/dma_controller.cpp
    src/dma/transfer.cpp
    src/timers/timers.cpp
    src/bios/boot_rom.cpp
    src/machine/machine.cpp
    src/machine/scheduler.cpp
    src/cpu/protection.cpp
    src/cpu/cp15.cpp
    src/cpu/coprocessor.cpp
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
    add_executable(nds_spi_tests tests/spi_test.cpp)
    target_include_directories(nds_spi_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_spi_tests PRIVATE nds_raven_core)
    add_test(NAME nds_spi COMMAND nds_spi_tests)
    add_executable(nds_input_tests tests/input_test.cpp)
    target_include_directories(nds_input_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_input_tests PRIVATE nds_raven_core)
    add_test(NAME nds_input COMMAND nds_input_tests)
    add_executable(nds_scheduler_tests tests/scheduler_test.cpp)
    target_include_directories(nds_scheduler_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_scheduler_tests PRIVATE nds_raven_core)
    add_test(NAME nds_scheduler COMMAND nds_scheduler_tests)
    add_executable(nds_wait_tests tests/wait_test.cpp)
    target_include_directories(nds_wait_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_wait_tests PRIVATE nds_raven_core)
    add_test(NAME nds_wait COMMAND nds_wait_tests)
    add_executable(nds_mpu_tests tests/mpu_test.cpp)
    target_include_directories(nds_mpu_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_mpu_tests PRIVATE nds_raven_core)
    add_test(NAME nds_mpu COMMAND nds_mpu_tests)
    add_executable(nds_cp15_tests tests/cp15_test.cpp)
    target_include_directories(nds_cp15_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_cp15_tests PRIVATE nds_raven_core)
    add_test(NAME nds_cp15 COMMAND nds_cp15_tests)
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
    add_executable(nds_interrupts_tests tests/interrupts_test.cpp)
    target_include_directories(nds_interrupts_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_interrupts_tests PRIVATE nds_raven_core)
    add_test(NAME nds_interrupts COMMAND nds_interrupts_tests)
    add_executable(nds_timers_tests tests/timers_test.cpp)
    target_include_directories(nds_timers_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_timers_tests PRIVATE nds_raven_core)
    add_test(NAME nds_timers COMMAND nds_timers_tests)
    add_executable(nds_ipc_tests tests/ipc_test.cpp)
    target_include_directories(nds_ipc_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_ipc_tests PRIVATE nds_raven_core)
    add_test(NAME nds_ipc COMMAND nds_ipc_tests)
    add_executable(nds_dma_tests tests/dma_test.cpp)
    target_include_directories(nds_dma_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_dma_tests PRIVATE nds_raven_core)
    add_test(NAME nds_dma COMMAND nds_dma_tests)
    add_executable(nds_lcd_tests tests/lcd_test.cpp)
    target_include_directories(nds_lcd_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_lcd_tests PRIVATE nds_raven_core)
    add_test(NAME nds_lcd COMMAND nds_lcd_tests)
    add_executable(nds_video_tests tests/video_test.cpp)
    target_include_directories(nds_video_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_video_tests PRIVATE nds_raven_core)
    add_test(NAME nds_video COMMAND nds_video_tests)
    add_executable(nds_2d_tests tests/graphics_test.cpp)
    target_include_directories(nds_2d_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_2d_tests PRIVATE nds_raven_core)
    add_test(NAME nds_2d COMMAND nds_2d_tests)
    add_executable(nds_effects_tests tests/effects_test.cpp)
    target_include_directories(nds_effects_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_effects_tests PRIVATE nds_raven_core)
    add_test(NAME nds_effects COMMAND nds_effects_tests)
    add_executable(nds_palette_tests tests/palette_test.cpp)
    target_include_directories(nds_palette_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_palette_tests PRIVATE nds_raven_core)
    add_test(NAME nds_palettes COMMAND nds_palette_tests)
    add_executable(nds_mosaic_tests tests/mosaic_test.cpp)
    target_include_directories(nds_mosaic_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_mosaic_tests PRIVATE nds_raven_core)
    add_test(NAME nds_mosaic COMMAND nds_mosaic_tests)
    add_executable(nds_capture_tests tests/capture_test.cpp)
    target_include_directories(nds_capture_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_capture_tests PRIVATE nds_raven_core)
    add_test(NAME nds_capture COMMAND nds_capture_tests)
    add_executable(nds_fifo_tests tests/fifo_test.cpp)
    target_include_directories(nds_fifo_tests PRIVATE ../common/tests/support)
    target_link_libraries(nds_fifo_tests PRIVATE nds_raven_core)
    add_test(NAME nds_fifo COMMAND nds_fifo_tests)
endif()
