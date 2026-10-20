#include "interpreter.hpp"

namespace ravenemu::nds::detail {

void Interpreter::arm_cp15(std::uint32_t raw) {
    if (cpu != Processor::arm9 || s.mode == CpuMode::user) {
        enter_exception(s, ram.exception_base(cpu), Exception::undefined_instruction, pc + 4U);
        return;
    }
    if ((raw & 0x00e00000U) != 0) unsupported(); // opcode1 ARM946 = 0
    const auto crn = (raw >> 16U) & 15U;
    const auto crm = raw & 15U;
    const auto opcode2 = (raw >> 5U) & 7U;
    const auto rd = (raw >> 12U) & 15U;
    if (raw & 0x00100000U) {
        const auto value = ram.cp15().read(crn, crm, opcode2);
        if (rd == 15) { // MRC vers r15 remplace NZCV, sans branchement ni changement de Q.
            s.negative = (value & 0x80000000U) != 0;
            s.zero = (value & 0x40000000U) != 0;
            s.carry = (value & 0x20000000U) != 0;
            s.overflow = (value & 0x10000000U) != 0;
        } else s.registers[rd] = value;
    } else {
        if (rd == 15) unsupported(); // valeur du pipeline MCR pc non modélisée
        ram.cp15().write(crn, crm, opcode2, reg(rd));
    }
}

} // namespace ravenemu::nds::detail
