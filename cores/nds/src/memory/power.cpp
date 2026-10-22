#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {

std::uint8_t Memory::power_byte(Processor cpu, unsigned offset) const noexcept {
    if (offset == 0) return post_flags_[cpu == Processor::arm9 ? 0U : 1U];
    return offset == 1 && cpu == Processor::arm7 ? halt_control_ : 0;
}

void Memory::validate_power_write(Processor cpu, std::uint32_t address, std::uint32_t value, unsigned width) const {
    if (cpu != Processor::arm7 || address > 0x04000301 || address + width <= 0x04000301) return;
    const auto mode = (value >> ((0x04000301U - address) * 8U)) & 0xc0U;
    if (mode == 0x40 || mode == 0xc0)
        throw Error("NDS : mode GBA/Sleep de HALTCNT non pris en charge");
}

void Memory::write_power_byte(Processor cpu, unsigned offset, std::uint8_t value, bool from_bios) noexcept {
    if (offset == 0) {
        auto& flag = post_flags_[cpu == Processor::arm9 ? 0U : 1U];
        if (cpu == Processor::arm9) flag = static_cast<std::uint8_t>((flag & 1U) | (value & 3U));
        else if (from_bios) flag |= value & 1U;
    } else if (offset == 1 && cpu == Processor::arm7) {
        halt_control_ = value & 0xc0U;
        if (halt_control_ == 0x80) arm7_halt_requested_ = true;
    }
}

} // namespace ravenemu::nds
