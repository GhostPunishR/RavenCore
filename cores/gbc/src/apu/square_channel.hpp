#pragma once

#include "apu/sampling.hpp"
#include "support/bits.hpp"

namespace ravenemu::cgb {

/**
 * Canaux carrés 1 et 2 (NR10-NR14, NR21-NR24) : minuteur de fréquence,
 * cycle de service, enveloppe de volume, longueur et, pour le canal 1,
 * balayage de fréquence. L'Apu les cadence et décode leurs registres.
 */
struct SquareChannel {
    static constexpr int trigger_delay_cycles = 8;
    static constexpr int restart_delay_cycles = 4;
    static constexpr int max_timer = 2048 * 4 + trigger_delay_cycles + 3;
    // Formes d'onde des quatre rapports cycliques : 12,5 %, 25 %, 50 % et 75 %.
    inline static constexpr std::array<std::array<int, 8>, 4> duty_table{{
        {{0, 0, 0, 0, 0, 0, 0, 1}}, {{1, 0, 0, 0, 0, 0, 0, 1}},
        {{1, 0, 0, 0, 0, 1, 1, 1}}, {{0, 1, 1, 1, 1, 1, 1, 0}},
    }};

    explicit SquareChannel(bool sweep) : has_sweep(sweep) {}
    bool has_sweep{};
    bool enabled{};
    bool dac_enabled{};
    int duty{};
    // Rapport cyclique de l'échantillon en cours, pris à son début.
    int sample_duty{};
    int length{};
    bool length_enabled{};
    int envelope_initial{};
    bool envelope_add{};
    int envelope_period{};
    int volume{};
    int envelope_timer{};
    int frequency{};
    int timer{2048 * 4};
    int duty_position{};
    int sweep_period{};
    bool sweep_negate{};
    int sweep_shift{};
    int sweep_timer{};
    bool sweep_enabled{};
    int shadow_frequency{};
    bool sweep_negate_used{};
    int accumulator{};
    bool timer_running{};
    bool force_first_step_zero{true};

    [[nodiscard]] int output() const noexcept {
        return enabled && dac_enabled && !force_first_step_zero &&
            duty_table[static_cast<std::size_t>(sample_duty)][static_cast<std::size_t>(duty_position)] == 1
            ? volume : 0;
    }
    [[nodiscard]] int dac_output() const noexcept { return dac_enabled ? 15 - output() * 2 : 0; }
    void step(int cycles) noexcept {
        // Arrêté (longueur écoulée, DAC coupé, balayage hors limites), le
        // canal fige son minuteur et sa position dans le cycle.
        if (!timer_running || !enabled) {
            accumulator += dac_output() * cycles;
            return;
        }
        int remaining = cycles;
        while (remaining > 0) {
            const int slice = std::min(timer, remaining);
            accumulator += dac_output() * slice; timer -= slice; remaining -= slice;
            if (timer <= 0) {
                timer = (2048 - frequency) * 4;
                duty_position = (duty_position + 1) & 7;
                // Un nouveau rapport cyclique n'agit qu'à l'échantillon
                // suivant (SameSuite, channel_1_duty_delay).
                sample_duty = duty;
                force_first_step_zero = false;
            }
        }
    }
    double drain_average() noexcept {
        const double average = static_cast<double>(accumulator) / apu_cycles_per_sample;
        accumulator = 0; return average;
    }
    void clock_length() noexcept { if (length_enabled && length > 0 && --length == 0) enabled = false; }
    /**
     * Cadence l'enveloppe de volume, à 64 Hz.
     *
     * Deux règles distinctes, et c'est leur confusion qui coûte cher :
     *
     * - le **compteur** traite une période nulle comme une période de
     *   huit ; c'est ce qui fixe l'instant du prochain top si le jeu
     *   réécrit NRx2 avec une période non nulle sans redéclencher ;
     * - le **volume**, lui, ne bouge que si la période est non nulle. Une
     *   période nulle veut dire « pas d'enveloppe » : le volume programmé
     *   est tenu tant que le canal joue.
     *
     * Ne garder que la première règle fait perdre un cran de volume toutes
     * les huit périodes à *tout* son de volume fixe, soit une extinction
     * complète en moins de deux secondes. C'est le cas le plus courant de
     * la musique Game Boy, qui tient ses notes à volume constant : les
     * tenues et les basses disparaissaient en cours de morceau.
     */
    void clock_envelope() noexcept {
        if (--envelope_timer > 0) return;
        envelope_timer = envelope_period != 0 ? envelope_period : 8;
        if (envelope_period == 0) return;
        volume = envelope_add ? std::min(15, volume + 1) : std::max(0, volume - 1);
    }
    int next_sweep_frequency() noexcept {
        const int delta = shadow_frequency >> sweep_shift;
        const int next = sweep_negate ? shadow_frequency - delta : shadow_frequency + delta;
        if (sweep_negate) sweep_negate_used = true;
        if (next > 2047) enabled = false;
        return next;
    }
    void clock_sweep() noexcept {
        if (!has_sweep || !sweep_enabled) return;
        if (--sweep_timer <= 0) {
            sweep_timer = sweep_period != 0 ? sweep_period : 8;
            if (sweep_period != 0) {
                const int next = next_sweep_frequency();
                if (next <= 2047 && sweep_shift != 0) {
                    shadow_frequency = next; frequency = next; static_cast<void>(next_sweep_frequency());
                }
            }
        }
    }
    void trigger(bool shorten_zero_length_reload, bool delay_envelope_clock) noexcept {
        const bool playing = enabled && timer_running;
        enabled = dac_enabled;
        if (length == 0) {
            length = 64;
            if (shorten_zero_length_reload) --length;
        }
        // Le premier échantillon arrive une période et deux tops de 1 MHz
        // après le déclenchement ; un seul top si le canal jouait encore.
        // La position dans le cycle de service ne revient pas à zéro, même
        // après un arrêt : seule l'extinction de l'APU (NR52) l'efface
        // (SameSuite, channel_1_delay, channel_1_restart et
        // channel_1_stop_restart).
        timer = ((2048 - frequency) * 4 +
                 (playing ? restart_delay_cycles : trigger_delay_cycles)) | (timer & 3);
        timer_running = true;
        volume = envelope_initial;
        envelope_timer = (envelope_period != 0 ? envelope_period : 8) +
            (delay_envelope_clock ? 1 : 0);
        if (has_sweep) {
            shadow_frequency = frequency; sweep_timer = sweep_period != 0 ? sweep_period : 8;
            sweep_enabled = sweep_period != 0 || sweep_shift != 0;
            sweep_negate_used = false;
            if (sweep_shift != 0) static_cast<void>(next_sweep_frequency());
        }
    }
    void reset() noexcept {
        enabled = false; dac_enabled = false; duty = 0; sample_duty = 0; length = 0; length_enabled = false;
        envelope_initial = 0; envelope_add = false; envelope_period = 0; volume = 0;
        envelope_timer = 0; frequency = 0; duty_position = 0; sweep_period = 0;
        sweep_negate = false; sweep_shift = 0; sweep_timer = 0; sweep_enabled = false;
        shadow_frequency = 0; sweep_negate_used = false;
        timer = 2048 * 4; accumulator = 0; timer_running = false;
        force_first_step_zero = true;
    }
    void save(BinaryWriter& out) const {
        out.boolean(enabled); out.boolean(dac_enabled); out.i32(duty); out.i32(length);
        out.boolean(length_enabled); out.i32(envelope_initial); out.boolean(envelope_add);
        out.i32(envelope_period); out.i32(volume); out.i32(envelope_timer);
        out.i32(frequency); out.i32(timer); out.i32(duty_position); out.i32(sweep_period);
        out.boolean(sweep_negate); out.i32(sweep_shift); out.i32(sweep_timer);
        out.boolean(sweep_enabled); out.i32(shadow_frequency); out.boolean(sweep_negate_used);
        out.i32(accumulator); out.boolean(timer_running); out.boolean(force_first_step_zero);
    }
    void load(BinaryReader& in) {
        enabled = in.boolean(); dac_enabled = in.boolean(); duty = in.bounded_i32(0, 3); length = in.bounded_i32(0, 64);
        length_enabled = in.boolean(); envelope_initial = in.bounded_i32(0, 15); envelope_add = in.boolean();
        envelope_period = in.bounded_i32(0, 7); volume = in.bounded_i32(0, 15); envelope_timer = in.bounded_i32(0, 9);
        frequency = in.bounded_i32(0, 2047); timer = in.bounded_i32(1, max_timer); duty_position = in.bounded_i32(0, 7); sweep_period = in.bounded_i32(0, 7);
        sweep_negate = in.boolean(); sweep_shift = in.bounded_i32(0, 7); sweep_timer = in.bounded_i32(0, 8);
        sweep_enabled = in.boolean(); shadow_frequency = in.bounded_i32(0, 2047);
        sweep_negate_used = in.boolean(); accumulator = in.bounded_i32(-1920, 1920);
        timer_running = in.boolean(); force_first_step_zero = in.boolean();
    }
};

} // namespace ravenemu::cgb
