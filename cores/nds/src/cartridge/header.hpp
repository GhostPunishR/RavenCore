#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

// En-tête d'une cartouche DS et ses vérifications (GBATEK, « DS Cartridge
// Header »).
namespace ravenemu::nds {

class CartridgeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CartridgeHeader {
    static constexpr std::size_t size = 0x170;  // partie recopiée en RAM

    std::string title;
    std::string game_code;
    std::uint8_t capacity{};  // taille de la puce : 128 Kio << capacity
    std::uint32_t arm9_offset{};
    std::uint32_t arm9_entry{};
    std::uint32_t arm9_address{};
    std::uint32_t arm9_size{};
    std::uint32_t arm7_offset{};
    std::uint32_t arm7_entry{};
    std::uint32_t arm7_address{};
    std::uint32_t arm7_size{};
    std::uint16_t secure_area_crc{};
    std::uint16_t header_crc{};
};

// Zone sécurisée : présente quand le code de l'ARM9 commence entre 0x4000 et
// 0x7FFF. Déchiffrée, elle porte « encryObj » (ou, déjà détruit par un BIOS,
// E7FFDEFFh deux fois) et les octets fixes FF DE FF E7 FF DE en 8-13.
enum class SecureArea { absent, decrypted, encrypted };

// CRC-16 réfléchi, polynôme 0xA001, valeur initiale 0xFFFF.
inline std::uint16_t crc16(std::span<const std::uint8_t> bytes, std::uint16_t initial = 0xffffU) noexcept {
    std::uint32_t crc = initial;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xa001U : crc >> 1U;
    }
    return static_cast<std::uint16_t>(crc);
}

CartridgeHeader parse_header(std::span<const std::uint8_t> rom);
SecureArea secure_area(std::span<const std::uint8_t> rom, const CartridgeHeader& header) noexcept;

} // namespace ravenemu::nds
