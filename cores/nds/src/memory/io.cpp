#include <ravenemu/nds/memory.hpp>

namespace ravenemu::nds {

InterruptController& Memory::controller(Processor cpu) noexcept {
    return cpu == Processor::arm9 ? arm9_interrupts_ : arm7_interrupts_;
}

const InterruptController& Memory::controller(Processor cpu) const noexcept {
    return cpu == Processor::arm9 ? arm9_interrupts_ : arm7_interrupts_;
}

void Memory::request_interrupt(Processor cpu, std::uint32_t sources) noexcept { controller(cpu).request(sources); }
bool Memory::irq_pending(Processor cpu) const noexcept { return controller(cpu).pending(); }

std::optional<std::uint8_t> Memory::io_byte(Processor cpu, std::uint32_t address) const noexcept {
    if (address >= 0x04000100 && address <= 0x0400010f)
        return timer_byte(cpu, address - 0x04000100U);
    const auto& irq = controller(cpu);
    if (address >= 0x04000208 && address <= 0x0400020b)
        return static_cast<std::uint8_t>(address == 0x04000208 && irq.master_enable());
    if (address >= 0x04000210 && address <= 0x04000213)
        return static_cast<std::uint8_t>(irq.enable() >> ((address - 0x04000210U) * 8U));
    if (address >= 0x04000214 && address <= 0x04000217)
        return static_cast<std::uint8_t>(irq.flags() >> ((address - 0x04000214U) * 8U));
    if ((cpu == Processor::arm9 && address == 0x04000247) ||
        (cpu == Processor::arm7 && address == 0x04000241)) return wram_control_;
    return std::nullopt;
}

void Memory::write_io_byte(Processor cpu, std::uint32_t address, std::uint8_t value) noexcept {
    if (address >= 0x04000100 && address <= 0x0400010f) {
        write_timer_byte(cpu, address - 0x04000100U, value);
        return;
    }
    auto& irq = controller(cpu);
    if (address == 0x04000208) irq.set_master_enable((value & 1U) != 0);
    else if (address >= 0x04000210 && address <= 0x04000213) {
        const auto shift = (address - 0x04000210U) * 8U;
        irq.set_enable((irq.enable() & ~(0xffU << shift)) | (static_cast<std::uint32_t>(value) << shift));
    } else if (address >= 0x04000214 && address <= 0x04000217)
        irq.acknowledge(static_cast<std::uint32_t>(value) << ((address - 0x04000214U) * 8U));
    else if (cpu == Processor::arm9 && address == 0x04000247) set_wram_control(value);
    // Octets réservés d'IME et WRAMSTAT ARM7 : écritures ignorées.
}

} // namespace ravenemu::nds
