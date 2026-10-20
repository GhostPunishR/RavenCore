#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void enter_exception(CpuState& state, std::uint32_t vector_base, Exception exception, std::uint32_t return_address) {
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
    state.registers[15] = vector_base + offset;
}

} // namespace ravenemu::nds::detail
