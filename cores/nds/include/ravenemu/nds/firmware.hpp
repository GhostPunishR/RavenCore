#pragma once

#include <ravenemu/nds/input.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ravenemu::nds {

inline constexpr std::uint32_t firmware_settings_address = 0x027ffc80;

struct TouchCalibrationPoint {
    std::uint16_t adc_x{}, adc_y{};
    std::uint8_t screen_x{}, screen_y{}; // origine firmware : (1,1)
};

struct TouchCalibration {
    TouchCalibrationPoint first, second;
    [[nodiscard]] bool valid() const noexcept;
    // Pixels hôte d'origine (0,0), bornés à 256x192 ; extrapolation saturée ADC.
    [[nodiscard]] TouchSample sample_at(std::uint16_t x, std::uint16_t y) const;
};

struct FirmwareSettings {
    std::array<std::uint8_t, 0x70> data{};
    std::uint32_t offset{};
    std::uint16_t counter{};
    std::optional<TouchCalibration> touch_calibration;
};

/** Image propriétaire 256/512 Kio de firmware DS, sans authentification du code.
 * Les réglages sont sélectionnés par CRC16 et compteur modulo 128.
 * Un firmware sans réglages valides reste lisible par SPI, sans étalonnage.
 */
class Firmware {
public:
    Firmware() = default;
    explicit Firmware(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::optional<FirmwareSettings>& settings() const noexcept { return settings_; }

private:
    friend class Spi;
    // Session série petite et copiable : aucune copie de l'image par octet SPI.
    struct Session {
        enum class Phase { command, address, dummy, data, id, status, ignore };
        Phase phase{Phase::command};
        std::uint32_t address{};
        unsigned address_bytes{}, id_index{};
        std::uint8_t command{};
        bool asleep{}, write_enabled{};
        void deselect() noexcept;
    };
    [[nodiscard]] std::uint8_t exchange(Session& session, std::uint8_t output) const;
    std::vector<std::uint8_t> bytes_;
    std::optional<FirmwareSettings> settings_;
};

} // namespace ravenemu::nds
