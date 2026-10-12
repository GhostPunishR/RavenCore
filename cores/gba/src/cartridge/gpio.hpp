#pragma once

#include "cartridge/rtc.hpp"

namespace ravenemu::gba {

class Gpio {
public:
    static constexpr int data_offset = 0xc4;
    static constexpr int direction_offset = 0xc6;
    static constexpr int control_offset = 0xc8;
    static constexpr int state_words = 23;

    /**
     * Port GPIO de la cartouche (0x080000C4..C9). Il porte l'horloge Seiko
     * (broches 0 à 2) et/ou un moteur de vibration (broche 3, sortie) ; un
     * composant absent ne répond pas.
     */
    Gpio(Rtc::Clock clock, bool rtc, bool rumble) : rtc_(std::move(clock)), has_rtc_(rtc), has_rumble_(rumble) {}
    [[nodiscard]] bool has_rtc() const noexcept { return has_rtc_; }
    [[nodiscard]] bool rumble_active() const noexcept { return has_rumble_ && (direction_ & data_ & 8) != 0; }
    static bool covers(int offset) noexcept { return offset >= 0xc4 && offset <= 0xc9; }
    int read16(int offset) const noexcept {
        if (offset == data_offset) return has_rtc_ ? rtc_.pin_state(data_, direction_) : data_ & direction_;
        if (offset == direction_offset) return direction_;
        if (offset == control_offset) return readable_ ? 1 : 0;
        return 0;
    }
    [[nodiscard]] bool readable() const noexcept { return readable_; }
    void write16(int offset, int value) {
        if (offset == data_offset) { data_ = value & 15; if (has_rtc_) rtc_.write(data_, direction_); }
        else if (offset == direction_offset) { direction_ = value & 15; if (has_rtc_) rtc_.write(data_, direction_); }
        else if (offset == control_offset) readable_ = (value & 1) != 0;
    }
    std::array<std::int32_t, state_words> export_state() const noexcept {
        std::array<std::int32_t, state_words> result{};
        result[0] = data_; result[1] = direction_; result[2] = readable_ ? 1 : 0;
        const auto rtc = rtc_.export_state();
        std::copy(rtc.begin(), rtc.end(), result.begin() + 3);
        return result;
    }
    void import_state(std::span<const std::int32_t> values) {
        if (values.size() != state_words || values[0] < 0 || values[0] > 15 ||
            values[1] < 0 || values[1] > 15) {
            throw SaveStateError("État GPIO GBA invalide");
        }
        data_ = values[0]; direction_ = values[1]; readable_ = values[2] != 0;
        rtc_.import_state(values.subspan(3));
    }

private:
    Rtc rtc_;
    bool has_rtc_{};
    bool has_rumble_{};
    int data_{};
    int direction_{};
    bool readable_{};
};

} // namespace ravenemu::gba
