// Inclut chaque en-tête public installé et appelle les deux interfaces.
#include <ravenemu/bridge.h>
#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gba/core.hpp>
#include <ravenemu/gbc/core.hpp>
#include <ravenemu/sdk.hpp>
#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/nds/machine.hpp>
#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/cpu_state.hpp>
#include <ravenemu/nds/hardware.hpp>
#include <ravenemu/nds/interrupts.hpp>
#include <ravenemu/nds/ipc.hpp>
#include <ravenemu/nds/dma.hpp>
#include <ravenemu/nds/lcd_timing.hpp>
#include <ravenemu/nds/video.hpp>

#include <cstdio>

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
