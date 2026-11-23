#pragma once

#include "machine/scheduler.hpp"

#include <cstdint>
#include <limits>

// Diviseur matériel de l'ARM9, à 0x04000280 (GBATEK, « DS Maths »). Le
// résultat est calculé dès l'écriture ; le drapeau « occupé » dure le temps
// de la vraie division, 18 ou 34 cycles système.
namespace ravenemu::nds {

class Divider {
public:
    // `offset` : adresse du mot d'E/S, de 0x280 à 0x2AC.
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, Cycles now) const noexcept {
        switch (offset) {
        case 0x280: return control_ | (by_zero_ ? 0x4000U : 0U) | (now < done_ ? 0x8000U : 0U);
        case 0x290: return low(numerator_);
        case 0x294: return high(numerator_);
        case 0x298: return low(denominator_);
        case 0x29c: return high(denominator_);
        case 0x2a0: return low(quotient_);
        case 0x2a4: return high(quotient_);
        case 0x2a8: return low(remainder_);
        case 0x2ac: return high(remainder_);
        default: return 0;
        }
    }
    // Écrire le mode, le numérateur ou le dénominateur relance la division.
    void write(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, Cycles now) noexcept {
        switch (offset) {
        case 0x280: control_ = ((control_ & ~mask) | (value & mask)) & 3U; break;
        case 0x290: numerator_ = merge(numerator_, value, mask, 0); break;
        case 0x294: numerator_ = merge(numerator_, value, mask, 32); break;
        case 0x298: denominator_ = merge(denominator_, value, mask, 0); break;
        case 0x29c: denominator_ = merge(denominator_, value, mask, 32); break;
        default: return;
        }
        start(now);
    }

private:
    static std::uint32_t low(std::uint64_t value) noexcept { return static_cast<std::uint32_t>(value); }
    static std::uint32_t high(std::uint64_t value) noexcept { return static_cast<std::uint32_t>(value >> 32U); }
    static std::uint64_t merge(std::uint64_t old, std::uint32_t value, std::uint32_t mask, unsigned shift) noexcept {
        const auto wide_mask = std::uint64_t{mask} << shift;
        return (old & ~wide_mask) | ((std::uint64_t{value} << shift) & wide_mask);
    }
    static std::int64_t as_signed(std::uint64_t value) noexcept { return static_cast<std::int64_t>(value); }
    static std::uint64_t as_unsigned(std::int64_t value) noexcept { return static_cast<std::uint64_t>(value); }

    void start(Cycles now) noexcept {
        // DIV0 n'est signalé que si le dénominateur complet de 64 bits est nul.
        by_zero_ = denominator_ == 0;
        if (control_ == 0) {
            divide32();
            done_ = now + 18;
        } else {
            divide64(control_ == 2 ? as_signed(denominator_)
                                   : static_cast<std::int32_t>(static_cast<std::uint32_t>(denominator_)));
            done_ = now + 34;
        }
    }

    // 32/32 : sur débordement (division par zéro, -MAX/-1), les 32 bits hauts
    // du quotient étendu sont inversés.
    void divide32() noexcept {
        const auto numerator = static_cast<std::int32_t>(static_cast<std::uint32_t>(numerator_));
        const auto denominator = static_cast<std::int32_t>(static_cast<std::uint32_t>(denominator_));
        std::int32_t quotient{};
        std::int32_t remainder{};
        bool overflow = true;
        if (denominator == 0) {
            quotient = numerator < 0 ? 1 : -1;
            remainder = numerator;
        } else if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1) {
            quotient = numerator;
        } else {
            quotient = numerator / denominator;
            remainder = numerator % denominator;
            overflow = false;
        }
        quotient_ = as_unsigned(quotient);
        if (overflow) quotient_ ^= 0xffff'ffff'0000'0000ULL;
        remainder_ = as_unsigned(remainder);
    }

    void divide64(std::int64_t denominator) noexcept {
        const auto numerator = as_signed(numerator_);
        if (denominator == 0) {
            quotient_ = as_unsigned(numerator < 0 ? 1 : -1);
            remainder_ = numerator_;
        } else if (numerator == std::numeric_limits<std::int64_t>::min() && denominator == -1) {
            quotient_ = numerator_;
            remainder_ = 0;
        } else {
            quotient_ = as_unsigned(numerator / denominator);
            remainder_ = as_unsigned(numerator % denominator);
        }
    }

    std::uint32_t control_{};
    bool by_zero_{};
    std::uint64_t numerator_{};
    std::uint64_t denominator_{};
    std::uint64_t quotient_{};
    std::uint64_t remainder_{};
    Cycles done_{};
};

} // namespace ravenemu::nds
