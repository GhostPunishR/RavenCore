#include <ravenemu/nds/cpu_state.hpp>
#include <ravenemu/core.hpp>

#include <algorithm>

namespace ravenemu::nds {

std::size_t CpuState::bank(CpuMode value) noexcept {
    switch (value) {
    case CpuMode::fiq: return 1;
    case CpuMode::irq: return 2;
    case CpuMode::supervisor: return 3;
    case CpuMode::abort: return 4;
    case CpuMode::undefined: return 5;
    default: return 0; // user et system partagent leurs registres
    }
}

void CpuState::switch_mode(CpuMode next) noexcept {
    if (next == mode) return;
    stack_banks_[bank(mode)] = {registers[13], registers[14]};
    auto& outgoing = mode == CpuMode::fiq ? fiq_r8_r12_ : user_r8_r12_;
    std::copy_n(registers.begin() + 8, 5, outgoing.begin());
    const auto& incoming = next == CpuMode::fiq ? fiq_r8_r12_ : user_r8_r12_;
    std::copy(incoming.begin(), incoming.end(), registers.begin() + 8);
    registers[13] = stack_banks_[bank(next)][0];
    registers[14] = stack_banks_[bank(next)][1];
    mode = next;
}

std::uint32_t CpuState::cpsr() const noexcept {
    return static_cast<std::uint32_t>(mode) | (thumb ? 0x20U : 0U) |
        (fiq_disabled ? 0x40U : 0U) | (irq_disabled ? 0x80U : 0U) |
        (sticky_overflow ? 0x08000000U : 0U) | (overflow ? 0x10000000U : 0U) |
        (carry ? 0x20000000U : 0U) | (zero ? 0x40000000U : 0U) | (negative ? 0x80000000U : 0U);
}

void CpuState::set_cpsr(std::uint32_t value) {
    const auto next = static_cast<CpuMode>(value & 0x1fU);
    switch (next) {
    case CpuMode::user: case CpuMode::fiq: case CpuMode::irq: case CpuMode::supervisor:
    case CpuMode::abort: case CpuMode::undefined: case CpuMode::system: break;
    default: throw Error("NDS : mode CPSR invalide");
    }
    // Valider avant de toucher aux banques ou aux drapeaux.
    switch_mode(next);
    negative = (value & 0x80000000U) != 0;
    zero = (value & 0x40000000U) != 0;
    carry = (value & 0x20000000U) != 0;
    overflow = (value & 0x10000000U) != 0;
    sticky_overflow = (value & 0x08000000U) != 0;
    irq_disabled = (value & 0x80U) != 0;
    fiq_disabled = (value & 0x40U) != 0;
    thumb = (value & 0x20U) != 0;
}

bool CpuState::has_spsr() const noexcept { return bank(mode) != 0; }

std::uint32_t CpuState::spsr() const {
    if (!has_spsr()) throw Error("NDS : mode sans SPSR");
    return saved_status_[bank(mode)];
}

void CpuState::set_spsr(std::uint32_t value) {
    if (!has_spsr()) throw Error("NDS : mode sans SPSR");
    saved_status_[bank(mode)] = value;
}

} // namespace ravenemu::nds
