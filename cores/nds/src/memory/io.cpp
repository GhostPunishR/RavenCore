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
    if (address >= 0x04000300 && address <= 0x04000303) return power_byte(cpu,address - 0x04000300U);
    if (address >= 0x040001a0 && address <= 0x040001af)
        return cpu == gamecard_owner() ? gamecard_.read_byte(address - 0x040001a0U) : std::uint8_t{0};
    if (address >= 0x04000204 && address <= 0x04000207) {
        const auto value = 0x6000U | exmem_shared_ | exmem_local_[cpu == Processor::arm9 ? 0U : 1U];
        return static_cast<std::uint8_t>(value >> ((address - 0x04000204U) * 8U));
    }
    if (cpu == Processor::arm7 && address >= 0x040001c0 && address <= 0x040001c3)
        return spi_.read_byte(address - 0x040001c0U);
    if (cpu == Processor::arm9) {
        if (address >= 0x04000068 && address <= 0x0400006b) return 0; // port FIFO sans retrait en lecture
        if (address >= 0x04000064 && address <= 0x04000067)
            return static_cast<std::uint8_t>(video_.capture_control_ >> ((address & 3U) * 8U));
        if (address >= 0x04001000 && address <= 0x04001003)
            return static_cast<std::uint8_t>(display_control_b_ >> ((address & 3U) * 8U));
        if ((address >= 0x04000008 && address <= 0x0400004f) ||
            (address >= 0x04000050 && address <= 0x04000055) ||
            (address >= 0x0400006c && address <= 0x0400006f) ||
            (address >= 0x04001008 && address <= 0x0400104f) ||
            (address >= 0x04001050 && address <= 0x04001055) ||
            (address >= 0x0400106c && address <= 0x0400106f))
            // MOSAIC est en écriture seule ; ses octets réservés se lisent aussi à zéro.
            return (address & 0xffcU) == 0x4c ? 0 : video_.registers_[(address >> 12U) & 1U][address & 0x7fU];
        if (address >= 0x04000000 && address <= 0x04000003)
            return static_cast<std::uint8_t>(display_control_ >> ((address & 3U) * 8U));
        if (address >= 0x04000240 && address <= 0x04000249 && address != 0x04000247)
            return video_.control(static_cast<VramBank>(address - 0x04000240U - (address >= 0x04000248 ? 1U : 0U)));
        if (address >= 0x04000304 && address <= 0x04000307)
            return static_cast<std::uint8_t>(static_cast<std::uint32_t>(graphics_power_) >> ((address & 3U) * 8U));
    } else if (address == 0x04000240) return video_.arm7_status();
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
    if (address >= 0x04000130 && address <= 0x04000133) {
        const auto value = address < 0x04000132 ? input_.key_input() : input_.control(cpu);
        return static_cast<std::uint8_t>(value >> ((address & 1U) * 8U));
    }
    if (cpu == Processor::arm7 && (address == 0x04000136 || address == 0x04000137)) {
        const auto value = input_.extended_input() | (spi_.pen_irq_enabled() ? 0U : 0x40U);
        return static_cast<std::uint8_t>(value >> ((address & 1U) * 8U));
    }
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

void Memory::write_io_byte(Processor cpu, std::uint32_t address, std::uint8_t value, bool from_bios) noexcept {
    if (address >= 0x04000300 && address <= 0x04000303) {
        write_power_byte(cpu,address - 0x04000300U,value,from_bios);
        return;
    }
    if (address >= 0x04000204 && address <= 0x04000207) {
        if (address == 0x04000204) {
            exmem_local_[cpu == Processor::arm9 ? 0U : 1U] = value & 0x7fU;
            if (cpu == Processor::arm9) exmem_shared_ = static_cast<std::uint16_t>((exmem_shared_ & ~0x80U) | (value & 0x80U));
        } else if (address == 0x04000205 && cpu == Processor::arm9)
            exmem_shared_ = static_cast<std::uint16_t>((exmem_shared_ & 0x80U) | ((static_cast<unsigned>(value) << 8U) & 0x8800U));
        return;
    }
    if (cpu == Processor::arm9) {
        if (address >= 0x04000064 && address <= 0x04000067) {
            video_.write_capture(address & 3U, value);
            return;
        }
        if (address >= 0x04001000 && address <= 0x04001003) {
            const auto shift = (address & 3U) * 8U;
            display_control_b_ = ((display_control_b_ & ~(0xffU << shift)) |
                (static_cast<std::uint32_t>(value) << shift)) & 0xc0b1fff7U;
            return;
        }
        if ((address >= 0x04000008 && address <= 0x0400004f) ||
            (address >= 0x04000050 && address <= 0x04000055) ||
            (address >= 0x0400006c && address <= 0x0400006f) ||
            (address >= 0x04001008 && address <= 0x0400104f) ||
            (address >= 0x04001050 && address <= 0x04001055) ||
            (address >= 0x0400106c && address <= 0x0400106f)) {
            video_.write_register((address >> 12U) & 1U, address & 0x7fU, value);
            return;
        }
        if (address >= 0x04000000 && address <= 0x04000003) {
            const auto shift = (address & 3U) * 8U;
            display_control_ = (display_control_ & ~(0xffU << shift)) | (static_cast<std::uint32_t>(value) << shift);
            return;
        }
        if (address >= 0x04000240 && address <= 0x04000249 && address != 0x04000247) {
            video_.set_control(static_cast<VramBank>(address - 0x04000240U - (address >= 0x04000248 ? 1U : 0U)), value);
            return;
        }
        if (address >= 0x04000304 && address <= 0x04000307) {
            const auto shift = (address & 3U) * 8U;
            const auto bits = (static_cast<std::uint32_t>(graphics_power_) & ~(0xffU << shift)) |
                (static_cast<std::uint32_t>(value) << shift);
            graphics_power_ = static_cast<std::uint16_t>(bits & 0x820fU);
            video_.power_ = graphics_power_;
            return;
        }
    }
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
        if (cpu == gamecard_owner() && gamecard_.ready()) dma(cpu).trigger(DmaController::Event::gamecard);
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
    if (address >= 0x04000130 && address <= 0x04000133) {
        if (address >= 0x04000132) {
            const auto shift = (address & 1U) * 8U;
            input_.write_control(cpu, static_cast<std::uint16_t>(static_cast<unsigned>(value) << shift),
                                 static_cast<std::uint16_t>(0xffU << shift));
        }
        return; // KEYINPUT en lecture seule
    }
    if (cpu == Processor::arm7 && (address == 0x04000136 || address == 0x04000137)) return;
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
