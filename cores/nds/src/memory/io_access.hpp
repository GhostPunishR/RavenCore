#pragma once

#include <cstdint>

// Les registres d'E/S se lisent et s'écrivent par mots de 32 bits ; un accès
// de 8 ou 16 bits n'écrit que ses octets, désignés par un masque.
namespace ravenemu::nds {

constexpr std::uint32_t io_mask(std::uint32_t address, unsigned width) noexcept {
    const auto bits = width == 4 ? 0xffff'ffffU : (1U << (8U * width)) - 1U;
    return bits << ((address & 3U) * 8U);
}
constexpr std::uint32_t io_shift(std::uint32_t address, std::uint32_t value) noexcept {
    return value << ((address & 3U) * 8U);
}
constexpr std::uint32_t io_extract(std::uint32_t word, std::uint32_t address, unsigned width) noexcept {
    const auto value = word >> ((address & 3U) * 8U);
    return width == 4 ? value : value & ((1U << (8U * width)) - 1U);
}
constexpr std::uint32_t merge(std::uint32_t old, std::uint32_t value, std::uint32_t mask) noexcept {
    return (old & ~mask) | (value & mask);
}

} // namespace ravenemu::nds
