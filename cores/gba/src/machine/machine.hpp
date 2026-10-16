#pragma once

#include "bios/bios.hpp"
#include "ppu/ppu.hpp"
#include "dma/dma_controller.hpp"
#include "apu/apu.hpp"
#include "timers/timers.hpp"
#include "serial/serial.hpp"

namespace ravenemu::gba {

class Machine {
public:
    Machine(RomImage rom, std::optional<GbaSaveType> forced, std::optional<bool> forced_rtc, Rtc::Clock clock)
        : Machine(rom, RomTraits::detect(*rom), forced, forced_rtc, std::move(clock)) {}
    Machine(RomImage rom, RomTraits traits, std::optional<GbaSaveType> forced,
            std::optional<bool> forced_rtc, Rtc::Clock clock)
        : cartridge(rom, traits, forced, forced_rtc, std::move(clock)), bus(cartridge), ppu(bus), timers(interrupts),
          dma(bus, interrupts), serial(bus, interrupts), cpu(bus), bios(cpu, bus) {
        bus.ppu = &ppu; bus.interrupts = &interrupts; bus.timers = &timers; bus.dma = &dma; bus.apu = &apu;
        bus.serial = &serial;
        ppu.interrupts = &interrupts; ppu.dma = &dma;
        timers.on_overflow = [this](int timer) { apu.timer_overflow(timer); };
        apu.on_fifo_request = [this](int channel) { dma.trigger_sound_fifo(channel); };
        // Un SWI HLE touche directement registres et mémoire : les
        // périphériques doivent d'abord rattraper les instructions passées.
        cpu.swi_handler = [this](int number) {
            sync_within_instruction(bus.wait_cycles);
            sync_peripherals();
            bios.handle_swi(number);
            next_event_ = 0;
        };
        bus.io_sync_context = this;
        bus.io_halt = [](void* context) { static_cast<Machine*>(context)->bios.halt(); };
        bus.io_sync = [](void* context, bool write) {
            auto& machine = *static_cast<Machine*>(context);
            // Le registre est lu ou écrit au premier cycle de son accès, après
            // ceux que l'instruction a déjà passés (lecture du code, attentes).
            machine.sync_within_instruction(machine.bus.wait_cycles - 1);
            machine.sync_peripherals();
            // Une écriture peut lancer un timer ou changer un registre vidéo :
            // le prochain événement est recalculé après l'instruction.
            if (write) machine.next_event_ = 0;
        };
        interrupts.on_request = [this](int mask) { bios.interrupt_raised(mask); bus.diagnostics.interrupt(mask); };
        cpu.reset(i32(0x08000000U));
    }
    void run_frame() {
        // Une instruction peut franchir la frontière PPU. La position courante
        // retranche ce dépassement à la trame suivante au lieu de l'accumuler.
        run_frame(ppu.cycles_until_next_frame());
    }
    void run_frame(int cycles) {
        auto elapsed = 0; bus.diagnostics.begin_frame();
        while (elapsed < cycles) {
            if (dma.active()) {
                sync_peripherals();
                const auto dma_cycles = std::min(cycles - elapsed, dma.cycles_until_event());
                // L'effet mémoire survient après que le coût de l'accès s'est
                // écoulé, à la même position temporelle dans tous les périphériques.
                advance_now(dma_cycles); dma.tick(dma_cycles); elapsed += dma_cycles;
                continue;
            }
            if (cpu.state.halted) {
                // Une interruption ne peut naître que d'un événement : le CPU
                // arrêté saute directement au prochain, sans retard.
                sync_peripherals();
                const auto idle = std::max(1, std::min(cycles - elapsed, next_event_));
                advance_now(idle); elapsed += idle;
                bus.diagnostics.wait_step(idle, bios.wait_state() ? bios.wait_state()->interrupt_mask : 0);
                if (bios.should_resume()) { cpu.state.halted = false; bus.diagnostics.wait_resolved(); bus.break_access_sequence(); }
                continue;
            }
            if (interrupts.pending() && !cpu.state.irq_disabled) {
                cpu.raise_exception(CpuState::mode_irq, 0x18, add32(cpu.state.regs[15], 4));
            }
            in_instruction_ = true;
            const auto consumed = cpu.step(); bus.diagnostics.instruction();
            in_instruction_ = false;
            pending_cycles_ += consumed - std::exchange(synced_in_instruction_, 0); elapsed += consumed;
            if (pending_cycles_ >= next_event_) sync_peripherals();
        }
        sync_peripherals();
    }
    /**
     * Les périphériques n'ont pas à être avancés après chaque instruction :
     * entre deux événements (fin de tracé, fin de ligne, débordement de
     * timer), le CPU ne peut observer leur état qu'en lisant un registre
     * d'E/S. Les cycles s'accumulent donc et sont rattrapés au prochain
     * événement, avant tout accès aux E/S et avant un SWI, un DMA ou un Halt.
     * L'ordre des effets est exactement celui d'une avance à chaque instruction.
     */
    /**
     * Avance les périphériques jusqu'au cycle `offset` de l'instruction en
     * cours : une lecture de timer ou une écriture d'E/S voit l'heure exacte
     * de son accès, pas celle du début de l'instruction.
     */
    void sync_within_instruction(int offset) noexcept {
        if (!in_instruction_ || offset <= synced_in_instruction_) return;
        pending_cycles_ += offset - synced_in_instruction_;
        synced_in_instruction_ = offset;
    }
    void sync_peripherals() {
        if (pending_cycles_ > 0) { const auto cycles = pending_cycles_; pending_cycles_ = 0; advance_peripherals(cycles); }
        next_event_ = next_event();
    }
    // Prochain changement que le CPU peut observer autrement que par les E/S.
    [[nodiscard]] int next_event() const noexcept {
        return std::min({ppu.cycles_until_next_event(), timers.cycles_until_next_overflow(), serial.cycles_until_completion()});
    }
    void advance_now(int cycles) {
        advance_peripherals(cycles);
        next_event_ = next_event();
    }
    void advance_peripherals(int cycles) {
        auto remaining = cycles;
        while (remaining > 0) {
            const auto step = std::max(1, std::min({remaining, timers.cycles_until_next_overflow(), apu.cycles_until_next_sample(),
                                                    serial.cycles_until_completion()}));
            ppu.tick(step); timers.tick(step); apu.tick(step); serial.tick(step); remaining -= step;
        }
    }

    Cartridge cartridge;
    Bus bus;
    Ppu ppu;
    InterruptController interrupts;
    Timers timers;
    DmaController dma;
    Serial serial;
    Apu apu;
    Cpu cpu;
    Bios bios;

private:
    int pending_cycles_{};
    int next_event_{};
    bool in_instruction_{};
    int synced_in_instruction_{};
};

} // namespace ravenemu::gba
