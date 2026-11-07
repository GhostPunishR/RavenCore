#pragma once

#include "ravenemu/core.hpp"
#include "ravenemu/binary_io.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RAVENEMU_GBA_HOT_INLINE : chemin de chaque instruction (lecture du code), en
// ligne même là où le compilateur la jugerait trop longue, comme GCC en -O2.
// RAVENEMU_GBA_NOINLINE : cas rares tenus hors de ce chemin, pour qu'ils ne lui
// imposent pas leur pile.
#if defined(_MSC_VER) && !defined(__clang__)
#define RAVENEMU_GBA_HOT_INLINE __forceinline
#define RAVENEMU_GBA_NOINLINE __declspec(noinline)
#else
#define RAVENEMU_GBA_HOT_INLINE inline __attribute__((always_inline))
#define RAVENEMU_GBA_NOINLINE __attribute__((noinline))
#endif

namespace ravenemu::gba {

using detail::BinaryReader;
using detail::BinaryWriter;

constexpr std::uint32_t u32(std::int32_t value) noexcept {
    return static_cast<std::uint32_t>(value);
}

constexpr std::int32_t i32(std::uint32_t value) noexcept {
    return static_cast<std::int32_t>(value);
}

constexpr std::int32_t add32(std::int32_t left, std::int32_t right) noexcept {
    return i32(u32(left) + u32(right));
}

constexpr std::int32_t sign_extend(std::uint32_t value, unsigned bits) noexcept {
    const auto shift = 32U - bits;
    return i32(value << shift) >> shift;
}

using RomImage = std::shared_ptr<const std::vector<std::uint8_t>>;

} // namespace ravenemu::gba
