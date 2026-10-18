#include <ravenemu/nds/cpu.hpp>
#include "interpreter.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ravenemu::nds {
namespace {

std::string fault_message(Processor cpu, std::uint32_t address, bool thumb,
                          std::optional<std::uint32_t> instruction, const std::string& reason) {
    std::ostringstream out;
    out << "NDS " << (cpu == Processor::arm9 ? "ARM9" : "ARM7") << (thumb ? " Thumb" : " ARM")
        << " PC=0x" << std::hex << std::setw(8) << std::setfill('0') << address;
    if (instruction) out << " instruction=0x" << std::setw(thumb ? 4 : 8) << *instruction;
    out << " : " << reason;
    return out.str();
}

} // namespace

CpuExecutionError::CpuExecutionError(Processor processor, std::uint32_t address, bool thumb,
                                     std::optional<std::uint32_t> instruction, const std::string& reason)
    : Error(fault_message(processor, address, thumb, instruction, reason)), processor_(processor),
      address_(address), thumb_(thumb), instruction_(instruction) {}

void Cpu::reset(std::uint32_t address, bool thumb) {
    if ((address & (thumb ? 1U : 3U)) != 0) throw std::invalid_argument("NDS : entrée CPU non alignée");
    state_ = {};
    state_.registers[15] = address;
    state_.thumb = thumb;
    irq_line_ = fiq_line_ = false;
}

void Cpu::set_register(std::size_t index, std::uint32_t value) {
    if (index >= 16) throw std::out_of_range("NDS : numéro de registre CPU");
    if (index == 15) value &= state_.thumb ? ~1U : ~3U;
    state_.registers[index] = value;
}

void Cpu::step(Memory& memory) {
    if ((fiq_line_ && !state_.fiq_disabled) || (irq_line_ && !state_.irq_disabled)) {
        const auto exception = fiq_line_ && !state_.fiq_disabled ? detail::Exception::fiq : detail::Exception::irq;
        auto prepared = state_;
        detail::enter_exception(prepared, processor_, exception, state_.registers[15] + 4U);
        state_ = prepared;
        return; // entrée d'exception, aucune instruction terminée sur ce pas
    }
    const auto pc = state_.registers[15];
    std::optional<std::uint32_t> instruction;
    try {
        instruction = state_.thumb ? memory.read16(processor_, pc) : memory.read32(processor_, pc);
        detail::Interpreter interpreter{state_, processor_, memory};
        interpreter.execute(*instruction);
        state_ = interpreter.s;
    } catch (const Error& error) {
        throw CpuExecutionError(processor_, pc, state_.thumb, instruction, error.what());
    }
}

} // namespace ravenemu::nds
