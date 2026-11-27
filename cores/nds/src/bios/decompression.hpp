#pragma once

#include <cstdint>

// Décompressions du BIOS qui lisent leur source directement (GBATEK, « BIOS
// Decompression Functions ») : BitUnPack (10h), LZ77 et RL vers des octets
// (11h, 14h), filtres différentiels de 8 et 16 bits (16h, 18h). Toutes passent
// par le bus du CPU, qui en compte les accès. Les variantes qui lisent la
// source par des fonctions de rappel du jeu (12h, 13h, 15h) sont du code ARM
// du BIOS de RavenCore (callback_decompression.hpp).
namespace ravenemu::nds::decompression {

// En-tête : bits 4-7 type, bits 8-31 taille décompressée en octets.
inline std::uint32_t decompressed_size(std::uint32_t header) noexcept { return header >> 8U; }

template <typename BusType>
void lz77(BusType& bus, std::uint32_t source, std::uint32_t destination) {
    const auto size = decompressed_size(static_cast<std::uint32_t>(bus.read32(source & ~3U)));
    source += 4;
    std::uint32_t written = 0;
    while (written < size) {
        auto flags = static_cast<std::uint32_t>(bus.read8(source++));
        for (unsigned block = 0; block < 8 && written < size; ++block, flags <<= 1U) {
            if ((flags & 0x80U) == 0) {
                bus.write8(destination + written++, bus.read8(source++));
                continue;
            }
            const auto first = static_cast<std::uint32_t>(bus.read8(source++));
            const auto second = static_cast<std::uint32_t>(bus.read8(source++));
            const auto distance = (((first & 15U) << 8U) | second) + 1U;
            for (auto length = (first >> 4U) + 3U; length > 0 && written < size; --length, ++written)
                bus.write8(destination + written, bus.read8(destination + written - distance));
        }
    }
}

template <typename BusType>
void run_length(BusType& bus, std::uint32_t source, std::uint32_t destination) {
    const auto size = decompressed_size(static_cast<std::uint32_t>(bus.read32(source & ~3U)));
    source += 4;
    std::uint32_t written = 0;
    while (written < size) {
        const auto flag = static_cast<std::uint32_t>(bus.read8(source++));
        if ((flag & 0x80U) != 0) {  // un octet répété (flag & 0x7F) + 3 fois
            const auto value = bus.read8(source++);
            for (auto length = (flag & 0x7fU) + 3U; length > 0 && written < size; --length) bus.write8(destination + written++, value);
        } else {  // (flag & 0x7F) + 1 octets tels quels
            for (auto length = (flag & 0x7fU) + 1U; length > 0 && written < size; --length)
                bus.write8(destination + written++, bus.read8(source++));
        }
    }
}

// Chaque unité est la différence avec la précédente.
template <typename BusType>
void differential(BusType& bus, std::uint32_t source, std::uint32_t destination, bool halfwords) {
    const auto size = decompressed_size(static_cast<std::uint32_t>(bus.read32(source & ~3U)));
    source += 4;
    std::uint32_t value = 0;
    for (std::uint32_t written = 0; written < size; written += halfwords ? 2U : 1U) {
        if (halfwords) {
            value = (value + static_cast<std::uint32_t>(bus.read16(source + written))) & 0xffffU;
            bus.write16(destination + written, value);
        } else {
            value = (value + static_cast<std::uint32_t>(bus.read8(source + written))) & 0xffU;
            bus.write8(destination + written, value);
        }
    }
}

// BitUnPack : r2 pointe sur la longueur de la source (16 bits), la largeur
// des unités source et destination (8 bits chacune), puis un décalage ajouté
// aux unités non nulles (bits 0-30), et aussi aux nulles si le bit 31 est levé.
// La destination s'écrit par mots de 32 bits.
template <typename BusType>
void bit_unpack(BusType& bus, std::uint32_t source, std::uint32_t destination, std::uint32_t info) {
    const auto length = static_cast<std::uint32_t>(bus.read16(info & ~1U)) & 0xffffU;
    const auto source_width = static_cast<std::uint32_t>(bus.read8(info + 2)) & 0xffU;
    const auto destination_width = static_cast<std::uint32_t>(bus.read8(info + 3)) & 0xffU;
    const auto offset_word = static_cast<std::uint32_t>(bus.read32((info + 4) & ~3U));
    const auto valid = [](std::uint32_t width) { return width == 1 || width == 2 || width == 4 || width == 8; };
    if (!valid(source_width) || !(valid(destination_width) || destination_width == 16 || destination_width == 32)) return;
    const auto offset = offset_word & 0x7fff'ffffU;
    const auto zero_too = (offset_word >> 31U) != 0;
    const auto source_mask = (1U << source_width) - 1U;
    const auto destination_mask = destination_width == 32 ? 0xffff'ffffU : (1U << destination_width) - 1U;
    std::uint32_t word = 0;
    unsigned filled = 0;
    destination &= ~3U;
    for (std::uint32_t index = 0; index < length; ++index) {
        const auto byte = static_cast<std::uint32_t>(bus.read8(source + index)) & 0xffU;
        for (unsigned bit = 0; bit < 8; bit += source_width) {
            auto unit = (byte >> bit) & source_mask;
            if (unit != 0 || zero_too) unit += offset;
            word |= (unit & destination_mask) << filled;
            filled += destination_width;
            if (filled >= 32) {
                bus.write32(destination, word);
                destination += 4;
                word = 0;
                filled = 0;
            }
        }
    }
}

} // namespace ravenemu::nds::decompression
