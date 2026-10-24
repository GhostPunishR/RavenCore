#pragma once

#include "bios/bios.hpp"
#include "ppu/ppu.hpp"
#include "dma/dma_controller.hpp"
#include "apu/apu.hpp"
#include "timers/timers.hpp"
#include "serial/serial.hpp"

namespace ravenemu::gba {

/** BIOS d'une console (16 Kio) et son empreinte, rangée dans les états. */
struct GbaBios {
    static constexpr std::size_t size = 0x4000;
    std::vector<std::uint8_t> image;
    std::array<std::uint8_t, 32> hash{};
};
using GbaBiosImage = std::shared_ptr<const GbaBios>;

class Machine {
public:
    Machine(RomImage rom, std::optional<GbaSaveType> forced, std::optional<bool> forced_rtc, Rtc::Clock clock)
        : Machine(rom, RomTraits::detect(*rom), forced, forced_rtc, std::move(clock)) {}
    /**
     * Sans `bios_image`, le BIOS réécrit de RavenCore (HLE) répond aux SWI et
     * le jeu démarre directement. Avec, ses SWI et ses IRQ passent par le vrai
     * BIOS, qui joue son animation de démarrage sauf avec `skip_intro`.
     */
    Machine(RomImage rom, RomTraits traits, std::optional<GbaSaveType> forced,
            std::optional<bool> forced_rtc, Rtc::Clock clock, GbaBiosImage bios_image = {}, bool skip_intro = false)
        : cartridge(rom, traits, forced, forced_rtc, std::move(clock)), bus(cartridge), ppu(bus), timers(interrupts),
          dma(bus, interrupts), serial(bus, interrupts), cpu(bus), bios(cpu, bus), bios_image(std::move(bios_image)) {
        bus.ppu = &ppu; bus.interrupts = &interrupts; bus.timers = &timers; bus.dma = &dma; bus.apu = &apu;
        bus.serial = &serial;
        ppu.interrupts = &interrupts; ppu.dma = &dma;
        timers.on_overflow = [this](int timer) { apu.timer_overflow(timer); };
        apu.on_fifo_request = [this](int channel) { dma.trigger_sound_fifo(channel); };
        // Un SWI HLE touche directement registres et mémoire : les
        // périphériques doivent d'abord rattraper les instructions passées.
        cpu.swi_handler = [this](int number) {
            if (number == Bios::busy_resume_swi && bios_busy_cycles_ > 0 &&
                u32(cpu.state.regs[15]) == (cpu.state.thumb ? Bios::busy_resume_thumb : Bios::busy_resume_arm)) {
                // Retour de l'IRQ qui a interrompu une fonction du BIOS : la
                // fin de son temps s'écoule, puis le jeu reprend après son SWI.
                cpu.state.halted = true;
                cpu.branch_to(bios_busy_return_);
                return;
            }
            sync_within_instruction(bus.wait_cycles);
            sync_peripherals();
            const auto start = bus.wait_cycles;
            const auto synced = synced_in_instruction_;
            const auto return_address = add32(cpu.state.regs[15], cpu.state.thumb ? 2 : 4);
            bios.handle_swi(number);
            if (number == 0x02 && cpu.state.halted) halt_return_ = return_address;
            // Le BIOS d'origine exécute ses fonctions avec les IRQ de
            // l'appelant : une longue copie (CpuSet, décompression…) les
            // laisse passer. Son temps s'écoule donc après le SWI, CPU arrêté
            // comme dans le BIOS, et une IRQ peut l'interrompre. Une fonction
            // qui a touché aux E/S garde son temps dans le SWI.
            if (bus.wait_cycles > start && bios_busy_cycles_ == 0 && synced_in_instruction_ == synced &&
                !cpu.state.halted) {
                bios_busy_cycles_ = bus.wait_cycles - start;
                bios_busy_return_ = return_address;
                bus.wait_cycles = start;
                cpu.state.halted = true;
            }
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
        if (!this->bios_image) {
            cpu.reset(i32(0x08000000U));
            return;
        }
        // Vrai BIOS : son code remplace celui du BIOS réécrit, ses fonctions
        // répondent aux SWI, et son gestionnaire d'IRQ laisse au jeu le soin
        // de noter l'interruption en 0x03007FF8.
        std::copy(this->bios_image->image.begin(), this->bios_image->image.end(), bus.bios.begin());
        cpu.swi_handler = nullptr;
        interrupts.on_request = [this](int mask) { bus.diagnostics.interrupt(mask); };
        if (skip_intro) {
            cpu.reset(i32(0x08000000U));
        } else {
            cpu.power_on();
            bus.executing_bios = true;
        }
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
                if (bios_busy_cycles_ > 0) {
                    // Temps d'une fonction du BIOS : rien ne s'exécute, sauf
                    // une IRQ, dont le gestionnaire revient au point de reprise.
                    if (irq_raised_at_ && !cpu.state.irq_disabled && settling == 0) {
                        cpu.state.halted = false;
                        cpu.branch_to(i32(cpu.state.thumb ? Bios::busy_resume_thumb : Bios::busy_resume_arm));
                        continue;
                    }
                    const auto spent = std::max(
                        1, std::min({cycles - elapsed, bios_busy_cycles_, settling > 0 ? settling : next_event_}));
                    advance_now(spent); elapsed += spent;
                    if ((bios_busy_cycles_ -= spent) == 0) cpu.state.halted = false;
                    continue;
                }
                const auto idle = std::max(1, std::min(cycles - elapsed, settling > 0 ? settling : next_event_));
                advance_now(idle); elapsed += idle;
                bus.diagnostics.wait_step(idle, bios.wait_state() ? bios.wait_state()->interrupt_mask : 0);
                if (irq_settling_cycles() == 0 && bios.should_resume()) {
                    bus.diagnostics.wait_resolved(); bus.break_access_sequence();
                    if (halt_return_ != 0) {
                        // Le Halt du BIOS (SWI 2) rend la main après son réveil :
                        // l'IRQ passe d'abord, puis le BIOS sort de sa fonction.
                        bios_busy_cycles_ = halt_return_cycles;
                        bios_busy_return_ = std::exchange(halt_return_, 0);
                        continue;
                    }
                    cpu.state.halted = false;
                } else if (bios.wait_state() && irq_raised_at_ && !cpu.state.irq_disabled &&
                           irq_settling_cycles() == 0) {
                    // IntrWait laisse passer les autres IRQ : le CPU repart sur
                    // le SWI, l'IRQ est prise avant lui, et son retour reprend l'attente.
                    cpu.state.halted = false; bus.break_access_sequence();
                    cpu.branch_to(add32(cpu.state.regs[15], cpu.state.thumb ? -2 : -4));
                }
                continue;
            }
            track_irq_line();
            if (irq_raised_at_ && !cpu.state.irq_disabled && irq_settling_cycles() == 0) {
                cpu.raise_exception(CpuState::mode_irq, 0x18, add32(cpu.state.regs[15], 4));
                // Cycles internes du CPU : l'unité de préchargement de la cartouche lit pendant ce temps.
                bus.idle(irq_entry_cycles);
                const auto entry = bus.take_wait_cycles();
                pending_cycles_ += entry; elapsed += entry;
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
    /**
     * Le Halt du BIOS d'origine (SWI 2) ne rend pas la main dès le réveil :
     * le gestionnaire d'IRQ passe, puis le BIOS sort de sa fonction et de son
     * SWI. La suite de tests de mGBA, lancée sur console, mesure la fin d'un
     * transfert série attendue par Halt : le jeu reprend 27 cycles après le
     * retour du gestionnaire. Le point de reprise en prend 3, le reste
     * s'écoule ici.
     */
    static constexpr int halt_return_cycles = 24;
    // Temps restant d'une fonction du BIOS et l'instruction qui la suit, puis
    // celle qui suit un Halt du BIOS pas encore réveillé (0 : aucun).
    [[nodiscard]] std::array<std::int32_t, 3> export_bios_busy() const noexcept {
        return {bios_busy_cycles_, bios_busy_cycles_ > 0 ? bios_busy_return_ : 0, halt_return_};
    }
    void restore_bios_busy(std::span<const std::int32_t> state) {
        if (state.size() != 3 || state[0] < 0 || state[0] > max_bios_busy_cycles || (state[0] == 0 && state[1] != 0) ||
            (state[2] != 0 && state[0] != 0))
            throw SaveStateError("État du BIOS GBA invalide");
        bios_busy_cycles_ = state[0];
        bios_busy_return_ = state[1];
        halt_return_ = state[2];
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
    GbaBiosImage bios_image;  // vide : BIOS réécrit de RavenCore

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

    // Plus long qu'aucune fonction du BIOS : décompresser 16 Mio prend moins.
    static constexpr std::int32_t max_bios_busy_cycles = 1 << 28;

    std::int64_t clock_{};                       // cycles vus par les périphériques
    std::optional<std::int64_t> irq_raised_at_;  // levée de la ligne d'IRQ
    int bios_busy_cycles_{};                     // temps restant d'une fonction du BIOS
    std::int32_t bios_busy_return_{};            // instruction qui suit son SWI
    std::int32_t halt_return_{};                 // instruction qui suit un Halt du BIOS en cours
    int pending_cycles_{};
    int next_event_{};
    bool in_instruction_{};
    int synced_in_instruction_{};
};

} // namespace ravenemu::gba
