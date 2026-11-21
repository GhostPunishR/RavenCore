#pragma once

#include "ppu/cgb_color.hpp"
#include "support/bits.hpp"

#include <bit>
#include <cstring>

namespace ravenemu::cgb {

/**
 * Encodage des deux images dans un état. Le format 10 les stockait en
 * i32 par pixel (69 % d'un état). Un pixel GB tient pourtant sur 16 bits :
 * teinte DMG 0..3 (ou 0 d'un écran effacé), ou couleur CGB 15 bits que
 * bgr555_to_argb développe. Chaque image commence par son encodage ; une
 * image dont un pixel ne se ré-encode pas à l'identique reste en i32.
 */
enum class FrameEncoding : std::uint8_t { raw_i32 = 0, compact_u16 = 1 };

/**
 * Encodage d'une image (160 × 144 pixels) dans un état instantané : en u16
 * par pixel quand tous se ré-encodent à l'identique, en i32 sinon.
 */
struct FrameCodec {
    static constexpr std::uint16_t literal_pixel = 0x8000;

    // Nul si le pixel est une teinte DMG (0..3) ou une couleur que
    // bgr555_to_argb produit : opaque, et dans chaque canal les 3 bits faibles
    // répètent les 3 bits forts (vérifié sur les 2^32 valeurs). Sans branche,
    // pour que la boucle d'une image entière se vectorise.
    [[nodiscard]] static constexpr std::uint32_t compact_miss(std::int32_t pixel) noexcept {
        const auto argb = static_cast<std::uint32_t>(pixel);
        const std::uint32_t literal_miss = argb & ~3U;
        const std::uint32_t color_miss = ((argb >> 24U) ^ 0xffU) | ((argb ^ (argb >> 5U)) & 0x070707U);
        return std::min(literal_miss, color_miss);
    }
    // Pixel compactable vers ses 16 bits, déjà dans l'ordre big-endian du
    // format une fois en mémoire.
    [[nodiscard]] static constexpr std::uint16_t encode_pixel(std::int32_t pixel) noexcept {
        const auto argb = static_cast<std::uint32_t>(pixel);
        const std::uint32_t literal = 0U - static_cast<std::uint32_t>(argb <= 3U);
        const std::uint32_t color = ((argb >> 19U) & 31U) | (((argb >> 11U) & 31U) << 5U) |
            (((argb >> 3U) & 31U) << 10U);
        return to_big_endian(static_cast<std::uint16_t>((literal & (literal_pixel | argb)) | (~literal & color)));
    }
    [[nodiscard]] static constexpr std::uint16_t to_big_endian(std::uint16_t value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return static_cast<std::uint16_t>((value << 8U) | (value >> 8U));
        }
        return value;
    }
    // Les images passent par blocs locaux : le compilateur sait alors que les
    // octets de l'état ne recouvrent pas l'image, et vectorise dès -O2.
    static constexpr std::size_t frame_block = 64;
    using FrameBlock = std::array<std::uint16_t, frame_block>;

    template <std::size_t Pixels>
    static void save(BinaryWriter& out, const std::array<std::int32_t, Pixels>& frame, bool tagged) {
        static_assert(Pixels % frame_block == 0);
        std::uint32_t miss = 0;
        if (tagged) for (const auto pixel : frame) miss |= compact_miss(pixel);
        const bool compact = tagged && miss == 0;
        if (tagged) {
            out.u8(static_cast<std::uint8_t>(compact ? FrameEncoding::compact_u16 : FrameEncoding::raw_i32));
        }
        if (!compact) {
            for (const auto pixel : frame) out.i32(pixel);
            return;
        }
        const auto bytes = out.append(frame.size() * 2);
        for (std::size_t first = 0; first < frame.size(); first += frame_block) {
            FrameBlock values{};
            for (std::size_t index = 0; index < frame_block; ++index) values[index] = encode_pixel(frame[first + index]);
            std::memcpy(bytes.data() + first * 2, values.data(), sizeof values);
        }
    }
    template <std::size_t Pixels>
    static void load(BinaryReader& in, std::array<std::int32_t, Pixels>& frame, bool tagged) {
        static_assert(Pixels % frame_block == 0);
        const auto encoding = tagged ? in.u8() : static_cast<std::uint8_t>(FrameEncoding::raw_i32);
        if (encoding == static_cast<std::uint8_t>(FrameEncoding::raw_i32)) {
            for (auto& pixel : frame) pixel = in.i32();
            return;
        }
        if (encoding != static_cast<std::uint8_t>(FrameEncoding::compact_u16)) {
            throw SaveStateError("État instantané corrompu (encodage d'image PPU)");
        }
        // Décodage sans branche ; un pixel littéral au-delà de 3 est refusé
        // après la boucle (l'image décodée n'est installée qu'en cas de succès).
        const auto bytes = in.view(frame.size() * 2);
        std::uint32_t invalid = 0;
        for (std::size_t first = 0; first < frame.size(); first += frame_block) {
            FrameBlock values{};
            std::memcpy(values.data(), bytes.data() + first * 2, sizeof values);
            for (std::size_t index = 0; index < frame_block; ++index) {
                const std::uint16_t value = to_big_endian(values[index]);
                const std::uint32_t literal = 0U - (std::uint32_t{value} >> 15U);
                invalid |= literal & value & 0x7ffcU;
                frame[first + index] = static_cast<std::int32_t>((literal & (value & 3U)) |
                    (~literal & static_cast<std::uint32_t>(bgr555_to_argb(value))));
            }
        }
        if (invalid != 0) throw SaveStateError("État instantané corrompu (pixel PPU)");
    }
};

} // namespace ravenemu::cgb
