#pragma once

#include <limits>

// Multiplications et arithmétique saturée (GBATEK, « Multiply and
// Multiply-Accumulate » et « Special ARM9 Instructions »). Inclus par cpu.hpp.
namespace ravenemu::arm {

namespace detail {
constexpr std::uint32_t saturate(std::int64_t value, bool& saturated) noexcept {
    constexpr auto high = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    constexpr auto low = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    if (value > high || value < low) {
        saturated = true;
        value = value > high ? high : low;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}
constexpr std::int64_t as_signed(std::uint32_t value) noexcept {
    return static_cast<std::int32_t>(value);
}
// Moitié haute (`top`) ou basse d'un registre, en signé.
constexpr std::int64_t signed_half(std::uint32_t value, bool top) noexcept {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(top ? value >> 16U : value));
}
} // namespace detail

// Cycles internes m : le multiplieur s'arrête dès que les octets de poids
// fort de l'opérande Rs sont tous à 0 (ou tous à 1 en signé).
template <Architecture Arch, Bus BusType>
int Cpu<Arch, BusType>::multiply_cycles(std::uint32_t operand, bool signed_operand) noexcept {
    int cycles = 1;
    for (unsigned shift = 8; shift < 32; shift += 8, ++cycles) {
        const auto top = operand >> shift;
        if (top == 0 || (signed_operand && top == (0xffff'ffffU >> shift))) return cycles;
    }
    return 4;
}

// MUL, MLA. Drapeaux : N et Z ; C est perdu sur l'ARMv4 (laissé tel quel ici)
// et inchangé sur l'ARMv5.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_multiply(std::uint32_t opcode) {
    const auto rd = (opcode >> 16U) & 15U;
    const auto rs = regs.r[(opcode >> 8U) & 15U];
    const auto accumulate = ((opcode >> 21U) & 1U) != 0;
    auto result = regs.r[opcode & 15U] * rs;
    if (accumulate) result += regs.r[(opcode >> 12U) & 15U];
    bus_.internal(multiply_cycles(rs, true) + (accumulate ? 1 : 0));
    if (((opcode >> 20U) & 1U) != 0) set_nz(result);
    write_register(rd, result);
}

// UMULL, UMLAL, SMULL, SMLAL : résultat de 64 bits dans RdHi:RdLo.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_multiply_long(std::uint32_t opcode) {
    const auto rd_high = (opcode >> 16U) & 15U;
    const auto rd_low = (opcode >> 12U) & 15U;
    const auto rs = regs.r[(opcode >> 8U) & 15U];
    const auto rm = regs.r[opcode & 15U];
    const auto is_signed = ((opcode >> 22U) & 1U) != 0;
    const auto accumulate = ((opcode >> 21U) & 1U) != 0;
    auto result = is_signed ? static_cast<std::uint64_t>(detail::as_signed(rm) * detail::as_signed(rs))
                            : static_cast<std::uint64_t>(rm) * rs;
    if (accumulate) result += (static_cast<std::uint64_t>(regs.r[rd_high]) << 32U) | regs.r[rd_low];
    bus_.internal(multiply_cycles(rs, is_signed) + (accumulate ? 2 : 1));
    if (((opcode >> 20U) & 1U) != 0) {
        regs.n = (result >> 63U) != 0;
        regs.z = result == 0;
    }
    write_register(rd_low, static_cast<std::uint32_t>(result));
    write_register(rd_high, static_cast<std::uint32_t>(result >> 32U));
}

// ARMv5TE : SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy. NZCV inchangés ; Q
// marque un débordement des additions de SMLAxy et SMLAWy, sans saturer.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_signed_multiply(std::uint32_t opcode) {
    const auto rd = (opcode >> 16U) & 15U;
    const auto rn = (opcode >> 12U) & 15U;
    const auto rs = regs.r[(opcode >> 8U) & 15U];
    const auto rm = regs.r[opcode & 15U];
    const auto x = ((opcode >> 5U) & 1U) != 0;
    const auto y = ((opcode >> 6U) & 1U) != 0;
    const auto accumulate_q = [this](std::int64_t product, std::uint32_t addend) {
        const auto sum = product + detail::as_signed(addend);
        if (sum != detail::as_signed(static_cast<std::uint32_t>(static_cast<std::uint64_t>(sum)))) regs.q = true;
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(sum));
    };
    switch ((opcode >> 21U) & 3U) {
    case 0:  // SMLAxy
        write_register(rd, accumulate_q(detail::signed_half(rm, x) * detail::signed_half(rs, y), regs.r[rn]));
        break;
    case 1: {  // SMLAWy (x = 0), SMULWy (x = 1) : bits 47-16 du produit de 48 bits
        const auto product = (detail::as_signed(rm) * detail::signed_half(rs, y)) >> 16;
        if (x) write_register(rd, static_cast<std::uint32_t>(static_cast<std::uint64_t>(product)));
        else write_register(rd, accumulate_q(product, regs.r[rn]));
        break;
    }
    case 2: {  // SMLALxy : RdHi:RdLo += produit ; Q inchangé
        bus_.internal(1);
        auto value = (static_cast<std::uint64_t>(regs.r[rd]) << 32U) | regs.r[rn];
        value += static_cast<std::uint64_t>(detail::signed_half(rm, x) * detail::signed_half(rs, y));
        write_register(rn, static_cast<std::uint32_t>(value));
        write_register(rd, static_cast<std::uint32_t>(value >> 32U));
        break;
    }
    default:  // SMULxy
        write_register(rd, static_cast<std::uint32_t>(
                               static_cast<std::uint64_t>(detail::signed_half(rm, x) * detail::signed_half(rs, y))));
        break;
    }
}

// ARMv5TE : QADD, QSUB, QDADD, QDSUB. Rn est d'abord doublé (et saturé) pour
// QDADD et QDSUB ; toute saturation lève Q.
template <Architecture Arch, Bus BusType>
void Cpu<Arch, BusType>::arm_saturating(std::uint32_t opcode) {
    const auto rm = detail::as_signed(regs.r[opcode & 15U]);
    auto rn = detail::as_signed(regs.r[(opcode >> 16U) & 15U]);
    const auto code = (opcode >> 21U) & 3U;
    bool saturated = false;
    if ((code & 2U) != 0) rn = detail::as_signed(detail::saturate(rn * 2, saturated));
    const auto result = detail::saturate((code & 1U) != 0 ? rm - rn : rm + rn, saturated);
    if (saturated) regs.q = true;
    write_register((opcode >> 12U) & 15U, result);
}

} // namespace ravenemu::arm
