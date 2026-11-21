#pragma once

#include "apu/sampling.hpp"
#include "support/bits.hpp"

namespace ravenemu::cgb {

/**
 * Canal 3 (NR30-NR34) : lecture de la wave RAM, volume par décalage et
 * longueur. L'Apu arbitre les accès du CPU à la wave RAM.
 */
struct WaveChannel {
    // Le déclenchement retarde la première lecture d'échantillon de trois
    // tops du canal (6 cycles) au-delà de la période : sans ce délai, la
    // wave RAM lue ou écrite canal actif l'était un octet trop tôt
    // (Blargg, dmg_sound et cgb_sound 09 et 12).
    static constexpr int trigger_delay_cycles = 6;
    static constexpr int max_timer = 2048 * 2 + trigger_delay_cycles;

    std::array<std::uint8_t, 16> ram{};
    bool enabled{};
    bool dac_enabled{};
    int length{};
    bool length_enabled{};
    int volume_code{};
    int frequency{};
    int timer{2048 * 2};
    int position{};
    int sample_buffer{};
    int ram_access_cycles{};
    int accumulator{};

    [[nodiscard]] int output() const noexcept {
        if (!enabled || !dac_enabled) return 0;
        if (volume_code == 0) return 0;
        if (volume_code == 1) return sample_buffer;
        return volume_code == 2 ? sample_buffer >> 1 : sample_buffer >> 2;
    }
    [[nodiscard]] int dac_output() const noexcept { return dac_enabled ? 15 - output() * 2 : 0; }
    void step(int cycles) noexcept {
        int remaining = cycles;
        while (remaining > 0) {
            const int slice = std::min(timer, remaining);
            ram_access_cycles = std::max(0, ram_access_cycles - slice);
            accumulator += dac_output() * slice; timer -= slice; remaining -= slice;
            if (timer <= 0) {
                timer = (2048 - frequency) * 2; position = (position + 1) & 31;
                const int value = ram[static_cast<std::size_t>(position >> 1)];
                sample_buffer = (position & 1) == 0 ? value >> 4 : value & 0x0f;
                ram_access_cycles = 2;
            }
        }
    }
    double drain_average() noexcept {
        const double average = static_cast<double>(accumulator) / apu_cycles_per_sample;
        accumulator = 0; return average;
    }
    void clock_length() noexcept { if (length_enabled && length > 0 && --length == 0) enabled = false; }
    void trigger(bool shorten_zero_length_reload, bool /* delay_envelope_clock */) noexcept {
        enabled = dac_enabled;
        if (length == 0) {
            length = 256;
            if (shorten_zero_length_reload) --length;
        }
        timer = (2048 - frequency) * 2 + trigger_delay_cycles; position = 0; ram_access_cycles = 0;
    }
    void reset() noexcept {
        enabled = false; dac_enabled = false; length = 0; length_enabled = false;
        volume_code = 0; frequency = 0; timer = 2048 * 2;
        position = 0; sample_buffer = 0; ram_access_cycles = 0; accumulator = 0;
    }
    void save(BinaryWriter& out) const {
        out.raw(ram); out.boolean(enabled); out.boolean(dac_enabled); out.i32(length);
        out.boolean(length_enabled); out.i32(volume_code); out.i32(frequency);
        out.i32(timer); out.i32(position); out.i32(sample_buffer); out.i32(ram_access_cycles);
        out.i32(accumulator);
    }
    void load(BinaryReader& in) {
        in.raw(ram); enabled = in.boolean(); dac_enabled = in.boolean(); length = in.bounded_i32(0, 256);
        length_enabled = in.boolean(); volume_code = in.bounded_i32(0, 3); frequency = in.bounded_i32(0, 2047);
        timer = in.bounded_i32(1, max_timer); position = in.bounded_i32(0, 31); sample_buffer = in.bounded_i32(0, 15);
        ram_access_cycles = in.bounded_i32(0, 2); accumulator = in.bounded_i32(-1920, 1920);
    }
};

} // namespace ravenemu::cgb
