#pragma once

#include <cstdint>

// Temps d'accès de chaque région (GBATEK, « DS Memory Timings »), en cycles
// système pour l'ARM7 et en cycles de l'ARM9 (deux par cycle système) pour
// l'ARM9. Les accès de 8 bits coûtent comme ceux de 16 bits. L'ARM9 lit
// toujours son code en accès non séquentiels de 32 bits ; un demi-mot Thumb
// coûte la moitié d'un mot.
//
// Les caches de l'ARM9 ne sont pas modélisés : quand CP15 les active, les
// lectures de la RAM principale et du BIOS sont comptées comme des succès.
// Le temps d'accès réglé par EXMEMCNT pour le slot GBA est celui de
// l'exemple de GBATEK (10, 6).
namespace ravenemu::nds {

enum class Region : std::uint8_t { tcm, main_ram, wram, bios, io, palette, vram, oam, gba_rom, gba_ram, none };

struct AccessCosts {
    std::uint8_t n32;
    std::uint8_t s32;
    std::uint8_t n16;
    std::uint8_t s16;

    [[nodiscard]] constexpr int cost(bool sequential, unsigned width) const noexcept {
        if (width == 4) return sequential ? s32 : n32;
        return sequential ? s16 : n16;
    }
};

constexpr AccessCosts arm7_code_costs(Region region) noexcept {
    switch (region) {
    case Region::main_ram: return {9, 2, 8, 1};
    case Region::palette:
    case Region::vram: return {2, 2, 1, 1};
    case Region::gba_rom: return {16, 12, 10, 6};
    case Region::gba_ram: return {9, 10, 9, 10};
    default: return {1, 1, 1, 1};
    }
}

// GBATEK note « 1? » pour un N32 en VRAM : un accès de 32 bits sur un bus de
// 16 bits, compté ici N16 + S16.
constexpr AccessCosts arm7_data_costs(Region region) noexcept {
    switch (region) {
    case Region::main_ram: return {10, 2, 9, 1};
    case Region::palette:
    case Region::vram: return {2, 2, 1, 1};
    case Region::gba_rom: return {15, 12, 9, 6};
    case Region::gba_ram: return {9, 10, 9, 10};
    default: return {1, 1, 1, 1};
    }
}

// Code de l'ARM9 : N32 + 3 attentes, sauf TCM.
constexpr int arm9_code_cost(Region region) noexcept {
    switch (region) {
    case Region::tcm: return 1;
    case Region::main_ram: return 18;
    case Region::palette:
    case Region::vram: return 10;
    case Region::gba_rom:
    case Region::gba_ram: return 38;
    default: return 8;
    }
}

constexpr AccessCosts arm9_data_costs(Region region) noexcept {
    switch (region) {
    case Region::tcm: return {1, 1, 1, 1};
    case Region::main_ram: return {20, 4, 18, 2};
    case Region::palette:
    case Region::vram: return {10, 4, 8, 2};
    case Region::gba_rom: return {38, 24, 26, 12};
    case Region::gba_ram: return {26, 20, 26, 20};
    default: return {8, 2, 8, 2};
    }
}

} // namespace ravenemu::nds
