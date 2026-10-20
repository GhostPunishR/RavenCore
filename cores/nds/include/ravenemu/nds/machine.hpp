#pragma once

#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/nds/memory.hpp>
#include <ravenemu/nds/cpu.hpp>

#include <optional>

namespace ravenemu::nds {

struct ProgramEntry {
    std::uint32_t address{}; // sans le bit Thumb, sans décalage de pipeline
    bool thumb{};
};

struct BootEntries {
    ProgramEntry arm9;
    ProgramEntry arm7;
};

/** Préparation mémoire d'un démarrage direct expérimental, pas un BIOS NDS.
 * Pas-à-pas ARM/Thumb sans synchronisation temporelle des deux CPU.
 * Les échecs de chargement préservent la machine.
 */
class Machine {
public:
    void load_cartridge(const Cartridge& cartridge);
    [[nodiscard]] Memory& memory() noexcept { return memory_; }
    [[nodiscard]] const Memory& memory() const noexcept { return memory_; }
    [[nodiscard]] VideoFrame render_frame() const { return memory_.render_frame(); }
    [[nodiscard]] const std::optional<BootEntries>& entries() const noexcept { return entries_; }
    [[nodiscard]] const CpuState& cpu_state(Processor cpu) const noexcept;
    void set_interrupt_lines(Processor cpu, bool irq, bool fiq = false) noexcept;
    void step(Processor cpu);
    // Horloge explicite timers/LCD/FIFO/capture ; step/run_steps restent sans timings.
    void advance_cycles(std::uint64_t cycles) { memory_.advance_cycles(cycles); }
    // Budget de pas (une unité DMA ou entrée d'exception consomme un pas sans
    // instruction), pas de cycles. Un échec laisse les instructions/transferts
    // déjà terminées en place et le CPU arrêté sur l'instruction fautive.
    void run_steps(Processor cpu, std::size_t instructions);

private:
    Memory memory_;
    std::optional<BootEntries> entries_;
    Cpu arm9_{Processor::arm9};
    Cpu arm7_{Processor::arm7};
};

} // namespace ravenemu::nds
