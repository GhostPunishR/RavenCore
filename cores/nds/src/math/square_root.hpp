#pragma once

#include "machine/scheduler.hpp"

#include <cstdint>

// Racine carrée matérielle de l'ARM9, à 0x040002B0 : partie entière, sur 32
// bits, d'un entier non signé de 32 ou 64 bits ; occupée 13 cycles système
// (GBATEK, « DS Maths »).
namespace ravenemu::nds {

// Racine entière exacte, bit à bit.
constexpr std::uint32_t integer_square_root(std::uint64_t value) noexcept {
    std::uint64_t result = 0;
    std::uint64_t bit = 1ULL << 62U;
    while (bit > value) bit >>= 2U;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return static_cast<std::uint32_t>(result);
}

class SquareRoot {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now) const noexcept {
        switch (offset) {
        case 0x2b0: return control_ | (now < done_ ? 0x8000U : 0U);
        case 0x2b4: return result_;
        case 0x2b8: return static_cast<std::uint32_t>(parameter_);
        case 0x2bc: return static_cast<std::uint32_t>(parameter_ >> 32U);
        default: return 0;
        }
    }
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now) noexcept {
        const auto merge = [&](unsigned shift) {
            const auto wide_mask = std::uint64_t{mask} << shift;
            parameter_ = (parameter_ & ~wide_mask) | ((std::uint64_t{value} << shift) & wide_mask);
        };
        switch (offset) {
        case 0x2b0: control_ = ((control_ & ~mask) | (value & mask)) & 1U; break;
        case 0x2b8: merge(0); break;
        case 0x2bc: merge(32); break;
        default: return;
        }
        result_ = integer_square_root(control_ != 0 ? parameter_ : parameter_ & 0xffff'ffffULL);
        done_ = now + 13;
    }

private:
    std::uint32_t control_{};
    std::uint64_t parameter_{};
    std::uint32_t result_{};
    Cycles done_{};
};

} // namespace ravenemu::nds
