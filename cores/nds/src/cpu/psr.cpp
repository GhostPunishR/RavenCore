#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void Interpreter::arm_psr(std::uint32_t raw, bool read) {
    const bool saved = (raw & 0x00400000U) != 0;
    if (read) {
        const auto rd = (raw >> 12U) & 15U;
        if (rd == 15) unsupported();
        s.registers[rd] = saved ? s.spsr() : s.cpsr();
        return;
    }
    const bool immediate = (raw & 0x02000000U) != 0;
    if (!immediate && (raw & 15U) == 15) unsupported();
    const auto value = immediate
        ? std::rotr(raw & 0xffU, static_cast<int>(((raw >> 8U) & 15U) * 2U)) : reg(raw & 15U);
    const auto fields = (raw >> 16U) & 15U;
    const auto flag_mask = cpu == Processor::arm9 ? 0xf8000000U : 0xf0000000U;
    auto mask = (fields & 8U) != 0 ? flag_mask : 0U;
    // Les octets x/s ne contiennent aucun bit implémenté sur ARMv4T/ARMv5TE.
    if (saved) {
        if ((fields & 1U) != 0) mask |= 0xffU;
        s.set_spsr((s.spsr() & ~mask) | (value & mask));
    } else {
        if (s.mode != CpuMode::user && (fields & 1U) != 0) {
            if (((value ^ s.cpsr()) & 0x20U) != 0) unsupported(); // changement de T par MSR imprévisible
            mask |= 0xdfU; // mode, I et F ; conserver T
        }
        s.set_cpsr((s.cpsr() & ~mask) | (value & mask));
    }
}

} // namespace ravenemu::nds::detail
