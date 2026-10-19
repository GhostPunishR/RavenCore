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

void Memory::apply_ipc_interrupts(Ipc::Interrupts sources) noexcept {
    arm9_interrupts_.request(sources.arm9);
    arm7_interrupts_.request(sources.arm7);
}

std::optional<std::uint8_t> Memory::io_byte(Processor cpu, std::uint32_t address) const noexcept {
    if (address == 0x04000004 || address == 0x04000005)
        return static_cast<std::uint8_t>(lcd_.status(cpu) >> ((address & 1U) * 8U));
    if (address == 0x04000006 || address == 0x04000007)
        return static_cast<std::uint8_t>(lcd_.vcount() >> ((address & 1U) * 8U));
    if (address >= 0x040000b0 && address <= 0x040000df)
        return dma(cpu).read_byte(address - 0x040000b0U);
    if (cpu == Processor::arm9 && address >= 0x040000e0 && address <= 0x040000ef)
        return dma_fill_[address - 0x040000e0U];
    if (address >= 0x04000100 && address <= 0x0400010f)
        return timer_byte(cpu, address - 0x04000100U);
    if (address >= 0x04000180 && address <= 0x04000183)
        return static_cast<std::uint8_t>(static_cast<std::uint32_t>(ipc_.sync(cpu)) >> ((address - 0x04000180U) * 8U));
    if (address >= 0x04000184 && address <= 0x04000187)
        return static_cast<std::uint8_t>(static_cast<std::uint32_t>(ipc_.control(cpu)) >> ((address - 0x04000184U) * 8U));
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
    if (address >= 0x04000004 && address <= 0x04000007) {
        if (address <= 0x04000005) {
            const auto shift = (address & 1U) * 8U;
            lcd_.write_status(cpu, static_cast<std::uint16_t>(static_cast<unsigned>(value) << shift),
                              static_cast<std::uint16_t>(0xffU << shift));
        }
        return; // VCOUNT : inspection seule dans ce premier modèle LCD
    }
    if (address >= 0x040000b0 && address <= 0x040000df) {
        dma(cpu).write_byte(address - 0x040000b0U, value);
        return;
    }
    if (cpu == Processor::arm9 && address >= 0x040000e0 && address <= 0x040000ef) {
        dma_fill_[address - 0x040000e0U] = value;
        return;
    }
    if (address >= 0x04000100 && address <= 0x0400010f) {
        write_timer_byte(cpu, address - 0x04000100U, value);
        return;
    }
    if (address >= 0x04000180 && address <= 0x04000187) {
        const auto shift = (address & 3U) * 8U;
        if (shift < 16) {
            const auto bits = static_cast<std::uint16_t>(static_cast<unsigned>(value) << shift);
            const auto mask = static_cast<std::uint16_t>(0xffU << shift);
            apply_ipc_interrupts(address < 0x04000184 ? ipc_.write_sync(cpu, bits, mask) : ipc_.write_control(cpu, bits, mask));
        }
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
