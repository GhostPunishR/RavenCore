#include "memory/bus.hpp"

#include "ppu/ppu.hpp"
#include "timers/timers.hpp"
#include "dma/dma_controller.hpp"
#include "apu/apu.hpp"
#include "serial/serial.hpp"


namespace ravenemu::gba {

Bus::Bus(Cartridge& value) : cartridge(value) {
    auto* save = cartridge.save();
    eeprom_ = dynamic_cast<Eeprom*>(save);
    if (dynamic_cast<Sram*>(save) != nullptr || dynamic_cast<Flash*>(save) != nullptr) byte_save_ = save;
    // Sans puce de sauvegarde, le bus de la cartouche se lit 0xFF.
    fallback_sram.fill(0xffU);
    reset_affine_matrices();
}

std::uint8_t* Bus::direct_write(std::uint32_t address) noexcept {
    switch (address >> 24U) {
    case 0x02: return ewram.data() + (address & 0x3ffffU);
    case 0x03: return iwram.data() + (address & 0x7fffU);
    case 0x05: return palette.data() + (address & 0x3ffU);
    case 0x06: return vram.data() + vram_offset(i32(address));
    case 0x07: return oam.data() + (address & 0x3ffU);
    default: return nullptr;
    }
}

bool Bus::eeprom_window(std::int32_t address) const noexcept {
    return eeprom_ != nullptr && (u32(address) >> 24U) == 0x0dU;
}

bool Bus::gpio_register(std::int32_t address) const noexcept {
    const auto region = (u32(address) >> 24U) & 0xffU;
    return region >= 0x08U && region <= 0x0dU && Gpio::covers(rom_offset(address));
}

namespace {

constexpr int unreadable = -1;

// Bits que le CPU relit dans un demi-mot d'E/S, d'après GBATEK et, pour les
// bits inutilisés qui se lisent 0, la suite de tests de mGBA lancée sur
// console. `unreadable` : registre en écriture seule ou emplacement inutilisé.
constexpr int io_read_mask(int offset) noexcept {
    switch (offset) {
    case 0x004: return 0xff3f;                          // DISPSTAT : bits 6 et 7 à 0
    case 0x006: return 0x00ff;                          // VCOUNT
    case 0x008: case 0x00a: return 0xdfff;              // BG0CNT, BG1CNT : sans bit 13
    case 0x048: case 0x04a: return 0x3f3f;              // WININ, WINOUT
    case 0x050: return 0x3fff;                          // BLDCNT
    case 0x052: return 0x1f1f;                          // BLDALPHA
    case 0x060: return 0x007f;                          // SOUND1CNT_L
    case 0x062: case 0x068: return 0xffc0;              // durée en écriture seule
    case 0x064: case 0x06c: case 0x074: return 0x4000;  // fréquence et départ en écriture seule
    case 0x070: return 0x00e0;                          // SOUND3CNT_L
    case 0x072: return 0xe000;                          // SOUND3CNT_H : durée en écriture seule
    case 0x078: return 0xff00;                          // SOUND4CNT_L : durée en écriture seule
    case 0x07c: return 0x40ff;                          // SOUND4CNT_H : départ en écriture seule
    case 0x080: return 0xff77;                          // SOUNDCNT_L
    case 0x082: return 0x770f;                          // SOUNDCNT_H : remises à zéro en écriture seule
    case 0x084: return 0x0080;                          // SOUNDCNT_X, sans APU
    case 0x0ba: case 0x0c6: case 0x0d2: return 0xf7e0;  // DMA0CNT_H à DMA2CNT_H
    case 0x0de: return 0xffe0;                          // DMA3CNT_H
    case 0x140: return 0x0040;                          // JOYCNT : drapeaux du JOY Bus, non émulé
    case 0x150: case 0x152: case 0x154: case 0x156: return 0;  // JOY_RECV, JOY_TRANS : se lisent 0
    case 0x204: return 0x7fff;                          // WAITCNT : bit 15 à 0, cartouche GBA
    case 0x208: return 0x0001;                          // IME
    case 0x300: return 0x00ff;                          // POSTFLG ; HALTCNT en écriture seule
    case 0x066: case 0x06a: case 0x06e: case 0x076: case 0x07a: case 0x07e: case 0x086: case 0x08a:
    case 0x08c: case 0x08e: case 0x206:
        return unreadable;
    default: break;
    }
    const auto within = [offset](int first, int last) { return offset >= first && offset <= last; };
    if (within(0x010, 0x046) || within(0x04c, 0x05e) || within(0x0a0, 0x0de) || within(0x0e0, 0x0fe) ||
        within(0x110, 0x11e) || within(0x12c, 0x12e) || within(0x136, 0x13e) || within(0x142, 0x14e) ||
        within(0x15a, 0x1fe) || within(0x20a, 0x2fe) || within(0x302, 0x3fe))
        return unreadable;
    return 0xffff;
}

} // namespace

int Bus::read_io(int offset) {
    const auto half = offset & ~1;
    const auto value = read_io_half(half);
    if (value != unreadable) return (value >> ((offset & 1) * 8)) & 0xff;
    // GBATEK, « Reading from Unused or Write-Only I/O Ports » : le bus ouvert
    // quand le mot de 32 bits entier est illisible, 0 quand l'autre moitié se lit.
    return io_read_mask(half ^ 2) == unreadable ? open_bus(u32(offset)) : 0;
}

int Bus::read_io_half(int offset) {
    switch (offset) {
    case 0x004: if (ppu) return ppu_dispstat_low(ppu) | io[0x005] << 8; break;
    case 0x006: if (ppu) return ppu_vcount(ppu); break;
    case 0x084: if (apu) return apu->sound_status(); break;
    case 0x120: case 0x122: case 0x124: case 0x126: case 0x128: case 0x12a: case 0x134:
        if (serial) return serial->read_register(offset);
        break;
    case 0x130: return keypad.input();
    case 0x200: if (interrupts) return interrupts->enable & 0xffff; break;
    case 0x202: if (interrupts) return interrupts->flags & 0xffff; break;
    case 0x208: if (interrupts) return interrupts->master_enable ? 1 : 0; break;
    default:
        if (timers && offset >= 0x100 && offset <= 0x10e) return read_timer_half(offset);
        break;
    }
    const auto mask = io_read_mask(offset);
    return mask == unreadable ? unreadable : io_half(offset) & mask;
}

int Bus::read_timer_half(int offset) {
    const auto timer = (offset - 0x100) / 4;
    return (offset & 2) == 0 ? timers->counter(timer) : timers->control(timer);
}

int Bus::read8_raw(std::int32_t address) {
    const auto raw = u32(address);
    const auto region = (raw >> 24U) & 0xffU;
    switch (region) {
    case 0x00:
        if (raw >= 0x4000U) { diagnostics.unsupported_access(address); return open_bus(raw); }
        if (!executing_bios) return static_cast<int>((bios_latch >> ((raw & 3U) * 8U)) & 0xffU);
        return bios[raw];
    case 0x02: return ewram[raw & 0x3ffffU];
    case 0x03: return iwram[raw & 0x7fffU];
    case 0x04:
        if (io_mapped(raw)) return read_io(static_cast<int>(raw & 0x3ffU));
        // Contrôle de la mémoire interne, répété tous les 64 Kio : sa valeur
        // au démarrage, non modifiable ici. Le reste de la région n'est pas câblé.
        if ((raw & 0xfffcU) == 0x0800U) return static_cast<int>((memory_control_reset >> ((raw & 3U) * 8U)) & 0xffU);
        diagnostics.unsupported_access(address);
        return open_bus(raw);
    case 0x05: return palette[raw & 0x3ffU];
    case 0x06: return vram[static_cast<std::size_t>(vram_offset(address))];
    case 0x07: return oam[raw & 0x3ffU];
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: {
        auto* port = cartridge.gpio();
        if (port && port->readable() && gpio_register(address)) {
            const auto offset = rom_offset(address);
            const auto half = port->read16(offset & ~1);
            return (offset & 1) == 0 ? half & 0xff : (half >> 8) & 0xff;
        }
        return cartridge.read8(rom_offset(address));
    }
    case 0x0e: case 0x0f: {
        if (auto* tilt = cartridge.tilt(); tilt && TiltSensor::reads(static_cast<int>(raw & 0xffffU)))
            return tilt->read(static_cast<int>(raw & 0xffffU));
        if (byte_save_) return byte_save_->read(static_cast<int>(raw));
        // Une cartouche à EEPROM n'a rien sur ce bus : il se lit 0xFF. Les jeux
        // Classic NES Series le vérifient et refusent une cartouche où l'écriture
        // y reste (« GAME PAK ERROR »).
        if (eeprom_) return 0xff;
        return fallback_sram[raw & 0xffffU];
    }
    default:
        diagnostics.unsupported_access(address);
        return open_bus(raw);
    }
}

int Bus::read8(std::int32_t address) {
    account(address, 1);
    sync_io(address, false);
    return read8_raw(address);
}

int Bus::read16(std::int32_t address) {
    if (save_bus(address)) { account(address, 2); return read8_raw(address) * 0x0101; }
    const auto aligned = i32(u32(address) & ~1U);
    account(aligned, 2);
    sync_io(aligned, false);
    if (eeprom_window(aligned)) return eeprom_->read(0);
    if (const auto* data = direct_read(u32(aligned), 2)) return static_cast<int>(load_le(data, 2));
    if (auto* port = cartridge.gpio(); port && port->readable() && gpio_register(aligned)) {
        return port->read16(rom_offset(aligned));
    }
    return read8_raw(aligned) | read8_raw(add32(aligned, 1)) << 8;
}

std::int32_t Bus::read32(std::int32_t address) {
    if (save_bus(address)) { account(address, 4); return i32(u32(read8_raw(address)) * 0x0101'0101U); }
    const auto aligned = i32(u32(address) & ~3U);
    account(aligned, 4);
    sync_io(aligned, false);
    if (const auto* data = direct_read(u32(aligned), 4)) return i32(load_le(data, 4));
    auto result = static_cast<std::uint32_t>(read8_raw(aligned));
    result |= static_cast<std::uint32_t>(read8_raw(add32(aligned, 1))) << 8U;
    result |= static_cast<std::uint32_t>(read8_raw(add32(aligned, 2))) << 16U;
    result |= static_cast<std::uint32_t>(read8_raw(add32(aligned, 3))) << 24U;
    return i32(result);
}

void Bus::track_bios_fetch(std::int32_t address) noexcept {
    // Le pipeline lit PC + 8 : le dernier mot préchargé depuis le BIOS est
    // exactement ce que rend ensuite une lecture du BIOS depuis l'extérieur.
    executing_bios = (u32(address) >> 24U) == 0;
    if (!executing_bios) return;
    const auto word = u32(address) & 0x3ffcU;
    bios_latch = static_cast<std::uint32_t>(bios[word]) | static_cast<std::uint32_t>(bios[word + 1]) << 8U |
                 static_cast<std::uint32_t>(bios[word + 2]) << 16U | static_cast<std::uint32_t>(bios[word + 3]) << 24U;
}

int Bus::fetch16_slow(std::int32_t aligned) {
    track_bios_fetch(aligned);
    return read8_raw(aligned) | read8_raw(add32(aligned, 1)) << 8;
}

std::int32_t Bus::fetch32_slow(std::int32_t aligned) {
    track_bios_fetch(aligned);
    auto result = static_cast<std::uint32_t>(read8_raw(aligned));
    result |= static_cast<std::uint32_t>(read8_raw(add32(aligned, 1))) << 8U;
    result |= static_cast<std::uint32_t>(read8_raw(add32(aligned, 2))) << 16U;
    result |= static_cast<std::uint32_t>(read8_raw(add32(aligned, 3))) << 24U;
    return i32(result);
}

void Bus::write8_raw(std::int32_t address, int value) {
    const auto raw = u32(address);
    const auto region = (raw >> 24U) & 0xffU;
    const auto byte = static_cast<std::uint8_t>(value);
    switch (region) {
    case 0x00: case 0x01: break;
    case 0x02: ewram[raw & 0x3ffffU] = byte; break;
    case 0x03: iwram[raw & 0x7fffU] = byte; break;
    case 0x04: if (io_mapped(raw)) io[raw & 0x3ffU] = byte; break;
    case 0x05: palette[raw & 0x3ffU] = byte; break;
    case 0x06: vram[static_cast<std::size_t>(vram_offset(address))] = byte; break;
    case 0x07: oam[raw & 0x3ffU] = byte; break;
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        // Le bus ROM/GPIO n'accepte pas STRB : seuls les transferts 16/32 bits
        // pilotent les broches (GBATEK, GBA Cart I/O Port).
        break;
    case 0x0e: case 0x0f:
        if (auto* tilt = cartridge.tilt(); tilt && TiltSensor::writes(static_cast<int>(raw & 0xffffU)))
            tilt->write(static_cast<int>(raw & 0xffffU), value);
        else if (byte_save_) byte_save_->write(static_cast<int>(raw), value);
        else if (!eeprom_) fallback_sram[raw & 0xffffU] = byte;
        break;
    default: diagnostics.unsupported_access(address); break;
    }
}

void Bus::write8(std::int32_t address, int value) {
    account(address, 1);
    sync_io(address, true);
    const auto region = (u32(address) >> 24U) & 0xffU;
    if (region == 0x05U || region == 0x06U) {
        const auto aligned = i32(u32(address) & ~1U);
        write8_raw(aligned, value); write8_raw(add32(aligned, 1), value);
    } else if (region == 0x07U) {
        return;
    } else {
        write8_raw(address, value);
        if (region == 0x04U && io_mapped(u32(address))) propagate_io_write(static_cast<int>(u32(address) & 0x3ffU), value, 1);
    }
}

void Bus::write16_raw(std::int32_t address, int value) {
    if (eeprom_window(address)) { eeprom_->write(0, value); return; }
    if (auto* port = cartridge.gpio(); port && gpio_register(address)) {
        port->write16(rom_offset(address), value & 0xffff); return;
    }
    if (auto* data = direct_write(u32(address))) { store_le(data, u32(value), 2); return; }
    write8_raw(address, value); write8_raw(add32(address, 1), value >> 8);
    if ((u32(address) >> 24U) == 0x04U && io_mapped(u32(address)))
        propagate_io_write(static_cast<int>(u32(address) & 0x3ffU), value, 2);
}

void Bus::write16(std::int32_t address, int value) {
    if (save_bus(address)) {
        account(address, 2); write8_raw(address, static_cast<int>(u32(value) >> ((u32(address) & 1U) * 8U)));
        return;
    }
    const auto aligned = i32(u32(address) & ~1U);
    account(aligned, 2); sync_io(aligned, true); write16_raw(aligned, value);
}

void Bus::write32(std::int32_t address, std::int32_t value) {
    if (save_bus(address)) {
        account(address, 4); write8_raw(address, static_cast<int>(u32(value) >> ((u32(address) & 3U) * 8U)));
        return;
    }
    const auto aligned = i32(u32(address) & ~3U);
    account(aligned, 4);
    sync_io(aligned, true);
    if ((u32(aligned) >> 24U) == 0x04U) {
        if (!io_mapped(u32(aligned))) return;
        const auto offset = static_cast<int>(u32(aligned) & 0x3ffU);
        for (unsigned index = 0; index < 4; ++index) {
            io[static_cast<std::size_t>(offset) + index] = static_cast<std::uint8_t>(u32(value) >> (index * 8U));
        }
        propagate_io_write(offset, value, 4);
    } else if (auto* data = direct_write(u32(aligned))) {
        store_le(data, u32(value), 4);
    } else {
        write16_raw(aligned, static_cast<int>(u32(value) & 0xffffU));
        write16_raw(add32(aligned, 2), static_cast<int>(u32(value) >> 16U));
    }
}

namespace {
[[nodiscard]] bool cheat_target(std::uint32_t address) noexcept {
    const auto region = address >> 24U;
    return region >= 0x02U && region <= 0x07U;
}
} // namespace

std::uint32_t Bus::cheat_read(std::uint32_t address, int size) {
    if (!cheat_target(address)) return 0;
    const auto wait = wait_cycles;
    const auto sequential = next_sequential_address;
    const auto prefetch = prefetch_;
    const auto at = i32(address);
    const auto value = size == 1 ? u32(read8(at)) : size == 2 ? u32(read16(at)) : u32(read32(at));
    wait_cycles = wait;
    next_sequential_address = sequential;
    prefetch_ = prefetch;
    return value;
}

void Bus::cheat_write(std::uint32_t address, int size, std::uint32_t value) {
    if (!cheat_target(address)) return;
    const auto wait = wait_cycles;
    const auto sequential = next_sequential_address;
    const auto prefetch = prefetch_;
    const auto at = i32(address);
    if (size == 1) write8(at, static_cast<int>(value & 0xffU));
    else if (size == 2) write16(at, static_cast<int>(value & 0xffffU));
    else write32(at, i32(value));
    wait_cycles = wait;
    next_sequential_address = sequential;
    prefetch_ = prefetch;
}

int Bus::io_half(int offset) const noexcept {
    return io[static_cast<std::size_t>(offset)] | io[static_cast<std::size_t>(offset + 1)] << 8;
}

void Bus::write_io_half(int offset, int value) noexcept {
    io[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(value);
    io[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>(value >> 8);
}

void Bus::propagate_io_write(int offset, std::int32_t value, int bytes) {
    if (offset <= 0x301 && offset + bytes > 0x301 && io_halt) io_halt(io_sync_context);
    if (offset >= 0x0a0 && offset <= 0x0a7) {
        if (apu) apu->push_fifo(offset < 0x0a4 ? 0 : 1, value, bytes);
        return;
    }
    auto reg = offset & ~1;
    const auto end = offset + bytes;
    while (reg < end) { handle_io_write(reg, io_half(reg)); reg += 2; }
}

void Bus::handle_io_write(int offset, int value) {
    if (offset >= 0x020 && offset <= 0x027) diagnostics.bg2_matrix_write();
    else if (offset >= 0x028 && offset <= 0x02f) { diagnostics.bg2_reference_write(); ppu_affine_reference_write(ppu, offset); }
    else if (offset >= 0x038 && offset <= 0x03f) ppu_affine_reference_write(ppu, offset);
    else if (offset == 0x200 && interrupts) interrupts->set_enable(value);
    else if (offset == 0x202 && interrupts) interrupts->acknowledge(value);
    else if (offset == 0x208 && interrupts) interrupts->set_master_enable((value & 1) != 0);
    else if (offset == 0x204) {
        timing.set_wait_control(value);
        close_fetch_window();
        if (!timing.prefetch_enabled()) prefetch_stop();
        else prefetch_.halfword_cycles = timing.sequential_halfword_cycles(prefetch_.head);
    }
    else if (timers && (offset == 0x100 || offset == 0x104 || offset == 0x108 || offset == 0x10c)) {
        timers->reload_write((offset - 0x100) / 4, value);
    } else if (timers && (offset == 0x102 || offset == 0x106 || offset == 0x10a || offset == 0x10e)) {
        timers->control_write((offset - 0x100) / 4, value);
    } else if (dma && (offset == 0x0ba || offset == 0x0c6 || offset == 0x0d2 || offset == 0x0de)) {
        const auto channel = offset == 0x0ba ? 0 : offset == 0x0c6 ? 1 : offset == 0x0d2 ? 2 : 3;
        dma_control_write(dma, channel, value);
    } else if (offset == 0x128) {
        serial_control_write(serial, value);
    } else if (offset == 0x134) {
        serial_rcnt_write(serial);
    } else if (apu && offset >= 0x060 && offset <= 0x09f) {
        // Son éteint (SOUNDCNT_X bit 7 à 0), les registres du PSG, 0x60 à 0x81,
        // restent à zéro et ignorent les écritures (GBATEK, « SOUNDCNT_X »).
        if (offset < 0x082 && !apu->master_enable) {
            write_io_half(offset, 0);
            return;
        }
        apu->write_register(offset, value);
        if (offset == 0x084 && !apu->master_enable) std::fill(io.begin() + 0x060, io.begin() + 0x082, std::uint8_t{0});
    }
}

void Bus::reset_affine_matrices() {
    for (const auto base : {0x020, 0x030}) {
        write_io_half(base, 0x0100); write_io_half(base + 2, 0);
        write_io_half(base + 4, 0); write_io_half(base + 6, 0x0100);
    }
}

void Bus::sync_timing_from_io() { timing.set_wait_control(io_half(0x204)); close_fetch_window(); }

} // namespace ravenemu::gba
