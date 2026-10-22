#pragma once

#include <ravenemu/nds/touchscreen.hpp>
#include <ravenemu/nds/firmware.hpp>
#include <ravenemu/nds/power_management.hpp>

#include <cstdint>

namespace ravenemu::nds {

/** Bus SPI ARM7 : transferts asynchrones 8 bits, Powerman, tactile et firmware.
 * Périphérique réservé et mode 16 bits restent diagnostiqués au premier strobe.
 * Aucune référence au bus, aux CPU ou aux entrées hôte n'est conservée.
 */
class Spi {
public:
    void reset() noexcept; // conserve l'image du firmware, réinitialise les sessions
    void load_firmware(std::span<const std::uint8_t> bytes);
    [[nodiscard]] const Firmware& firmware() const noexcept { return firmware_; }
    void acknowledge_firmware_saved(std::uint64_t generation) noexcept { firmware_.acknowledge_saved(generation); }
    [[nodiscard]] std::uint8_t read_byte(unsigned offset) const noexcept;
    void validate_write(unsigned offset, std::uint32_t value, unsigned width) const;
    void write(unsigned offset, std::uint32_t value, unsigned width, const InputState& input);
    // Renvoie une demande IF.23 unique à la fin d'un transfert autorisé.
    [[nodiscard]] bool advance_cycles(std::uint64_t cycles) noexcept;
    [[nodiscard]] bool pen_irq_enabled() const noexcept { return touchscreen_.pen_irq_enabled(); }
    [[nodiscard]] const PowerManagement& power_management() const noexcept { return power_management_; }
    void set_battery_low(bool low) noexcept { power_management_.set_battery_low(low); }
    // Échéance d'une extinction en cours, ou UINT64_MAX sans extinction prévue.
    [[nodiscard]] std::uint64_t cycles_until_power_off() const noexcept;

private:
    void reset_bus() noexcept;
    [[nodiscard]] std::uint16_t proposed_control(unsigned offset, std::uint32_t value, unsigned width) const noexcept;
    std::uint16_t control_{};
    std::uint32_t remaining_{};
    std::uint8_t data_{}, received_{};
    Touchscreen touchscreen_, pending_touchscreen_;
    Firmware firmware_;
    Firmware::Session firmware_session_, pending_firmware_session_;
    PowerManagement power_management_;
    PowerManagement::Session power_session_, pending_power_session_;
};

} // namespace ravenemu::nds
