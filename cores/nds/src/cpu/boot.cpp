#include <ravenemu/nds/cpu.hpp>

namespace ravenemu::nds {

void Cpu::reset_direct_boot(std::uint32_t address, bool thumb) {
    reset(address,thumb);
    const bool arm9 = processor_ == Processor::arm9;
    // GBATEK, BIOS RAM Usage : valeurs de boot cartouche, pas de SWI SoftReset.
    state_.set_cpsr(0xd2); // IRQ, I/F masqués
    state_.registers[13] = arm9 ? 0x03003f80 : 0x0380ff80;
    state_.set_cpsr(0xd3); // Supervisor
    state_.registers[13] = arm9 ? 0x03003fc0 : 0x0380ffc0;
    state_.set_cpsr(0xdfU | (thumb ? 0x20U : 0U)); // System
    state_.registers[13] = arm9 ? 0x03002f7c : 0x0380fd80;
    const auto entry = address | (thumb ? 1U : 0U);
    state_.registers[12] = state_.registers[14] = entry;
}

} // namespace ravenemu::nds
