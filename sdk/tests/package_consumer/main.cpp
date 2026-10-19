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
