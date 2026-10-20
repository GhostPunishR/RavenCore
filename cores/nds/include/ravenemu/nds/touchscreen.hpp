#pragma once

#include <ravenemu/nds/input.hpp>

#include <cstdint>

namespace ravenemu::nds {

/** Convertisseur série TSC2046 (DS originale), sans modèle électrique analogique.
 * Flux MSB-first, bit muet puis résultat 12/8 bits ; commandes en pipeline.
 * Température et microphone sont provisoirement des entrées nulles.
 */
class Touchscreen {
public:
    void reset() noexcept { *this = {}; }
    void deselect() noexcept;
    [[nodiscard]] bool pen_irq_enabled() const noexcept { return (power_mode_ & 1U) == 0; }
    [[nodiscard]] std::uint8_t exchange(std::uint8_t output, const InputState& input) noexcept;

private:
    void convert(std::uint8_t command, const InputState& input) noexcept;
    std::uint16_t reply_{};
    unsigned reply_bits_{};
    std::uint8_t command_{};
    unsigned command_bits_{};
    std::uint8_t power_mode_{};
};

} // namespace ravenemu::nds
