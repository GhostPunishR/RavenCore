#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ravenemu::nds::test_support {

inline void put16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}
inline void put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
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
