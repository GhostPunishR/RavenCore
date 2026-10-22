#include "interpreter.hpp"

#include <algorithm>

namespace ravenemu::nds::detail {
namespace {
constexpr std::int64_t minimum = std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t maximum = std::numeric_limits<std::int32_t>::max();
std::int64_t signed_word(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
std::int64_t signed_half(std::uint32_t value, bool top) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value >> (top ? 16U : 0U)));
}
} // namespace

// Implémentation originale des opérations ARMv5TE décrites dans GBATEK :
// Special ARM9 Instructions et Signed Halfword Multiply. Aucun calcul signé
// ne déborde ; seules les conversions vers les résultats non signés bouclent.
void Interpreter::arm_saturating(std::uint32_t raw) {
    const auto rd = (raw >> 12U) & 15U, rm = raw & 15U, rn = (raw >> 16U) & 15U;
    if (cpu != Processor::arm9 || rd == 15 || rm == 15 || rn == 15) unsupported();
    const auto saturate = [&](std::int64_t value) {
        if (value < minimum || value > maximum) s.sticky_overflow = true;
        return std::clamp(value,minimum,maximum);
    };
    const auto first = signed_word(reg(rm));
    auto second = signed_word(reg(rn));
    if (raw & 0x00400000U) second = saturate(second * 2);
    const auto result = saturate((raw & 0x00200000U) ? first - second : first + second);
    s.registers[rd] = static_cast<std::uint32_t>(result);
}

void Interpreter::arm_dsp_multiply(std::uint32_t raw) {
    const auto operation = (raw >> 21U) & 3U;
    const auto rd = (raw >> 16U) & 15U, rn = (raw >> 12U) & 15U;
    const auto rs = (raw >> 8U) & 15U, rm = raw & 15U;
    const bool x = (raw & 0x20U) != 0, y = (raw & 0x40U) != 0;
    const bool accumulate = operation != 3 && !(operation == 1 && x);
    if (cpu != Processor::arm9 || rd == 15 || rm == 15 || rs == 15 ||
        (accumulate ? rn == 15 : rn != 0) || (operation == 2 && rd == rn)) unsupported();
    // Capturer toutes les sources avant les destinations permet leurs recouvrements ARM9.
    const auto product = operation == 1 ? (signed_word(reg(rm)) * signed_half(reg(rs),y)) >> 16 :
                                         signed_half(reg(rm),x) * signed_half(reg(rs),y);
    if (operation == 2) {
        const auto previous = (std::uint64_t{reg(rd)} << 32U) | reg(rn);
        const auto result = previous + static_cast<std::uint64_t>(product);
        s.registers[rn] = static_cast<std::uint32_t>(result);
        s.registers[rd] = static_cast<std::uint32_t>(result >> 32U);
    } else {
        const auto result = product + (accumulate ? signed_word(reg(rn)) : 0);
        if (accumulate && (result < minimum || result > maximum)) s.sticky_overflow = true;
        s.registers[rd] = static_cast<std::uint32_t>(result);
    }
    // NZCV toujours conservés. Q est collant ; seuls SMLAxy/SMLAWy peuvent le lever.
}

} // namespace ravenemu::nds::detail
