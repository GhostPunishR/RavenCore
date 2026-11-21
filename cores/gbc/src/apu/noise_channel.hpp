#pragma once

#include "apu/sampling.hpp"
#include "support/bits.hpp"

namespace ravenemu::cgb {

/**
 * Canal 4 (NR41-NR44) : LFSR de 15 ou 7 bits, diviseur d'horloge, enveloppe
 * de volume et longueur.
 */
struct NoiseChannel {
    static constexpr int trigger_delay_cycles = 4;
    static constexpr int restart_delay_cycles = 8;
    static constexpr int max_timer = (112 << 15) + restart_delay_cycles;
    inline static constexpr std::array<int, 8> divisors{{8, 16, 32, 48, 64, 80, 96, 112}};

    bool enabled{};
    bool dac_enabled{};
    int length{};
    bool length_enabled{};
    int envelope_initial{};
    bool envelope_add{};
    int envelope_period{};
    int volume{};
    int envelope_timer{};
    int divisor_code{};
    bool width_mode7{};
    int clock_shift{};
    int lfsr{0x7fff};
    int timer{8};
    int accumulator{};

    [[nodiscard]] int period() const noexcept { return divisors[static_cast<std::size_t>(divisor_code)] << clock_shift; }
    [[nodiscard]] int output() const noexcept { return enabled && dac_enabled && (lfsr & 1) == 0 ? volume : 0; }
    [[nodiscard]] int dac_output() const noexcept { return dac_enabled ? 15 - output() * 2 : 0; }
    void step(int cycles) noexcept {
        int remaining = cycles;
        while (remaining > 0) {
            const int slice = std::min(timer, remaining);
            accumulator += dac_output() * slice; timer -= slice; remaining -= slice;
            if (timer <= 0) {
                timer += period();
                // Les valeurs 14 et 15 déconnectent l'horloge du LFSR :
                // le timer interne avance, mais aucun nouveau bit n'est produit.
                if (clock_shift < 14) {
                    const int feedback = (lfsr ^ (lfsr >> 1)) & 1;
                    lfsr = (lfsr >> 1) | (feedback << 14);
                    if (width_mode7) lfsr = (lfsr & ~(1 << 6)) | (feedback << 6);
                }
            }
        }
    }
    double drain_average() noexcept {
        const double average = static_cast<double>(accumulator) / apu_cycles_per_sample;
        accumulator = 0; return average;
    }
    void clock_length() noexcept { if (length_enabled && length > 0 && --length == 0) enabled = false; }
    /** Même règle que pour les canaux carrés : période nulle, volume tenu. */
    void clock_envelope() noexcept {
        if (--envelope_timer > 0) return;
        envelope_timer = envelope_period != 0 ? envelope_period : 8;
        if (envelope_period == 0) return;
        volume = envelope_add ? std::min(15, volume + 1) : std::max(0, volume - 1);
    }
    void trigger(bool shorten_zero_length_reload, bool delay_envelope_clock) noexcept {
        const bool enabled_before_trigger = enabled;
        enabled = dac_enabled;
        if (length == 0) {
            length = 64;
            if (shorten_zero_length_reload) --length;
        }
        // Le premier pas du LFSR arrive une période et un M-cycle après un
        // départ, et deux M-cycles si le canal jouait encore (SameSuite,
        // channel_4_lfsr, channel_4_lfsr_restart, channel_4_lfsr_7_15).
        timer = period() + (enabled_before_trigger ? restart_delay_cycles : trigger_delay_cycles);
        volume = envelope_initial;
        envelope_timer = (envelope_period != 0 ? envelope_period : 8) +
            (delay_envelope_clock ? 1 : 0);
        lfsr = 0x7fff;
    }
    void reset() noexcept {
        enabled = false; dac_enabled = false; length = 0; length_enabled = false;
        envelope_initial = 0; envelope_add = false; envelope_period = 0; volume = 0;
        envelope_timer = 0; divisor_code = 0; width_mode7 = false; clock_shift = 0;
        lfsr = 0x7fff; timer = 8; accumulator = 0;
    }
    void save(BinaryWriter& out) const {
        out.boolean(enabled); out.boolean(dac_enabled); out.i32(length); out.boolean(length_enabled);
        out.i32(envelope_initial); out.boolean(envelope_add); out.i32(envelope_period);
        out.i32(volume); out.i32(envelope_timer); out.i32(divisor_code);
        out.boolean(width_mode7); out.i32(clock_shift); out.i32(lfsr); out.i32(timer);
        out.i32(accumulator);
    }
    void load(BinaryReader& in) {
        enabled = in.boolean(); dac_enabled = in.boolean(); length = in.bounded_i32(0, 64); length_enabled = in.boolean();
        envelope_initial = in.bounded_i32(0, 15); envelope_add = in.boolean(); envelope_period = in.bounded_i32(0, 7);
        volume = in.bounded_i32(0, 15); envelope_timer = in.bounded_i32(0, 9); divisor_code = in.bounded_i32(0, 7);
        width_mode7 = in.boolean(); clock_shift = in.bounded_i32(0, 15); lfsr = in.bounded_i32(0, 32767); timer = in.bounded_i32(1, max_timer);
        accumulator = in.bounded_i32(-1920, 1920);
    }
};

} // namespace ravenemu::cgb
