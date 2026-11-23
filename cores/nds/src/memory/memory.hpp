#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Mémoires partagées par les deux CPU (GBATEK, « DS Memory Maps » et « DS
// Memory Control - WRAM ») : RAM principale de 4 Mio, WRAM partagée de 32 Kio
// répartie par WRAMCNT, WRAM de l'ARM7 (64 Kio), palettes et OAM.
namespace ravenemu::nds {

class Memory {
public:
    static constexpr std::size_t main_ram_size = 4 * 1024 * 1024;
    static constexpr std::size_t shared_wram_size = 32 * 1024;
    static constexpr std::size_t arm7_wram_size = 64 * 1024;
    static constexpr std::size_t palette_size = 2 * 1024;
    static constexpr std::size_t oam_size = 2 * 1024;

    std::vector<std::uint8_t> main_ram = std::vector<std::uint8_t>(main_ram_size);
    std::vector<std::uint8_t> shared_wram = std::vector<std::uint8_t>(shared_wram_size);
    std::vector<std::uint8_t> arm7_wram = std::vector<std::uint8_t>(arm7_wram_size);
    std::vector<std::uint8_t> palette = std::vector<std::uint8_t>(palette_size);
    std::vector<std::uint8_t> oam = std::vector<std::uint8_t>(oam_size);
    // WRAMCNT, 0 à 3 : 32/0 Kio, 2e/1er 16 Kio, 1er/2e 16 Kio, 0/32 Kio
    // (ARM9/ARM7).
    std::uint8_t wram_control{3};

    std::uint8_t* main(std::uint32_t address) noexcept { return &main_ram[address & (main_ram_size - 1)]; }

    // WRAM partagée vue par l'ARM9 (0x03000000-0x03FFFFFF) ; nullptr quand
    // l'ARM9 n'en a pas : la zone est alors vide.
    std::uint8_t* wram9(std::uint32_t address) noexcept {
        switch (wram_control & 3U) {
        case 0: return &shared_wram[address & 0x7fffU];
        case 1: return &shared_wram[0x4000U + (address & 0x3fffU)];
        case 2: return &shared_wram[address & 0x3fffU];
        default: return nullptr;
        }
    }
    // WRAM vue par l'ARM7 : la part partagée à 0x03000000-0x037FFFFF (ou, sans
    // part, des miroirs de la WRAM de l'ARM7), puis sa WRAM à 0x03800000.
    std::uint8_t* wram7(std::uint32_t address) noexcept {
        if (address >= 0x0380'0000U) return &arm7_wram[address & 0xffffU];
        switch (wram_control & 3U) {
        case 0: return &arm7_wram[address & 0xffffU];
        case 1: return &shared_wram[address & 0x3fffU];
        case 2: return &shared_wram[0x4000U + (address & 0x3fffU)];
        default: return &shared_wram[address & 0x7fffU];
        }
    }
};

// Lecture et écriture petit-boutistes d'une valeur de `size` octets.
inline std::uint32_t load(const std::uint8_t* at, unsigned size) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, at, size);  // hôtes petit-boutistes (x86, ARM)
    return value;
}
inline void store(std::uint8_t* at, std::uint32_t value, unsigned size) noexcept { std::memcpy(at, &value, size); }

} // namespace ravenemu::nds
