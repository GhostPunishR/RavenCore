#pragma once

#include "interrupts/interrupt_controller.hpp"
#include "machine/scheduler.hpp"

#include <array>
#include <cstdint>

// Les quatre timers d'un CPU, à 0x04000100 : comme ceux du GBA, cadencés par
// l'horloge système de 33 MHz sur les deux CPU (GBATEK, « DS Timers » et
// « GBA Timers »). Ils avancent par rattrapage, jusqu'à la date demandée.
namespace ravenemu::nds {

class Timers {
public:
    explicit Timers(InterruptController& irq) noexcept : irq_(irq) {}

    // Avance jusqu'à `now`, débordements et cascades compris.
    void advance(Cycles now) noexcept {
        if (now <= last_) return;
        const auto elapsed = now - last_;
        last_ = now;
        for (unsigned index = 0; index < 4; ++index) {
            auto& timer = timers_[index];
            if (!running(timer) || cascades(index)) continue;
            const auto shift = prescaler_shift(timer);
            const auto total = timer.fraction + elapsed;
            timer.fraction = total & ((Cycles{1} << shift) - 1U);
            add_ticks(index, total >> shift);
        }
    }

    // Date du prochain débordement d'un timer à horloge propre.
    [[nodiscard]] Cycles next_overflow() const noexcept {
        auto next = Scheduler::never;
        for (unsigned index = 0; index < 4; ++index) {
            const auto& timer = timers_[index];
            if (!running(timer) || cascades(index)) continue;
            const auto shift = prescaler_shift(timer);
            const auto cycles = ((Cycles{0x10000} - timer.counter) << shift) - timer.fraction;
            next = std::min(next, last_ + cycles);
        }
        return next;
    }

    // Mots de 0x100 à 0x10C : compteur (bits 0-15), contrôle (bits 16-31).
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now) noexcept {
        advance(now);
        const auto& timer = timers_[(offset >> 2U) & 3U];
        return timer.counter | (std::uint32_t{timer.control} << 16U);
    }
    // Le rechargement s'écrit avant le contrôle : un mot de 32 bits qui lance
    // le timer le démarre sur la nouvelle valeur.
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now) noexcept {
        advance(now);
        auto& timer = timers_[(offset >> 2U) & 3U];
        const auto reload_mask = mask & 0xffffU;
        timer.reload = static_cast<std::uint16_t>((timer.reload & ~reload_mask) | (value & reload_mask));
        if ((mask & 0xffff'0000U) == 0) return;
        const auto control_mask = (mask >> 16U) & 0xc7U;
        const auto control = static_cast<std::uint16_t>((timer.control & ~control_mask) | ((value >> 16U) & control_mask));
        if ((control & 0x80U) != 0 && !running(timer)) {
            timer.counter = timer.reload;
            timer.fraction = 0;
        }
        timer.control = control;
    }

private:
    struct Timer {
        std::uint16_t reload{};
        std::uint16_t control{};
        std::uint32_t counter{};
        Cycles fraction{};  // cycles du prédiviseur pas encore comptés
    };

    static bool running(const Timer& timer) noexcept { return (timer.control & 0x80U) != 0; }
    [[nodiscard]] bool cascades(unsigned index) const noexcept { return index != 0 && (timers_[index].control & 4U) != 0; }
    static unsigned prescaler_shift(const Timer& timer) noexcept {
        constexpr std::array<unsigned, 4> shifts{0, 6, 8, 10};  // F/1, F/64, F/256, F/1024
        return shifts[timer.control & 3U];
    }

    void add_ticks(unsigned index, Cycles ticks) noexcept {
        auto& timer = timers_[index];
        while (ticks > 0) {
            const auto room = Cycles{0x10000} - timer.counter;
            if (ticks < room) {
                timer.counter += static_cast<std::uint32_t>(ticks);
                return;
            }
            ticks -= room;
            timer.counter = timer.reload;
            if ((timer.control & 0x40U) != 0) irq_.request(irq::timer0 + index);
            if (index < 3 && running(timers_[index + 1]) && cascades(index + 1)) add_ticks(index + 1, 1);
        }
    }

    InterruptController& irq_;
    std::array<Timer, 4> timers_{};
    Cycles last_{};
};

} // namespace ravenemu::nds
