#include "memory/bus7.hpp"
#include "memory/io_access.hpp"

// Registres d'E/S de l'ARM7 déjà modélisés (GBATEK, « DS I/O Maps ») ; les
// autres se lisent à 0 et ignorent les écritures.
namespace ravenemu::nds {

std::uint32_t Bus7::other_read(std::uint32_t address, unsigned width, Region region) {
    if (region == Region::io) return io_extract(io_word(address & ~3U), address, width);
    if (region == Region::vram) return hw_.vram.read(Vram::arm7, address & 0x3ffffU, width);
    if (region != Region::gba_rom && region != Region::gba_ram) return 0;
    if (!hw_.gba_slot_arm7()) return 0;
    if (region == Region::gba_ram) return width == 4 ? 0xffff'ffffU : (1U << (8U * width)) - 1U;
    const auto half = [](std::uint32_t at) { return (at >> 1U) & 0xffffU; };
    const auto aligned = address & ~1U;
    if (width == 4) return half(aligned) | (half(aligned + 2) << 16U);
    return width == 2 ? half(aligned) : (half(aligned) >> ((address & 1U) * 8U)) & 0xffU;
}

std::uint32_t Bus7::io_word(std::uint32_t address) {
    auto& hw = hw_;
    const auto now = hw.arm7_time();
    if (address == 0x0410'0000U) return hw.ipc.receive(1);
    if (address == 0x0410'0010U) return hw.card_arm7() ? hw.card.read_data(now, hw.scheduler, hw.irq7) : 0U;
    if (address >= 0x0400'1000U) return 0;  // Wi-Fi à 0x04800000 : pas encore modélisée
    const auto offset = address & 0xfffU;
    if (offset >= 0x100 && offset < 0x110) return hw.timers7.read(offset - 0x100, now);
    if (offset >= 0x0b0 && offset < 0x0e0) return dma_register(hw.dma7, offset);
    if (offset >= 0x1a0 && offset < 0x1c0) return hw.card_arm7() ? hw.card.read(offset - 0x1a0, now) : 0U;
    if (offset == 0x1c0) return hw.spi.read(now);
    if (offset >= 0x400 && offset < 0x520) return hw.sound.read(offset - 0x400, now);
    switch (offset) {
    case 0x004: return hw.lcd.read(1);
    case 0x130: return hw.keys | (hw.key_control[1] << 16U);
    case 0x138: return hw.rtc.read();
    case 0x134: return hw.rtc_interrupt.read_rcnt(hw.rtc, now) | (hw.extended_keys << 16U);  // RCNT, puis EXTKEYIN
    case 0x180: return hw.ipc.read_sync(1);
    case 0x184: return hw.ipc.read_control(1);
    // EXMEMSTAT : bits 0-6 propres, 7-15 recopiés de l'EXMEMCNT de l'ARM9.
    case 0x204: return (hw.external_memory[1] & 0x7fU) | (hw.external_memory[0] & 0xff80U);
    case 0x208:
    case 0x210:
    case 0x214: return hw.irq7.read(offset);
    // VRAMSTAT (0x04000240), puis WRAMSTAT (0x04000241).
    case 0x240: return hw.vram.arm7_status() | (std::uint32_t{hw.memory.wram_control} << 8U);
    case 0x300: return hw.post_boot[1];
    case 0x304: return hw.power_control7;
    default: return 0;
    }
}

void Bus7::io_write(std::uint32_t address, std::uint32_t value, unsigned width) {
    io_write_word(address & ~3U, io_shift(address, value), io_mask(address, width));
}

void Bus7::io_write_word(std::uint32_t address, std::uint32_t value, std::uint32_t mask) {
    auto& hw = hw_;
    const auto now = hw.arm7_time();
    if (address >= 0x0400'1000U) return;
    const auto offset = address & 0xfffU;
    if (offset >= 0x100 && offset < 0x110) return hw.timers7.write(offset - 0x100, value, mask, now);
    if (offset >= 0x1a0 && offset < 0x1c0) {
        if (hw.card_arm7()) hw.card.write(offset - 0x1a0, value, mask, now, hw.scheduler, instruction());
        return;
    }
    if (offset == 0x1c0) return hw.spi.write(value, mask, now, hw.scheduler);
    if (offset >= 0x400 && offset < 0x520) return hw.sound.write(offset - 0x400, value, mask, now);
    if (offset >= 0x0b0 && offset < 0x0e0) {
        if (write_dma_register(hw.dma7, offset, value, mask, false))
            run_dma(*this, hw.dma7[(offset - 0x0b0) / 12], (offset - 0x0b0) / 12, false, hw.irq7);
        return;
    }
    if (offset == 0x138) {
        hw.rtc.write(value, mask, now);
        return hw.rtc_interrupt.check(hw.rtc, hw.scheduler, hw.irq7, now);
    }
    if (offset == 0x134) return hw.rtc_interrupt.write_rcnt(value, mask & 0xffffU);  // EXTKEYIN en lecture seule
    switch (offset) {
    case 0x004: hw.lcd.write(1, value, mask); break;
    case 0x130: hw.key_control[1] = merge(hw.key_control[1], value >> 16U, mask >> 16U) & 0xc3ffU; break;
    case 0x180: hw.ipc.write_sync(1, value, mask); break;
    case 0x184: hw.ipc.write_control(1, value, mask); break;
    case 0x188: hw.ipc.send(1, value); break;
    case 0x204: hw.external_memory[1] = merge(hw.external_memory[1], value, mask & 0x7fU); break;
    case 0x208:
    case 0x210:
    case 0x214: hw.irq7.write(offset, value, mask); break;
    case 0x300:
        hw.post_boot[1] = merge(hw.post_boot[1], value, mask & 1U);
        // HALTCNT (0x04000301), bits 6-7 : 2 = Halt, 3 = veille (comptée
        // comme Halt), 1 = mode GBA (non modélisé).
        if ((mask & 0xff00U) != 0 && ((value >> 15U) & 1U) != 0) hw.arm7_halted = true;
        break;
    case 0x304: hw.power_control7 = merge(hw.power_control7, value, mask & 3U); break;
    default: break;
    }
}

} // namespace ravenemu::nds
