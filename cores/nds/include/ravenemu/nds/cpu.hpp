#pragma once

#include <ravenemu/core.hpp>
#include <ravenemu/nds/memory.hpp>
#include <ravenemu/nds/cpu_state.hpp>

#include <optional>
#include <string>

namespace ravenemu::nds {

class CpuExecutionError final : public Error {
public:
    CpuExecutionError(Processor processor, std::uint32_t address, bool thumb,
                      std::optional<std::uint32_t> instruction, const std::string& reason);
    [[nodiscard]] Processor processor() const noexcept { return processor_; }
    [[nodiscard]] std::uint32_t address() const noexcept { return address_; }
    [[nodiscard]] bool thumb() const noexcept { return thumb_; }
    // Absente si la lecture de l'instruction elle-même a échoué.
    [[nodiscard]] std::optional<std::uint32_t> instruction() const noexcept { return instruction_; }

private:
    Processor processor_;
    std::uint32_t address_;
    bool thumb_;
    std::optional<std::uint32_t> instruction_;
};

/** Interpréteur expérimental ARM/Thumb, avec quelques extensions ARMv5 sur ARM9.
 * Pas encore de cycles ni de pipeline préchargé ; CP15 ARM9 partiel.
 * Une instruction non implémentée ou un accès mémoire refusé lève
 * CpuExecutionError, sans avancer le CPU ni modifier ses registres.
 * Un DMA déclenché transfère une unité par pas en suspendant le CPU ; ses
 * erreurs lèvent DmaTransferError et préservent l'unité fautive.
 */
class Cpu {
public:
    explicit Cpu(Processor processor) : processor_(processor) {}
    void reset(std::uint32_t address, bool thumb);
    void set_register(std::size_t index, std::uint32_t value);
    [[nodiscard]] const CpuState& state() const noexcept { return state_; }
    void set_interrupt_lines(bool irq, bool fiq = false) noexcept { irq_line_ = irq; fiq_line_ = fiq; }
    void step(Memory& memory);

private:
    Processor processor_;
    CpuState state_;
    bool irq_line_{};
    bool fiq_line_{};
};

} // namespace ravenemu::nds
