#pragma once

#include "check.hpp"
#include "program_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ravenemu::nds::test_support {
using ravenemu::testing::check;

// Générateur de fixtures par table de demi-octets, distinct du calcul du cœur.
inline void seal(std::span<std::uint8_t> bytes, std::size_t offset) {
    constexpr std::array<unsigned,16> table{0,0xcc01,0xd801,0x1400,0xf001,0x3c00,0x2800,0xe401,
                                           0xa001,0x6c00,0x7800,0xb401,0x5000,0x9c01,0x8801,0x4400};
    unsigned crc = 0xffff;
    for (std::size_t i = 0; i < 0x70; ++i) {
        crc ^= bytes[offset + i];
        crc = (crc >> 4U) ^ table[crc & 15U];
        crc = (crc >> 4U) ^ table[crc & 15U];
    }
    put16(bytes,offset + 0x72,static_cast<std::uint16_t>(crc));
}

inline void settings(std::span<std::uint8_t> bytes, std::uint32_t offset, std::uint16_t counter, std::uint8_t color) {
    std::fill_n(bytes.begin() + offset,0x70,0);
    put16(bytes,offset,5); bytes[offset + 2] = color;
    put16(bytes,offset + 0x58,0x200); put16(bytes,offset + 0x5a,0x300);
    bytes[offset + 0x5c] = 33; bytes[offset + 0x5d] = 25;
    put16(bytes,offset + 0x5e,0xe00); put16(bytes,offset + 0x60,0xd00);
    bytes[offset + 0x62] = 225; bytes[offset + 0x63] = 169;
    put16(bytes,offset + 0x70,counter); seal(bytes,offset);
}

inline std::vector<std::uint8_t> firmware(std::size_t size = 0x40000, std::uint32_t offset = 0x3fe00) {
    std::vector<std::uint8_t> bytes(size,0xff);
    put16(bytes,0x20,static_cast<std::uint16_t>(offset / 8));
    settings(bytes,offset,10,3); settings(bytes,offset + 0x100,9,7);
    bytes[0] = 0x5a; bytes[size - 1] = 0xa6;
    // Oracle obtenu par division polynomiale MSB-first puis réflexion, hors cœur.
    check(bytes[offset + 0x72] == 0x43 && bytes[offset + 0x73] == 0x9b, "fixture CRC attendue 0x9b43");
    return bytes;
}

} // namespace ravenemu::nds::test_support
