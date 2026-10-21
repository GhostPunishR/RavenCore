#pragma once

#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/nds/memory.hpp>
#include <ravenemu/nds/cpu.hpp>
#include <ravenemu/nds/scheduler.hpp>

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
 * Pas-à-pas explicite ou ordonnanceur commun expérimental ARM9/ARM7.
 * Les échecs de chargement préservent la machine.
 */
class Machine {
public:
    void load_cartridge(const Cartridge& cartridge);
    void load_firmware(std::span<const std::uint8_t> bytes) { memory_.load_firmware(bytes); }
    [[nodiscard]] bool firmware_dirty() const noexcept { return memory_.firmware_dirty(); }
    [[nodiscard]] FirmwareSnapshot snapshot_firmware() const { return memory_.snapshot_firmware(); }
    void acknowledge_firmware_saved(std::uint64_t generation) noexcept { memory_.acknowledge_firmware_saved(generation); }
    void set_input(InputState state) noexcept { memory_.set_input(state); }
    void set_key(Key key, bool pressed) { memory_.set_key(key, pressed); }
    void set_touch_position(std::uint16_t x, std::uint16_t y, bool pressed = true) { memory_.set_touch_position(x,y,pressed); }
    [[nodiscard]] const InputState& input_state() const noexcept { return memory_.input_state(); }
    [[nodiscard]] Memory& memory() noexcept { return memory_; }
    [[nodiscard]] const Memory& memory() const noexcept { return memory_; }
    [[nodiscard]] VideoFrame render_frame() const { return memory_.render_frame(); }
    [[nodiscard]] const std::optional<BootEntries>& entries() const noexcept { return entries_; }
    [[nodiscard]] const CpuState& cpu_state(Processor cpu) const noexcept;
    void set_interrupt_lines(Processor cpu, bool irq, bool fiq = false) noexcept;
    void step(Processor cpu);
    [[nodiscard]] const SchedulerState& scheduler_state() const noexcept { return scheduler_; }
    // Budget en cycles communs à timer_clock_hz. Coût uniforme d'un pas par
    // créneau CPU : ne reproduit pas encore les timings des instructions/bus.
    // Une reprise termine la phase interrompue sans rejouer les pas terminés.
    void run_cycles(std::uint64_t cycles);
    // Exécute jusqu'à la prochaine frontière LCD (une image entière à phase zéro).
    void run_frame();
    // Horloge explicite timers/SPI/LCD/FIFO/capture ; step/run_steps restent sans timings.
    void advance_cycles(std::uint64_t cycles) { memory_.advance_cycles(cycles); }
    // Budget de pas (une unité DMA ou entrée d'exception consomme un pas sans
    // instruction), pas de cycles. Un échec laisse les instructions/transferts
    // déjà terminées en place et le CPU arrêté sur l'instruction fautive.
    void run_steps(Processor cpu, std::size_t instructions);

private:
    SchedulerState scheduler_;
    Memory memory_;
    std::optional<BootEntries> entries_;
    Cpu arm9_{Processor::arm9};
    Cpu arm7_{Processor::arm7};
};

} // namespace ravenemu::nds
