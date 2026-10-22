#pragma once

#include <cstdint>
#include <optional>

namespace ravenemu::nds {

/** DIV/SQRT ARM9, deux opérations indépendantes à l'horloge commune 33 MHz.
 * Les résultats précédents restent exposés pendant busy ; aucune IRQ.
 */
class MathUnit {
public:
    void reset() noexcept { *this = {}; }
    // Offsets relatifs à 0x04000280 ; les trous ne sont pas mappés.
    [[nodiscard]] std::optional<std::uint8_t> read_byte(unsigned offset) const noexcept;
    void validate_write(unsigned offset, unsigned width) const;
    void write(unsigned offset, std::uint32_t value, unsigned width);
    void advance_cycles(std::uint64_t cycles) noexcept;

private:
    void start_division() noexcept;
    void start_square_root() noexcept;
    std::uint64_t numerator_{}, denominator_{}, parameter_{};
    std::uint64_t quotient_{}, remainder_{}, pending_quotient_{}, pending_remainder_{};
    std::uint32_t square_root_{}, pending_square_root_{};
    std::uint8_t division_mode_{}, square_root_mode_{};
    std::uint8_t division_remaining_{}, square_root_remaining_{};
    bool division_by_zero_{};
};

} // namespace ravenemu::nds
