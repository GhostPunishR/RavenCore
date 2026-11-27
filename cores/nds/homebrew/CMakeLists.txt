# Homebrew de démonstration du cœur NDS : C et assembleur écrits pour
# RavenCore, sans libnds, compilés pour l'ARM9 (ARM946E-S) et l'ARM7
# (ARM7TDMI) par Clang et ld.lld, puis réunis en ROM .nds par nds_make_rom.
# La ROM est construite dans le répertoire de build, jamais versionnée.
find_program(RAVENEMU_ARM_CLANG NAMES clang clang-18 clang-17 REQUIRED)
find_program(RAVENEMU_ARM_LLD NAMES ld.lld ld.lld-18 ld.lld-17 REQUIRED)

set(homebrew_c_flags
    --target=arm-none-eabi -marm -mfloat-abi=soft -ffreestanding -fno-builtin -nostdlib -O2 -std=c11
    -Wall -Wextra -Wconversion -Werror -ffunction-sections -fdata-sections -fno-common)
set(homebrew_headers charset.h ds.h font.h text.h)
list(TRANSFORM homebrew_headers PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

# Binaire brut d'un processeur : crt0.s et les sources C, liés par `script`.
function(ravenemu_nds_homebrew_binary name cpu script)
    set(directory ${CMAKE_CURRENT_BINARY_DIR}/${name})
    set(objects)
    foreach(source crt0.s ${ARGN})
        get_filename_component(stem ${source} NAME_WE)
        set(object ${directory}/${stem}.o)
        if(source MATCHES "\\.s$")
            set(flags --target=arm-none-eabi -marm)
        else()
            set(flags ${homebrew_c_flags})
        endif()
        add_custom_command(OUTPUT ${object}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${directory}
            COMMAND ${RAVENEMU_ARM_CLANG} ${flags} -mcpu=${cpu} -c ${CMAKE_CURRENT_SOURCE_DIR}/${source} -o ${object}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source} ${homebrew_headers}
            COMMENT "Homebrew NDS : ${name}/${source}"
            VERBATIM)
        list(APPEND objects ${object})
    endforeach()
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin
        COMMAND ${RAVENEMU_ARM_LLD} -T ${CMAKE_CURRENT_SOURCE_DIR}/${script} --gc-sections --oformat=binary
                -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin ${objects}
        DEPENDS ${objects} ${CMAKE_CURRENT_SOURCE_DIR}/${script}
        COMMENT "Homebrew NDS : ${name}.bin"
        VERBATIM)
endfunction()

ravenemu_nds_homebrew_binary(arm9 arm946e-s arm9.ld arm9.c font.c text.c runtime.c)
ravenemu_nds_homebrew_binary(arm7 arm7tdmi arm7.ld arm7.c runtime.c)

add_executable(nds_make_rom make_rom.cpp)
target_link_libraries(nds_make_rom PRIVATE nds_raven_core)

set(homebrew_rom ${CMAKE_CURRENT_BINARY_DIR}/raven_hello.nds)
add_custom_command(OUTPUT ${homebrew_rom}
    COMMAND nds_make_rom ${CMAKE_CURRENT_BINARY_DIR}/arm9.bin ${CMAKE_CURRENT_BINARY_DIR}/arm7.bin ${homebrew_rom}
    DEPENDS nds_make_rom ${CMAKE_CURRENT_BINARY_DIR}/arm9.bin ${CMAKE_CURRENT_BINARY_DIR}/arm7.bin
    COMMENT "Homebrew NDS : raven_hello.nds"
    VERBATIM)
add_custom_target(nds_raven_homebrew ALL DEPENDS ${homebrew_rom})

if(BUILD_TESTING)
    add_executable(nds_raven_homebrew_tests ../tests/nds_homebrew_test.cpp)
    target_include_directories(nds_raven_homebrew_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../common/tests/support)
    target_link_libraries(nds_raven_homebrew_tests PRIVATE nds_raven_core)
    add_dependencies(nds_raven_homebrew_tests nds_raven_homebrew)
    add_test(NAME nds_raven_homebrew COMMAND nds_raven_homebrew_tests ${homebrew_rom})
endif()
