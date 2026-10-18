#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void enter_exception(CpuState& state, Processor cpu, Exception exception, std::uint32_t return_address) {
    CpuMode mode;
    std::uint32_t offset;
    switch (exception) {
    case Exception::software_interrupt: mode = CpuMode::supervisor; offset = 8; break;
    case Exception::undefined_instruction: mode = CpuMode::undefined; offset = 4; break;
    case Exception::irq: mode = CpuMode::irq; offset = 0x18; break;
    case Exception::fiq: mode = CpuMode::fiq; offset = 0x1c; break;
    default: throw Error("NDS : exception CPU invalide");
    }
    const auto saved = state.cpsr();
    auto control = (saved & ~0x3fU) | static_cast<std::uint32_t>(mode) | 0x80U;
    if (exception == Exception::fiq) control |= 0x40U;
    state.set_cpsr(control);
    state.set_spsr(saved);
    state.registers[14] = return_address;
    // Configuration NDS retenue tant que CP15 n'est pas implémenté.
    state.registers[15] = (cpu == Processor::arm9 ? 0xffff0000U : 0U) + offset;
}

} // namespace ravenemu::nds::detail
