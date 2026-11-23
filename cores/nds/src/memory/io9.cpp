#include "memory/bus9.hpp"
#include "memory/io_access.hpp"

#include <utility>

// Registres d'E/S de l'ARM9 déjà modélisés (GBATEK, « DS I/O Maps ») ; les
// autres se lisent à 0 et ignorent les écritures.
namespace ravenemu::nds {

namespace {

// VRAM vue par l'ARM9 : fonds et objets des moteurs A et B, puis LCDC.
std::pair<Vram::Area, std::uint32_t> vram_area(std::uint32_t address) noexcept {
    switch ((address >> 21U) & 7U) {
    case 0: return {Vram::bg_a, address & 0x7ffffU};
    case 1: return {Vram::bg_b, address & 0x1ffffU};
    case 2: return {Vram::obj_a, address & 0x3ffffU};
    case 3: return {Vram::obj_b, address & 0x1ffffU};
    default: return {Vram::lcdc, address & 0xfffffU};
    }
}

std::uint32_t vram_read(const Vram& vram, std::uint32_t address, unsigned width) noexcept {
    const auto [area, offset] = vram_area(address);
    return vram.read(area, offset, width);
}

// Slot GBA vide (GBATEK, « DS Memory Control - Cartridges and Main RAM ») :
// la ROM rend l'adresse / 2 par demi-mot, la RAM des 0xFF ; le CPU qui n'a pas
// le slot lit des 0.
std::uint32_t empty_gba_slot(std::uint32_t address, unsigned width, Region region, bool owner) noexcept {
    if (!owner) return 0;
    if (region == Region::gba_ram) return width == 4 ? 0xffff'ffffU : (1U << (8U * width)) - 1U;
    const auto half = [](std::uint32_t at) { return (at >> 1U) & 0xffffU; };
    const auto aligned = address & ~1U;
    if (width == 4) return half(aligned) | (half(aligned + 2) << 16U);
    const auto value = half(aligned);
    return width == 2 ? value : (value >> ((address & 1U) * 8U)) & 0xffU;
}

// 0x04000240 + index : VRAMCNT_A à G, puis H et I après WRAMCNT (0x04000247).
constexpr unsigned vram_bank(unsigned index) noexcept { return index < 7 ? index : index - 1; }

} // namespace

std::uint32_t Bus9::other_read(std::uint32_t address, unsigned width, Region region) {
    switch (region) {
    case Region::io: return io_extract(io_word(address & ~3U), address, width);
    case Region::gba_rom:
    case Region::gba_ram: return empty_gba_slot(address, width, region, !hw_.gba_slot_arm7());
    case Region::vram: return vram_read(hw_.vram, address, width);
    default: return 0;  // WRAM absente, zones vides
    }
}

std::uint32_t Bus9::io_word(std::uint32_t address) {
    auto& hw = hw_;
    const auto now = hw.arm9_time();
    if (address == 0x0410'0000U) return hw.ipc.receive(0);
    if (address >= 0x0400'1000U && address < 0x0400'1070U) return hw.engine_b.read(address & 0xfffU);
    if (address >= 0x0400'1000U) return 0;
    const auto offset = address & 0xfffU;
    if (offset >= 0x100 && offset < 0x110) return hw.timers9.read(offset - 0x100, now);
    if (offset >= 0x0b0 && offset < 0x0e0) return dma_register(hw.dma9, offset);
    if (offset >= 0x0e0 && offset < 0x0f0) return hw.dma_fill[(offset - 0x0e0) / 4];
    if (offset >= 0x280 && offset < 0x2b0) return hw.divider.read(offset, now);
    if (offset >= 0x2b0 && offset < 0x2c0) return hw.square_root.read(offset, now);
    if (offset != 0x004 && offset < 0x070) return hw.engine_a.read(offset);
    switch (offset) {
    case 0x004: return hw.lcd.read(0);
    case 0x130: return hw.keys | (hw.key_control[0] << 16U);
    case 0x180: return hw.ipc.read_sync(0);
    case 0x184: return hw.ipc.read_control(0);
    case 0x204: return hw.external_memory[0];
    case 0x208:
    case 0x210:
    case 0x214: return hw.irq9.read(offset);
    case 0x240:
    case 0x244:
    case 0x248: {
        std::uint32_t word = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto index = offset - 0x240 + byte;
            const auto value = index == 7 ? hw.memory.wram_control : (index < 10 ? hw.vram.control(vram_bank(index)) : 0U);
            word |= std::uint32_t{value} << (8U * byte);
        }
        return word;
    }
    case 0x300: return hw.post_boot[0];
    case 0x304: return hw.power_control9;
    default: return 0;
    }
}

void Bus9::vram_write(std::uint32_t address, std::uint32_t value, unsigned width) {
    const auto [area, offset] = vram_area(address);
    hw_.vram.write(area, offset, value, width);
}

void Bus9::io_write(std::uint32_t address, std::uint32_t value, unsigned width) {
    io_write_word(address & ~3U, io_shift(address, value), io_mask(address, width));
}

void Bus9::io_write_word(std::uint32_t address, std::uint32_t value, std::uint32_t mask) {
    auto& hw = hw_;
    const auto now = hw.arm9_time();
    if (address >= 0x0400'1000U && address < 0x0400'1070U) return hw.engine_b.write(address & 0xfffU, value, mask);
    if (address >= 0x0400'1000U) return;
    const auto offset = address & 0xfffU;
    if (offset != 0x004 && offset < 0x070) return hw.engine_a.write(offset, value, mask);
    if (offset >= 0x100 && offset < 0x110) return hw.timers9.write(offset - 0x100, value, mask, now);
    if (offset >= 0x0b0 && offset < 0x0e0) {
        if (write_dma_register(hw.dma9, offset, value, mask, true))
            run_dma(*this, hw.dma9[(offset - 0x0b0) / 12], (offset - 0x0b0) / 12, true, hw.irq9);
        return;
    }
    if (offset >= 0x0e0 && offset < 0x0f0) {
        auto& fill = hw.dma_fill[(offset - 0x0e0) / 4];
        fill = merge(fill, value, mask);
        return;
    }
    if (offset >= 0x280 && offset < 0x2b0) return hw.divider.write(offset, value, mask, now);
    if (offset >= 0x2b0 && offset < 0x2c0) return hw.square_root.write(offset, value, mask, now);
    switch (offset) {
    case 0x004: hw.lcd.write(0, value, mask); break;
    case 0x130: hw.key_control[0] = merge(hw.key_control[0], value >> 16U, mask >> 16U) & 0xc3ffU; break;
    case 0x180: hw.ipc.write_sync(0, value, mask); break;
    case 0x184: hw.ipc.write_control(0, value, mask); break;
    case 0x188: hw.ipc.send(0, value); break;
    case 0x204: hw.external_memory[0] = merge(hw.external_memory[0], value, mask & 0xe8ffU); break;
    case 0x208:
    case 0x210:
    case 0x214: hw.irq9.write(offset, value, mask); break;
    case 0x240:
    case 0x244:
    case 0x248:
        for (unsigned byte = 0; byte < 4; ++byte) {
            if (((mask >> (8U * byte)) & 0xffU) == 0) continue;
            const auto index = offset - 0x240 + byte;
            const auto data = static_cast<std::uint8_t>(value >> (8U * byte));
            if (index == 7) hw.memory.wram_control = data & 3U;
            else if (index < 10) hw.vram.set_control(vram_bank(index), data);
        }
        break;
    case 0x300: hw.post_boot[0] = merge(hw.post_boot[0], value, mask & 3U); break;
    case 0x304: hw.power_control9 = merge(hw.power_control9, value, mask & 0x820fU); break;
    default: break;
    }
}

} // namespace ravenemu::nds
