#pragma once

#include <cstdint>

namespace ravenemu::nds {

/** Mitsumi 3152A de la DS originale ; état possédé, sans référence à l'hôte.
 * Les valeurs initiales servent au démarrage direct, pas à simuler une rampe
 * d'alimentation analogique. Batterie bonne par défaut, réglable par l'hôte.
 */
class PowerManagement {
public:
    void reset() noexcept; // conserve l'entrée batterie de l'hôte
    void set_battery_low(bool low) noexcept { battery_low_ = low; }
    [[nodiscard]] bool battery_low() const noexcept { return battery_low_; }
    [[nodiscard]] std::uint8_t control() const noexcept { return control_; }
    [[nodiscard]] std::uint8_t microphone_control() const noexcept { return microphone_control_; }
    [[nodiscard]] std::uint8_t microphone_gain() const noexcept { return microphone_gain_; }
    [[nodiscard]] bool powered_off() const noexcept { return (control_ & 0x40U) != 0; }
    [[nodiscard]] bool upper_backlight_enabled() const noexcept { return !powered_off() && (control_ & 8U); }
    [[nodiscard]] bool lower_backlight_enabled() const noexcept { return !powered_off() && (control_ & 4U); }

private:
    friend class Spi;
    struct Session {
        std::uint8_t index{}, data{};
        bool selected{}, write_pending{};
    };
    [[nodiscard]] std::uint8_t exchange(Session& session, std::uint8_t output) const noexcept;
    void complete_byte(Session& session) noexcept;
    [[nodiscard]] static bool requests_power_off(const Session& session) noexcept {
        return session.write_pending && (session.index & 3U) == 0 && (session.data & 0x40U);
    }
    std::uint8_t control_{0x0d}; // amplificateur et deux rétroéclairages activés
    std::uint8_t microphone_control_{}, microphone_gain_{1};
    bool battery_low_{};
};

} // namespace ravenemu::nds
