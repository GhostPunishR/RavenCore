// Inclut chaque en-tête public installé et appelle les deux interfaces.
#include <ravenemu/bridge.h>
#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gba/core.hpp>
#include <ravenemu/gbc/core.hpp>
#include <ravenemu/sdk.hpp>
#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/nds/machine.hpp>
#include <ravenemu/nds/scheduler.hpp>
#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/cp15.hpp>
#include <ravenemu/nds/cpu_state.hpp>
#include <ravenemu/nds/hardware.hpp>
#include <ravenemu/nds/interrupts.hpp>
#include <ravenemu/nds/ipc.hpp>
#include <ravenemu/nds/input.hpp>
#include <ravenemu/nds/spi.hpp>
#include <ravenemu/nds/touchscreen.hpp>
#include <ravenemu/nds/firmware.hpp>
#include <ravenemu/nds/gamecard.hpp>
#include <ravenemu/nds/backup.hpp>
#include <ravenemu/nds/dma.hpp>
#include <ravenemu/nds/lcd_timing.hpp>
#include <ravenemu/nds/video.hpp>

#include <cstdio>
#include <algorithm>
#include <vector>

int main() {
    ravenemu::nds::Machine nds;
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x02000000, 0xeafffffe);
    if (nds.memory().read32(ravenemu::nds::Processor::arm7, 0x02000000) != 0xeafffffe) return 1;
    ravenemu::nds::Cpu nds_cpu{ravenemu::nds::Processor::arm9};
    nds_cpu.reset(0x02000000, false);
    nds_cpu.step(nds.memory());
    if (nds_cpu.state().registers[15] != 0x02000000 || nds_cpu.state().instructions != 1) return 1;
    if (nds_cpu.state().mode != ravenemu::nds::CpuMode::system || nds_cpu.state().cpsr() != 0xdf) return 1;
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000210, 8);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000208, 1);
    nds.memory().request_interrupt(ravenemu::nds::Processor::arm9, 8);
    if (!nds.memory().irq_pending(ravenemu::nds::Processor::arm9)) return 1;
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000214, 8);
    if (nds.memory().irq_pending(ravenemu::nds::Processor::arm9)) return 1;
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000100, 0x00c0fffe);
    nds.advance_cycles(2);
    if (ravenemu::nds::timer_clock_hz != 33513982 ||
        nds.memory().read16(ravenemu::nds::Processor::arm9, 0x04000100) != 0xfffe ||
        !nds.memory().irq_pending(ravenemu::nds::Processor::arm9)) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04000184, 0x8000);
    nds.memory().write16(ravenemu::nds::Processor::arm7, 0x04000184, 0x8000);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000188, 0x12345678);
    const auto& nds_view = nds.memory();
    if (nds_view.peek32(ravenemu::nds::Processor::arm7, 0x04100000) != 0x12345678 ||
        nds.memory().read32(ravenemu::nds::Processor::arm7, 0x04100000) != 0x12345678 ||
        nds_view.peek16(ravenemu::nds::Processor::arm7, 0x04000184) != 0x8101) return 1;
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x040000b0, 0x02000000);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x040000b4, 0x02000004);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x040000b8, 0x84000001);
    nds_cpu.step(nds.memory()); // transfert DMA, CPU suspendu
    if (nds_cpu.state().instructions != 1 || nds.memory().dma_pending(ravenemu::nds::Processor::arm9) ||
        nds_view.peek32(ravenemu::nds::Processor::arm9, 0x02000004) != 0xeafffffe) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04000004, 0x10);
    nds.advance_cycles(1534); // LCD déjà avancé de deux cycles par le test timer
    if (ravenemu::nds::LcdTiming::line_cycles != 2130 ||
        !(nds_view.peek32(ravenemu::nds::Processor::arm9, 0x04000214) & 2U)) return 1;
    nds.advance_cycles(70);
    if (!(nds_view.peek16(ravenemu::nds::Processor::arm9, 0x04000004) & 2U)) return 1;
    nds.memory().write8(ravenemu::nds::Processor::arm9, 0x04000240, 0x80);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x06800000, 0x801f);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000000, 0x00020000);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04000304, 0x8001);
    const auto nds_frame = nds.render_frame();
    if (nds_frame.upper.size() != 256 * 192 || nds_frame.upper[0] != 0x001f || nds_frame.lower[0] != 0x7fff) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04000304, 0x8203);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x05000400, 0x7c00);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04001000, 0x00010000);
    const auto nds_two_engines = nds.render_frame();
    if (nds_two_engines.upper[0] != 0x001f || nds_two_engines.lower[0] != 0x7c00) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04001050, 0x00a0); // éclaircir le backdrop B
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04001054, 16);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04001040, 0x0008);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04001044, 0x0008);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04001048, 0x0020);
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x0400104a, 0);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04001000, 0x00012000);
    const auto nds_window = nds.render_frame();
    if (nds_window.lower[0] != 0x7fff || nds_window.lower[8] != 0x7c00 || nds_window.upper[0] != 0x001f) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x04000304, 0x8201); // A 2D éteint, VRAM directe active
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x0400006c, 0x8010);
    const auto nds_master = nds.render_frame();
    if (nds.memory().read32(ravenemu::nds::Processor::arm9, 0x0400006c) != 0x8010 ||
        nds_master.upper[0] != 0 || nds_master.lower[0] != 0x7fff || nds_master.lower[8] != 0x7c00) return 1;
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x0400104c, 0xffffffff);
    if (nds.memory().read32(ravenemu::nds::Processor::arm9, 0x0400104c) != 0 ||
        nds.render_frame().lower[8] != 0x7c00) return 1; // MOSAIC en écriture seule, backdrop inchangé
    nds.memory().write8(ravenemu::nds::Processor::arm9, 0x04000243, 0x80); // D en destination LCDC
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000064, 0xa0030000); // capture VRAM A -> D
    nds.advance_cycles(2 * ravenemu::nds::LcdTiming::frame_cycles);
    if ((nds.memory().read32(ravenemu::nds::Processor::arm9, 0x04000064) & 0x80000000U) ||
        nds.memory().read16(ravenemu::nds::Processor::arm9, 0x06860000) != 0x801f) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm9, 0x0400006c, 0);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000000, 0x30000);
    for (unsigned i = 0; i < 8; ++i) nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000068, 0xfc00801f);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x04000064, 0xa2030000); // source B FIFO -> D
    nds.advance_cycles(2 * ravenemu::nds::LcdTiming::frame_cycles);
    const auto nds_fifo = nds.render_frame();
    if (nds_fifo.upper[0] != 0x001f || nds_fifo.upper[1] != 0x7c00 || nds_fifo.lower[8] != 0x7c00 ||
        nds.memory().read32(ravenemu::nds::Processor::arm9, 0x06860000) != 0xfc00801f) return 1;
    nds.memory().cp15().write(9, 1, 0, 0x0201000a);
    nds.memory().cp15().write(1, 0, 0, 0x12078);
    nds.memory().write32(ravenemu::nds::Processor::arm9, 0x02010000, 0x12345678);
    if (nds.memory().read32(ravenemu::nds::Processor::arm9, 0x02010000) != 0x12345678 ||
        nds.memory().read32(ravenemu::nds::Processor::arm7, 0x02010000) == 0x12345678 ||
        nds.memory().cp15().read(0, 0, 0) != 0x41059461) return 1;
    nds.memory().cp15().write(6, 0, 0, 0x0200002b);
    nds.memory().cp15().write(5, 0, 2, 3);
    nds.memory().cp15().write(5, 0, 3, 3);
    nds.memory().cp15().write(1, 0, 0, 0x12079);
    nds_cpu.reset(0x02000000, false);
    nds_cpu.step(nds.memory()); // b . dans la région autorisée
    if (nds_cpu.state().mode != ravenemu::nds::CpuMode::system ||
        nds_cpu.state().instructions != 1 ||
        nds.memory().cp15().permits(0x04000000, ravenemu::nds::Cp15::Access::read, true)) return 1;
    std::vector<std::uint8_t> scheduled_rom(0x20c);
    const auto scheduled_put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) scheduled_rom[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
    };
    scheduled_put(0x20,0x200); scheduled_put(0x24,0x02000000); scheduled_put(0x28,0x02000000); scheduled_put(0x2c,8);
    scheduled_put(0x30,0x208); scheduled_put(0x34,0x03800000); scheduled_put(0x38,0x03800000); scheduled_put(0x3c,4);
    scheduled_put(0x200,0xee070f90); scheduled_put(0x204,0xeafffffe); scheduled_put(0x208,0xeafffffe);
    nds.load_cartridge(ravenemu::nds::Cartridge{scheduled_rom});
    if (nds.memory().read32(ravenemu::nds::Processor::arm9,0x027ffe20) != 0x200 ||
        nds.memory().read16(ravenemu::nds::Processor::arm7,0x027ffc40) != 1 ||
        nds.cpu_state(ravenemu::nds::Processor::arm9).registers[13] != 0x03002f7c ||
        nds.cpu_state(ravenemu::nds::Processor::arm7).registers[13] != 0x0380fd80 ||
        nds.memory().cp15().read(9,1,0) != 0x0300000a) return 1;
    nds.advance_cycles(ravenemu::nds::LcdTiming::frame_cycles - 2);
    nds.run_frame();
    if (nds.scheduler_state().cycles != 2 || nds.scheduler_state().phase != ravenemu::nds::SchedulerState::Phase::arm9_first ||
        !nds.cpu_state(ravenemu::nds::Processor::arm9).waiting_for_interrupt ||
        nds.cpu_state(ravenemu::nds::Processor::arm9).instructions != 1 ||
        nds.cpu_state(ravenemu::nds::Processor::arm7).instructions != 2) return 1;
    nds.run_cycles(1);
    if (nds.scheduler_state().cycles != 3 || ravenemu::nds::arm9_clock_hz != 2 * ravenemu::nds::arm7_clock_hz) return 1;
    ravenemu::nds::InputState nds_inputs{};
    nds_inputs.pen_down = true;
    nds_inputs.lid_closed = true;
    nds.set_input(nds_inputs);
    nds.set_key(ravenemu::nds::Key::x,true);
    nds.memory().write16(ravenemu::nds::Processor::arm9,0x04000132,0x4001);
    nds.memory().write32(ravenemu::nds::Processor::arm9,0x04000210,0x1000);
    nds.memory().write32(ravenemu::nds::Processor::arm9,0x04000208,1);
    nds.set_key(ravenemu::nds::Key::a,true);
    nds.run_cycles(1);
    if (nds.cpu_state(ravenemu::nds::Processor::arm9).waiting_for_interrupt ||
        nds.memory().read16(ravenemu::nds::Processor::arm7,0x04000130) != 0x3fe ||
        nds.memory().read16(ravenemu::nds::Processor::arm7,0x04000136) != 0xbe ||
        nds.input_state().pressed_keys != 0x401) return 1;
    nds_inputs = nds.input_state();
    nds_inputs.touch = {0xabc,0x321,0x456,0x789};
    nds.set_input(nds_inputs);
    nds.memory().write16(ravenemu::nds::Processor::arm7,0x040001c0,0x8a01);
    const auto nds_spi_byte = [&](std::uint8_t value) {
        nds.memory().write8(ravenemu::nds::Processor::arm7,0x040001c2,value);
        nds.advance_cycles(128);
        return nds.memory().read16(ravenemu::nds::Processor::arm7,0x040001c2);
    };
    if (nds_spi_byte(0xd0) != 0 || nds_spi_byte(0) != 0x55 || nds_spi_byte(0) != 0xe0 ||
        nds.memory().read16(ravenemu::nds::Processor::arm7,0x040001c0) != 0x8a01) return 1;
    // Image synthétique avec réglages et étalonnage ; aucun firmware tiers.
    std::vector<std::uint8_t> nds_firmware(0x40000,0xff);
    const auto firmware_put16 = [&](std::size_t offset, std::uint16_t value) {
        nds_firmware[offset] = static_cast<std::uint8_t>(value);
        nds_firmware[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
    };
    firmware_put16(0x20,0x7fc0);
    std::fill_n(nds_firmware.begin() + 0x3fe00,0x70,0);
    firmware_put16(0x3fe00,5); nds_firmware[0x3fe02] = 3;
    firmware_put16(0x3fe58,0x200); firmware_put16(0x3fe5a,0x300);
    nds_firmware[0x3fe5c] = 33; nds_firmware[0x3fe5d] = 25;
    firmware_put16(0x3fe5e,0xe00); firmware_put16(0x3fe60,0xd00);
    nds_firmware[0x3fe62] = 225; nds_firmware[0x3fe63] = 169;
    firmware_put16(0x3fe70,10); firmware_put16(0x3fe72,0x9b43);
    nds.load_firmware(nds_firmware);
    nds.load_cartridge(ravenemu::nds::Cartridge{scheduled_rom});
    nds.set_touch_position(128,96);
    if (nds.input_state().touch != ravenemu::nds::TouchSample{0x800,0x800} ||
        nds.memory().read8(ravenemu::nds::Processor::arm9,ravenemu::nds::firmware_settings_address + 2) != 3 ||
        !nds.memory().firmware().settings()) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm7,0x040001c0,0x8900);
    if (nds_spi_byte(0x9f) != 0xff || nds_spi_byte(0) != 0x20 || nds_spi_byte(0) != 0x40 || nds_spi_byte(0) != 0x12) return 1;
    const auto nds_firmware_before = nds.snapshot_firmware();
    if (nds.firmware_dirty()) return 1;
    nds.memory().write16(ravenemu::nds::Processor::arm7,0x040001c0,0);
    nds.memory().write16(ravenemu::nds::Processor::arm7,0x040001c0,0x8100);
    static_cast<void>(nds_spi_byte(6)); // WREN, CS haut après la commande
    nds.memory().write16(ravenemu::nds::Processor::arm7,0x040001c0,0x8900);
    for (const auto value : {0x0a,1,0x20,0}) static_cast<void>(nds_spi_byte(static_cast<std::uint8_t>(value)));
    nds.memory().write16(ravenemu::nds::Processor::arm7,0x040001c0,0x8100);
    static_cast<void>(nds_spi_byte(0xa5));
    if (!nds.memory().firmware().busy()) return 1;
    nds.advance_cycles(368654);
    if (!nds.firmware_dirty() || nds.memory().firmware().bytes()[0x12000] != 0xa5) return 1;
    const auto nds_firmware_after = nds.snapshot_firmware();
    nds.acknowledge_firmware_saved(nds_firmware_before.generation);
    if (!nds.firmware_dirty()) return 1;
    nds.acknowledge_firmware_saved(nds_firmware_after.generation);
    if (nds.firmware_dirty()) return 1;
    ravenemu::nds::Machine nds_restored;
    nds_restored.load_firmware(nds_firmware_after.data);
    if (nds_restored.memory().firmware().bytes()[0x12000] != 0xa5 || nds_restored.firmware_dirty()) return 1;
    scheduled_rom.resize(0x9000,0xff);
    scheduled_put(0x8000,0x12345678);
    nds.load_cartridge(ravenemu::nds::Cartridge{scheduled_rom});
    const auto card_cpu = ravenemu::nds::Processor::arm9;
    nds.memory().write32(card_cpu,0x040000b0,0x04100010);
    nds.memory().write32(card_cpu,0x040000b4,0x02003000);
    nds.memory().write32(card_cpu,0x040000b8,0xed000001); // DMA cartouche, source fixe, un mot
    nds.memory().write16(card_cpu,0x040001a0,0xc000);
    nds.memory().write32(card_cpu,0x040001a8,0x800000b7);
    nds.memory().write32(card_cpu,0x040001ac,0);
    nds.memory().write32(card_cpu,0x040001a4,0xa7000000);
    nds.advance_cycles(60);
    if (!nds.memory().dma_pending(card_cpu) || !nds.memory().step_dma(card_cpu) ||
        nds.memory().read32(card_cpu,0x02003000) != 0x12345678 ||
        (nds.memory().peek32(card_cpu,0x040001a4) & 0x80800000U)) return 1;
    nds.load_backup(ravenemu::nds::BackupType::eeprom_512);
    const auto backup_byte = [&](std::uint8_t value, bool last) {
        nds.memory().write16(card_cpu,0x040001a0,last ? 0xa000 : 0xa040);
        nds.memory().write8(card_cpu,0x040001a2,value);
        nds.advance_cycles(64);
        return nds.memory().read8(card_cpu,0x040001a2);
    };
    static_cast<void>(backup_byte(6,true));
    static_cast<void>(backup_byte(0x0a,false));
    static_cast<void>(backup_byte(0x23,false));
    static_cast<void>(backup_byte(0x5a,true));
    nds.advance_cycles(167570);
    const auto backup_saved = nds.snapshot_backup();
    if (!nds.backup_dirty() || backup_saved.data.size() != 512 || backup_saved.data[0x123] != 0x5a) return 1;
    nds.acknowledge_backup_saved(backup_saved.generation);
    if (nds.backup_dirty()) return 1;
    nds_restored.load_cartridge(ravenemu::nds::Cartridge{scheduled_rom});
    nds_restored.load_backup(ravenemu::nds::BackupType::eeprom_512,backup_saved.data,backup_saved.status);
    if (nds_restored.snapshot_backup().data != backup_saved.data || nds_restored.backup_dirty()) return 1;
    nds.load_backup(ravenemu::nds::BackupType::flash_1m);
    static_cast<void>(backup_byte(0x9f,false));
    if (backup_byte(0,false) != 0x20 || backup_byte(0,false) != 0x40 || backup_byte(0,true) != 0x14) return 1;
    static_cast<void>(backup_byte(6,true));
    for (const auto value : {0x0a,0x0f,0xff,0xff}) static_cast<void>(backup_byte(static_cast<std::uint8_t>(value),false));
    static_cast<void>(backup_byte(0xa5,true));
    nds.advance_cycles(368654);
    const auto flash_saved = nds.snapshot_backup();
    if (!nds.backup_dirty() || flash_saved.data.size() != 0x100000 || flash_saved.data[0xfffff] != 0xa5) return 1;
    nds_restored.load_backup(ravenemu::nds::BackupType::flash_1m,flash_saved.data);
    if (nds_restored.snapshot_backup().data != flash_saved.data || nds_restored.backup_dirty()) return 1;
    for (const auto value : {3,0x0f,0xff,0xff}) static_cast<void>(backup_byte(static_cast<std::uint8_t>(value),false));
    if (backup_byte(0,true) != 0xa5) return 1;
    nds.load_backup(ravenemu::nds::BackupType::fram_32k);
    static_cast<void>(backup_byte(6,true));
    for (const auto value : {2,0x7f,0xff}) static_cast<void>(backup_byte(static_cast<std::uint8_t>(value),false));
    static_cast<void>(backup_byte(0x12,false));
    // La FRAM est déjà modifiée avant CS haut, sans cycle interne WIP.
    if (nds.snapshot_backup().data[0x7fff] != 0x12 || nds.memory().backup().busy()) return 1;
    static_cast<void>(backup_byte(0x34,true));
    const auto fram_saved = nds.snapshot_backup();
    if (fram_saved.data.size() != 0x8000 || fram_saved.data[0] != 0x34 || !nds.backup_dirty()) return 1;
    nds.acknowledge_backup_saved(fram_saved.generation);
    nds_restored.load_backup(ravenemu::nds::BackupType::fram_32k,fram_saved.data,fram_saved.status);
    if (nds.backup_dirty() || nds_restored.snapshot_backup().data != fram_saved.data || nds_restored.backup_dirty()) return 1;
    if (nds.memory().read8(card_cpu,0x04000300) != 1 ||
        nds.memory().read8(ravenemu::nds::Processor::arm7,0x04000300) != 1) return 1;
    const auto before_halt = nds.cpu_state(ravenemu::nds::Processor::arm7).instructions;
    nds.memory().write8(ravenemu::nds::Processor::arm7,0x04000301,0x80); nds.run_cycles(1);
    if (!nds.cpu_state(ravenemu::nds::Processor::arm7).waiting_for_interrupt) return 1;
    nds.memory().write32(ravenemu::nds::Processor::arm7,0x04000210,8);
    nds.memory().request_interrupt(ravenemu::nds::Processor::arm7,8); nds.run_cycles(1);
    if (nds.cpu_state(ravenemu::nds::Processor::arm7).waiting_for_interrupt ||
        nds.cpu_state(ravenemu::nds::Processor::arm7).instructions != before_halt ||
        nds.memory().irq_pending(ravenemu::nds::Processor::arm7)) return 1;
    // Le nouveau composant NDS doit aussi être exporté par le paquet installé.
    try {
        const ravenemu::nds::Cartridge invalid{std::span<const std::uint8_t>{}};
        return 1;
    } catch (const ravenemu::RomLoadError&) {
    }
    using ravenemu::sdk::System;
    for (const auto system : {System::game_boy, System::game_boy_color, System::game_boy_advance}) {
        if (!ravenemu::sdk::make_core(system)) return 1;
    }
    if (ravenemu::gba::make_core()->console() != ravenemu::Console::game_boy_advance) return 1;

    for (const int system : {RAVENEMU_GB, RAVENEMU_GBC, RAVENEMU_GBA}) {
        auto* handle = ravenemu_create(system);
        if (handle == nullptr) {
            std::fprintf(stderr, "ravenemu_create : %s\n", ravenemu_last_error());
            return 1;
        }
        ravenemu_destroy(handle);
    }
    std::puts("paquet RavenCore consommé avec succès");
    return 0;
}
