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

    // Les entiers s'ajoutent en un seul agrandissement du tampon : un état
    // compte des centaines de milliers d'octets, écrits à chaque frame par le
    // rewind ou le run-ahead d'un frontend. Le format reste big-endian.
    void u16(std::uint16_t value) {
        auto* out = grow(2);
        out[0] = static_cast<std::uint8_t>(value >> 8U);
        out[1] = static_cast<std::uint8_t>(value);
    }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

    void u32(std::uint32_t value) {
        auto* out = grow(4);
        out[0] = static_cast<std::uint8_t>(value >> 24U);
        out[1] = static_cast<std::uint8_t>(value >> 16U);
        out[2] = static_cast<std::uint8_t>(value >> 8U);
        out[3] = static_cast<std::uint8_t>(value);
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

    /** Réserve `count` octets à remplir sur place (encodages en bloc). */
    [[nodiscard]] std::span<std::uint8_t> append(std::size_t count) { return {grow(count), count}; }

    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

private:
    [[nodiscard]] std::uint8_t* grow(std::size_t count) {
        const auto at = bytes_.size();
        bytes_.resize(at + count);
        return bytes_.data() + at;
    }

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

    // `consume` avance la position une seule fois par valeur ; les octets sont
    // ensuite combinés sans effet de bord, donc l'ordre big-endian ne dépend
    // pas de l'ordre d'évaluation choisi par le compilateur.
    [[nodiscard]] std::uint16_t u16() {
        const auto* in = consume(2);
        return static_cast<std::uint16_t>((in[0] << 8U) | in[1]);
    }

    [[nodiscard]] std::uint32_t u32() {
        const auto* in = consume(4);
        return (static_cast<std::uint32_t>(in[0]) << 24U) | (static_cast<std::uint32_t>(in[1]) << 16U) |
            (static_cast<std::uint32_t>(in[2]) << 8U) | static_cast<std::uint32_t>(in[3]);
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

    /** Les `count` octets suivants, vérifiés une seule fois (décodages en bloc). */
    [[nodiscard]] std::span<const std::uint8_t> view(std::size_t count) { return {consume(count), count}; }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
    [[nodiscard]] bool exhausted() const noexcept { return position_ == bytes_.size(); }

private:
    // Une seule vérification de bornes par valeur, puis lecture directe :
    // les octets sont lus dans l'ordre du format, sans effet de bord.
    [[nodiscard]] const std::uint8_t* consume(std::size_t count) {
        require(count);
        const auto* data = bytes_.data() + position_;
        position_ += count;
        return data;
    }

    void require(std::size_t count) const {
        if (count > remaining()) throw SaveStateError("État instantané corrompu ou tronqué");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

} // namespace ravenemu::detail
