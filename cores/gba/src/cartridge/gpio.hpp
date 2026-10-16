#pragma once

#include "cartridge/rtc.hpp"
#include "cartridge/sensors.hpp"

namespace ravenemu::gba {

class Gpio {
public:
    static constexpr int data_offset = 0xc4;
    static constexpr int direction_offset = 0xc6;
    static constexpr int control_offset = 0xc8;
    static constexpr int state_words = 23;
    // Capteurs, enregistrés à part depuis le format d'état 13.
    static constexpr int sensor_state_words = SolarSensor::state_words + GyroSensor::state_words;

    /**
     * Port GPIO de la cartouche (0x080000C4..C9). Il porte l'horloge Seiko
     * (broches 0 à 2), un moteur de vibration (broche 3, sortie), le capteur
     * solaire de Boktai ou le gyroscope de WarioWare: Twisted! ; un composant
     * absent ne répond pas.
     */
    Gpio(Rtc::Clock clock, Rtc::Changed rtc_changed, bool rtc, bool rumble, bool solar = false, bool gyro = false)
        : rtc_(std::move(clock), std::move(rtc_changed)),
          has_rtc_(rtc),
          has_rumble_(rumble),
          has_solar_(solar),
          has_gyro_(gyro) {}
    [[nodiscard]] bool has_rtc() const noexcept { return has_rtc_; }
    [[nodiscard]] Rtc* rtc() noexcept { return has_rtc_ ? &rtc_ : nullptr; }
    [[nodiscard]] const Rtc* rtc() const noexcept { return has_rtc_ ? &rtc_ : nullptr; }
    [[nodiscard]] bool rumble_active() const noexcept { return has_rumble_ && (direction_ & data_ & 8) != 0; }
    [[nodiscard]] bool has_solar() const noexcept { return has_solar_; }
    [[nodiscard]] bool has_gyro() const noexcept { return has_gyro_; }
    [[nodiscard]] SolarSensor* solar() noexcept { return has_solar_ ? &solar_ : nullptr; }
    [[nodiscard]] GyroSensor* gyro() noexcept { return has_gyro_ ? &gyro_ : nullptr; }
    static bool covers(int offset) noexcept { return offset >= 0xc4 && offset <= 0xc9; }
    int read16(int offset) const noexcept {
        if (offset == data_offset) return pins();
        if (offset == direction_offset) return direction_;
        if (offset == control_offset) return readable_ ? 1 : 0;
        return 0;
    }
    [[nodiscard]] bool readable() const noexcept { return readable_; }
    void write16(int offset, int value) {
        if (offset == data_offset) { data_ = value & 15; drive_pins(); }
        else if (offset == direction_offset) { direction_ = value & 15; drive_pins(); }
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
    std::array<std::int32_t, sensor_state_words> export_sensor_state() const noexcept {
        std::array<std::int32_t, sensor_state_words> result{};
        const auto solar = solar_.export_state();
        const auto gyro = gyro_.export_state();
        std::copy(solar.begin(), solar.end(), result.begin());
        std::copy(gyro.begin(), gyro.end(), result.begin() + SolarSensor::state_words);
        return result;
    }
    void import_sensor_state(std::span<const std::int32_t> values) {
        if (values.size() != sensor_state_words) throw SaveStateError("État des capteurs GPIO GBA invalide");
        solar_.import_state(values.first(SolarSensor::state_words));
        gyro_.import_state(values.subspan(SolarSensor::state_words));
    }

private:
    // Niveau des broches vu par le jeu : sorties telles qu'écrites, entrées
    // fournies par les composants.
    [[nodiscard]] int pins() const noexcept {
        auto result = has_rtc_ ? rtc_.pin_state(data_, direction_) : data_ & direction_;
        if (has_solar_ && (direction_ & 8) == 0 && solar_.comparator()) result |= 8;
        if (has_gyro_ && (direction_ & 4) == 0) result |= gyro_.data_out() << 2;
        return result;
    }
    void drive_pins() {
        if (has_rtc_) rtc_.write(data_, direction_);
        if (has_solar_) solar_.write(data_, direction_);
        if (has_gyro_) gyro_.write(data_, direction_);
    }
    Rtc rtc_;
    SolarSensor solar_;
    GyroSensor gyro_;
    bool has_rtc_{};
    bool has_rumble_{};
    bool has_solar_{};
    bool has_gyro_{};
    int data_{};
    int direction_{};
    bool readable_{};
};

} // namespace ravenemu::gba
