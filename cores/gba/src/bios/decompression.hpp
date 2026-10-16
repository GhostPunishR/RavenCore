#pragma once

#include "memory/bus.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ravenemu::gba {

/**
 * Décompression du BIOS GBA (SWI 0x10 à 0x18, d'après GBATEK) : BitUnPack,
 * LZ77, Huffman, RLE et filtres différentiels. Comme le BIOS d'origine, elles
 * lisent la source et écrivent le résultat par le bus ; une destination en
 * VRAM reçoit des demi-mots. Une taille ou des paramètres invalides sont
 * signalés aux diagnostics, sans rien écrire.
 */
class BiosDecompression {
public:
    explicit BiosDecompression(Bus& bus) : bus_(bus) {}

    void lz77(std::int32_t source, std::int32_t destination, bool vram) {
        const auto size = output_size(source);
        if (!validate_size(size)) return;
        std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
        auto input = add32(source, 4);
        int written{};
        while (written < size) {
            const auto flags = bus_.read8(input);
            input = add32(input, 1);
            for (int bit = 7; bit >= 0 && written < size; --bit) {
                if (((flags >> bit) & 1) == 0) {
                    output[static_cast<std::size_t>(written++)] = static_cast<std::uint8_t>(bus_.read8(input));
                    input = add32(input, 1);
                    continue;
                }
                const auto first = bus_.read8(input);
                const auto second = bus_.read8(add32(input, 1));
                input = add32(input, 2);
                const auto length = (first >> 4) + 3;
                const auto distance = ((first & 15) << 8 | second) + 1;
                auto from = written - distance;
                if (from < 0) {
                    decompression_error("référence LZ77 GBA invalide");
                    return;
                }
                for (int count = 0; count < length && written < size; ++count)
                    output[static_cast<std::size_t>(written++)] = output[static_cast<std::size_t>(from++)];
            }
        }
        write_output(destination, output, vram);
    }
    void run_length(std::int32_t source, std::int32_t destination, bool vram) {
        const auto size = output_size(source);
        if (!validate_size(size)) return;
        std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
        auto input = add32(source, 4);
        int written{};
        while (written < size) {
            const auto flag = bus_.read8(input);
            input = add32(input, 1);
            const auto length = (flag & 0x7f) + ((flag & 0x80) != 0 ? 3 : 1);
            if ((flag & 0x80) != 0) {
                const auto value = static_cast<std::uint8_t>(bus_.read8(input));
                input = add32(input, 1);
                for (int i = 0; i < length && written < size; ++i) output[static_cast<std::size_t>(written++)] = value;
            } else
                for (int i = 0; i < length && written < size; ++i) {
                    output[static_cast<std::size_t>(written++)] = static_cast<std::uint8_t>(bus_.read8(input));
                    input = add32(input, 1);
                }
        }
        write_output(destination, output, vram);
    }
    void huffman(std::int32_t source, std::int32_t destination) {
        const auto header = bus_.read32(source);
        const auto size = static_cast<int>((u32(header) >> 8U) & 0xffffffU);
        const auto symbol_bits = static_cast<int>(u32(header) & 15U);
        if (!validate_size(size) || (symbol_bits != 4 && symbol_bits != 8)) {
            decompression_error("flux Huffman GBA invalide");
            return;
        }
        const auto tree_start = add32(source, 4);
        const auto tree_bytes = (bus_.read8(tree_start) + 1) * 2;
        const auto root = add32(tree_start, 1);
        const auto tree_end = add32(tree_start, tree_bytes);
        auto stream = tree_end;
        std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
        int written{};
        bool high{};
        int pending{};
        auto node = root;
        std::uint32_t word{};
        int bits{};
        std::int64_t budget = static_cast<std::int64_t>(size) * 8 * 32 + 64;
        while (written < size) {
            if (--budget <= 0) {
                decompression_error("flux Huffman GBA sans fin");
                return;
            }
            if (bits == 0) {
                word = u32(bus_.read32(stream));
                stream = add32(stream, 4);
                bits = 32;
            }
            const auto bit = static_cast<int>(word >> 31U);
            word <<= 1U;
            --bits;
            const auto node_value = bus_.read8(node);
            const auto next = add32(i32(u32(node) & ~1U), ((node_value & 0x3f) + 1) * 2 + bit);
            const auto leaf = bit == 0 ? (node_value & 0x80) != 0 : (node_value & 0x40) != 0;
            if (u32(next) < u32(root) || u32(next) >= u32(tree_end)) {
                decompression_error("arbre Huffman GBA invalide");
                return;
            }
            if (!leaf) {
                node = next;
                continue;
            }
            const auto symbol = bus_.read8(next);
            node = root;
            if (symbol_bits == 8)
                output[static_cast<std::size_t>(written++)] = static_cast<std::uint8_t>(symbol);
            else if (!high) {
                pending = symbol & 15;
                high = true;
            } else {
                output[static_cast<std::size_t>(written++)] = static_cast<std::uint8_t>(pending | (symbol & 15) << 4);
                high = false;
            }
        }
        write_output(destination, output, true);
    }
    void differential(std::int32_t source, std::int32_t destination, bool wide, bool vram) {
        const auto size = output_size(source);
        if (!validate_size(size)) return;
        std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
        auto input = add32(source, 4);
        if (wide) {
            int previous{};
            for (int index = 0; index + 1 < size; index += 2) {
                const auto delta = bus_.read8(input) | bus_.read8(add32(input, 1)) << 8;
                input = add32(input, 2);
                previous = (previous + delta) & 0xffff;
                output[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(previous);
                output[static_cast<std::size_t>(index + 1)] = static_cast<std::uint8_t>(previous >> 8);
            }
        } else {
            int previous{};
            for (int index = 0; index < size; ++index) {
                previous = (previous + bus_.read8(input)) & 0xff;
                input = add32(input, 1);
                output[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(previous);
            }
        }
        write_output(destination, output, vram);
    }
    void bit_unpack(std::int32_t source, std::int32_t destination, std::int32_t parameters) {
        const auto length = bus_.read16(parameters);
        const auto source_width = bus_.read8(add32(parameters, 2));
        const auto destination_width = bus_.read8(add32(parameters, 3));
        const auto offset_word = u32(bus_.read32(add32(parameters, 4)));
        const auto data_offset = offset_word & 0x7fffffffU;
        const auto offset_zero = (offset_word & 0x80000000U) != 0;
        if (length <= 0 || source_width <= 0 || source_width > 8 || destination_width <= 0 || destination_width > 32 ||
            8 % source_width != 0) {
            decompression_error("paramètres BitUnPack GBA invalides");
            return;
        }
        std::uint64_t buffer{};
        int bits{};
        const auto mask = (1U << static_cast<unsigned>(source_width)) - 1U;
        for (int index = 0; index < length; ++index) {
            const auto byte = static_cast<unsigned>(bus_.read8(add32(source, index)));
            for (int consumed = 0; consumed < 8; consumed += source_width) {
                const auto value = (byte >> static_cast<unsigned>(consumed)) & mask;
                const auto expanded = value == 0 && !offset_zero ? 0U : value + data_offset;
                buffer |= static_cast<std::uint64_t>(expanded) << static_cast<unsigned>(bits);
                bits += destination_width;
                if (bits >= 32) {
                    bus_.write32(destination, i32(static_cast<std::uint32_t>(buffer)));
                    destination = add32(destination, 4);
                    buffer >>= 32U;
                    bits -= 32;
                }
            }
        }
        if (bits > 0) bus_.write32(destination, i32(static_cast<std::uint32_t>(buffer)));
    }

private:
    static constexpr std::size_t max_output = 1U << 20U;

    [[nodiscard]] int output_size(std::int32_t source) {
        return static_cast<int>((u32(bus_.read32(source)) >> 8U) & 0x00ff'ffffU);
    }
    bool validate_size(int size) {
        if (size > 0 && static_cast<std::size_t>(size) <= max_output) return true;
        bus_.diagnostics.report(DiagnosticEvent::decompression_error, "taille de décompression GBA invalide");
        return false;
    }
    void decompression_error(std::string message) {
        bus_.diagnostics.report(DiagnosticEvent::decompression_error, std::move(message));
    }
    void write_output(std::int32_t destination, std::span<const std::uint8_t> output, bool halfwords) {
        if (!halfwords) {
            for (std::size_t index = 0; index < output.size(); ++index)
                bus_.write8(add32(destination, static_cast<int>(index)), output[index]);
            return;
        }
        std::size_t index{};
        for (; index + 1 < output.size(); index += 2)
            bus_.write16(add32(destination, static_cast<int>(index)), output[index] | output[index + 1] << 8);
        if (index < output.size()) bus_.write16(add32(destination, static_cast<int>(index)), output[index]);
    }

    Bus& bus_;
};

}  // namespace ravenemu::gba
