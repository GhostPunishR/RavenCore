#include "cartridge/header.hpp"

#include <algorithm>
#include <array>

namespace ravenemu::nds {

namespace {

std::uint32_t word(std::span<const std::uint8_t> rom, std::size_t at) noexcept {
    return std::uint32_t{rom[at]} | (std::uint32_t{rom[at + 1]} << 8U) | (std::uint32_t{rom[at + 2]} << 16U) |
           (std::uint32_t{rom[at + 3]} << 24U);
}

std::string text(std::span<const std::uint8_t> rom, std::size_t at, std::size_t length) {
    std::string value;
    for (std::size_t index = 0; index < length && rom[at + index] != 0; ++index)
        value.push_back(static_cast<char>(rom[at + index]));
    return value;
}

// Le binaire tient dans la ROM et dans la zone où le BIOS accepte de le
// charger : [first, last] pour l'adresse de départ, `limit` pour la fin.
bool fits(std::uint64_t address, std::uint64_t size, std::uint32_t first, std::uint32_t limit) noexcept {
    return address >= first && address + size <= limit;
}

} // namespace

CartridgeHeader parse_header(std::span<const std::uint8_t> rom) {
    if (rom.size() < 0x200) throw CartridgeError("ROM DS trop courte pour son en-tête");
    CartridgeHeader header;
    header.title = text(rom, 0x000, 12);
    header.game_code = text(rom, 0x00c, 4);
    header.capacity = rom[0x014];
    header.arm9_offset = word(rom, 0x020);
    header.arm9_entry = word(rom, 0x024);
    header.arm9_address = word(rom, 0x028);
    header.arm9_size = word(rom, 0x02c);
    header.arm7_offset = word(rom, 0x030);
    header.arm7_entry = word(rom, 0x034);
    header.arm7_address = word(rom, 0x038);
    header.arm7_size = word(rom, 0x03c);
    header.secure_area_crc = static_cast<std::uint16_t>(rom[0x06c] | (rom[0x06d] << 8U));
    header.header_crc = static_cast<std::uint16_t>(rom[0x15e] | (rom[0x15f] << 8U));
    if (crc16(rom.first(0x15e)) != header.header_crc) throw CartridgeError("CRC de l'en-tête DS invalide");
    const auto in_rom = [&](std::uint64_t offset, std::uint64_t size) { return offset + size <= rom.size(); };
    // Zones de chargement de GBATEK : RAM principale jusqu'à 0x023BFE00 ;
    // pour l'ARM7, aussi la WRAM de 0x037F8000 à 0x03807E00.
    const auto arm9_ok = in_rom(header.arm9_offset, header.arm9_size) &&
                         fits(header.arm9_address, header.arm9_size, 0x0200'0000U, 0x023b'fe00U);
    const auto arm7_ok = in_rom(header.arm7_offset, header.arm7_size) &&
                         (fits(header.arm7_address, header.arm7_size, 0x0200'0000U, 0x023b'fe00U) ||
                          fits(header.arm7_address, header.arm7_size, 0x037f'8000U, 0x0380'7e00U));
    if (!arm9_ok) throw CartridgeError("binaire ARM9 hors de la ROM ou de la RAM principale");
    if (!arm7_ok) throw CartridgeError("binaire ARM7 hors de la ROM ou des zones de chargement");
    return header;
}

SecureArea secure_area(std::span<const std::uint8_t> rom, const CartridgeHeader& header) noexcept {
    if (header.arm9_offset < 0x4000 || header.arm9_offset >= 0x8000 || rom.size() < 0x4800) return SecureArea::absent;
    // Homebrew (code de jeu nul ou « #### ») : pas de zone sécurisée.
    if (header.game_code.empty() || header.game_code == "####") return SecureArea::absent;
    const auto area = rom.subspan(0x4000, 0x800);
    constexpr std::array<std::uint8_t, 8> raw_id{'e', 'n', 'c', 'r', 'y', 'O', 'b', 'j'};
    constexpr std::array<std::uint8_t, 8> destroyed_id{0xff, 0xde, 0xff, 0xe7, 0xff, 0xde, 0xff, 0xe7};
    constexpr std::array<std::uint8_t, 6> fixed{0xff, 0xde, 0xff, 0xe7, 0xff, 0xde};
    if (std::equal(raw_id.begin(), raw_id.end(), area.begin())) return SecureArea::decrypted;
    if (std::equal(destroyed_id.begin(), destroyed_id.end(), area.begin())) return SecureArea::decrypted;
    if (std::equal(fixed.begin(), fixed.end(), area.begin() + 8)) return SecureArea::decrypted;
    // Pas de zone sécurisée du tout (homebrew) : les 2 premiers Kio sont nuls.
    if (std::all_of(area.begin(), area.end(), [](std::uint8_t byte) { return byte == 0; })) return SecureArea::absent;
    return SecureArea::encrypted;
}

} // namespace ravenemu::nds
