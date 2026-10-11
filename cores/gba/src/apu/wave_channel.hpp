#pragma once

#include "support/bits.hpp"

namespace ravenemu::gba {

class WaveChannel {
public:
    void trigger() noexcept {
        enabled = dac_enabled;
        if (length_counter == 0) length_counter = 256;
        timer_ = (2048 - frequency) * 2;
        position_ = 0;
    }
    /**
     * Fait avancer la lecture, en cumulant chaque point le temps qu'il dure.
     *
     * Même raison que pour les canaux carrés : c'est la moyenne sur la fenêtre
     * d'un échantillon qui doit être prélevée, non la valeur qui se trouve là
     * à l'instant du prélèvement.
     */
    void tick(int cycles) noexcept {
        if (!enabled) return;
        const auto period = std::max(1, (2048 - frequency) * 2);
        int remaining = cycles;
        while (remaining > 0) {
            if (timer_ <= 0) timer_ = period;
            const int slice = std::min(timer_, remaining);
            accumulator_ += output() * slice;
            timer_ -= slice;
            remaining -= slice;
            if (timer_ <= 0) {
                timer_ = period;
                position_ = (position_ + 1) & 31;
            }
        }
    }
    /**
     * Rend le cumul de la fenêtre écoulée et le remet à zéro.
     *
     * Le cumul est rendu **tel quel**, sans division : c'est le mélangeur qui
     * divise, une seule fois, à la toute fin. Diviser ici demanderait un
     * flottant — la moyenne n'est presque jamais entière — et ferait dépendre
     * la sortie de l'ordre des arrondis. Tout le mixage reste ainsi en entiers
     * exacts, ce qui vaut aussi pour le modèle de référence Kotlin auquel ce
     * cœur est confronté trame par trame.
     */
    [[nodiscard]] int drain_accumulator() noexcept {
        const auto total = accumulator_;
        accumulator_ = 0;
        return total;
    }
    void clock_length() noexcept {
        if (length_enabled && length_counter > 0 && --length_counter == 0) enabled = false;
    }
    [[nodiscard]] int output() const noexcept {
        if (!enabled || !dac_enabled || volume_code == 0) return 0;
        const auto sample = wave_ram[static_cast<std::size_t>(position_)];
        return volume_code == 1 ? sample : volume_code == 2 ? sample >> 1 : sample >> 2;
    }
    void reset() noexcept {
        enabled = false; timer_ = 0; position_ = 0; length_counter = 0; accumulator_ = 0;
    }
    bool enabled{};
    bool dac_enabled{};
    int frequency{};
    int length_counter{};
    bool length_enabled{};
    int volume_code{};
    std::array<int, 32> wave_ram{};
    void save(BinaryWriter& out) const {
        out.boolean(enabled);
        out.boolean(dac_enabled);
        out.i32(frequency);
        out.i32(length_counter);
        out.boolean(length_enabled);
        out.i32(volume_code);
        out.i32(timer_);
        out.i32(accumulator_);
        out.i32(position_);
        for (auto value : wave_ram) out.i32(value);
    }
    void load(BinaryReader& in) {
        enabled = in.boolean();
        dac_enabled = in.boolean();
        frequency = in.bounded_i32(0, 2047);
        length_counter = in.bounded_i32(0, 256);
        length_enabled = in.boolean();
        volume_code = in.bounded_i32(0, 3);
        timer_ = in.bounded_i32(0, 4096);
        accumulator_ = in.bounded_i32(0, 1920);
        position_ = in.bounded_i32(0, 31);
        for (auto& value : wave_ram) value = in.bounded_i32(0, 15);
    }
private:
    int timer_{};
    int accumulator_{};
    int position_{};
};

} // namespace ravenemu::gba
