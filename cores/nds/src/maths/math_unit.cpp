#include <ravenemu/nds/math_unit.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {
namespace {
constexpr std::uint64_t upper_word = 0xffffffff00000000ULL;

std::uint64_t extend_word(std::uint64_t value) noexcept {
    value &= 0xffffffffU;
    return (value & 0x80000000U) ? value | upper_word : value;
}

void replace_byte(std::uint64_t& target, unsigned byte, std::uint8_t value) noexcept {
    const auto shift = byte * 8U;
    target = (target & ~(std::uint64_t{0xff} << shift)) | (std::uint64_t{value} << shift);
}
} // namespace

std::optional<std::uint8_t> MathUnit::read_byte(unsigned offset) const noexcept {
    std::uint64_t value;
    unsigned byte;
    if (offset < 4) {
        value = division_mode_ | (division_by_zero_ ? 0x4000U : 0U) | (division_remaining_ ? 0x8000U : 0U);
        byte = offset;
    } else if (offset >= 0x10 && offset < 0x30) {
        switch ((offset - 0x10) / 8) {
        case 0: value = numerator_; break;
        case 1: value = denominator_; break;
        case 2: value = quotient_; break;
        default: value = remainder_; break;
        }
        byte = offset & 7U;
    } else if (offset >= 0x30 && offset < 0x34) {
        value = square_root_mode_ | (square_root_remaining_ ? 0x8000U : 0U);
        byte = offset - 0x30;
    } else if (offset >= 0x34 && offset < 0x38) {
        value = square_root_; byte = offset - 0x34;
    } else if (offset >= 0x38 && offset < 0x40) {
        value = parameter_; byte = offset - 0x38;
    } else return std::nullopt;
    return static_cast<std::uint8_t>(value >> (byte * 8U));
}

void MathUnit::validate_write(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4) || offset >= 0x40 || width > 0x40 - offset ||
        (offset & (width - 1U))) throw Error("NDS : accès DIV/SQRT invalide");
    for (unsigned i = 0; i < width; ++i)
        if (!read_byte(offset + i)) throw Error("NDS : registre DIV/SQRT non mappé");
}

void MathUnit::write(unsigned offset, std::uint32_t value, unsigned width) {
    validate_write(offset,width);
    bool divide = false, root = false;
    for (unsigned i = 0; i < width; ++i) {
        const auto address = offset + i;
        const auto byte = static_cast<std::uint8_t>(value >> (i * 8U));
        if (address < 2) {
            if (address == 0) division_mode_ = byte & 3U;
            divide = true;
        } else if (address >= 0x10 && address < 0x20) {
            replace_byte(address < 0x18 ? numerator_ : denominator_,address & 7U,byte);
            divide = true;
        } else if (address == 0x30 || address == 0x31) {
            if (address == 0x30) square_root_mode_ = byte & 1U;
            root = true;
        } else if (address >= 0x38) {
            replace_byte(parameter_,address & 7U,byte);
            root = true;
        }
        // Résultats et voies réservées : écritures ignorées, sans relance.
    }
    if (divide) start_division();
    if (root) start_square_root();
}

void MathUnit::start_division() noexcept {
    division_remaining_ = division_mode_ == 0 ? 18 : 34;
    division_by_zero_ = denominator_ == 0; // les 64 bits, même en mode 32 bits
    const auto numerator = division_mode_ == 0 ? extend_word(numerator_) : numerator_;
    const auto denominator = division_mode_ == 2 ? denominator_ : extend_word(denominator_);
    const bool negative_numerator = (numerator >> 63U) != 0;
    const bool negative_denominator = (denominator >> 63U) != 0;
    if (!denominator) {
        pending_quotient_ = negative_numerator ? 1 : ~std::uint64_t{0};
        if (division_mode_ == 0) pending_quotient_ ^= upper_word;
        pending_remainder_ = numerator;
        return;
    }
    // Magnitudes non signées : ni négation de INT64_MIN ni division signée
    // débordante. Le modulo 2^64 rend aussi le débordement 64 bits matériel.
    const auto magnitude_numerator = negative_numerator ? 0 - numerator : numerator;
    const auto magnitude_denominator = negative_denominator ? 0 - denominator : denominator;
    const auto quotient = magnitude_numerator / magnitude_denominator;
    const auto remainder = magnitude_numerator % magnitude_denominator;
    pending_quotient_ = negative_numerator != negative_denominator ? 0 - quotient : quotient;
    pending_remainder_ = negative_numerator ? 0 - remainder : remainder;
    // En mode 0, INT32_MIN/-1 donne +0x80000000 sur 64 bits : l'inversion
    // matérielle du mot haut est déjà reproduite par le calcul en magnitude.
}

void MathUnit::start_square_root() noexcept {
    square_root_remaining_ = 13;
    const auto parameter = square_root_mode_ ? parameter_ : parameter_ & 0xffffffffU;
    std::uint64_t root = 0;
    // Construction du plancher bit par bit. Un candidat reste sur 32 bits,
    // donc son carré tient toujours sur 64 bits, même pour UINT64_MAX.
    for (unsigned bit = 32; bit-- > 0;) {
        const auto candidate = root | (std::uint64_t{1} << bit);
        if (candidate * candidate <= parameter) root = candidate;
    }
    pending_square_root_ = static_cast<std::uint32_t>(root);
}

void MathUnit::advance_cycles(std::uint64_t cycles) noexcept {
    if (division_remaining_) {
        if (cycles < division_remaining_) division_remaining_ -= static_cast<std::uint8_t>(cycles);
        else {
            division_remaining_ = 0;
            quotient_ = pending_quotient_; remainder_ = pending_remainder_;
        }
    }
    if (square_root_remaining_) {
        if (cycles < square_root_remaining_) square_root_remaining_ -= static_cast<std::uint8_t>(cycles);
        else { square_root_remaining_ = 0; square_root_ = pending_square_root_; }
    }
}

} // namespace ravenemu::nds
