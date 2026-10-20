#pragma once

#include <ravenemu/nds/touchscreen.hpp>
#include <ravenemu/nds/firmware.hpp>

#include <cstdint>

namespace ravenemu::nds {

/** Bus SPI ARM7 : transferts asynchrones 8 bits, tactile et lecture du firmware.
 * Powerman et mode 16 bits restent diagnostiqués au premier strobe.
 * Aucune référence au bus, aux CPU ou aux entrées hôte n'est conservée.
 */
class Spi {
public:
    void reset() noexcept; // conserve l'image du firmware, réinitialise les sessions
    void load_firmware(std::span<const std::uint8_t> bytes);
    [[nodiscard]] const Firmware& firmware() const noexcept { return firmware_; }
    [[nodiscard]] std::uint8_t read_byte(unsigned offset) const noexcept;
    void validate_write(unsigned offset, std::uint32_t value, unsigned width) const;
    void write(unsigned offset, std::uint32_t value, unsigned width, const InputState& input);
    // Renvoie une demande IF.23 unique à la fin d'un transfert autorisé.
    [[nodiscard]] bool advance_cycles(std::uint64_t cycles) noexcept;
    [[nodiscard]] bool pen_irq_enabled() const noexcept { return touchscreen_.pen_irq_enabled(); }

private:
    [[nodiscard]] std::uint16_t proposed_control(unsigned offset, std::uint32_t value, unsigned width) const noexcept;
    std::uint16_t control_{};
    std::uint32_t remaining_{};
    std::uint8_t data_{}, received_{};
    Touchscreen touchscreen_, pending_touchscreen_;
    Firmware firmware_;
    Firmware::Session firmware_session_, pending_firmware_session_;
};

} // namespace ravenemu::nds
