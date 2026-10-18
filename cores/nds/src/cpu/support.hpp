#pragma once

#include <ravenemu/nds/cpu.hpp>

#include <bit>
#include <limits>

namespace ravenemu::nds::detail {

inline std::uint32_t sign_extend(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint32_t{1} << (bits - 1U);
    return (value ^ sign) - sign;
}

struct Result {
    std::uint32_t value;
    bool carry;
    bool overflow;
};

inline Result add(std::uint32_t x, std::uint32_t y, bool carry) {
    const auto sum = static_cast<std::uint64_t>(x) + y + (carry ? 1U : 0U);
    const auto signed_sum = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(x)) +
                            std::bit_cast<std::int32_t>(y) + (carry ? 1 : 0);
    return {static_cast<std::uint32_t>(sum), (sum >> 32U) != 0,
            signed_sum < std::numeric_limits<std::int32_t>::min() ||
            signed_sum > std::numeric_limits<std::int32_t>::max()};
}

inline bool condition(const CpuState& s, unsigned code) {
    switch (code) {
    case 0: return s.zero;
    case 1: return !s.zero;
    case 2: return s.carry;
    case 3: return !s.carry;
    case 4: return s.negative;
    case 5: return !s.negative;
    case 6: return s.overflow;
    case 7: return !s.overflow;
    case 8: return s.carry && !s.zero;
    case 9: return !s.carry || s.zero;
    case 10: return s.negative == s.overflow;
    case 11: return s.negative != s.overflow;
    case 12: return !s.zero && s.negative == s.overflow;
    case 13: return s.zero || s.negative != s.overflow;
    case 14: return true;
    default: return false;
    }
}

struct Shifted {
    std::uint32_t value;
    bool carry;
};

inline Shifted shift(std::uint32_t value, unsigned type, unsigned amount, bool carry, bool by_register = false) {
    if (by_register) {
        amount &= 0xffU;
        if (amount == 0) return {value, carry};
    } else if (amount == 0) {
        if (type == 0) return {value, carry};
        if (type == 3) return {(value >> 1U) | (carry ? 0x80000000U : 0U), (value & 1U) != 0};
        amount = 32;
    }
    switch (type) {
    case 0:
        if (amount > 32) return {0, false};
        return {amount == 32 ? 0U : value << amount, ((value >> (32U - amount)) & 1U) != 0};
    case 1:
        if (amount > 32) return {0, false};
        return {amount == 32 ? 0U : value >> amount, ((value >> (amount - 1U)) & 1U) != 0};
    case 2: {
        const bool negative = (value & 0x80000000U) != 0;
        if (amount >= 32) return {negative ? 0xffffffffU : 0U, negative};
        const auto fill = negative ? 0xffffffffU << (32U - amount) : 0U;
        return {(value >> amount) | fill, ((value >> (amount - 1U)) & 1U) != 0};
    }
    default: {
        const auto rotated = std::rotr(value, static_cast<int>(amount & 31U));
        return {rotated, (rotated >> 31U) != 0};
    }
    }
}

} // namespace ravenemu::nds::detail
