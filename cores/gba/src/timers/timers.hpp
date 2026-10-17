#pragma once

#include "memory/bus.hpp"

namespace ravenemu::gba {

class Timers {
public:
    explicit Timers(InterruptController& interrupts) : interrupts_(interrupts) {}
    // La recharge écrite ne sert qu'à partir du cycle suivant : un débordement
    // dans le cycle même de l'écriture recharge encore l'ancienne valeur.
    // Mesuré sur console : FFFF puis 0 écrits coup sur coup, le compteur
    // déborde deux fois, et passe à 0 au même cycle qu'avec FFFE.
    void reload_write(int timer, int value) {
        flush();
        next_reload_[static_cast<std::size_t>(timer)] = value & 0xffff;
    }
    void control_write(int timer, int value) {
        flush();
        const auto index = static_cast<std::size_t>(timer);
        const auto was_enabled = (control_[index] & 0x80) != 0;
        control_[index] = value & 0xffff;
        if (!was_enabled && (value & 0x80) != 0) {
            // Une écriture 32 bits donne recharge et contrôle d'un coup : le
            // timer part de la recharge qu'elle vient d'écrire.
            counter_[index] = next_reload_[index] != no_reload ? next_reload_[index] : reload_[index];
            prescaler_counter_[index] = 0;
            // Le compteur part 2 cycles après l'écriture qui active le timer.
            start_delay_[index] = start_latency;
        }
        cycles_until_overflow_ = compute_budget();
    }
    int counter(int timer) {
        const auto index = static_cast<std::size_t>(timer);
        if (pending_cycles_ < cycles_until_overflow_) {
            const auto settings = control_[index];
            if ((settings & 0x80) == 0 || (settings & 0x04) != 0) return counter_[index];
            const auto prescale = prescale_[static_cast<std::size_t>(settings & 3)];
            const auto running = std::max(0, pending_cycles_ - start_delay_[index]);
            return counter_[index] + (prescaler_counter_[index] + running) / prescale;
        }
        flush();
        return counter_[index];
    }
    [[nodiscard]] int control(int timer) const noexcept { return control_[static_cast<std::size_t>(timer)]; }
    [[nodiscard]] int cycles_until_next_overflow() const noexcept {
        if (cycles_until_overflow_ == no_overflow) return no_overflow;
        return std::max(1, cycles_until_overflow_ - pending_cycles_);
    }
    void tick(int cycles) {
        if (cycles_until_overflow_ == no_overflow) {
            // Aucun timer ne compte : rien ne peut déborder avec l'ancienne recharge.
            if (cycles > 0) commit_reloads();
            return;
        }
        pending_cycles_ += cycles;
        if (pending_cycles_ >= cycles_until_overflow_) flush();
    }
    std::array<std::int32_t, 16> export_state() {
        flush();
        std::array<std::int32_t, 16> result{};
        std::copy(counter_.begin(), counter_.end(), result.begin());
        std::copy(reload_.begin(), reload_.end(), result.begin() + 4);
        std::copy(control_.begin(), control_.end(), result.begin() + 8);
        std::copy(prescaler_counter_.begin(), prescaler_counter_.end(), result.begin() + 12);
        return result;
    }
    void import_state(std::span<const std::int32_t> state) {
        if (state.size() != 16) throw SaveStateError("État timers GBA invalide");
        // Compteur, recharge et contrôle sont des registres 16 bits : une
        // recharge à 0x10000 donnerait une période nulle (division par zéro
        // dans `advance`). Le pré-diviseur reste sous le plus grand diviseur.
        const auto out_of_range = [](std::int32_t value, std::int32_t maximum) {
            return value < 0 || value > maximum;
        };
        for (std::size_t i = 0; i < 12; ++i) {
            if (out_of_range(state[i], 0xffff)) throw SaveStateError("État timers GBA invalide");
        }
        for (std::size_t i = 12; i < 16; ++i) {
            if (out_of_range(state[i], prescale_.back() - 1)) throw SaveStateError("État timers GBA invalide");
        }
        std::copy_n(state.begin(), 4, counter_.begin());
        std::copy_n(state.begin() + 4, 4, reload_.begin());
        std::copy_n(state.begin() + 8, 4, control_.begin());
        std::copy_n(state.begin() + 12, 4, prescaler_counter_.begin());
        pending_cycles_ = 0;
        cycles_until_overflow_ = compute_budget();
    }
    // Format 15 : recharges écrites qui ne servent pas encore (-1 : aucune).
    std::array<std::int32_t, 4> export_pending_reloads() {
        flush();
        return next_reload_;
    }
    void import_pending_reloads(std::span<const std::int32_t> state) {
        if (state.size() != 4) throw SaveStateError("État timers GBA invalide");
        for (const auto value : state) {
            if (value < no_reload || value > 0xffff) throw SaveStateError("État timers GBA invalide");
        }
        std::copy_n(state.begin(), 4, next_reload_.begin());
    }
    // Format 14 : cycles restant avant que chaque timer activé ne compte.
    std::array<std::int32_t, 4> export_start_delay() {
        flush();
        std::array<std::int32_t, 4> result{};
        std::copy(start_delay_.begin(), start_delay_.end(), result.begin());
        return result;
    }
    void import_start_delay(std::span<const std::int32_t> state) {
        if (state.size() != 4) throw SaveStateError("État timers GBA invalide");
        for (const auto value : state) {
            if (value < 0 || value > start_latency) throw SaveStateError("État timers GBA invalide");
        }
        std::copy_n(state.begin(), 4, start_delay_.begin());
        cycles_until_overflow_ = compute_budget();
    }
    void reset() noexcept {
        counter_.fill(0); reload_.fill(0); control_.fill(0); prescaler_counter_.fill(0); start_delay_.fill(0);
        next_reload_.fill(no_reload);
        pending_cycles_ = 0; cycles_until_overflow_ = no_overflow;
    }
    std::function<void(int)> on_overflow;

private:
    static constexpr int no_overflow = std::numeric_limits<int>::max();
    static constexpr std::array prescale_{1, 64, 256, 1024};
    static constexpr int start_latency = 2;
    static constexpr int no_reload = -1;
    void flush() {
        auto cycles = std::exchange(pending_cycles_, 0);
        // Le premier cycle après une écriture de recharge garde l'ancienne.
        const auto reload_pending = std::any_of(next_reload_.begin(), next_reload_.end(),
                                                [](int value) { return value != no_reload; });
        if (cycles > 0 && reload_pending) {
            apply_cycles(1);
            --cycles;
            commit_reloads();
        }
        if (cycles > 0) apply_cycles(cycles);
        cycles_until_overflow_ = compute_budget();
    }
    void commit_reloads() noexcept {
        for (std::size_t i = 0; i < 4; ++i) {
            if (next_reload_[i] != no_reload) reload_[i] = std::exchange(next_reload_[i], no_reload);
        }
    }
    [[nodiscard]] int compute_budget() const noexcept {
        auto budget = no_overflow;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto settings = control_[i];
            if ((settings & 0x80) == 0 || (settings & 0x04) != 0) continue;
            const auto divisor = prescale_[static_cast<std::size_t>(settings & 3)];
            const auto cycles = (0x10000 - counter_[i]) * divisor - prescaler_counter_[i] + start_delay_[i];
            budget = std::min(budget, cycles);
        }
        return budget;
    }
    void apply_cycles(int cycles) {
        for (std::size_t i = 0; i < 4; ++i) {
            const auto settings = control_[i];
            if ((settings & 0x80) == 0 || (settings & 0x04) != 0) continue;
            const auto divisor = prescale_[static_cast<std::size_t>(settings & 3)];
            const auto waited = std::min(start_delay_[i], cycles);
            start_delay_[i] -= waited;
            if (cycles == waited) continue;
            const auto total = prescaler_counter_[i] + cycles - waited;
            prescaler_counter_[i] = total % divisor;
            if (total >= divisor) advance(static_cast<int>(i), total / divisor);
        }
    }
    void advance(int timer, int ticks) {
        const auto index = static_cast<std::size_t>(timer);
        const auto value = counter_[index] + ticks;
        if (value <= 0xffff) {
            counter_[index] = value;
            return;
        }
        const auto period = 0x10000 - reload_[index];
        const auto excess = value - 0x10000;
        const auto overflows = 1 + excess / period;
        counter_[index] = reload_[index] + excess % period;
        for (int i = 0; i < overflows; ++i) overflow(timer);
    }
    void overflow(int timer) {
        const auto index = static_cast<std::size_t>(timer);
        if ((control_[index] & 0x40) != 0) interrupts_.request(InterruptController::timer0 + timer);
        if (on_overflow) on_overflow(timer);
        const auto next = timer + 1;
        if (next < 4) {
            const auto settings = control_[static_cast<std::size_t>(next)];
            if ((settings & 0x84) == 0x84) advance(next, 1);
        }
    }

    InterruptController& interrupts_;
    std::array<int, 4> counter_{};
    std::array<int, 4> reload_{};
    std::array<int, 4> control_{};
    std::array<int, 4> prescaler_counter_{};
    std::array<int, 4> start_delay_{};
    std::array<std::int32_t, 4> next_reload_{no_reload, no_reload, no_reload, no_reload};
    int pending_cycles_{};
    int cycles_until_overflow_{no_overflow};
};

} // namespace ravenemu::gba
