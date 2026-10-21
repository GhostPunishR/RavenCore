#pragma once

#include "check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ravenemu::nds::test_support {
using ravenemu::testing::check;

inline void put16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}
inline void put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

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

struct Program {
    struct Literal { unsigned instruction, reg; std::uint32_t value; };
    std::vector<std::uint32_t> code;
    std::vector<Literal> literals;
    void load(unsigned reg, std::uint32_t value) {
        literals.push_back({static_cast<unsigned>(code.size()),reg,value}); code.push_back(0);
    }
    void store(unsigned reg, std::uint32_t destination) {
        load(4,destination); code.push_back(0xe5840000U | (reg << 12U));
    }
    void spi_byte(std::uint8_t output, unsigned result = 0) {
        code.insert(code.end(),{0xe3a01000U | output,0xe5c01002, // mov r1,#octet ; strb r1,[r0,#2]
                                0xe1d010b0,0xe3110080,0x1afffffc}); // attendre busy
        if (result) code.push_back(0xe5d00002U | (result << 12U));
    }
    std::vector<std::uint32_t> finish() {
        code.push_back(0xeafffffe);
        for (const auto& literal : literals) {
            const auto displacement = static_cast<unsigned>(code.size()) * 4U - literal.instruction * 4U - 8U;
            code[literal.instruction] = 0xe59f0000U | (literal.reg << 12U) | displacement;
            code.push_back(literal.value);
        }
        return code;
    }
};

inline std::vector<std::uint8_t> cartridge_image(const std::vector<std::uint32_t>& a9 = {0xeafffffe},
                                          const std::vector<std::uint32_t>& a7 = {0xeafffffe}) {
    const auto offset7 = static_cast<std::uint32_t>(0x200 + a9.size() * 4);
    std::vector<std::uint8_t> bytes(offset7 + a7.size() * 4);
    put32(bytes,0x20,0x200); put32(bytes,0x24,0x02000000); put32(bytes,0x28,0x02000000);
    put32(bytes,0x2c,static_cast<std::uint32_t>(a9.size() * 4));
    put32(bytes,0x30,offset7); put32(bytes,0x34,0x03800000); put32(bytes,0x38,0x03800000);
    put32(bytes,0x3c,static_cast<std::uint32_t>(a7.size() * 4));
    for (unsigned i = 0; i < a9.size(); ++i) put32(bytes,0x200 + i * 4,a9[i]);
    for (unsigned i = 0; i < a7.size(); ++i) put32(bytes,offset7 + i * 4,a7[i]);
    return bytes;
}

} // namespace ravenemu::nds::test_support
