#pragma once

#include "support.hpp"
#include "data_bus.hpp"

namespace ravenemu::nds::detail {

enum class Exception { software_interrupt, undefined_instruction, prefetch_abort, data_abort, irq, fiq };
void enter_exception(CpuState& state, std::uint32_t vector_base, Exception exception, std::uint32_t return_address);

class Interpreter {
public:
    Interpreter(CpuState state, Processor processor, Memory& memory)
        : s(state), cpu(processor), ram(memory), bus(memory, processor, state.mode != CpuMode::user, state.registers[15]), pc(state.registers[15]) {}

    CpuState s;

    void execute(std::uint32_t raw) {
        s.registers[15] = pc + (s.thumb ? 2U : 4U);
        if (s.thumb) execute_thumb(raw);
        else execute_arm(raw);
        ++s.instructions;
    }

private:
    Processor cpu;
    Memory& ram;
    DataBus bus;
    std::uint32_t pc;

    [[noreturn]] static void unsupported() { throw Error("instruction ou variante non implémentée"); }
    [[nodiscard]] std::uint32_t reg(unsigned r) const {
        return r == 15 ? pc + (s.thumb ? 4U : 8U) : s.registers[r];
    }
    void branch(std::uint32_t address) { s.registers[15] = address & (s.thumb ? ~1U : ~3U); }
    void exchange(std::uint32_t address) {
        s.thumb = (address & 1U) != 0;
        branch(address);
    }
    void load_pc(std::uint32_t address) {
        // ARMv5, CP15.L4=0 : LDR/LDM/POP vers PC prennent le bit T de l'adresse.
        // ARMv4T : ils conservent le mode courant ; seul BX l'échange.
        if (cpu == Processor::arm9 && ram.cp15().load_interworking()) exchange(address);
        else branch(address);
    }
    void nz(std::uint32_t value) {
        s.negative = (value >> 31U) != 0;
        s.zero = value == 0;
    }
    std::uint32_t arithmetic(Result result) {
        nz(result.value);
        s.carry = result.carry;
        s.overflow = result.overflow;
        return result.value;
    }
    void write_reg(unsigned r, std::uint32_t value) {
        if (r == 15) branch(value);
        else s.registers[r] = value;
    }

    void execute_arm(std::uint32_t raw);

    void arm_data(std::uint32_t raw);

    void arm_cp15(std::uint32_t raw);

    void arm_psr(std::uint32_t raw, bool read);

    void arm_transfer(std::uint32_t raw);

    void arm_halfword(std::uint32_t raw);

    void arm_multiply(std::uint32_t raw, bool wide);

    void arm_saturating(std::uint32_t raw);

    void arm_dsp_multiply(std::uint32_t raw);

    void arm_doubleword(std::uint32_t raw);

    void arm_swap(std::uint32_t raw);

    void multiple(unsigned rn, std::uint32_t list, bool load, bool pre, bool up, bool writeback,
                  bool restore_status = false);

    void execute_thumb(std::uint32_t raw);

    void thumb_transfer(unsigned rd, std::uint32_t address, unsigned width, bool load);

    void thumb_alu(std::uint32_t raw);
};

} // namespace ravenemu::nds::detail
