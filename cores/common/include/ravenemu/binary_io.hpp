#pragma once

#include "ravenemu/core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ravenemu::detail {

class BinaryWriter {
public:
    explicit BinaryWriter(std::size_t reserve = 0) { bytes_.reserve(reserve); }

    void boolean(bool value) { u8(value ? 1U : 0U); }
    void u8(std::uint8_t value) { bytes_.push_back(value); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

    void u32(std::uint32_t value) {
        u8(static_cast<std::uint8_t>(value >> 24U));
        u8(static_cast<std::uint8_t>(value >> 16U));
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value >> 32U));
        u32(static_cast<std::uint32_t>(value));
    }

    void f64(double value) {
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        u64(std::bit_cast<std::uint64_t>(value));
    }

    void raw(std::span<const std::uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    template <std::size_t N>
    void raw(const std::array<std::uint8_t, N>& bytes) {
        raw(std::span<const std::uint8_t>{bytes});
    }

    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool boolean() { return u8() != 0; }

    [[nodiscard]] std::uint8_t u8() {
        require(1);
        return bytes_[position_++];
    }

    // Chaque lecture fait avancer `position_` : les opérandes d'un `|` ne sont
    // pas séquencés en C++, d'où une instruction par octet lu. Sinon un
    // compilateur pourrait lire l'octet de poids faible en premier.
    [[nodiscard]] std::uint16_t u16() {
        const auto high = static_cast<std::uint16_t>(u8());
        const auto low = static_cast<std::uint16_t>(u8());
        return static_cast<std::uint16_t>((high << 8U) | low);
    }

    [[nodiscard]] std::uint32_t u32() {
        const auto high = static_cast<std::uint32_t>(u16());
        const auto low = static_cast<std::uint32_t>(u16());
        return (high << 16U) | low;
    }

    [[nodiscard]] std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    [[nodiscard]] std::int32_t bounded_i32(std::int32_t minimum, std::int32_t maximum) {
        const auto value = i32();
        if (value < minimum || value > maximum) throw SaveStateError("État instantané hors limites : " + std::to_string(value) + " [" + std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        const auto high = static_cast<std::uint64_t>(u32());
        const auto low = static_cast<std::uint64_t>(u32());
        return (high << 32U) | low;
    }

    [[nodiscard]] std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    [[nodiscard]] double f64() { return std::bit_cast<double>(u64()); }

    void raw(std::span<std::uint8_t> destination) {
        require(destination.size());
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                    static_cast<std::ptrdiff_t>(destination.size()), destination.begin());
        position_ += destination.size();
    }

    [[nodiscard]] std::vector<std::uint8_t> raw(std::size_t count) {
        std::vector<std::uint8_t> result(count);
        raw(result);
        return result;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
    [[nodiscard]] bool exhausted() const noexcept { return position_ == bytes_.size(); }

private:
    void require(std::size_t count) const {
        if (count > remaining()) throw SaveStateError("État instantané corrompu ou tronqué");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

} // namespace ravenemu::detail
