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
        interrupts.on_change = [this] { track_irq_line(); };
        cpu.reset(i32(0x08000000U));
    }
    void run_frame() {
        // Une instruction peut franchir la frontière PPU. La position courante
        // retranche ce dépassement à la trame suivante au lieu de l'accumuler.
        run_frame(ppu.cycles_until_next_frame());
    }
    void run_frame(int cycles) {
        bus.diagnostics.begin_frame();
        run_for(cycles);
    }
    /**
     * Avance d'au moins `cycles` cycles et rend ceux réellement écoulés (une
     * instruction peut dépasser). Le câble link avance ainsi plusieurs
     * machines par tranches ; il a pu changer le port série entre deux tranches.
     */
    int run_for(int cycles) {
        auto elapsed = 0;
        sync_peripherals();
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
                // arrêté saute directement au prochain. Si elle doit être
                // prise, il ne repart qu'une fois la ligne synchronisée, pour
                // que le gestionnaire passe avant le code du jeu.
                sync_peripherals();
                track_irq_line();
                const auto settling = irq_settling_cycles();
                const auto idle = std::max(1, std::min(cycles - elapsed, settling > 0 ? settling : next_event_));
                advance_now(idle); elapsed += idle;
                bus.diagnostics.wait_step(idle, bios.wait_state() ? bios.wait_state()->interrupt_mask : 0);
                if (irq_settling_cycles() == 0 && bios.should_resume()) {
                    cpu.state.halted = false; bus.diagnostics.wait_resolved(); bus.break_access_sequence();
                }
                continue;
            }
            track_irq_line();
            if (irq_raised_at_ && !cpu.state.irq_disabled && irq_settling_cycles() == 0) {
                cpu.raise_exception(CpuState::mode_irq, 0x18, add32(cpu.state.regs[15], 4));
                pending_cycles_ += irq_entry_cycles; elapsed += irq_entry_cycles;
            }
            in_instruction_ = true;
            const auto consumed = cpu.step(); bus.diagnostics.instruction();
            in_instruction_ = false;
            pending_cycles_ += consumed - std::exchange(synced_in_instruction_, 0); elapsed += consumed;
            if (pending_cycles_ >= next_event_) sync_peripherals();
        }
        sync_peripherals();
        return elapsed;
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
            clock_ += step;
            ppu.tick(step); timers.tick(step); apu.tick(step); serial.tick(step); remaining -= step;
        }
    }

    /**
     * Ligne d'IRQ (IME, et une source à la fois dans IE et IF) : le CPU ne
     * prend une IRQ qu'à la fin d'une instruction, une fois la ligne levée
     * depuis 4 cycles, et l'entrée dans l'exception coûte 2 cycles de plus
     * qu'un saut. Ces deux durées sont mesurées sur console : une IRQ de
     * timer laisse encore passer trois NOP, pas quatre, et son gestionnaire
     * arrête le timer au même cycle que sur la console. Les états gardent
     * l'âge de la ligne.
     */
    static constexpr int irq_sync_cycles = 4;
    static constexpr int irq_entry_cycles = 2;
    [[nodiscard]] int irq_line_age() const noexcept {
        if (!irq_raised_at_) return -1;
        return static_cast<int>(std::min<std::int64_t>(irq_sync_cycles, clock_ + pending_cycles_ - *irq_raised_at_));
    }
    void restore_irq_line_age(int age) {
        if (age < -1 || age > irq_sync_cycles) throw SaveStateError("État d'interruption GBA invalide");
        if (age < 0) irq_raised_at_.reset();
        else irq_raised_at_ = clock_ + pending_cycles_ - age;
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
    // Suit la ligne d'IRQ : l'instant où elle s'est levée, rien au repos.
    void track_irq_line() noexcept {
        if (!interrupts.pending()) irq_raised_at_.reset();
        else if (!irq_raised_at_) irq_raised_at_ = clock_ + pending_cycles_;
    }
    // Cycles avant que l'IRQ en attente ne puisse être prise ; 0 s'il n'y a
    // rien à prendre, ou si elle peut l'être dès maintenant.
    [[nodiscard]] int irq_settling_cycles() const noexcept {
        if (!irq_raised_at_ || cpu.state.irq_disabled) return 0;
        const auto age = clock_ + pending_cycles_ - *irq_raised_at_;
        return static_cast<int>(std::max<std::int64_t>(0, irq_sync_cycles - age));
    }

    std::int64_t clock_{};                       // cycles vus par les périphériques
    std::optional<std::int64_t> irq_raised_at_;  // levée de la ligne d'IRQ
    int pending_cycles_{};
    int next_event_{};
    bool in_instruction_{};
    int synced_in_instruction_{};
};

} // namespace ravenemu::gba
