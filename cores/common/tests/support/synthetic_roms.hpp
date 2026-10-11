#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ravenemu::testing {

std::vector<std::uint8_t> minimal_game_boy_rom() {
    std::vector<std::uint8_t> rom(0x8000);
    rom[0x0147] = 0x00; // no MBC
    // Endless JP 0100 loop, enough to exercise a complete deterministic frame.
    rom[0x0100] = 0xc3;
    rom[0x0101] = 0x00;
    rom[0x0102] = 0x01;
    return rom;
}

std::vector<std::uint8_t> minimal_gba_rom() {
    std::vector<std::uint8_t> rom(0x200);
    // LDR r0, [pc, #8] ; MOV r1, #0x5a ; STRB r1, [r0] ; B .
    const std::array<std::uint32_t, 4> program{
        0xe59f0008U,
        0xe3a0105aU,
        0xe5c01000U,
        0xeafffffeU,
    };
    for (std::size_t index = 0; index < program.size(); ++index) {
        const auto word = program[index];
        for (unsigned byte = 0; byte < 4; ++byte) {
            rom[index * 4U + byte] = static_cast<std::uint8_t>(word >> (byte * 8U));
        }
    }
    // Literal loaded by the first instruction: SRAM base.
    rom[16] = 0x00;
    rom[17] = 0x00;
    rom[18] = 0x00;
    rom[19] = 0x0e;
    rom[0xb2] = 0x96;
    return rom;
}

} // namespace ravenemu::testing
